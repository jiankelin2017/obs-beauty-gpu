// obs-beauty —— OBS 美颜滤镜（自研实现，无参考实现美颜库依赖）
//
// 结构：
//   obs-beauty.cpp     OBS 胶水层：属性页、帧捕获、CPU 回传、绘制、性能埋点
//   beauty_core.cpp    引擎抽象 + CoreEngine（自研实现，唯一的生产路径）
//   beauty_smooth.cpp  磨皮核（三层表面模糊 / 双尺度带通 + 振幅门控）
//   beauty_freq.cpp    色调（清晰 / 美白 / 红润）与细节回填（锐化）
//   beauty_face.cpp    人脸检测/关键点（YuNet ONNX 模型）
//   beauty_region.cpp  五官局部五项
//
// 数据流（每帧）：
//   GPU 纹理 → 下载到 CPU（RGBA）→ CoreEngine::process（检测在独立 worker 上
//   异步跑）→ 上传回纹理 → 绘制。全 0 参数时逐字节透传，不进引擎。
//
// 唯一不能改的是 source id：OBS 场景 JSON 里存的就是这个字符串，改了老场景里
// 已挂好的滤镜会变成「缺失滤镜」。
//
// 3.1.21：删掉「镜头画面旋转」「恢复默认 / 保存 preset.json / 读取 preset.json」
// 三个属性；总开关置顶；旧 id 别名不再注册（老场景已一次性迁移）。
#include <windows.h>
#include <gl/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/bmem.h>

#include "beauty_params.h"   // BeautySettings / P_*（纯数据，不引入任何引擎实现）
#include "beauty_log.h"
// 引擎抽象层：本文件只认 IBeautyEngine，不直接依赖任何具体实现。
// 3.1.7 起只有自研 CoreEngine 一条生产路径（旧链路只在离线基准 DLL 里保留）。
#include "beauty_core.h"
// 4d：五官椭圆对拍的 CPU 基准要直接调 beautycore::applyEllipses —— 这是刻意的：
// 几何快照已经把**同一份**椭圆交给宿主，用同一个算子算基准，比对才有意义。
#include "beauty_region.h"

#define PLUGIN_NAME "obs-beauty"
#define PLUGIN_VERSION BEAUTY_BUILD_STR
// 5b-2 切片 4：显存帧环槽数（见 beauty_ensure_ring 的注释）。
// 放在文件顶部是因为析构函数里也要按它遍历，而析构在环的实现之前。
// S3（2.5.0）：2 → 3。异步回读让 CPU 侧整体晚一帧，引擎的 heldTag_ 可能落后
// 两帧；两槽时那一格刚被「本帧」覆盖，环取不到 → 白回退成整帧上传。
#define BEAUTY_RING_SLOTS 3

// source id。**永远不要改** —— 场景文件里存的就是这个字符串。
// 3.1.21 起不再注册「旧 id 别名」，留两个 id 只会让滤镜列表出现两个条目；
// 老场景已由装机脚本一次性把 id 改写成本值。
#define BEAUTY_FILTER_ID "obs_beauty_filter"

// source id。**永远不要改** —— 场景文件里存的是这个字符串。
// 3.1.21 起不再注册「旧 id 别名」：老场景已在装机脚本里一次性迁移到本 id
// （见 05_文档/CHANGELOG 3.1.21）。留两个 id 只会让滤镜列表出现两个条目。

// ---------------------------------------------------------------------------
// 引擎 → OBS 日志钩子。模块加载时装一次；不装的话引擎消息落到 stderr。
//
// ⚠ blog() 在 libobs 里**没有** printf 格式标注，编译器（/W3）不会检查格式串与
// 参数是否配对。3.1.4 就因此崩过：给 init OK 加了 engine=%s/%s 却忘了删掉后面
// 的 sdk=%s，7 个占位符配 6 个参数，region=%s 拿到的是整数 itemId，ucrtbase
// 解引用 0x5350 直接访问违例，OBS 在模块加载阶段连崩两次。
// **改下面任何一条多参数日志，必须手工数一遍 % 的个数与参数个数。**
// ---------------------------------------------------------------------------
static void beauty_obs_log(int level, const char* msg)
{
	blog(level == BEAUTY_LOG_ERROR	  ? LOG_ERROR
	     : level == BEAUTY_LOG_WARNING ? LOG_WARNING
	     : level == BEAUTY_LOG_DEBUG	  ? LOG_DEBUG
					  : LOG_INFO,
	     "[" PLUGIN_NAME "] %s", msg);
}

// region 状态的措辞。两种引擎的 off 含义完全不同，日志里必须能分开看：
//   旧链路 —— 没装 皮肤分割库 / 明文 bundle，退回 v1.4 的颜色启发式（画面仍然正常）；
//   core —— YuNet 人脸模型没加载，updateMask() 直接返回 false，磨皮退化成全帧
//           （背景一起磨、CPU 顶满），且清晰/美白/红润/五官五项/脸内锐化全部不执行。
static const char *region_state(const IBeautyEngine &e)
{
	if (e.regionActive()) {
		// 自研核额外报出掩码提交节奏：0 = 每帧（3.1.8 起的默认），4 = 老节奏。
		// 真机上要对比「拖影 / CPU」就用 BEAUTY_MASK_EVERY 改，这里能看出改没改上。
		if (e.id() && strcmp(e.id(), "core") == 0) {
			static char buf[48];
			snprintf(buf, sizeof(buf), "on(every=%d)", beautyCoreMaskCadence());
			return buf;
		}
		return "on";
	}
	if (e.id() && strcmp(e.id(), "core") == 0)
		return "off(无掩码/全帧)";
	return "off(颜色启发式)";
}

// ------------------------------------------------------------------
// Parameter table
//
// 每一行是属性页上的一个滑块。key 同时是 OBS settings key（预设/场景文件里
// 存的就是它，**不要改**）。面板刻度统一 0..100，max_val 是面板 100 对应的
// 内部量程（value = panel / 100 * max_val）。
//
// 面板一共 11 项：磨皮 / 清晰 / 美白 / 红润 / 锐化 / 全局锐化 / 五官立体 /
// 亮眼 / 去黑眼圈 / 去法令纹 / 祛斑痘。
// ------------------------------------------------------------------
struct beauty_param_def {
	const char *key;    // 同时是 OBS settings key
	const char *label;  // panel label
	double dflt;        // default, panel units (0..100)
	double max_val;     // 面板 100 对应的内部量程
};

static const struct beauty_param_def g_params[] = {
	{"blur_level", "磨皮", 20.0, 6.0},
	{"clarity", "清晰", 0.0, 1.0},
	{"color_level", "美白", 0.0, 2.0},
	{"red_level", "红润", 0.0, 1.0},
	{"sharpen", "锐化", 0.0, 1.0},
	{"global_sharpen", "全局锐化", 0.0, 1.0},
	{"face_threed", "五官立体", 0.0, 1.0},
	{"eye_bright", "亮眼", 0.0, 1.0},
	{"remove_pouch_strength", "去黑眼圈", 0.0, 1.0},
	{"remove_nasolabial_folds_strength", "去法令纹", 0.0, 1.0},
	// 3.0.16 新增：祛斑痘。确实生效，但会让帧间闪动上升约 56%，
	// 且官方说明「消耗较多的电脑资源」，所以默认 0（关）。
	{"delspot_level", "祛斑痘", 0.0, 1.0},
	// 3.1.27 新增：美形三项。默认全 0 —— 几何变形会改变人脸轮廓，属于
	// 「用户主动要才做」的一类；默认关掉保证升级后画面逐字节不变。
	{"thin_face", "瘦脸", 0.0, 1.0},
	{"big_eye", "大眼", 0.0, 1.0},
	{"chin", "下巴", 0.0, 1.0},
};

#define BEAUTY_NPARAM ((int)(sizeof(g_params) / sizeof(g_params[0])))

// 3.0.13：把「一一对应」从注释变成编译期约束。以前只写在注释里，改了枚举
// 忘了改表（或反之）会静默错位一个滑块 —— 这正是历史上 P_WHITEN=2 那个坑。
static_assert(BEAUTY_NPARAM == P_COUNT,
              "g_params[] 与 beauty_engine.h 的 P_* 枚举必须一一对应");

// 与引擎的参数枚举一一对应（compile-time 断言在文件末尾）。
#define BEAUTY_IDX_BLUR P_BLUR

// Bumped whenever the meaning of a stored slider changes.
//   1 = 1.x：滑块从内部原值改成 0..100 面板刻度
//   2 = 2.0：删除 17 项美型 / 脸型预设 / 滤镜（旧预设里这些值被忽略）
#define BEAUTY_PARAM_VERSION 2

// ------------------------------------------------------------------
// OBS filter
// ------------------------------------------------------------------
struct beauty_filter {
	obs_source_t *context;
	gs_texrender_t *tr;    // capture upstream frame
	gs_stagesurf_t *stage; // CPU readback
	gs_texture_t *out_tex; // processed result
	uint32_t w, h;
	uint8_t *buf_in;
	uint8_t *buf_out;
	int frame_id;
	bool verified;
	// BUG 4 修复（2.1.1）：引擎 GPU 资源是否已按当前尺寸预热。
	// 病史：sm_* / det_* / chain_rt / skin_tex / mask_buf / mask_rt_* 历来都是
	// 「参数首次非 0」那一帧才在钩子里懒加载创建的。于是开启美颜的第一帧要
	// 同时付出 ①十几张纹理创建 ②检测模型首帧推理 ③完整渲染链，渲染线程被卡
	// 几十毫秒 → 用户看到「开启的一瞬间画面抽搐一下」。
	// 预热把分配时机提前到「尺寸就绪」的第一帧（参数全 0 的透传帧），与
	// 「参数是否非 0」解耦。尺寸变化时置回 false 重新预热。
	bool warm;
	// 性能埋点（BEAUTY_PROFILE=1 时才统计/输出）
	// 为什么要埋这个：插件每帧要把整帧从 GPU 下载到 CPU 再传回去，
	// 1080p RGBA 是 8 MB × 2。这笔开销从未实测过，而它是「要不要整体
	// 改走 GPU」的唯一决策依据。见 05_文档/开源参考仓库_对照与可复用清单.md §3。
	bool prof_on;
	int prof_frames;
	double prof_dl;    // 累计下载耗时 ms
	double prof_dl_wait;  // 其中：排队等 map 返回（2.4.2 起的取证拆项）
	double prof_dl_copy;  // 其中：memcpy 整帧（省不掉的那部分）
	double prof_proc;  // 累计引擎耗时 ms
	double prof_up;    // 累计上传+绘制耗时 ms
	// 3.1.47：引擎内部分项（掩码/磨皮/色调/五官/细节/美形），窗口累加。
	// 目的只有一个：4K 那 65 ms 到底花在哪。没有这两行，优化方向只能靠猜。
	double prof_seg[6];
	// 3.1.50：磨皮段内部三段细分（窗口累加，毫秒）。3.1.49 测出磨皮 1.94 ms，
	// 但那一整段里混着三件优化手段完全不同的事：
	//   draw = beauty_gpu_smooth() 里 23 次 draw 的**提交**时间（CPU 侧建命令，
	//          GPU 是否真算完要看下一段 —— D3D11 是延迟执行）
	//   map  = gs_stagesurface_map()，这里才是 GPU→CPU 的真实同步等待
	//   copy = map 出来的行数据 memcpy 进 dst
	// 合成一个数没法决策：draw 多要减 pass，map 多要减像素或干脆不出 GPU，
	// copy 多要改缓冲复用。3.1.49 曾经凭「回读和下载一样大」推断 map≈1.1 ms，
	// 这组数就是来证实/推翻那个推断的。
	double prof_sm_draw;
	double prof_sm_map;
	double prof_sm_copy;
	// 3.1.51：钩子「准备段」= 掩码上传 + 整帧上传。3.1.50 时这段时间是
	// 「磨皮 seg 减去 sm 三段和」倒推出来的（恒定 0.55 ms），只能靠估；
	// 拆出 mask 与 frame 两个分量之后，「交错省掉多少 / 上传占多少」才有数。
	double prof_sm_prep;      // 准备段合计
	double prof_sm_mask;      // 其中掩码两张纹理的上传
	int    prof_sm_n;   // 三段都成功的帧数（回退帧不进分母）
	double prof_t0;    // 本次计时的起点
	// 3.1.44：本窗口的掩码统计。Phase 0 实测同一分辨率下引擎耗时可差 2.3 倍，
	// 唯一变量就是「掩码覆盖了多少像素」与「有没有脸触发美形三项」。缺了这两项，
	// 两个窗口的毫秒数根本不可比（也就没法当 Phase 3~5 的验收基线）。
	// prof_cover 每帧累加覆盖率（0..1），窗口末除以帧数得平均；
	// prof_face_frames 数「检出脸的帧数」，用来算有脸帧占比。
	double prof_cover;
	int    prof_face_frames;
	// 3.1.48：ROI 包围盒面积比（0..1）的窗口累加，-1 的帧不计入。
	// prof_bounds_n 是「有包围盒的帧数」—— 平均值的分母必须用它，
	// 不能用 prof_frames，否则无掩码帧会把均值稀释掉。
	double prof_bounds;
	int    prof_bounds_n;
	// 3.1.10：掩码↔帧对齐计数的窗口基线（见 prof_frames 归零处的用法）。
	// 探针给的是从启动起的全量值，这里存「上个日志窗口结束时」的值，
	// 打印时取差值得到**本窗口内**的对齐比率 —— 90s @30fps 一个窗口，
	// 比全量比率更容易看出 worker 是不是间歇性追不上。
	uint64_t prof_align0_used;
	uint64_t prof_align0_total;
	// 3.1.63 / T7：本窗口的「每次 render 调用属于哪个视频 tick」计数。
	// tick_calls = 调用次数，tick_ticks = 不同 tick 数，tick_max = 单个 tick
	// 里最多的调用次数（1 = 正常，2 = 同一帧被渲染两次）。
	uint64_t tick_calls;
	uint64_t tick_ticks;
	uint32_t tick_last;   // 上一次看到的 obs_get_total_frames()
	int      tick_cur;    // 当前 tick 已被调用几次
	int      tick_max;
	bool enabled;
	bool mirror;
	int blur_type;    // 1=双边(最强) 0=高斯 2=弱磨皮
	int skin_detect;  // 1 = 只磨皮肤（精准美肤）
	int smooth_mode;  // 3.0.17 磨皮模式 0人像 1清晰 2精细 3朦胧（纯 CPU 侧）
	int landmark_q;   // fuFaceProcessorSetFaceLandmarkQuality (面板: 识别模式)
	int max_faces;    // fuSetMaxFaces (面板: 人物个数)
	int input_rot;     // 2.10.0 方案 B：输入方向 0/90/180/270（检测缩略图旋转）
	double v[BEAUTY_NPARAM]; // panel units, 0..100
	int slot;              // 引擎槽位（每个滤镜实例独占一份上下文）
	bool dirty;            // 进程级检测选项待下发（渲染线程 flush）
	bool err_reported;     // 引擎报错只提示一次，避免每帧刷屏
	// ---- 方案 A / Phase 2：GPU 色调渲染管线（默认不启用，诊断用）----
	gs_effect_t *tone_fx;  // tone.hlsl 编译后的 effect（懒加载，失败则回落 CPU）
	gs_texrender_t *tone_rt; // 色调 pass 渲染目标
	gs_texture_t *mask_tex; // 皮肤掩码纹理（CPU 算好上传，单通道 GS_R8）
	uint8_t *mask_buf;     // 皮肤掩码 CPU 缓冲（w*h 字节，0/255）
	size_t mask_cap;       // mask_buf 容量（字节）
	uint32_t mask_w, mask_h; // mask_tex 当前尺寸（分辨率变更时重建）
	bool gpu_tone_reported; // 自检日志只报一次
	bool gpu_tone_diag;     // BEAUTY_GPU_TONE=1 且 supportsGpu() 时开（诊断，不改画面）
	bool tone_tried;        // tone.hlsl 只尝试加载一次（失败就别每帧重试）

	// ---- 方案 A / Phase 3：GPU 磨皮管线（默认不启用，诊断用）----
	// 与 tone 同一形态：跑一遍、与 CPU 对拍、打一行日志，**不改 buf_out**。
	// 中间层纹理：3.1.76-p3 起用 RGBA16F（sm_is_16f=true，创建失败回退 32F）。
	// 原用 32F 是因 CPU 端磨皮全程 float、值域 0..255；但 R32G32B32A32_FLOAT
	// 渲染目标在 D3D11 默认不支持线性过滤，配合 smooth.hlsl 合成 pass 的 Linear
	// 采样会在 RTX 5060 上触发 device-lost 闪退。16F 可线性过滤且细节段已验证
	// 稳定；精度 ulp≈0.125 灰阶，对拍阈值已放宽到 0.3（见 beauty_gpu_smooth_selfcheck）。
	gs_effect_t *smooth_fx;   // smooth.hlsl（懒加载）
	gs_texrender_t *sm_d;     // 下采样结果（半分辨率，只读源，Comp 不再需要）
	gs_texrender_t *sm_a;     // B1 结果
	gs_texrender_t *sm_b;     // 临时/amp 结果
	gs_texrender_t *sm_c;     // B2 结果
	gs_texrender_t *sm_out;   // 合成结果（全分辨率 GS_RGBA，可被 stage 回读）
	bool sm_is_16f;            // 磨皮中间纹理是否用了 RGBA16F（false=32F 回退）
	gs_texture_t *skin_tex;   // 皮肤掩码 GS_R8（3.1.51 起从 R8G8 联合纹理拆出）
	gs_texture_t *eye_tex;    // 眼睛保护 GS_R8（同上）
	// 5b-2 切片 3：链上「当前生效的掩码」。默认 NULL = 用上面两张 CPU 掩码纹理；
	// GPU 掩码模式下指向 mask.hlsl 的输出（对拍/生产都走这里的同一套绑定）。
	// 用「覆盖指针」而不是临时改 skin_tex/eye_tex：后者一旦中途 `return 0`
	// 就把成员留在 RT 纹理上 —— 那是悬垂指针，且下一帧会拿旧掩码算。
	gs_texture_t *mask_skin_use;
	gs_texture_t *mask_eye_use;
	uint8_t *protect_buf;     // 眼睛保护掩码 CPU 缓冲（w*h）—— 诊断路径的唯一来源
	size_t gray_cap;          // 上面两张纹理能容纳的**像素数**（不是字节数）
	uint32_t gray_w, gray_h;  // 两张掩码纹理当前尺寸
	bool smooth_tried;        // smooth.hlsl 只尝试加载一次
	bool gpu_smooth_diag;     // BEAUTY_GPU_SMOOTH=1 且 supportsGpu()
	bool gpu_smooth_reported; // 对拍只报一次
	bool gpu_smooth_broken;   // 本会话确认 GPU 磨皮会崩/算错，永久回退 CPU
	// 3.1.46：等「真的检出人脸」的帧再对拍。启动后第 1 帧人脸检测还没出
	// 结果（首轮推理要跨 1~2 帧），那时对拍照样能「通过」，但磨皮只覆盖
	// region 兜底区域、改动量 0.17 灰阶，属于弱效果，证明力不足。
	int smooth_wait;          // 已跳过多少帧（等脸）；见 kSmoothWaitMax

	// ---- Phase 3.5：GPU 磨皮生产路径（BEAUTY_GPU_SMOOTH_PROD=1）--------
	bool gpu_smooth_prod;     // 开关；默认关，关掉时行为与 3.1.48 完全一致
	gs_texture_t *hook_src_tex;  // 引擎递来的帧 → 纹理（对齐换帧时才用得上）
	uint32_t hook_w, hook_h;
	gs_texture_t *hook_cur_in;   // 本帧 in_tex；src 与之同一份时可省一次上传
	bool hook_eye_zero;       // eye_tex 已填成全 0 且此后没被覆盖过
	long long hook_hit0, hook_call0;  // 上一个日志窗口的累计命中数（算差值）

	// ---- Phase 5a：段区间钩子（3.1.52）----------------------------------
	// 一次接管「磨皮 → 色调 → 细节」，中间结果在显存里走到认领区间末尾才回读
	// 一次。开关 BEAUTY_CHAIN_PROD 默认随 gpu_smooth_prod 一起开，
	// 设成 0 就退回 3.1.51 的单段钩子（用于 A/B 对比）。
	bool chain_prod;
	// ---- T2：同会话 A/B 切臂（BEAUTY_CHAIN_AB=1，诊断，不改生产行为）------
	// 为什么需要它：`BEAUTY_CHAIN_PROD` 只在 filter create 时读一次 env，
	// 改不了已创建实例的臂；而**跨会话**比毫秒数根本不可比（同一份代码在
	// 不同会话里整帧能差 2.3 倍，见经验 E9，那是背景负载不是代码）。
	// 做法：每 180 帧（正好一个日志窗口）翻一次臂，相位与 prof_frames 严格
	// 对齐，所以**一行日志必然整行属于同一个臂**，相邻两行就是一对可比样本。
	bool chain_ab;            // BEAUTY_CHAIN_AB=1 且 gpu_smooth_prod
	bool chain_ab_out;        // BEAUTY_AB_TARGET=out：翻的是 5b-1 开关而不是 chain
	int  chain_ab_total;      // 累计帧数（与 prof_frames 同处递增，保相位不漂）

	// ---- 5b-1：链尾不回读（BEAUTY_CHAIN_OUT=1）----------------------------
	// 段区间钩子已经让中间结果留在显存里，但末尾仍无条件 map+memcpy 回 CPU，
	// 主机再 set_image 传回去 —— 这一来一回纯属多余。当本帧 flags 被**全部**
	// 认领时（引擎每个 CPU 段前都有 `claimed` 守卫，见 beauty_core.cpp:1892
	// /1902），dst 一个字节都不会被写，没人需要这份回读，于是可以：
	//   钩子跳过 map+copy → 把最终纹理交给主机 → 主机直接画它。
	// 只有「五官/美形本帧要跑」时才退回老路：那两段还是 CPU 实现，吃的就是
	// CPU 内存里的像素（美形要等 5c 才有 GPU 版）。
	bool chain_out;             // BEAUTY_CHAIN_OUT=1（默认关 = 3.1.57 行为）
	gs_texture_t *chain_out_tex;   // 本帧链的最终输出纹理（钩子写，render 读）
	bool chain_out_valid;          // 本帧真的走了不回读路径
	bool chainout_cmp;             // BEAUTY_CHAINOUT_CMP=1：一次性绘制路径对拍
	bool chainout_reported;
	// 细节回填的中间平面。**必须两张**：BoxV 会把亮度平面覆写掉，而 Refill
	// 还要用逐像素亮度（靠 .b 从 BoxH 透传过来再带回去）。
	gs_effect_t *detail_fx;   // detail.hlsl（懒加载）
	bool detail_tried;
	gs_texrender_t *det_a;    // Lum 输出 / BoxV 输出（.r box3, .g box7, .b 亮度）
	gs_texrender_t *det_b;    // BoxH 输出（.r box3h, .g box7h, .b 亮度）
	gs_texrender_t *det_hist; // 亮度历史（EMA），GS_RGBA 装 0..1 的灰阶
	// 链共用的中间渲染目标：色调的输出、色调未认领时细节回填的输出。
	// **必须与细节段解耦、提前创建** —— 链钩子里色调排在细节之前，若等
	// beauty_chain_detail 才建它，色调会因 `!f->chain_rt` 永远返回 NULL、
	// 永远认领不了（回落 CPU 每帧约 1 ms，并因 tone_ok=false 连带拦掉细节，
	// 最终 claim 覆盖不全 → 每帧多付一次全帧回读）。见 beauty_ensure_chain_rt。
	gs_texrender_t *chain_rt;
	uint32_t chain_w, chain_h;
	uint32_t det_w, det_h;
	bool det_hist_ok;         // 历史与上一帧连续（否则下一帧走 copyOnly 重置）
	bool detail_err_reported; // 细节输出不可用的警告只报一次（防刷屏）
	unsigned chain_claim;     // 本帧认领的段（钩子写，render 读完用来判历史）
	long long chain_hit0, chain_call0;
	unsigned long long chain_seg0[5];
	double prof_ch_tone;      // 色调 pass（窗口累加）
	double prof_ch_detail;    // 细节四趟（窗口累加）

	// ---- T1：GPU 细节对拍（BEAUTY_GPU_DETAIL=1，诊断，不改画面）----------
	// 与色调/磨皮自检同形态：链钩子认领 DETAIL 时把引擎算好的 job.detail
	// 暂存下来（宿主不能自己重算，beta 带运动 EMA 状态会漂）；自检时用
	// beauty_chain_detail(..., emaOn=0) 单独跑一趟**无状态** detail（base=原图），
	// 与 beautyCoreDetailCpu() 逐像素对拍。只打一行日志，不动 buf_out。
	bool gpu_detail_diag;      // BEAUTY_GPU_DETAIL=1 且 supportsGpu()
	bool gpu_detail_reported;  // 对拍只报一次
	int  detail_wait;          // 已跳过多少帧（等脸）；见 kDetailWaitMax
	bool diag_detail_ok;       // 本帧链钩子认领过 DETAIL（有 dp 可比）
	BeautyDetailDiag diag_detail; // 链钩子递出的细节参数快照

	// ---- 5c：GPU 美形（3.1.62）-----------------------------------------
	// 与细节段同一形态：一个 effect、逐脸一趟、失败整段交回 CPU。
	// 中间纹理必须是 **GS_RGBA8**（不是 16F）：CPU 那边就是 8bit 落笔
	// （`(unsigned char)(v + 0.5)`），中间放浮点会引入 CPU 根本没有的量化误差，
	// 逐像素对拍就永远对不上。而这里本来就要求「一进一出与原图同精度」。
	gs_effect_t *warp_fx;      // warp.hlsl（懒加载）
	bool warp_tried;
	gs_texrender_t *warp_a;    // 逐脸 ping-pong（face[i] 读 A 写 B，i+1 反过来）
	gs_texrender_t *warp_b;
	uint32_t warp_w, warp_h;
	double prof_ch_warp;       // 美形各趟合计（窗口累加）
	// 5c 对拍：BEAUTY_GPU_WARP=1。与 T1 细节对拍同形态 —— 链钩子认领 WARP 时
	// 把引擎算好的 job.warp 暂存，随后用 beautyCoreWarpCpu() 在 CPU 上按**同一份
	// 几何**重跑一遍，与 GPU 输出逐像素比。只打日志，不动 buf_out。
	bool gpu_warp_diag;        // BEAUTY_GPU_WARP=1 且 supportsGpu()
	bool gpu_warp_reported;
	int  warp_wait;            // 已跳过多少帧（等脸 + 等强效果）
	bool diag_warp_ok;         // 本帧链钩子认领过 WARP（有几何可比）
	BeautyWarpDiag diag_warp;  // 链钩子递出的美形几何快照

	// ---- 5b-2：GPU 掩码（切片 2，3.1.64，纯诊断）------------------------
	// 今天只做一件事：用 mask.hlsl 在 GPU 上按引擎给的几何重算一遍掩码，
	// 读回来与 CPU 的 mask_/protect_ 逐像素比（M1 门禁）。**不参与生产链**：
	// 链仍然采样 f->skin_tex / f->eye_tex（还是 CPU 掩码上传的那两张）。
	// 等 M1 稳定通过，切片 3 才把链切到 GPU 掩码、并省掉上传与下载。
	gs_effect_t *mask_fx;      // mask.hlsl（懒加载）
	bool mask_tried;
	gs_texrender_t *mask_rt_a; // 对拍专用 scratch（R8），不碰生产纹理
	gs_texrender_t *mask_rt_b;
	gs_stagesurf_t *mask_stage;      // 对拍专用暂存面（RGBA，避免抢 f->stage）
	uint32_t maskcmp_w, maskcmp_h;   // 对拍 scratch 尺寸（不复用 mask_w/mask_h）
	bool gpu_mask_cmp;         // BEAUTY_GPU_MASK_CMP=1 且 supportsGpu()
	bool gpu_mask_reported;
	// 5b-2 切片 3：把 GPU 掩码真正接进生产链（跳过两块掩码上传）。
	// BEAUTY_GPU_MASK=1，默认关 —— 等两档 A/B 确认收益再决定是否转默认。
	bool gpu_mask_chain;
	// 4d：五官几何快照的一次性日志（BEAUTY_REGION_GEOM=1）
	int  region_geom_reported;
	// 4d：GPU 五官进生产链（BEAUTY_GPU_REGION=0 退回 CPU）
	bool gpu_region;
	// 优化：BEAUTY_REGION_GEOM 每次启动只查一次环境变量
	int  rg_env_checked;
	int  rg_env_on;
	// T9：抽搐诊断计数器（只统计，不改画面）。拖滑块时哪一类帧在涨，就指向哪条成因。
	uint32_t an_nofresh;   // 掩码不是本帧的 → 「当前帧 + 旧掩码」配对
	uint32_t an_masknull;  // job.mask == NULL → 全皮肤磨皮（整帧被磨）
	uint32_t an_histrst;   // 细节历史被重置（EMA 断帧 → 纹理跳一下）
	uint32_t an_claimdrop;// 认领位不满（本帧走了 CPU/回退）
	// S4a（2.6.0）：只出几何的帧，GPU 掩码建图失败 → 本帧整帧跳过处理。
	// 正常应恒为 0；一旦涨就说明掩码 RT/shader 出问题了（那时画面是原样，不是花屏）。
	uint32_t an_geomskip;
	// S4a：本窗口内「只出几何、由 GPU 画掩码」的帧数。应 ≈ 窗口帧数（对比 gskip）。
	uint32_t an_geomonly;
	// ---- 2.7.0（S4b）：检测输入缩略图 -----------------------------------
	// 检测要的只是「整帧等比压到 640×640」这一张，而现行办法是把**整帧**回读到
	// CPU（4K 33 MB ≈ 2.5~3 ms）再用 letterboxRgb 缩一遍。这里改成 GPU 上缩、
	// 只回读 1.6 MB —— 省下来的正是「下载」里那段 memcpy 及它占的队列时间。
	//
	// 前提只有一条：**上一帧全认领**（chain_out_valid —— flags 每一位都被钩子
	// 拿下，CPU 侧一个字节都不需要）。这时 buf_in 可以整帧不更新。
	// 本帧掉链子（认领不全）时没法补救：没有全帧像素，任何 CPU 段都跑不了，
	// 只能整帧跳过（画面原样）并让下一帧回退全帧。这是缩略图模式的固有代价，
	// 也是 an_thumbskip 要盯住的那个数。
	bool detect_small;         // BEAUTY_DETECT_SMALL=0 回退全帧回读
	gs_texrender_t *thumb_rt;  // S×S 检测缩略图（RGBA8，带 letterbox 黑边）
	gs_stagesurf_t *thumb_stage;
	uint32_t thumb_w, thumb_h; // = S（默认 640）
	gs_color_format thumb_fmt;  // 与 in_tex 同格式（sRGB 对称，避免色偏）
	unsigned char *thumb_buf;  // 回读目标（S*S*4 字节）
	float thumb_scale;         // 与 letterboxRgb 同源的三个数（每帧算）
	int thumb_ox, thumb_oy;
	bool thumb_prev_ok;        // 上一帧 chain_out_valid → 本帧可以只回读缩略图
	bool thumb_frame;          // 本帧真的走了缩略图路径（钩子据此拒绝上传旧帧）
	// ---- 2.11.0（方案C）：运动判定搬 GPU + 缩略图按提交帧回读 -----------
	// 问题：S7 之后静止时每 8 帧才提交一次检测，但 640×640 缩略图**每帧都回读**
	// （1.6 MB），而它每帧唯一的用途是给运动判定采 256 个点。实测日志里
	// `下载 == tdl`：4K 0.61~1.57 ms、720p 0.61~1.18 ms，是往返里最大的一笔。
	// 做法：每帧只回读 64×64（16 KB）喂运动判定；预判到「本帧要提交检测」才回读
	// 那张 1.6 MB 的。开关 BEAUTY_MOTION_GPU=1（默认关，未过完整验收前不动默认）。
	bool motion_gpu;           // 开关
	gs_texrender_t *mot_rt;    // 64×64 运动采样 RT
	gs_stagesurf_t *mot_stage; // 双缓冲：本帧 stage_texture 到一张，map 上一帧那张
	gs_stagesurf_t *mot_stage_alt;
	bool mot_primed;           // 已经有一份可以 map 的上一帧样本
	int mot_ping;              // 0/1，指向本帧要写入的那张
	unsigned char *mot_buf;    // 64*64*4 = 16 KB
	uint32_t mot_side;         // = kMotionSide
	bool nopix_frame;          // 走了缩略图路径但本帧没回读任何像素（预判不提交）
	uint32_t an_nopix;         // 省掉缩略图回读的帧数（窗口内，诊断）
	uint64_t mot_skipped;      // 累计省掉的回读帧数（会话级，诊断）
	uint64_t an_sub0;          // 本窗口起点的提交计数（2.11.1 窗口差值用）
	int      mot_dump_i;       // 64×64 小图已存盘张数（BEAUTY_MOT_DUMP）
	char     mot_dump_dir[512];// 空 = 不存
	uint32_t an_thumb;         // 走缩略图的帧数（窗口内）
	uint32_t an_thumbskip;     // 走了缩略图、但本帧认领不全 → 整帧跳过
	// ---- 2.8.0（S7）：检测周期自适应 ------------------------------------
	// dper_max = 本窗口内实际用到的**最大**提交周期。静止段应等于
	// BEAUTY_DET_ADAPT_STILL（默认 8），人一晃动就掉回 1 —— 这是「自适应真的
	// 生效」以及「运动时不掉帧」的唯一直接证据，光看毫秒数看不出来（检测在
	// worker 上，渲染线程一分钱不多花）。
	// dmot_max = 窗口内网格平均亮度差的最大值，用来标定阈值：真实摄像头静止时
	// 的噪点如果逼近 4，就得把 BEAUTY_DET_ADAPT_THR 抬高，否则等于没降频。
	uint32_t dper_max;
	int      dmot_max;
	int thumb_dbg_done;        // 一次性判据快照是否已打
	bool thumb_fail_logged;    // 构建失败只报一次（防刷屏）
	bool thumb_skip_logged;    // 认领不全导致跳过的原因只报一次
	int  rot_warned;           // 2.10.1 用过；2.13.1 起不再写也不再读（保留字段避免结构体布局变动）
	bool thumb_dumped;         // BEAUTY_THUMB_DUMP：缩略图只落盘一次
	double prof_dl_thumb;      // 缩略图「缩放 + 回读」耗时（窗口累加）
	// 4d：五官椭圆族的 GPU 对拍（R1-lite，BEAUTY_REGION_CMP=1）
	bool region_cmp;                 // 开关
	bool region_reported;            // 样本已取满（不再进对拍）
	int region_cmp_n;                // 要取几个样本（BEAUTY_REGION_CMP_N，默认 1）
	int region_cmp_done;             // 已取样本数
	bool region_skip_logged;         // 「掩码全 0 跳过」只报一次（防每帧刷屏）
	int region_cmp_delay;            // 再等多少个「可对拍帧」才取样（预热用）
	double region_mask_pct;          // 最近一次对拍的掩码覆盖率（%）
	double region_mask_avg;          // 最近一次对拍的掩码均值（0..255）
	gs_effect_t *region_fx;          // region.hlsl（懒加载）
	bool region_tried;
	gs_texture_t *region_param;      // 64×1 RGBA32F 椭圆参数表（每椭圆 3 texel）
	gs_texrender_t *region_rt[4];    // 4 张 scratch（同一趟里不能读自己写的 RT）
	gs_stagesurf_t *region_stage;    // 对拍读回
	uint32_t region_w, region_h;
	// 5b-2 切片 4：显存帧环。每帧把 in_tex 原样拷一份（gs_copy_texture，
	// 同格式同尺寸、无采样），按宿主帧号索引：对齐换帧时链要处理的是**更早那一帧**，
	// 有了环就不必再从 CPU 上传（4K 那次上传 ≈3 ms）。
	// 只在 GPU 掩码模式下用（掩码与帧必须同源），其它情况一律走上传。
	bool gpu_ring_ok;              // BEAUTY_GPU_FRAME_RING 且 supportsGpu()
	// S2b：环拷贝按需。环里那一份只有「对齐换帧」时才会被读，而对齐换帧要的
	// 永远是**送去检测的那一帧**（heldFrame_ 的 tag 来自 submitAsync 时的
	// g_frameTag）。其余帧链直接采样 in_tex（beauty_hook_prepare 里
	// src==buf_in 就复用 hook_cur_in），环里那份从头到尾没人读。
	// 所以默认只在「本帧真的提交了检测」时才拷；`BEAUTY_RING_ALWAYS=1` 回到
	// 每帧无条件拷贝（2.4.0 / S2 的老行为，回退用）。
	bool ring_always;              // BEAUTY_RING_ALWAYS=1（回退老行为）
	bool ring_skip_logged;         // 首次跳过时打一行日志，证明分支真的走到了
	gs_texture_t *ring_tex[3];
	unsigned long long ring_tag[3];
	unsigned long long frame_tag;  // 宿主帧号（单调自增）
	gs_color_format ring_fmt;
	uint32_t ring_w, ring_h;
	// S3（Q3）：暂存面双缓冲回读。上一帧的暂存面在上一次 render 里就提交了，
	// 到本帧 map 时那趟拷贝早已完成 —— 先读它（几乎不等 GPU），再把本帧提交到
	// 另一张面。代价是 CPU 这一侧整体晚一帧：引擎在第 n 帧拿到的是第 n−1 帧的
	// 像素，所以送给引擎的帧号是 frame_tag−1，画的也必须是同一帧（取自帧环），
	// 否则「掩码对应 n−1、画面是 n」会直接变成拖影。
	// 只在 gpu_ring_ok 时开：没有环就拿不到 n−1 的纹理，只能退回整帧上传，
	// 那比同步路径更慢。回退开关 BEAUTY_ASYNC_STAGE=0。
	bool stage_async;           // BEAUTY_ASYNC_STAGE=1 且 gpu_ring_ok
	gs_stagesurf_t *stage_alt;  // ping-pong 的另一半
	int stage_submit;           // 上一次提交到哪张：0=f->stage 1=stage_alt -1=无
	bool async_logged;          // 首次生效打一行日志
	int  mask_wait;            // 已跳过多少帧（等脸 / 等多脸消失）
};

// 面板值 → 引擎设置。每帧构一次，代价是 9 个 double 拷贝。
static void beauty_filter_settings(struct beauty_filter *f, BeautySettings &s)
{
	for (int i = 0; i < BEAUTY_NPARAM; i++)
		s.panel[i] = f->v[i];
	s.blur_type = f->blur_type;
	s.skin_detect = f->skin_detect ? 1 : 0;
	s.smooth_mode = f->smooth_mode;
	s.input_rot = f->input_rot;
}

// Phase 3.5：GPU 磨皮生产路径的回调（实现在 beauty_gpu_smooth_selfcheck 之后）。
// 这里先声明，因为 beauty_filter_create 要在挂钩子时用到它的地址。
static bool beauty_smooth_hook(void *user, const unsigned char *src,
			       unsigned char *dst, int w, int h,
			       const unsigned char *mask,
			       const unsigned char *protect,
			       const BeautyBandDiag &bp);

// Phase 5a：段区间钩子（实现在 beauty_smooth_hook 之后）。它比单段钩子优先，
// 但两者都挂 —— 段区间钩子返回 0 时引擎仍可用单段钩子接管磨皮。
static unsigned beauty_chain_hook(void *user, const BeautyChainJob &job);

static void beauty_filter_read(struct beauty_filter *f, obs_data_t *s)
{
	f->enabled = obs_data_get_bool(s, "enabled");
	f->mirror = obs_data_get_bool(s, "mirror");
	f->blur_type = (int)obs_data_get_int(s, "blur_type");
	f->skin_detect = obs_data_get_bool(s, "skin_detect") ? 1 : 0;
	f->smooth_mode = (int)obs_data_get_int(s, "smooth_mode");
	f->landmark_q = (int)obs_data_get_int(s, "landmark_q");
	f->max_faces = (int)obs_data_get_int(s, "max_faces");
	f->input_rot = (int)obs_data_get_int(s, "input_rot");
	for (int i = 0; i < BEAUTY_NPARAM; i++)
		f->v[i] = obs_data_get_double(s, g_params[i].key);
}

// Convert settings written by earlier builds.
//   v0 -> v1: pre-0.2 builds stored raw internal units on a handful of sliders.
//   v1 -> v2: 2.0 dropped the 17 美型 sliders, the 脸型预设 and the filter;
//             those keys are simply no longer read.
static void beauty_migrate(obs_data_t *s)
{
	const int ver = (int)obs_data_get_int(s, "param_version");
	if (ver >= BEAUTY_PARAM_VERSION)
		return;
	// v0 -> v1（2.0.0）：1.x 的滑块本来就是 0..100 面板刻度 —— 见 1.x 下发处
	// g_fuItemSetParamd(item, key, v / 100.0 * max_val)，存下来的 v 已经是
	// 面板值。所以这里**不做任何数值换算**。
	//
	// 早期版本误写成 v / max_val * 100：用户的磨皮 15 会算成 250，再被滑块
	// 上限截到 100，等于把调好的 15 直接拉到满格。max_val > 1 的两项
	// （磨皮 6.0、美白 2.0）受害最重，其余七项 max_val 都是 1.0，×100 之后
	// 一律顶到 100。
	//
	// v1 -> v2：只是删掉了 18 个已废弃的键（17 项美型 + 全局滤镜），
	// 同样不需要换算。
	obs_data_set_int(s, "param_version", BEAUTY_PARAM_VERSION);
	blog(LOG_INFO, "[" PLUGIN_NAME "] settings migrated to v%d (美型/滤镜 已移除)",
	     BEAUTY_PARAM_VERSION);
}

// 高精度单调时钟。GetTickCount64 只有 15.6ms 精度，量几毫秒的环节不够用。
static double beauty_now_ms(void)
{
	static double freq = 0.0;
	LARGE_INTEGER c;
	if (freq == 0.0) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		freq = (f.QuadPart > 0) ? (double)f.QuadPart : 1.0;
	}
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / freq;
}

// ---- 3.1.11：把滤镜版本号暴露给其它模块（面板左下角显示用）------------------
//
// 为什么：面板角标打的是**面板自己的版本**（3.0.22），用户看到它就会问
// 「装的滤镜是不是旧版」—— 版本号必须放在它能被看到的地方。proc handler 是
// OBS 官方的跨模块查询通道：面板拿到滤镜 source 后调 beauty_version 就能拿到，
// 老面板没有这个 proc 也只是显示不到，不会报错。
// 只报 meta（不加载引擎、不读设置），任何滤镜实例无论状态都能答。
// 签名注意：本版 libobs 的 proc 回调是 `(void *data, calldata_t *params)`，
// 参数全走 calldata，obs_source 不做隐式注入（见 callback/proc.c 的 dispatch）。
static void beauty_version_proc(void *, calldata_t *params)
{
	calldata_set_string(params, "version", PLUGIN_VERSION);
}

static void *beauty_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct beauty_filter *f = (struct beauty_filter *)bzalloc(sizeof(struct beauty_filter));
	f->context = source;
	f->frame_id = 0;
	{
		proc_handler_t *ph = obs_source_get_proc_handler(source);
		if (ph)
			proc_handler_add(ph, "void beauty_version(out string version)",
					 beauty_version_proc, NULL);
	}
	f->slot = -1;
	f->dirty = true;
	f->err_reported = false;
	// 只在显式开启时统计，避免给生产会话平白加两次计时调用
	{
		const char *p = getenv("BEAUTY_PROFILE");
		f->prof_on = (p && p[0] == '1');
	}
	// 方案 A 便捷开关：BEAUTY_GPU=1 等价于 BEAUTY_ENGINE_IMPL=gpu。
	// 真正生效的那次在 obs_module_load() 最前面；这里保留一份是幂等兜底
	// （万一将来有别的宿主先建了滤镜再加载模块）。只在这里设**不够** ——
	// 模块加载阶段引擎单例就已按 env 选定了实现，3.1.40 的 bug 就是这么来的。
	{
		const char *gp = getenv("BEAUTY_GPU");
		const char *ei = getenv("BEAUTY_ENGINE_IMPL");
		if (gp && gp[0] == '1' && (!ei || !ei[0]))
			_putenv_s("BEAUTY_ENGINE_IMPL", "gpu");
	}
	beauty_migrate(settings);
	beauty_filter_read(f, settings);

	// 每个实例独占一份引擎上下文。第一次调用会顺带完成预热（约 2 秒），
	// 因此首个滤镜创建时会有一个可感的停顿 —— 与 1.x 在模块加载时预热相比，
	// 好处是「没拖滤镜的 OBS 会话完全不付这笔开销」。
	std::string err;
	f->slot = beautyEngine().acquireSlot(err);
	if (f->slot < 0)
		blog(LOG_ERROR, "[" PLUGIN_NAME "] 引擎槽位获取失败：%s（滤镜将直通）",
		     err.c_str());

	// Phase 2 诊断开关：BEAUTY_GPU_TONE=1 且引擎确实宣称走 GPU 路径时才开。
	// 默认关 —— 这条路径不改画面，只是把 tone.hlsl 与 CPU toneColor 对一次账，
	// 用来证明「着色器能在真机 D3D11 上跑，且算得和 CPU 一样」。
	// 必须在 acquireSlot 之后判断：beautyEngine() 是在那一步才初始化实现的。
	{
		const char *gt = getenv("BEAUTY_GPU_TONE");
		f->gpu_tone_diag = (gt && gt[0] == '1' && beautyEngine().supportsGpu());
	}
	// Phase 3 诊断开关：同样的形态，磨皮着色器跑一遍 + 与 CPU 对拍，不改画面。
	{
		const char *gsm = getenv("BEAUTY_GPU_SMOOTH");
		f->gpu_smooth_diag = (gsm && gsm[0] == '1' && beautyEngine().supportsGpu());
	}
	// T1 诊断开关：GPU 细节对拍。同样只跑一次、不改画面。
	{
		const char *gd = getenv("BEAUTY_GPU_DETAIL");
		f->gpu_detail_diag = (gd && gd[0] == '1' && beautyEngine().supportsGpu());
	}
	// 5c 诊断开关：GPU 美形对拍。同样只跑一次、不改画面。
	{
		const char *gw = getenv("BEAUTY_GPU_WARP");
		f->gpu_warp_diag = (gw && gw[0] == '1' && beautyEngine().supportsGpu());
	}
	// 5b-2 诊断开关：GPU 掩码对拍（M1）。只比掩码，不改画面、不动生产链。
	// 与前面几个同形态：一次性、只在当前进程里生效、失败就什么都不做。
	{
		const char *gm = getenv("BEAUTY_GPU_MASK_CMP");
		f->gpu_mask_cmp = (gm && gm[0] == '1' && beautyEngine().supportsGpu());
	}
	// 5b-2 切片 3：GPU 掩码进生产链。**默认开**（与 5b-1 转默认同一套做法）：
	// 只有显式 `BEAUTY_GPU_MASK=0` 才退回「CPU 掩码 + 上传」。
	// 只认 `0` 作关闭，是为了让「拼错环境变量」退回**开**（已验证的安全侧）。
	{
		const char *gm = getenv("BEAUTY_GPU_MASK");
		f->gpu_mask_chain = (!gm || gm[0] != '0') &&
				    beautyEngine().supportsGpu();
	}
	// 5b-2 切片 4：显存帧环（跳过对齐换帧的整帧上传）。**默认开**，
	// `BEAUTY_GPU_FRAME_RING=0` 退回「从 CPU 上传那一帧」（4K ≈2.9 ms）。
	// 代价是每帧一次全帧 VRAM 拷贝（4K 33 MB，实测已含在引擎时间里）。
	{
		// 3.1.73 曾因 2026-11-01 08:10 的 4K 崩溃把它改回 opt-in。2.4.0（S2）
		// 翻回默认开，依据是根因两侧都已封堵 + 实测：
		//   · beauty_ring_put() 每次拷贝前逐张核对尺寸+格式，不符直接跳过
		//     （那条 `device_copy_texture ... same dimensions` 再也发不出来）
		//   · 触发驱动崩溃的 23-draw 磨皮生产路径（BEAUTY_GPU_SMOOTH_PROD）
		//     已在 3.1.76-p2 退役，崩溃入口不可达
		//   · 2026-11-02 实跑：4K 连续 30 分钟 54180 帧零异常、画布 720p↔
		//     1080p↔4K 切换 9 次不崩、热重载 3 轮正常（见 PROGRESS 9.3）
		// 判据用「不设或首字符非 0 即开」，与 BEAUTY_GPU_MASK 等开关一致。
		//
		// 另外绑定 gpu_mask_chain：环只在「掩码也是 GPU 现画的」时才会被取用
		// （见 beauty_hook 里的 gpu_mask_ok）。掩码走 CPU 时没人读环，就不该
		// 白付每帧 33 MB 的拷贝。
		const char *gr = getenv("BEAUTY_GPU_FRAME_RING");
		f->gpu_ring_ok = (!gr || gr[0] != '0') && f->gpu_mask_chain;
		// S2b：环拷贝按需（见 struct 里 ring_always 的注释）。同样只认 '1' 作
		// 回退，拼错开关留在**新的安全侧**——按需拷贝只是少拷，最坏退化成从
		// CPU 上传那一帧（慢几毫秒），不会画错。
		const char *ra = getenv("BEAUTY_RING_ALWAYS");
		f->ring_always = (ra && ra[0] == '1');
		// S3（Q3）：暂存面双缓冲回读。默认**关**（opt-in），等验收通过再翻默认。
		// 只认 '1' 开启：拼错开关留在同步路径上（已验证的安全侧）。
		// 绑定 gpu_ring_ok：异步回读把 CPU 侧整体推迟一帧，链要画的是**上一帧**，
		// 那一帧的纹理只能从环里取；没有环就无法保证「掩码与画面同源」。
		const char *as = getenv("BEAUTY_ASYNC_STAGE");
		f->stage_async = (as && as[0] == '1') && f->gpu_ring_ok;
		f->stage_submit = -1;
	}
	// S4b（2.7.0）：检测输入走 GPU 缩略图（默认开，`BEAUTY_DETECT_SMALL=0` 回退）。
	// 与 S3 的 stage_async **互斥**：S3 让 CPU 侧晚一帧（引擎手上是 n−1 帧的像素），
	// 而缩略图是按本帧 in_tex 缩的 —— 两个都开会把第 n 帧的掩码盖在第 n−1 帧上。
	// 这里不强行关（那是 init 时的静态选择），render 里 want_thumb 直接要求
	// !stage_async，两者同时设时缩略图让位（S3 的收益更大且已验证）。
	{
		const char *ds = getenv("BEAUTY_DETECT_SMALL");
		f->detect_small = (!ds || ds[0] != '0') && f->gpu_mask_chain &&
				  f->gpu_ring_ok;
	}
	// S4a（2.6.0）：告诉引擎「有 GPU 掩码链」—— 单脸时它就可以只算几何、
	// 不画那张 4K 全帧 CPU 掩码（实测 `seg 掩码` 2.27~2.82 ms 的大头）。
	// 必须在本处调用：gpu_mask_chain 要到 acquireSlot 之后才确定。
	// 回退 BEAUTY_MASK_GEOM_ONLY=0（在引擎的 setter 里判，不受调用顺序影响）。
	beautyCoreSetMaskGeomOnly(f->gpu_mask_chain);
	// 4d：五官椭圆对拍（纯诊断，一次性，默认关）
	{
		const char *rc = getenv("BEAUTY_REGION_CMP");
		f->region_cmp = (rc && rc[0] == '1' && beautyEngine().supportsGpu());
		// 2.8.3：默认只取 1 个样本（一次对拍 = 3 次全帧回读，4K 会掉一帧）。
		// 想要统计量就设 BEAUTY_REGION_CMP_N=5 —— 连续 5 帧各取一次。
		const char *rn = getenv("BEAUTY_REGION_CMP_N");
		const int n = rn ? atoi(rn) : 1;
		f->region_cmp_n = (n < 1) ? 1 : (n > 20 ? 20 : n);
		f->region_cmp_done = 0;
		// 预热：OBS 刚起来那几帧掩码/几何还没稳定，取样太早量到的是启动态。
		// 单位是「可对拍帧」（单脸 + 掩码非零的帧），30fps 下 120 帧 ≈ 4 s。
		const char *rd = getenv("BEAUTY_REGION_CMP_DELAY");
		const int dly = rd ? atoi(rd) : 0;
		f->region_cmp_delay = (dly < 0) ? 0 : (dly > 900 ? 900 : dly);
	}
	// 4d：GPU 五官进生产链。**默认关**，`BEAUTY_GPU_REGION=1` 才进 GPU 五官；
	// 不设/设 0 = CPU 五官（老路）。
	{
		// 为什么还是关（2.8.1 已修掉 54–58 ms 那条崩溃主因）：4K 有一次 R1
		// maxdiff=2，缺「能证明等价」的判据 —— 截图对拍在 geomOnly 路径不成立
		// （PROGRESS §9.19），同帧自检只在 5 个样本 × 2 个分辨率上 maxdiff≤1
		// （§9.20.2）。判据补齐之前只作为 opt-in。
		const char *gr = getenv("BEAUTY_GPU_REGION");
		f->gpu_region = (gr && gr[0] == '1') &&
				beautyEngine().supportsGpu();
	}
	// 2.11.0（方案C）：运动判定搬 GPU + 缩略图按提交帧回读。
	// 默认**关**：它改变的是「检测输入什么时候被回读」，属于节流而不是新算法，
	// 但阈值与新采样方式不逐点等价（见 PROGRESS §9.29 的标定），必须先过 A/B。
	// 只认 '1' 开启 —— 拼错落在「关」侧 = 2.10.x 的已验证行为。
	{
		const char *mg = getenv("BEAUTY_MOTION_GPU");
		f->motion_gpu = (mg && mg[0] == '1');
		if (f->motion_gpu)
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 方案C 生效：运动判定走 64×64 GPU 采样，"
			     "检测输入只在提交帧回读（BEAUTY_MOTION_GPU=0 回退）");
	}
	// 2.11.1（诊断）：把喂给引擎的 64×64 小图逐张存成裸 RGBA（16 KB/张），
	// 前 40 张。只在排查「判定为什么一直是在动」时用，不参与任何生产路径。
	{
		const char *md = getenv("BEAUTY_MOT_DUMP");
		f->mot_dump_i = 0;
		f->mot_dump_dir[0] = 0;
		if (md && md[0])
			snprintf(f->mot_dump_dir, sizeof(f->mot_dump_dir), "%s", md);
	}
	// Phase 3.5：GPU 磨皮**生产**路径。与上面那个诊断开关的区别是它会改画面
	// —— 磨皮整段搬到 GPU，引擎只拿回读结果继续算色调/五官/细节/美形。
	//
	// 3.1.76-p3（2026-11-02）复活：该路径原在 RTX 5060（Blackwell）上硬崩，
	// 根因是磨皮 4 张中间纹理用 GS_RGBA32F、而合成 pass 对它做 Linear 采样；
	// D3D11 的 R32G32B32A32_FLOAT 渲染目标默认不支持线性过滤，触发 device-lost
	// 崩溃（无日志）。修复：中间纹理改 GS_RGBA16F（0..255 浮点 ulp≈0.125 灰阶，
	// 对 3/7 盒式足够），失败再退 32F，与细节段（det_a/det_b）同构。16F 路径
	// 在 RTX 5060 的细节段已验证稳定。
	// 注意：钩子是进程级单例，多个滤镜实例时最后一个生效；本插件正常只会挂
	// 一个实例（每源一个），如果以后要支持多实例，得把 user 指针换成实例表。
	{
		const char *gsp = getenv("BEAUTY_GPU_SMOOTH_PROD");
		// 2.0.0 起默认开启 Phase 3.5 GPU 磨皮生产路径（RTX 5060 上已验证
		// 16F 中间纹理稳定，无 device-lost）。仅当显式设 BEAUTY_GPU_SMOOTH_PROD=0
		// 时才退回 CPU 磨皮。
		f->gpu_smooth_prod = beautyEngine().supportsGpu() &&
				     !(gsp && gsp[0] == '0');
		if (f->gpu_smooth_prod)
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] Phase 3.5 GPU 磨皮生产路径已启用"
			     "（默认开启；BEAUTY_GPU_SMOOTH_PROD=0 退回 CPU；"
			     "中间纹理 RGBA16F 绕过 RTX 5060 32F+线性采样 device-lost）");
		// Phase 5a：段区间钩子。默认随生产开关一起开，BEAUTY_CHAIN_PROD=0
		// 可以单独关掉退回单段钩子 —— 这就是 3.1.51 与 3.1.52 的 A/B 开关，
		// 不用改任何别的东西。
		const char *gch = getenv("BEAUTY_CHAIN_PROD");
		f->chain_prod = f->gpu_smooth_prod && !(gch && gch[0] == '0');
		if (f->chain_prod) {
			beautyCoreSetChainHook(beauty_chain_hook, f);
			// 两个钩子都挂上：段区间钩子优先，但它返回 0（着色器缺失 /
			// 首帧 / 画幅太小）时引擎会用单段钩子接管磨皮，少一次 CPU 全帧
			// 磨皮。这条兜底是 3.1.49 已验收的路径，留着不亏。
			beautyCoreSetSmoothHook(beauty_smooth_hook, f);
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 段区间钩子已启用（Phase 5a：磨皮→色调"
			     "→细节，失败自动回退 CPU）");
		} else if (f->gpu_smooth_prod) {
			beautyCoreSetSmoothHook(beauty_smooth_hook, f);
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] GPU 磨皮生产路径已启用（Phase 3.5，"
			     "失败自动回退 CPU）");
		}
		// T2：同会话 A/B 切臂开关。**只在 chain 臂本身就开着时才有意义**
		// （关掉 chain 的话两条臂是同一个，翻也没用）。计数靠 prof 那条路径
		// 递增，所以 prof 关着时它自然退化成「永远停在 chain 臂」—— 也就是
		// 生产行为，不会因为忘了关这个开关而变慢或变样。
		const char *gab = getenv("BEAUTY_CHAIN_AB");
		f->chain_ab = f->chain_prod && gab && gab[0] == '1';
		// 切臂对象：默认翻「段区间钩子」（chain / 1seg）。设成 out 则翻
		// 「链尾不回读」（out / rb），用来同会话量 5b-1 的净收益 —— 两条臂
		// 都保持段区间钩子开着，只比回读与否。
		const char *gtg = getenv("BEAUTY_AB_TARGET");
		f->chain_ab_out = f->chain_ab && gtg && strcmp(gtg, "out") == 0;
		if (f->chain_ab)
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] A/B 切臂已启用（每 180 帧翻一次：%s，"
			     "日志行尾 ab= 标明本窗口属于哪条臂）",
			     f->chain_ab_out ? "out / rb（5b-1 开关）"
					     : "chain / 1seg（段区间钩子）");
		// 5b-1：链尾不回读。依赖段区间钩子（没有它就没有「留在显存里的
		// 最终结果」可画），所以与 chain_prod 绑定。
		// 3.1.60：**默认开**（实测 720p 4.05→2.04、4K 34.59→13.52 ms，
		// 对拍 RGB maxdiff=0 已验收）。要退回老路（回读+上传）显式设
		// BEAUTY_CHAIN_OUT=0 —— 只认 '0' 作关闭，其余值/不设都算开，
		// 避免拼错环境变量就悄悄退回旧行为。
		//
		// alpha 行为（3.1.60 定案，透传）：直绘透传源帧 alpha，老路无条件
		// 强制 0xFF。对铺满整帧的相机源零差别（720p 对拍 maxdiff=0）；
		// 对带透明区的场景源，新行为保留透明（老路会把透明区画成不透明
		// 黑块，是 bug）。这条差异是对拍里 alpha 6,220,800 px = 75.00% 的
		// 唯一来源，RGB 逐像素一致。
		const char *gco = getenv("BEAUTY_CHAIN_OUT");
		f->chain_out = f->chain_prod && (!gco || gco[0] != '0');
		// 一次性对拍：把「直绘纹理」与「回读+上传后绘制」两条路各渲染进一张
		// scratch RT 再逐像素比。这是唯一能证明「直绘与老路像素一致」的办法
		// —— 比纹理内容没有意义（两条路本来就是同一份字节）。
		const char *gcmp = getenv("BEAUTY_CHAINOUT_CMP");
		f->chainout_cmp = f->chain_out && gcmp && gcmp[0] == '1';
		if (f->chain_out)
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 链尾不回读已启用（5b-1 默认开：本帧 flags "
			     "全认领时跳过 map+copy+上传，直接绘制显存里的纹理；要跑"
			     "五官/美形时自动退回回读；BEAUTY_CHAIN_OUT=0 可退回老路）");
	}
	return f;
}

static void beauty_filter_destroy(void *data)
{
	struct beauty_filter *f = (struct beauty_filter *)data;
	if (!f)
		return;
	if (f->slot >= 0) {
		beautyEngine().releaseSlot(f->slot);
		f->slot = -1;
	}
	// Phase 2 GPU 色调资源（这些全是 graphics 对象，必须在图形线程/上下文
	// 仍然有效时释放；destroy 由 OBS 在销毁 source 时调用，满足条件）。
	if (f->tone_fx)
		gs_effect_destroy(f->tone_fx);
	if (f->tone_rt)
		gs_texrender_destroy(f->tone_rt);
	if (f->mask_tex)
		gs_texture_destroy(f->mask_tex);
	free(f->mask_buf);
	// Phase 3 GPU 磨皮资源（同一批约定：图形对象在此统一释放）
	if (f->gpu_smooth_prod)
		beautyCoreSetSmoothHook(NULL, NULL);   // 先摘钩子，避免悬空 user
	if (f->chain_prod)
		beautyCoreSetChainHook(NULL, NULL);    // 段区间钩子优先，先摘它
	// Phase 5a 资源
	if (f->detail_fx)
		gs_effect_destroy(f->detail_fx);
	if (f->det_a)
		gs_texrender_destroy(f->det_a);
	if (f->det_b)
		gs_texrender_destroy(f->det_b);
	if (f->det_hist)
		gs_texrender_destroy(f->det_hist);
	if (f->chain_rt)
		gs_texrender_destroy(f->chain_rt);
	// 5c 资源
	if (f->warp_fx)
		gs_effect_destroy(f->warp_fx);
	if (f->warp_a)
		gs_texrender_destroy(f->warp_a);
	if (f->warp_b)
		gs_texrender_destroy(f->warp_b);
	// 5b-2 资源（诊断用）
	if (f->mask_fx)
		gs_effect_destroy(f->mask_fx);
	if (f->mask_rt_a)
		gs_texrender_destroy(f->mask_rt_a);
	if (f->mask_rt_b)
		gs_texrender_destroy(f->mask_rt_b);
	if (f->mask_stage)
		gs_stagesurface_destroy(f->mask_stage);
	for (int i = 0; i < BEAUTY_RING_SLOTS; i++) {
		if (f->ring_tex[i])
			gs_texture_destroy(f->ring_tex[i]);
	}
	if (f->region_param)
		gs_texture_destroy(f->region_param);
	for (int i = 0; i < 4; i++) {
		if (f->region_rt[i])
			gs_texrender_destroy(f->region_rt[i]);
	}
	if (f->region_stage)
		gs_stagesurface_destroy(f->region_stage);
	if (f->region_fx)
		gs_effect_destroy(f->region_fx);
	if (f->hook_src_tex)
		gs_texture_destroy(f->hook_src_tex);
	if (f->smooth_fx)
		gs_effect_destroy(f->smooth_fx);
	if (f->sm_d)
		gs_texrender_destroy(f->sm_d);
	if (f->sm_a)
		gs_texrender_destroy(f->sm_a);
	if (f->sm_b)
		gs_texrender_destroy(f->sm_b);
	if (f->sm_c)
		gs_texrender_destroy(f->sm_c);
	if (f->sm_out)
		gs_texrender_destroy(f->sm_out);
	if (f->skin_tex)
		gs_texture_destroy(f->skin_tex);
	if (f->eye_tex)
		gs_texture_destroy(f->eye_tex);
	free(f->protect_buf);
	if (f->out_tex)
		gs_texture_destroy(f->out_tex);
	if (f->stage)
		gs_stagesurface_destroy(f->stage);
	if (f->stage_alt)
		gs_stagesurface_destroy(f->stage_alt);
	// S4b：检测缩略图三件套
	if (f->thumb_rt)
		gs_texrender_destroy(f->thumb_rt);
	if (f->thumb_stage)
		gs_stagesurface_destroy(f->thumb_stage);
	if (f->thumb_buf)
		bfree(f->thumb_buf);
	// 2.11.0（方案C）：运动采样三件套
	if (f->mot_rt)
		gs_texrender_destroy(f->mot_rt);
	if (f->mot_stage)
		gs_stagesurface_destroy(f->mot_stage);
	if (f->mot_stage_alt)
		gs_stagesurface_destroy(f->mot_stage_alt);
	if (f->mot_buf)
		bfree(f->mot_buf);
	if (f->tr)
		gs_texrender_destroy(f->tr);
	free(f->buf_in);
	free(f->buf_out);
	bfree(f);
}

static void beauty_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "enabled", true);
	obs_data_set_default_bool(settings, "mirror", false);
	obs_data_set_default_int(settings, "blur_type", 1);
	obs_data_set_default_int(settings, "smooth_mode", 0);
	obs_data_set_default_int(settings, "input_rot", 0);
	// 2.0 起默认开：BGRA 修正 + 语义掩码之后，"只磨皮肤"才是预期行为，
	// 1.x 默认关是因为当时人脸检测基本失效，开着只会全局磨糊。
	obs_data_set_default_bool(settings, "skin_detect", true);
	obs_data_set_default_int(settings, "landmark_q", 1);
	obs_data_set_default_int(settings, "max_faces", 4);
	obs_data_set_default_int(settings, "param_version", BEAUTY_PARAM_VERSION);
	for (int i = 0; i < BEAUTY_NPARAM; i++)
		obs_data_set_default_double(settings, g_params[i].key, g_params[i].dflt);
}

static const char *beauty_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	// 3.0.15：对外显示名统一为「OBS美颜」。source id 仍是 obs_beauty_filter，
	// 旧场景里已挂好的滤镜不受影响（OBS 按 id 查找，不按显示名）。
	return "OBS美颜";
}

/* 3.1.28：整段「参数悬停备注」已删除（beauty_param_note 函数一并移除）。
 *
 * 用户要求「悬停备注也不需要」—— 面板与 OBS 原生属性页都不再显示任何
 * long_description。标签名本身已经说清功能，长文案只会变成噪音。
 * 历史文案（3.1.21 重写的那一版，含各项实测量级）见 git 历史 /
 * 05_文档\CHANGELOG_提交记录.md 的 3.1.27 之前条目。 */

/* 3.1.22：按 key 建滑块。
 * 属性改成按功能分组之后，滑块不再是一个 for 循环建出来的，只能按名字找。
 * 找不到就报错返回 NULL —— 绝不静默建一个名字不对的滑块，
 * 那会写到用户设置里的另一个键上，把别的功能推歪。 */
static obs_property_t *add_beauty_slider(obs_properties_t *ppts, const char *key)
{
	for (int i = 0; i < BEAUTY_NPARAM; i++) {
		if (strcmp(g_params[i].key, key) != 0)
			continue;
		/* 3.1.28：不再挂 long_description（用户要求「悬停备注也不需要」）。
		 * 滑块只留标签本身，悬停无提示、原生属性页下方也不显示说明。 */
		return obs_properties_add_float_slider(
			ppts, g_params[i].key, g_params[i].label, 0.0, 100.0, 1.0);
	}
	blog(LOG_ERROR, "[%s] add_beauty_slider: unknown key '%s'",
	     PLUGIN_NAME, key ? key : "(null)");
	return NULL;
}

static obs_properties_t *beauty_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *ppts = obs_properties_create();
	// 总开关放第一位：它是「关掉整个滤镜」的唯一入口，比任何滑块都用得多。
	// 3.1.22：标签去掉「（总开关）」。这一行本来就在属性表最顶上、又是第一个
	// 加进去的，位置已经说明了它是总开关，不必再在文字里喊一遍。
	obs_properties_add_bool(ppts, "enabled", "启用美颜");

	obs_property_t *dm = obs_properties_add_list(ppts, "landmark_q", "识别模式", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dm, "标准", 0);
	obs_property_list_add_int(dm, "高精度", 1);

	obs_property_t *mf = obs_properties_add_list(ppts, "max_faces", "人物个数", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	for (int i = 1; i <= 8; i++) {
		char lbl[8];
		snprintf(lbl, sizeof(lbl), "%d", i);
		obs_property_list_add_int(mf, lbl, i);
	}

	obs_properties_add_bool(ppts, "mirror", "镜像");

	// ---- 美颜：按功能类型分成 4 组 ----
	// 3.1.22：原来 11 个滑块 + 3 个选择器全塞在一个「美颜」组里，一屏拉到底
	// 还是很混。现在按类型拆成「磨皮 / 肤色 / 锐化 / 五官」四组，
	// OBS 原生滤镜属性页和自研面板同时受益（同一份属性表）。
	//
	// 顺序上有个巧合值得记一笔：分组后各组内、以及组与组之间合起来，
	// 恰好仍与 P_* 枚举同序（磨皮 → 清晰/美白/红润 → 锐化/全局锐化 → 五官五项）。
	// 引擎取参是按 key（obs_data 存 key→value），所以顺序其实不影响数值，
	// 但保持同序能让日志、截图、下标直觉都对得上。

	// ---- 3.1.29：磨皮算法 / 磨皮模式 提到**顶层** ----
	// 用户要求把这两个下拉放到面板**底部栏**（见 obs-beauty-ui.cpp 的 mode_row_）。
	// 它们不属于任何参数页：是「全局档位」，切到哪一页都该一直看得见。
	// 放在顶层还有一个好处 —— OBS 原生「滤镜属性」窗口里它们也落在参数组之前，
	// 与面板的视觉顺序一致。
	obs_property_t *bt = obs_properties_add_list(ppts, "blur_type", "磨皮算法",
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bt, "双边滤波", 1);
	obs_property_list_add_int(bt, "高斯滤波", 0);
	obs_property_list_add_int(bt, "弱磨皮", 2);

	obs_property_t *sm = obs_properties_add_list(
		ppts, "smooth_mode", "磨皮模式",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sm, "人像磨皮", 0);
	obs_property_list_add_int(sm, "清晰磨皮", 1);
	obs_property_list_add_int(sm, "精细磨皮", 2);
	obs_property_list_add_int(sm, "朦胧磨皮", 3);

	// 2.10.0 方案 B：输入方向。把摄像头/来源的旋转吸收进滤镜，使检测几何、五官、
	// 美形、保护椭圆对齐到正确人脸位置；用户保留场景项旋转负责显示方向。
	obs_property_t *ir = obs_properties_add_list(
		ppts, "input_rot", "输入方向",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ir, "原样 (0°)", 0);
	obs_property_list_add_int(ir, "顺时针 90°", 90);
	obs_property_list_add_int(ir, "180°", 180);
	obs_property_list_add_int(ir, "顺时针 270°", 270);

	// ---- 组 1：美颜（3.1.29 合并磨皮与肤色；3.1.30 改名为「美颜」）----
	// 为什么合并：这两类是**天天要调的主参数**，分成两页反而每次都要切。
	// 合并后第一页 5 行：精准美肤 / 磨皮 / 清晰 / 美白 / 红润 —— 一屏看全，
	// 且顺序仍是 P_* 枚举顺序（磨皮 → 清晰/美白/红润）。
	// 「磨皮算法」「磨皮模式」已经移到顶层（底部栏），不在这里。
	obs_properties_t *gb_blur = obs_properties_create();
	obs_properties_add_bool(gb_blur, "skin_detect", "精准美肤");
	add_beauty_slider(gb_blur, "blur_level");
	add_beauty_slider(gb_blur, "clarity");
	add_beauty_slider(gb_blur, "color_level");
	add_beauty_slider(gb_blur, "red_level");
	obs_properties_add_group(ppts, "grp_blur", "美颜", OBS_GROUP_NORMAL, gb_blur);

	// ---- 组 2：锐化 ----
	obs_properties_t *gb_sharp = obs_properties_create();
	add_beauty_slider(gb_sharp, "sharpen");
	add_beauty_slider(gb_sharp, "global_sharpen");
	obs_properties_add_group(ppts, "grp_sharp", "锐化", OBS_GROUP_NORMAL, gb_sharp);

	// ---- 组 3：五官 ----
	obs_properties_t *gb_face = obs_properties_create();
	add_beauty_slider(gb_face, "face_threed");
	add_beauty_slider(gb_face, "eye_bright");
	add_beauty_slider(gb_face, "remove_pouch_strength");
	add_beauty_slider(gb_face, "remove_nasolabial_folds_strength");
	add_beauty_slider(gb_face, "delspot_level");
	obs_properties_add_group(ppts, "grp_face", "五官", OBS_GROUP_NORMAL, gb_face);

	// ---- 组 4：美形（3.1.27 新增）----
	// 与「五官」分开的原因：五官五项是**颜色层**（提亮/压暗/祛斑），像素位置
	// 不动；美形三项是**几何层**（改像素位置）。混在一页里用户会以为它们是
	// 同一类强度旋钮，而且几何变形的副作用（脸边轻微拖影、耗时更高）也该
	// 单独归一类说明。
	obs_properties_t *gb_shape = obs_properties_create();
	add_beauty_slider(gb_shape, "thin_face");
	add_beauty_slider(gb_shape, "big_eye");
	add_beauty_slider(gb_shape, "chin");
	obs_properties_add_group(ppts, "grp_shape", "美形", OBS_GROUP_NORMAL, gb_shape);

	// 3.1.21：不再提供「恢复默认 / 保存到 preset.json / 读取 preset.json」
	// 三个按钮。理由：面板底部已有「恢复默认」，预设文件的场景极少数人用，
	// 而三个长按钮会把属性页撑得很乱（用户明确要求删掉）。

	return ppts;
}

static void beauty_filter_update(void *data, obs_data_t *settings)
{
	struct beauty_filter *f = (struct beauty_filter *)data;
	beauty_migrate(settings);
	beauty_filter_read(f, settings);
	// 只置脏；进程级检测选项（旋转/识别模式/人数）推迟到渲染线程下发。
	// 九个效果滑块不需要脏标记 —— 引擎每槽位做参数指纹比对，不变就不下发。
	f->dirty = true;
}

// ------------------------------------------------------------------
// video_render: capture -> CPU roundtrip -> CoreEngine -> draw
// ------------------------------------------------------------------

// Mean |Laplacian| of the luma plane: a simple, scale-independent measure of
// image detail ("skin texture energy"). A working skin smoother must lower it.
//
// 3.1.21：改成 **4×4 抽样**。原来逐像素扫全帧，1080p 一趟 200 万次五点取样，
// 两次（buf_in + buf_out）在首帧上就是 15 ms 上下 —— 而这只是「开关有没有真的
// 生效」的一次性自检，抽样 1/16 的统计量完全够用（同一幅画面的均值误差 < 1%），
// 代价从 7 ms 降到 0.4 ms。首帧卡顿的一半来自这里。
static double frame_detail(const uint8_t *buf, uint32_t cx, uint32_t cy)
{
	if (!buf || cx < 3 || cy < 3)
		return 0.0;
	const uint32_t STEP = 4;
	uint64_t acc = 0;
	uint64_t n = 0;
	for (uint32_t y = 1; y + 1 < cy; y += STEP) {
		const uint8_t *row = buf + (size_t)y * cx * 4;
		const uint8_t *up = buf + (size_t)(y - 1) * cx * 4;
		const uint8_t *dn = buf + (size_t)(y + 1) * cx * 4;
		for (uint32_t x = 1; x + 1 < cx; x += STEP) {
			const size_t o = (size_t)x * 4;
			int c = row[o] + row[o + 1] + row[o + 2];
			int l = row[o - 4] + row[o - 3] + row[o - 2];
			int r = row[o + 4] + row[o + 5] + row[o + 6];
			int u = up[o] + up[o + 1] + up[o + 2];
			int d = dn[o] + dn[o + 1] + dn[o + 2];
			int lap = 4 * c - l - r - u - d;
			acc += (uint64_t)(lap < 0 ? -lap : lap);
			n++;
		}
	}
	return n ? (double)acc / (double)n / 3.0 : 0.0;
}

// Horizontal mirror on the RGBA buffer (the panel's 镜像 button).
static void frame_mirror(uint8_t *buf, uint32_t cx, uint32_t cy)
{
	const size_t row = (size_t)cx * 4;
	for (uint32_t y = 0; y < cy; y++) {
		uint8_t *r = buf + (size_t)y * row;
		for (uint32_t x = 0; x < cx / 2; x++) {
			uint8_t *a = r + (size_t)x * 4;
			uint8_t *b = r + (size_t)(cx - 1 - x) * 4;
			for (int k = 0; k < 4; k++) {
				const uint8_t t = a[k];
				a[k] = b[k];
				b[k] = t;
			}
		}
	}
}

// ------------------------------------------------------------------
// 方案 A / Phase 2：GPU 色调（**诊断路径**，默认关闭）
//
// 这一段刻意不参与成像：把 tone.hlsl 跑一遍、读回结果、与 CPU 端
// beautyCoreToneCpu() 逐像素对拍，然后只打一行日志。buf_out / out_tex 一个字节
// 都不改，所以开着它也**不可能**造成画面回归 —— 这也是 Phase 2 的交付形态。
//
// 为什么不直接翻转成「GPU 出图」：Phase 2 只把 tone 搬上 GPU、磨皮仍在 CPU，
// 会多一次掩码上传 + 结果下载，而 tone 本身很便宜 → 净功耗为负。真正的收益在
// Phase 5 全链路（tone+blur+morph+detail 全留显存、只上传一次掩码）。
// ------------------------------------------------------------------

// getenv() 返回的是 ANSI（当前代码页）字符串，而 OBS 内部路径一律按 UTF-8
// 处理。项目目录常含中文（D:\插件\...）：不转换的话拼出来的路径在日志里
// 显示成 "D:\???\OBS????GPU??\01_???\shaders/tone.hlsl"，gs_effect_create_from_file
// 直接报 "Could not load effect file"。
static char *beauty_env_path_to_utf8(const char *ansi)
{
	if (!ansi || !ansi[0])
		return NULL;

	int wn = MultiByteToWideChar(CP_ACP, 0, ansi, -1, NULL, 0);
	if (wn <= 0)
		return NULL;

	wchar_t *w = (wchar_t *)bmalloc(sizeof(wchar_t) * (size_t)wn);
	if (!w)
		return NULL;
	MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, wn);

	char *u8 = NULL;
	int un = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (un > 0) {
		u8 = (char *)bmalloc((size_t)un);
		if (u8)
			WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, un, NULL, NULL);
	}
	bfree(w);
	return u8;
}

// 懒加载 tone.hlsl。路径走 OBS 模块 data 目录（obs_module_file），所以装机时
// 必须把它放到 data/obs-plugins/obs-beauty/shaders/tone.hlsl。
static gs_effect_t *beauty_load_tone_effect(struct beauty_filter *f)
{
	if (f->tone_fx)
		return f->tone_fx;
	if (f->tone_tried)   // 失败过就别每帧重试，日志会刷屏
		return NULL;
	f->tone_tried = true;

	char *path = NULL;
	// 开发期覆盖：BEAUTY_SHADER_DIR=<目录> 时直接从该目录加载 tone.hlsl。
	// 没有它，每改一次着色器都要提权覆盖 Program Files\...\data\obs-plugins\
	// obs-beauty\shaders\ —— Phase 3~5 是几十次迭代，这条路走不通。
	// 未设该变量时行为完全不变（走 obs_module_file，即装机后的正式路径）。
	const char *sdir = getenv("BEAUTY_SHADER_DIR");
	if (sdir && sdir[0]) {
		char *u8 = beauty_env_path_to_utf8(sdir);   // ANSI → UTF-8，见上方说明
		if (u8) {
			const size_t n = strlen(u8) + 16;
			path = (char *)bmalloc(n);
			if (path)
				snprintf(path, n, "%s/tone.hlsl", u8);
			bfree(u8);
		}
	} else {
		path = obs_module_file("shaders/tone.hlsl");
	}
	if (!path) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] tone.hlsl 未找到（data 目录缺 shaders/tone.hlsl）："
		     "GPU 色调诊断已关闭");
		return NULL;
	}

	char *errors = NULL;
	f->tone_fx = gs_effect_create_from_file(path, &errors);
	if (!f->tone_fx) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] tone.hlsl 编译失败：%s",
		     errors ? errors : "(no detail)");
		bfree(errors);
	} else {
		blog(LOG_INFO, "[" PLUGIN_NAME "] tone.hlsl 已加载（%s）", path);
	}
	bfree(path);
	return f->tone_fx;
}

// 掩码缓冲 + 掩码纹理：分辨率不变就复用，变了整组重建。
static bool beauty_ensure_mask(struct beauty_filter *f, uint32_t cx, uint32_t cy)
{
	if (f->mask_w == cx && f->mask_h == cy && f->mask_tex && f->mask_buf)
		return true;

	const size_t px = (size_t)cx * cy;
	if (f->mask_cap < px) {
		uint8_t *nb = (uint8_t *)realloc(f->mask_buf, px);
		if (!nb) {
			blog(LOG_ERROR, "[" PLUGIN_NAME "] 掩码缓冲分配失败 %ux%u", cx, cy);
			return false;
		}
		f->mask_buf = nb;
		f->mask_cap = px;
	}

	if (f->mask_tex) {
		gs_texture_destroy(f->mask_tex);
		f->mask_tex = NULL;
	}
	// GS_DYNAMIC 是硬要求：gs_texture_set_image() 走 Map()，D3D11 只允许
	// DYNAMIC 纹理被 Map。非 DYNAMIC 会**静默**失败（纹理内容保持未初始化），
	// 与上面 out_tex 那段注释是同一个坑。GS_R8 = 单通道，掩码本身就是 0..255。
	f->mask_tex = gs_texture_create(cx, cy, GS_R8, 1, NULL, GS_DYNAMIC);
	if (!f->mask_tex) {
		f->mask_w = f->mask_h = 0;
		blog(LOG_ERROR, "[" PLUGIN_NAME "] 掩码纹理创建失败 %ux%u", cx, cy);
		return false;
	}
	f->mask_w = cx;
	f->mask_h = cy;
	return true;
}

// 跑一次色调 pass：in_tex + mask → tone_rt。返回值指向 tone_rt 的纹理，
// 下一次调用（reset）之前有效。mask 是 w*h 字节的皮肤掩码。
static gs_texture_t *beauty_gpu_tone(struct beauty_filter *f, gs_texture_t *in_tex,
				     uint32_t cx, uint32_t cy, const uint8_t *mask,
				     float clarity, float whiten, float rosy)
{
	gs_effect_t *fx = beauty_load_tone_effect(f);
	if (!fx || !beauty_ensure_mask(f, cx, cy))
		return NULL;

	gs_texture_set_image(f->mask_tex, mask, cx, false);

	if (!f->tone_rt)
		f->tone_rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!f->tone_rt)
		return NULL;

	gs_texrender_reset(f->tone_rt);   // texrender 一次性：再次 begin 前必须 reset
	if (!gs_texrender_begin(f->tone_rt, cx, cy))
		return NULL;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);   // 覆盖写，不与旧内容混合
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	gs_eparam_t *p;
	p = gs_effect_get_param_by_name(fx, "image");
	if (p)
		gs_effect_set_texture(p, in_tex);
	p = gs_effect_get_param_by_name(fx, "mask");
	if (p)
		gs_effect_set_texture(p, f->mask_tex);
	p = gs_effect_get_param_by_name(fx, "clarity");
	if (p)
		gs_effect_set_float(p, clarity);
	p = gs_effect_get_param_by_name(fx, "whiten");
	if (p)
		gs_effect_set_float(p, whiten);
	p = gs_effect_get_param_by_name(fx, "rosy");
	if (p)
		gs_effect_set_float(p, rosy);

	// 与主路径同一个理由要用 begin/begin_pass 而不是 gs_effect_loop()：
	// OBS 可能已经在我们外面 begin 了一个 technique，loop 会直接报
	// "An effect is already active" 退出。
	size_t passes = 0;
	gs_technique_t *tech = gs_effect_get_technique(fx, "ToneTech");
	if (tech) {
		passes = gs_technique_begin(tech);
		for (size_t i = 0; i < passes; i++) {
			gs_technique_begin_pass(tech, i);
			gs_draw_sprite(in_tex, 0, cx, cy);
			gs_technique_end_pass(tech);
		}
		gs_technique_end(tech);
	}

	gs_blend_state_pop();
	gs_texrender_end(f->tone_rt);

	// 不要在这里补 gs_ortho：texrender_end 已还原 OBS 的画布投影，见
	// smooth_rt_begin() 上方那段说明（2.2.1 修的就是这里补出来的 1.5 倍放大）。

	return passes ? gs_texrender_get_texture(f->tone_rt) : NULL;
}

// 一次性对拍：GPU 色调 vs CPU toneColor。逐像素比 RGB 三个通道。
//
// 判据：maxdiff=0 → 与 CPU 逐字节等价；1 → 仅浮点末位/舍入差（HLSL 可能把
// t[i]+(t[i+1]-t[i])*f 合成一次 fma），可接受；>=2 → 着色器数学有问题。
// 只跑一次（成功后 log 一行），跑的过程中不改任何成像缓冲。
static void beauty_gpu_tone_selfcheck(struct beauty_filter *f, gs_texture_t *in_tex,
				      uint32_t cx, uint32_t cy)
{
	if (!f->buf_in || !f->stage)
		return;
	if (!beauty_ensure_mask(f, cx, cy))
		return;

	// 掩码必须是「当前正在用的那份」。引擎的异步检测还没到货时拿到的是全 0，
	// 那种帧上对拍没有意义 —— 直接放弃，下一帧再来。
	if (!beautyCoreCopyMask(f->mask_buf, (int)cx, (int)cy))
		return;

	// 面板值 → 引擎同尺度（0..1）。三项全 0 时用 0.5 探针，否则这条自检在
	// 默认参数下永远只是「原图进、原图出」，什么也没验证到。
	float c = (float)(f->v[P_CLARITY] / 100.0);
	float w = (float)(f->v[P_WHITEN] / 100.0);
	float r = (float)(f->v[P_RED] / 100.0);
	bool probe = false;
	if (c <= 0.0f && w <= 0.0f && r <= 0.0f) {
		c = w = r = 0.5f;
		probe = true;
	}

	gs_texture_t *gpu_tex =
		beauty_gpu_tone(f, in_tex, cx, cy, f->mask_buf, c, w, r);
	if (!gpu_tex)
		return;

	const size_t px = (size_t)cx * cy;
	const size_t bytes = px * 4;
	uint8_t *gpu_buf = (uint8_t *)malloc(bytes);
	uint8_t *cpu_buf = (uint8_t *)malloc(bytes);
	if (!gpu_buf || !cpu_buf) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	// f->stage 在此时是空闲的（它的内容早在上一步下载时已拷进 buf_in）。
	bool got = false;
	gs_stage_texture(f->stage, gpu_tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(f->stage, &ptr, &linesize) && ptr) {
		const size_t row = (size_t)cx * 4;
		if (linesize == row) {
			memcpy(gpu_buf, ptr, bytes);
		} else {
			for (uint32_t y = 0; y < cy; y++)
				memcpy(gpu_buf + (size_t)y * row,
				       ptr + (size_t)y * linesize, row);
		}
		gs_stagesurface_unmap(f->stage);
		got = true;
	}
	if (!got) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] GPU 色调自检：tone_rt 回读失败");
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	// 同一帧、同一份掩码、同一组参数，跑 CPU 基准。
	if (!beautyCoreToneCpu(f->buf_in, (int)cx, (int)cy, f->mask_buf, c, w, r,
			       cpu_buf)) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	int maxdiff = 0;
	uint64_t sum_abs = 0;
	uint64_t diff_px = 0;
	uint64_t gpu_sum = 0;
	uint64_t cpu_sum = 0;
	for (size_t k = 0; k < px; k++) {
		const uint8_t *gp = gpu_buf + k * 4;
		const uint8_t *cp = cpu_buf + k * 4;
		int worst = 0;
		for (int ch = 0; ch < 3; ch++) {
			int d = (int)gp[ch] - (int)cp[ch];
			if (d < 0)
				d = -d;
			if (d > worst)
				worst = d;
			sum_abs += (uint64_t)d;
		}
		if (worst)
			diff_px++;
		if (worst > maxdiff)
			maxdiff = worst;
		gpu_sum += (uint64_t)gp[0] + gp[1] + gp[2];
		cpu_sum += (uint64_t)cp[0] + cp[1] + cp[2];
	}

	// 掩码覆盖率：全 0 说明这一帧其实没有皮肤区（对拍会「假通过」），必须打出来。
	uint64_t masked = 0;
	for (size_t k = 0; k < px; k++)
		if (f->mask_buf[k])
			masked++;

	const double n3 = (double)px * 3.0;
	std::string msg = "GPU 色调对拍 ";
	msg += std::to_string(cx) + "x" + std::to_string(cy);
	msg += "（panel clarity=" + std::to_string(f->v[P_CLARITY]) +
	       " 美白=" + std::to_string(f->v[P_WHITEN]) +
	       " 红润=" + std::to_string(f->v[P_RED]) + "）";
	if (probe)
		msg += " [参数全 0，用 0.5 探针]";
	msg += "：maxdiff=" + std::to_string(maxdiff);
	msg += " 平均偏差=" + std::to_string(n3 > 0 ? (double)sum_abs / n3 : 0.0);
	msg += " 差异像素=" + std::to_string((double)diff_px * 100.0 / (double)px) + "%";
	msg += " 亮度 gpu=" + std::to_string((double)gpu_sum / n3);
	msg += " cpu=" + std::to_string((double)cpu_sum / n3);
	msg += " 细节 gpu=" + std::to_string(frame_detail(gpu_buf, cx, cy));
	msg += " cpu=" + std::to_string(frame_detail(cpu_buf, cx, cy));
	msg += " 掩码覆盖=" + std::to_string((double)masked * 100.0 / (double)px) + "%";
	msg += "（tone.hlsl 只跑诊断，画面仍是 CPU 结果）";
	blog(LOG_INFO, "[" PLUGIN_NAME "] %s", msg.c_str());

	free(gpu_buf);
	free(cpu_buf);
	f->gpu_tone_reported = true;
}

// ===========================================================================
// Phase 3：GPU 磨皮（双尺度带通 + 振幅门控）
//
// 与 Phase 2 的色调同一种形态：跑一遍、与 CPU 逐像素对拍、打一行日志，
// **不改 buf_out / out_tex**。理由与 Phase 2 相同 —— 只把磨皮搬上 GPU、其余
// 仍在 CPU，会多一次掩码上传 + 一次结果下载，而磨皮本身是引擎里最贵的一项，
// 净收益仍然是负的。生产翻转留到 Phase 5（全链留显存、只上传一次掩码）。
//
// 数值口径见 smooth.hlsl 文件头：中间层一律 RGBA32F 且装 0..255 的浮点值，
// 与 CPU 的 surface3RgbBand 逐字同式。
// ===========================================================================

// 盒宽：与 beauty_smooth.cpp 的 bandBoxW 逐字同式（含 minW 与「偶数 +1」）。
// 这里必须用**半分辨率宽度**换算 —— 用全分辨率会得到两倍宽的核，而对拍会
// 显示成「GPU 磨得比 CPU 糊」，很容易被误判成核半径传错了。
static int smooth_box_w(int ww, float frac, int minW)
{
	int k = (int)(frac * (float)ww + 0.5f);
	if (k < minW)
		k = minW;
	if ((k & 1) == 0)
		k++;
	return k;
}

static gs_effect_t *beauty_load_smooth_effect(struct beauty_filter *f)
{
	if (f->smooth_fx)
		return f->smooth_fx;
	if (f->smooth_tried)
		return NULL;
	f->smooth_tried = true;

	char *path = NULL;
	const char *sdir = getenv("BEAUTY_SHADER_DIR");
	if (sdir && sdir[0]) {
		char *u8 = beauty_env_path_to_utf8(sdir);
		if (u8) {
			const size_t n = strlen(u8) + 16;
			path = (char *)bmalloc(n);
			if (path)
				snprintf(path, n, "%s/smooth.hlsl", u8);
			bfree(u8);
		}
	} else {
		path = obs_module_file("shaders/smooth.hlsl");
	}
	if (!path) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] smooth.hlsl 未找到（data 目录缺 shaders/smooth.hlsl）："
		     "GPU 磨皮诊断已关闭");
		return NULL;
	}

	char *errors = NULL;
	f->smooth_fx = gs_effect_create_from_file(path, &errors);
	if (!f->smooth_fx) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] smooth.hlsl 编译失败：%s",
		     errors ? errors : "(no detail)");
		bfree(errors);
	} else {
		// 3.1.51：参数存在性自检。**DLL 与着色器版本必须配套**：3.1.51 把掩码
		// 从一张 `gray`（R8G8）拆成了 `skinMask` / `eyeMask` 两张 R8。若只装了新
		// DLL 而 data 目录里还是旧 smooth.hlsl，`smooth_bind_tex` 找不到这两个名字
		// 会静默跳过（它本来就是空指针安全的），采样器就绑到一张空纹理上 ——
		// 结果是 skin 恒 0、磨皮整体失效，而对拍会显示「两边都不磨」而**判为通过**。
		// 这种失效不报错、不崩溃，只会「看起来没效果」，所以在这里硬性拦住：
		// 缺参数就把 effect 整个丢弃，退回 CPU 路径（比画面出错好）。
		static const char *kNeed[] = {"skinMask", "eyeMask", "image", "image2",
					      "image3", "image4"};
		bool ok = true;
		for (size_t i = 0; i < sizeof(kNeed) / sizeof(kNeed[0]); i++) {
			if (!gs_effect_get_param_by_name(f->smooth_fx, kNeed[i])) {
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] smooth.hlsl 缺少参数 `%s`："
				     "着色器与 DLL 版本不匹配（3.1.51 起掩码要求 "
				     "skinMask/eyeMask 两张纹理）。GPU 磨皮已关闭，"
				     "请重装插件以同步 data 目录下的 shaders。",
				     kNeed[i]);
				ok = false;
				break;
			}
		}
		if (!ok) {
			gs_effect_destroy(f->smooth_fx);
			f->smooth_fx = NULL;
		} else {
			blog(LOG_INFO, "[" PLUGIN_NAME "] smooth.hlsl 已加载（%s）", path);
		}
	}
	bfree(path);
	return f->smooth_fx;
}

// 两张掩码纹理（GS_R8：皮肤 / 眼睛保护）+ 一张 CPU 侧保护缓冲。
// 3.1.51：从「一张 GS_R8G8 + 每帧 CPU 交错」改成「两张 GS_R8 直传」。
static bool beauty_ensure_gray(struct beauty_filter *f, uint32_t cx, uint32_t cy)
{
	if (f->gray_w == cx && f->gray_h == cy && f->skin_tex && f->eye_tex &&
	    f->protect_buf)
		return true;

	const size_t px = (size_t)cx * (size_t)cy;
	if (f->gray_cap < px) {
		uint8_t *np = (uint8_t *)realloc(f->protect_buf, px);
		if (!np) {
			blog(LOG_ERROR, "[" PLUGIN_NAME "] 保护掩码缓冲分配失败 %ux%u", cx, cy);
			return false;
		}
		f->protect_buf = np;
		f->gray_cap = px;
	}

	if (f->skin_tex) {
		gs_texture_destroy(f->skin_tex);
		f->skin_tex = NULL;
	}
	if (f->eye_tex) {
		gs_texture_destroy(f->eye_tex);
		f->eye_tex = NULL;
	}
	// 同 mask_tex 的硬要求：GS_DYNAMIC，否则 gs_texture_set_image 静默失败。
	f->skin_tex = gs_texture_create(cx, cy, GS_R8, 1, NULL, GS_DYNAMIC);
	f->eye_tex = gs_texture_create(cx, cy, GS_R8, 1, NULL, GS_DYNAMIC);
	if (!f->skin_tex || !f->eye_tex) {
		if (f->skin_tex) {
			gs_texture_destroy(f->skin_tex);
			f->skin_tex = NULL;
		}
		if (f->eye_tex) {
			gs_texture_destroy(f->eye_tex);
			f->eye_tex = NULL;
		}
		f->gray_w = f->gray_h = 0;
		blog(LOG_ERROR, "[" PLUGIN_NAME "] 掩码纹理创建失败 %ux%u", cx, cy);
		return false;
	}
	f->gray_w = cx;
	f->gray_h = cy;
	// 尺寸一换，上一帧那张「protect 为空」的状态就不再适用了
	f->hook_eye_zero = false;
	return true;
}

// 半分辨率中间层 4 张 + 全分辨率输出 1 张（RGBA）。
// 中间层先试 GS_RGBA16F（0..255 浮点 ulp≈0.125 灰阶，对 3/7 盒式足够；
// 关键：RTX 5060 上 32F 渲染目标 + Linear 采样会触发 device-lost 崩溃，
// 16F 可过滤），失败再退 32F，与细节段（det_a/det_b）同构。输出层固定 RGBA
// （8bit，可被 stage 回读）。尺寸由 gs_texrender_begin() 决定，所以只在第一次建。
static bool beauty_ensure_smooth_rt(struct beauty_filter *f)
{
	if (f->sm_d && f->sm_a && f->sm_b && f->sm_c && f->sm_out)
		return true;   // 都已建好，直接复用

	// 还没建全时才决定格式：若任一中间层已存在，说明上次 16F 成功，沿用；
	// 否则先试 16F，失败再回退 32F。
	const bool need_try16 = !(f->sm_d || f->sm_a || f->sm_b || f->sm_c);
	const enum gs_color_format mid_fmt =
		need_try16 ? GS_RGBA16F : GS_RGBA32F;

	if (!f->sm_d)
		f->sm_d = gs_texrender_create(mid_fmt, GS_ZS_NONE);
	if (!f->sm_a)
		f->sm_a = gs_texrender_create(mid_fmt, GS_ZS_NONE);
	if (!f->sm_b)
		f->sm_b = gs_texrender_create(mid_fmt, GS_ZS_NONE);
	if (!f->sm_c)
		f->sm_c = gs_texrender_create(mid_fmt, GS_ZS_NONE);
	if (!f->sm_out)
		f->sm_out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (need_try16 && (!f->sm_d || !f->sm_a || !f->sm_b || !f->sm_c)) {
		// 16F 创建失败（极少见），销毁重建为 32F
		if (f->sm_d) { gs_texrender_destroy(f->sm_d); f->sm_d = NULL; }
		if (f->sm_a) { gs_texrender_destroy(f->sm_a); f->sm_a = NULL; }
		if (f->sm_b) { gs_texrender_destroy(f->sm_b); f->sm_b = NULL; }
		if (f->sm_c) { gs_texrender_destroy(f->sm_c); f->sm_c = NULL; }
		if (f->sm_out) { gs_texrender_destroy(f->sm_out); f->sm_out = NULL; }
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] GS_RGBA16F 不能作磨皮渲染目标，磨皮中间层改用 GS_RGBA32F");
		return beauty_ensure_smooth_rt(f);   // 这次 need_try16=false，走 32F
	}

	f->sm_is_16f = (mid_fmt == GS_RGBA16F);
	return f->sm_d && f->sm_a && f->sm_b && f->sm_c && f->sm_out;
}

static void smooth_bind_tex(gs_effect_t *fx, const char *name, gs_texture_t *t)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(fx, name);
	if (p)
		gs_effect_set_texture(p, t);
}

static void smooth_bind_float(gs_effect_t *fx, const char *name, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(fx, name);
	if (p)
		gs_effect_set_float(p, v);
}

// 5b-2 切片 3：链上「当前生效的掩码纹理」。
// 默认回落到 CPU 掩码上传的 skin_tex/eye_tex；GPU 掩码模式下由钩子在链开始前
// 指向 mask.hlsl 的输出，链结束后清空（见 hook 里的 MaskGuard）。
static gs_texture_t *beauty_mask_skin(struct beauty_filter *f)
{
	return f->mask_skin_use ? f->mask_skin_use : f->skin_tex;
}

static gs_texture_t *beauty_mask_eye(struct beauty_filter *f)
{
	return f->mask_eye_use ? f->mask_eye_use : f->eye_tex;
}

// ---------------------------------------------------------------------------
// 5b-2 切片 4：显存帧环
// ---------------------------------------------------------------------------
// 为什么需要：对齐换帧时链要处理的是**更早那一帧**（掩码描述的那帧），而这帧在
// 显存里已经没有了 —— 今天靠从 CPU 上传（4K ≈3 ms）。环里留最近 3 帧的纹理，
// 就能按帧号直接取。
//
// 为什么用 gs_copy_texture 而不是画一个 quad：拷贝不做采样、不做色彩转换，
// 像素与 in_tex 逐字节相同 —— 这一点被消掉了「帧与掩码是否同源」的全部风险
// （M1 的源图 dump 已经证明 in_tex == CPU 那份 buf_in）。
//
// 三槽够不够：worker 通常落后 1~2 帧（15 ms 推理 vs 33 ms 帧预算）；取不到就回退上传。

static bool beauty_ensure_ring(struct beauty_filter *f, uint32_t cx, uint32_t cy,
			       gs_color_format fmt)
{
	if (f->ring_w == cx && f->ring_h == cy && f->ring_fmt == fmt &&
	    f->ring_tex[0])
		return true;

	for (int i = 0; i < BEAUTY_RING_SLOTS; i++) {
		if (f->ring_tex[i]) {
			gs_texture_destroy(f->ring_tex[i]);
			f->ring_tex[i] = NULL;
		}
		f->ring_tag[i] = 0;
	}
	f->ring_w = f->ring_h = 0;
	for (int i = 0; i < BEAUTY_RING_SLOTS; i++) {
		f->ring_tex[i] = gs_texture_create(cx, cy, fmt, 1, NULL, 0);
		if (!f->ring_tex[i]) {
			for (int j = 0; j < BEAUTY_RING_SLOTS; j++) {
				if (f->ring_tex[j]) {
					gs_texture_destroy(f->ring_tex[j]);
					f->ring_tex[j] = NULL;
				}
			}
			return false;
		}
	}
	f->ring_w = cx;
	f->ring_h = cy;
	f->ring_fmt = fmt;
	return true;
}

static void beauty_ring_put(struct beauty_filter *f, unsigned long long tag,
			    gs_texture_t *tex)
{
	const int slot = (int)(tag % BEAUTY_RING_SLOTS);
	if (!f->ring_tex[slot] || !tex)
		return;
	// ⚠ gs_copy_texture 要求**同尺寸同格式**，否则刷
	//   `device_copy_texture (D3D11): Source and destination must have the same dimensions`
	// 然后在 D3D11 里留下未定义状态（2026-11-01 4K 会话崩溃前就出现了这条）。
	// 画布/源尺寸在会话中会变，所以这里每次都比一次，不信任缓存。
	if (gs_texture_get_width(f->ring_tex[slot]) != gs_texture_get_width(tex) ||
	    gs_texture_get_height(f->ring_tex[slot]) != gs_texture_get_height(tex) ||
	    gs_texture_get_color_format(f->ring_tex[slot]) !=
		    gs_texture_get_color_format(tex))
		return;
	gs_copy_texture(f->ring_tex[slot], tex);
	f->ring_tag[slot] = tag;
}

// 取「帧号恰好是 tag」的那张；取不到返回 NULL（调用方回退上传）。
static gs_texture_t *beauty_ring_get(struct beauty_filter *f,
				     unsigned long long tag)
{
	const int slot = (int)(tag % BEAUTY_RING_SLOTS);
	if (f->ring_tex[slot] && f->ring_tag[slot] == tag)
		return f->ring_tex[slot];
	return NULL;
}

static void smooth_bind_int(gs_effect_t *fx, const char *name, int v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(fx, name);
	if (p)
		gs_effect_set_int(p, v);
}

static void smooth_bind_vec2(gs_effect_t *fx, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(fx, name);
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

// 开一个 texrender 作为绘制目标。三次 push（blend/proj/matrix）由 texrender 自己
// 处理，这里只补 blend 覆盖写 + 正交投影 + 清屏 —— 与 beauty_gpu_tone 同一套。
//
// ⚠⚠ 出了 smooth_rt_end() 之后**绝对不要**再补一记 gs_ortho(0,cx,0,cy)"恢复"
//     主路径的投影。gs_texrender_end() 已经把投影矩阵弹回 begin 之前那一份
//     （obs-studio 32.1.1 libobs/graphics/texture-render.c: gs_texrender_begin
//     做 gs_projection_push()，gs_texrender_end 做 gs_projection_pop()），那一份
//     **不是**源尺寸投影，而是 OBS 画布的投影：
//       · obs-video.c set_render_size()  → gs_ortho(0, 画布宽, 0, 画布高)
//       · obs-source.c render_filter_tex() → gs_draw_sprite(tex, 0, w, h) 不设投影
//       · obs-scene.c draw_item()        → 只压 draw_transform 矩阵，不设投影
//     也就是说「源像素 → 画布像素」由场景项的矩阵负责，(0..cx,0..cy) 只在
//     画布 == 源尺寸时才等价。把它改成源尺寸后，同一个 draw_transform 会在
//     一个更小的投影里铺开 → 整幅画面被放大 画布/cx 倍，并以画布左上角为锚点。
//     画布 1920x1080 + 相机 1280x720 就是 1.5 倍：这正是 2.2.0 及以前
//     「磨皮 = 0 画面正常、磨皮 > 0 画面被放大」的现象（不带 SMOOTH 的帧
//     在链钩子第一道判断就 return 0，一次投影都没改；一旦磨皮 > 0，磨皮/色调/
//     细节三段每帧各补一记 gs_ortho，于是整帧被放大）。
//     现已由 beauty_filter_render() 入口的 projection guard 兜底；这里的
//     调用点也一并删掉，别再把它当成"必要的一步"加回来。
static bool smooth_rt_begin(gs_texrender_t *rt, uint32_t cx, uint32_t cy)
{
	gs_texrender_reset(rt);   // texrender 一次性：再次 begin 前必须 reset
	if (!gs_texrender_begin(rt, cx, cy))
		return false;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	struct vec4 cc;
	vec4_zero(&cc);
	gs_clear(GS_CLEAR_COLOR, &cc, 0.0f, 0);
	return true;
}

static void smooth_rt_end(gs_texrender_t *rt)
{
	gs_blend_state_pop();
	gs_texrender_end(rt);
}

// 跑一个 technique（单 pass）。刻意不用 gs_effect_loop()：OBS 可能已在外层
// begin 了一个 technique，loop 会直接报 "An effect is already active" 退出。
static void smooth_draw(gs_effect_t *fx, const char *tech_name, gs_texture_t *draw_tex,
			uint32_t cx, uint32_t cy)
{
	gs_technique_t *tech = gs_effect_get_technique(fx, tech_name);
	if (!tech)
		return;
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(draw_tex, 0, cx, cy);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
}

// 一趟可分离盒式：src → rt。
// rad == 0 时退化为「逐像素复制」（循环只跑 k=0、invBox=1），Copy 步骤就靠它，
// 不必再写一个只做 copy 的 technique。
static void smooth_box_pass(gs_effect_t *fx, gs_texrender_t *rt, gs_texture_t *src,
			    uint32_t w, uint32_t h, int rad, bool horiz,
			    gs_texture_t *draw_tex)
{
	smooth_bind_tex(fx, "image", src);
	smooth_bind_vec2(fx, "texel", horiz ? 1.0f / (float)w : 0.0f,
			 horiz ? 0.0f : 1.0f / (float)h);
	smooth_bind_int(fx, "rad", rad);
	smooth_bind_float(fx, "invBox", 1.0f / (float)(2 * rad + 1));

	if (!smooth_rt_begin(rt, w, h))
		return;
	smooth_draw(fx, horiz ? "BoxHTech" : "BoxVTech", draw_tex, w, h);
	smooth_rt_end(rt);
}

// 完整磨皮：in_tex + mask/protect + 参数 → sm_out 的纹理。
//
// 诊断（默认关）：BEAUTY_SMOOTH_PASSES=1..6 强制磨皮的盒式趟数。
//
// 只为一个决策服务：量出「趟数 → 耗时」的斜率，判断 S5 的收益上限落在
// **提交/RT 切换成本**还是 **GPU 带宽**上（PROGRESS 9.6 / 9.6.1）。
// ⚠ 不是产品开关：趟数小 = 平滑不足，画面会变。只在测时用，别写进默认配置。
// 覆盖点选在宿主而不是引擎，是为了让 GPU 与「CPU 对拍基准」用的是同一个值
// （两边都从 BeautyBandDiag 取 passes），否则自检会自己和自己打架。
static int beauty_smooth_passes_force()
{
	static int v = -1;
	if (v < 0) {
		v = 0;
		const char *e = getenv("BEAUTY_SMOOTH_PASSES");
		if (e && e[0]) {
			const int n = atoi(e);
			if (n >= 1 && n <= 6) {
				v = n;
				// 用 INFO 而不是 WARNING：`gate.py` 把 `[obs-beauty].*warning`
				// 计进 warn 列，探针跑会被误判成「不干净」。这条只是记录生效值。
				blog(LOG_INFO,
				     "[" PLUGIN_NAME "] 磨皮趟数被 BEAUTY_SMOOTH_PASSES 强制为 %d"
				     "（诊断用，非产品配置）", n);
			}
		}
	}
	return v;
}

// 缓冲编排（半分辨率，共 4 张 RGBA32F）：
//   D = 下采样源，全程只读，Amp 之后才释放
//   A = B1 结果        B = 临时 / amp 结果        C = B2 结果
//
//     1) Down   in → D
//     2) Copy   D  → A                      （A 作 B1 的工作区）
//     3) B1     (A,B) 交替 nPass 趟         → A = B1
//     4) Copy   D  → C                      （C 作 B2 的工作区）
//     5) B2     (C,B) 交替 nPass 趟         → C = B2
//     6) Amp    D,C → B                      （B = |D.g − C.g|）
//     7) Ampbox (B,D) 交替 nPass 趟         → B = amp   （D 此时已无用，拿来 ping-pong）
//     8) Comp   in,D?否 —— in + A + C + B + gray → sm_out
//
// 为什么 3) 和 5) 都要先 Copy：B1/B2 都从「下采样源」出发，而可分离盒式的
// ping-pong 会覆写工作区。直接拿 D 当工作区会在第 2 趟就把源冲掉。
static gs_texture_t *beauty_gpu_smooth(struct beauty_filter *f, gs_texture_t *in_tex,
				       uint32_t cx, uint32_t cy, BeautyBandDiag *bp)
{
	gs_effect_t *fx = beauty_load_smooth_effect(f);
	if (!fx || !bp)
		return NULL;
	if (!beauty_ensure_gray(f, cx, cy) || !beauty_ensure_smooth_rt(f))
		return NULL;

	const uint32_t ws = (cx + 1) / 2;
	const uint32_t hs = (cy + 1) / 2;
	if (ws < 8 || hs < 8)   // 与 CPU 的小画幅回退同条件
		return NULL;

	int w1 = smooth_box_w((int)ws, bp->f1, 3);
	int w2 = smooth_box_w((int)ws, bp->f2, 5);
	if (w2 <= w1 + 1)
		w2 = w1 + 2;      // 保证 B2 确实比 B1 宽（CPU 同一条约束）
	const int wa = smooth_box_w((int)ws, bp->fa, 3);
	const int r1 = (w1 - 1) / 2;
	const int r2 = (w2 - 1) / 2;
	const int ra = (wa - 1) / 2;
	const int nPass = (bp->passes < 1) ? 1 : ((bp->passes > 6) ? 6 : bp->passes);

	// 1) Down
	smooth_bind_tex(fx, "image", in_tex);
	smooth_bind_vec2(fx, "srcSize", (float)cx, (float)cy);
	smooth_bind_vec2(fx, "dstSize", (float)ws, (float)hs);
	if (!smooth_rt_begin(f->sm_d, ws, hs))
		return NULL;
	smooth_draw(fx, "DownTech", in_tex, ws, hs);
	smooth_rt_end(f->sm_d);

	gs_texture_t *tex_d = gs_texrender_get_texture(f->sm_d);
	if (!tex_d)
		return NULL;

	// 2) Copy D → A
	smooth_box_pass(fx, f->sm_a, tex_d, ws, hs, 0, true, in_tex);
	gs_texture_t *tex_a = gs_texrender_get_texture(f->sm_a);
	if (!tex_a)
		return NULL;

	// 3) B1：(A,B) 交替，nPass 趟 → 最终落在 A
	for (int i = 0; i < nPass; i++) {
		smooth_box_pass(fx, f->sm_b, tex_a, ws, hs, r1, true, in_tex);
		gs_texture_t *tb = gs_texrender_get_texture(f->sm_b);
		if (!tb)
			return NULL;
		smooth_box_pass(fx, f->sm_a, tb, ws, hs, r1, false, in_tex);
		tex_a = gs_texrender_get_texture(f->sm_a);
		if (!tex_a)
			return NULL;
	}

	// 4) Copy D → C
	smooth_box_pass(fx, f->sm_c, tex_d, ws, hs, 0, true, in_tex);
	gs_texture_t *tex_c = gs_texrender_get_texture(f->sm_c);
	if (!tex_c)
		return NULL;

	// 5) B2：(C,B) 交替，nPass 趟 → 最终落在 C
	for (int i = 0; i < nPass; i++) {
		smooth_box_pass(fx, f->sm_b, tex_c, ws, hs, r2, true, in_tex);
		gs_texture_t *tb = gs_texrender_get_texture(f->sm_b);
		if (!tb)
			return NULL;
		smooth_box_pass(fx, f->sm_c, tb, ws, hs, r2, false, in_tex);
		tex_c = gs_texrender_get_texture(f->sm_c);
		if (!tex_c)
			return NULL;
	}

	// 6) Amp 初值：|D.g − C.g| → B
	smooth_bind_tex(fx, "image", tex_d);
	smooth_bind_tex(fx, "image2", tex_c);
	if (!smooth_rt_begin(f->sm_b, ws, hs))
		return NULL;
	smooth_draw(fx, "AmpTech", in_tex, ws, hs);
	smooth_rt_end(f->sm_b);
	gs_texture_t *tex_b = gs_texrender_get_texture(f->sm_b);
	if (!tex_b)
		return NULL;

	// 7) amp 的盒式平滑：(B,D) 交替 → 最终落在 B。D 到这里已经不被需要。
	for (int i = 0; i < nPass; i++) {
		smooth_box_pass(fx, f->sm_d, tex_b, ws, hs, ra, true, in_tex);
		gs_texture_t *td = gs_texrender_get_texture(f->sm_d);
		if (!td)
			return NULL;
		smooth_box_pass(fx, f->sm_b, td, ws, hs, ra, false, in_tex);
		tex_b = gs_texrender_get_texture(f->sm_b);
		if (!tex_b)
			return NULL;
	}

	// 8) Comp（全分辨率）
	smooth_bind_tex(fx, "image", in_tex);
	smooth_bind_tex(fx, "image2", tex_a);   // B1
	smooth_bind_tex(fx, "image3", tex_c);   // B2
	smooth_bind_tex(fx, "image4", tex_b);   // amp
	smooth_bind_tex(fx, "skinMask", beauty_mask_skin(f));
	smooth_bind_tex(fx, "eyeMask", beauty_mask_eye(f));
	smooth_bind_float(fx, "gI", bp->I);
	smooth_bind_float(fx, "gE", bp->e);
	smooth_bind_float(fx, "gT", (bp->T > 1e-3f) ? bp->T : 1.0f);
	smooth_bind_float(fx, "gGwp", bp->gwp);
	smooth_bind_float(fx, "gEyeK", bp->protectK);
	if (!smooth_rt_begin(f->sm_out, cx, cy))
		return NULL;
	smooth_draw(fx, "CompTech", in_tex, cx, cy);
	smooth_rt_end(f->sm_out);

	// 不要恢复投影（见 smooth_rt_begin 上方的说明）
	return gs_texrender_get_texture(f->sm_out);
}

// 一次性对拍：GPU 磨皮 vs CPU surface3RgbBand。
//
// 判据（比色调的 maxdiff<=1 宽一档）：磨皮要经过 20 多趟盒式，CPU 用滑动窗
// 差分累加、GPU 每像素独立求和，浮点重排的误差会累积；maxdiff<=2 且平均偏差
// <0.05 灰阶即可视为「同一套数学」。>=3 说明是真的算错了（比如值域、盒宽或
// 半径传错），必须查。
static void beauty_gpu_smooth_selfcheck(struct beauty_filter *f, gs_texture_t *in_tex,
					uint32_t cx, uint32_t cy)
{
	// 等磨皮起效上限：等 bandDiag 置位（磨皮这一帧真的在跑）最多 600 帧，
	// 正常是第 1~2 帧。磨皮没开时永远等不到，也就不该对拍。
	static const int kSmoothWaitMax = 600;
	// 等强效果上限：等「检出人脸 + 掩码有实质面积」最多 60 帧（30/60fps 下
	// 约 1~2 秒）。超时就用全帧掩码兜底完成数学对拍，只标注弱效果 ——
	// 旧行为是等满 600 帧才兜底、期间**静默丢弃**这次机会，那正是「磨皮对拍
	// 看起来依赖 TONE / 依赖人脸」的根因：不开 TONE（或镜头前没人）时日志里
	// 干干净净，被误判成对压根没触发。
	static const int kStrongWaitMax = 60;
	// 掩码覆盖率下限：低于这个比例基本等于没有可磨的区域，对拍等于在比空白。
	static const double kSmoothMinCover = 3.0;

	BeautyBandDiag bp;
	bool bd = beautyCoreBandDiag(&bp);

	if (!f->buf_in || !f->stage)
		return;

	if (!bd) {
		if (f->smooth_wait < kSmoothWaitMax)
			f->smooth_wait++;
		return;
	}

	// 对拍与 TONE / 美白 / 五官等无关：只要磨皮在跑（bandDiag=1）就比对数学。
	// 人脸只决定「磨哪里」，不影响「怎么磨」。
	int mask_px = -1, faces = 0;
	beautyCoreMaskStat(&mask_px, &faces);
	const double cover = (mask_px > 0 && cx && cy)
				     ? 100.0 * (double)mask_px / ((double)cx * (double)cy)
				     : 0.0;
	const bool strong = (faces >= 1) && (cover >= kSmoothMinCover);

	// 优先取一帧强效果，但有界：等不到（无人脸 / 检测还没起来）就走全帧兜底，
	// 不再无限等待、也不再静默丢弃这次机会。
	if (!strong && f->smooth_wait < kStrongWaitMax) {
		f->smooth_wait++;
		return;
	}

	// ⚠ 两个 ensure 都要：ensure_gray 只分配 protect_buf + skin_tex/eye_tex，
	// **mask_buf 归 beauty_ensure_mask 管**。曾漏掉后者，导致只有先跑过色调
	// 自检（beauty_gpu_tone → beauty_ensure_mask）时 mask_buf 才被分配好，
	// 于是「磨皮对拍看起来依赖 BEAUTY_GPU_TONE」—— 实际是缓冲没分配，
	// CopyMask 一进来就因 out==NULL 失败并静默返回。
	if (!beauty_ensure_gray(f, cx, cy) || !beauty_ensure_mask(f, cx, cy))
		return;

	const size_t px = (size_t)cx * (size_t)cy;
	if (!beautyCoreCopyMask(f->mask_buf, (int)cx, (int)cy)) {
		// 区分两种失败，别混为一谈：
		//   · mask_px >= 0 → 掩码有效但**尺寸对不上**（刚换分辨率）。此时 GPU
		//     侧用的是它自己那张脸掩码，这边若填全帧就变成「GPU 磨脸 / CPU
		//     磨全帧」口径打架 → 对拍假失败 → 而本函数末尾的失败兜底会
		//     **永久卸载磨皮钩子**。宁可跳过这一帧，等尺寸对齐。
		//   · mask_px < 0  → 真没掩码（无人脸 / 检测未就绪）。GPU 侧本来就是
		//     全帧磨皮（haveMask=false → job.mask=nullptr），这边填全 255 才是
		//     同口径，可以放心兜底对拍，只是标注为弱效果。
		if (mask_px >= 0)
			return;
		memset(f->mask_buf, 0xFF, px);
	}
	beautyCoreCopyProtect(f->protect_buf, (int)cx, (int)cy);   // 失败则全 0，可比

	// 3.1.51：两张纹理各自直传，不再在 CPU 上交错成 R8G8。
	// mask_buf / protect_buf 本身就是连续单通道缓冲，set_image 一行就够。
	gs_texture_set_image(f->skin_tex, f->mask_buf, cx, false);
	gs_texture_set_image(f->eye_tex, f->protect_buf, cx, false);
	f->hook_eye_zero = false;   // 诊断路径把 eye_tex 也写过了

	gs_texture_t *gpu_tex = beauty_gpu_smooth(f, in_tex, cx, cy, &bp);
	if (!gpu_tex)
		return;

	const size_t bytes = px * 4;
	uint8_t *gpu_buf = (uint8_t *)malloc(bytes);
	uint8_t *cpu_buf = (uint8_t *)malloc(bytes);
	if (!gpu_buf || !cpu_buf) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	bool got = false;
	gs_stage_texture(f->stage, gpu_tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(f->stage, &ptr, &linesize) && ptr) {
		const size_t row = (size_t)cx * 4;
		if (linesize == row) {
			memcpy(gpu_buf, ptr, bytes);
		} else {
			for (uint32_t y = 0; y < cy; y++)
				memcpy(gpu_buf + (size_t)y * row,
				       ptr + (size_t)y * linesize, row);
		}
		gs_stagesurface_unmap(f->stage);
		got = true;
	}
	if (!got) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] GPU 磨皮对拍：sm_out 回读失败");
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	// 同一帧、同一份掩码、同一组参数跑 CPU 基准
	if (!beautyCoreSmoothCpu(f->buf_in, cpu_buf, (int)cx, (int)cy, f->mask_buf,
				 f->protect_buf, bp)) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	int maxdiff = 0;
	uint64_t sum_abs = 0;
	uint64_t diff_px = 0;
	uint64_t gpu_sum = 0;
	uint64_t cpu_sum = 0;
	// 改动量：GPU 结果相对源图的平均绝对改变。这是磨皮「磨了多少」的直接刻度，
	// 比 RMSE 更能说明「核宽/强度」有没有传错 —— 两者若不一致，这一项会先露馅。
	uint64_t gpu_chg = 0;
	uint64_t cpu_chg = 0;
	for (size_t k = 0; k < px; k++) {
		const uint8_t *gp = gpu_buf + k * 4;
		const uint8_t *cp = cpu_buf + k * 4;
		const uint8_t *op = f->buf_in + k * 4;
		int worst = 0;
		for (int ch = 0; ch < 3; ch++) {
			int d = (int)gp[ch] - (int)cp[ch];
			if (d < 0)
				d = -d;
			if (d > worst)
				worst = d;
			sum_abs += (uint64_t)d;
			gpu_chg += (uint64_t)((int)gp[ch] > op[ch] ? gp[ch] - op[ch]
								  : op[ch] - gp[ch]);
			cpu_chg += (uint64_t)((int)cp[ch] > op[ch] ? cp[ch] - op[ch]
								  : op[ch] - cp[ch]);
		}
		if (worst)
			diff_px++;
		if (worst > maxdiff)
			maxdiff = worst;
		gpu_sum += (uint64_t)gp[0] + gp[1] + gp[2];
		cpu_sum += (uint64_t)cp[0] + cp[1] + cp[2];
	}

	uint64_t masked = 0;
	for (size_t k = 0; k < px; k++)
		if (f->mask_buf[k])
			masked++;

	const double n3 = (double)px * 3.0;
	const double avg_abs = n3 > 0 ? (double)sum_abs / n3 : 0.0;
	// 16F 精度：0..255 浮点 ulp≈0.125，盒式求和后全域平均偏差可能到 0.1~0.3，
	// 远超 32F 的 <0.05 阈值，故平均偏差上限放宽到 0.3；maxdiff 仍卡 2 以捕获
	// 结构性错误（值域/盒宽传错会让最坏像素差 >=10，绝不会被 2 放过）。
	const bool pass = (maxdiff <= 2) && (avg_abs < 0.3);

	std::string msg = "GPU 磨皮对拍 ";
	msg += std::to_string(cx) + "x" + std::to_string(cy);
	msg += "（半分辨率 " + std::to_string(cx / 2) + "x" + std::to_string(cy / 2);
	msg += " 盒宽 " + std::to_string(smooth_box_w((int)((cx + 1) / 2), bp.f1, 3));
	msg += "/" + std::to_string(smooth_box_w((int)((cx + 1) / 2), bp.f2, 5));
	msg += " 参数 I=" + std::to_string(bp.I) + " e=" + std::to_string(bp.e);
	msg += " T=" + std::to_string(bp.T) + " gwp=" + std::to_string(bp.gwp) + "）";
	msg += "：maxdiff=" + std::to_string(maxdiff);
	msg += " 平均偏差=" + std::to_string(avg_abs);
	msg += " 差异像素=" + std::to_string((double)diff_px * 100.0 / (double)px) + "%";
	msg += " 亮度 gpu=" + std::to_string((double)gpu_sum / n3);
	msg += " cpu=" + std::to_string((double)cpu_sum / n3);
	msg += " 改动量 gpu=" + std::to_string((double)gpu_chg / n3);
	msg += " cpu=" + std::to_string((double)cpu_chg / n3);
	msg += " 掩码覆盖=" + std::to_string((double)masked * 100.0 / (double)px) + "%";
	msg += " faces=" + std::to_string(faces);
	// 等脸帧数：强效果时它就是「检测模型第几帧出结果」的实测值，比猜有用。
	msg += " 等脸=" + std::to_string(f->smooth_wait) + "帧";
	msg += pass ? " 判定=通过" : " 判定=失败";
	msg += f->sm_is_16f ? " 磨皮RT=RGBA16F" : " 磨皮RT=RGBA32F";
	if (!strong)
		msg += "【弱效果兜底：等满 " + std::to_string(f->smooth_wait) +
		       " 帧仍无人脸，改动量偏小，证明力有限】";
	msg += "（smooth.hlsl 只跑诊断，画面仍是 CPU 结果）";
	blog(pass ? LOG_INFO : LOG_WARNING, "[" PLUGIN_NAME "] %s", msg.c_str());

	// 对拍失败兜底：GPU 磨皮输出不可信，永久卸载生产磨皮钩子回退 CPU 磨皮，
	// 避免持续产错画面。16F 修复已消除 device-lost 类崩溃根因，这里只防
	// 「能跑但算错」的残余（如极端驱动对 16F 仍失真）。selfcheck 仅跑一次，
	// gpu_smooth_reported 在此函数末尾置位，不会重复卸载。
	if (!pass) {
		f->gpu_smooth_broken = true;
		blog(LOG_ERROR,
		     "[" PLUGIN_NAME "] GPU 磨皮对拍失败，卸载磨皮钩子回退 CPU 磨皮");
		beautyCoreSetSmoothHook(NULL, NULL);
	}

	free(gpu_buf);
	free(cpu_buf);
	f->gpu_smooth_reported = true;
}

// ---------------------------------------------------------------------------
// 钩子准备段（3.1.52 从单段钩子里抽出来，两条钩子共用）
// ---------------------------------------------------------------------------
// 做三件事：拿到本帧的输入纹理、上传两张掩码、报告两段耗时。
// 抽出来的理由不是为了复用代码量，而是因为这三种失败模式都**不报错**：
//   · in_tex 与 src 不是同一张时忘了上传 → 磨皮用的是上一帧的画面；
//   · 纹理不是 GS_DYNAMIC → gs_texture_set_image 静默无操作；
//   · 没有 protect 时没把 eye_tex 清 0 → 眼睛保护还用着上一帧的掩码。
// 两份拷贝迟早会只改一处，所以只能有一份。
//
// t_frame / t_mask 输出毫秒（可为 NULL）。成功返回可采样的输入纹理。
// skip_mask=true 时跳过两块掩码上传（GPU 掩码模式，见 5b-2 切片 3）。
static gs_texture_t *beauty_hook_prepare(struct beauty_filter *f,
					 const unsigned char *src, int w, int h,
					 const unsigned char *mask,
					 const unsigned char *protect,
					 double *t_frame, double *t_mask,
					 bool skip_mask = false)
{
	const double t0 = beauty_now_ms();

	// ---- 输入纹理 -----------------------------------------------------
	// src 与 f->buf_in 是同一份时（多数帧），in_tex 里已经是这张图，直接采样，
	// 省掉一次整帧上传（720p 0.45 ms / 4K 约 3 ms）。对齐换帧时才需要上传。
	gs_texture_t *in = NULL;
	if (src == f->buf_in && f->hook_cur_in) {
		in = f->hook_cur_in;
	} else {
		if (!f->hook_src_tex || f->hook_w != (uint32_t)w ||
		    f->hook_h != (uint32_t)h) {
			if (f->hook_src_tex) {
				gs_texture_destroy(f->hook_src_tex);
				f->hook_src_tex = NULL;
			}
			// GS_RGBA + GS_DYNAMIC：与 out_tex 上传引擎结果的写法一致，
			// 通道序天然与 in_tex / buf_in 对齐（见主路径那两处拷贝）。
			f->hook_src_tex = gs_texture_create((uint32_t)w, (uint32_t)h,
							    GS_RGBA, 1, NULL,
							    GS_DYNAMIC);
			if (!f->hook_src_tex) {
				f->hook_w = f->hook_h = 0;
				return NULL;
			}
			f->hook_w = (uint32_t)w;
			f->hook_h = (uint32_t)h;
		}
		gs_texture_set_image(f->hook_src_tex, src, (uint32_t)w * 4, false);
		in = f->hook_src_tex;
	}

	const double t1 = beauty_now_ms();

	// ---- 准备段之二：掩码两张纹理 --------------------------------------
	// 3.1.51：掩码不再在 CPU 上交错。引擎递来的 mask / protect 就是两块连续
	// 的 w*h 缓冲，直接 set_image 上传，零拷贝 —— 原来那趟 92 万次迭代的
	// 交错写（720p 1.8 MB）整段消失。上传字节数与联合纹理时相同。
	const size_t px = (size_t)w * (size_t)h;
	// 5b-2 切片 3：GPU 掩码模式下这两块上传整段跳过 —— 链直接采样 mask.hlsl 的
	// 输出。4K 两张 R8 合计 16.6 MB，是这一段的主要开销（实测 2.05 ms）。
	if (!skip_mask) {
		if (mask) {
			gs_texture_set_image(f->skin_tex, mask, (uint32_t)w, false);
		} else {
			// 无掩码 = 全皮肤，与 CPU 侧 mask==nullptr 时 m 恒 255 同义。
			memset(f->protect_buf, 0xff, px);
			gs_texture_set_image(f->skin_tex, f->protect_buf,
					     (uint32_t)w, false);
		}
		if (protect) {
			gs_texture_set_image(f->eye_tex, protect, (uint32_t)w, false);
			f->hook_eye_zero = false;
		} else if (!f->hook_eye_zero) {
			// 没有保护掩码时 eye_tex 必须是全 0。清一次、记住，别每帧都清。
			memset(f->protect_buf, 0, px);
			gs_texture_set_image(f->eye_tex, f->protect_buf,
					     (uint32_t)w, false);
			f->hook_eye_zero = true;
		}
	}

	const double t2 = beauty_now_ms();
	if (t_frame)
		*t_frame = t1 - t0;
	if (t_mask)
		*t_mask = t2 - t1;
	return in;
}

// ---------------------------------------------------------------------------
// Phase 3.5：GPU 磨皮生产路径
// ---------------------------------------------------------------------------
//
// 与上面那条对拍路径共用 beauty_gpu_smooth() 的同一套 8 段 draw，差别只在两头：
//   · 入口：帧由引擎递过来（可能是掩码对齐换来的 heldFrame_），不是当前 in_tex；
//   · 出口：结果直接写进 dst 交给后续步骤，不回读做比对。
//
// 位置很关键：它由引擎在 band 磨皮段内部调用，而不是在主路径「下载 → 引擎 →
// 上传」外侧。原因见 beauty_core.h 里 BeautySmoothHook 的注释（对齐换帧）。
//
// 返回 false 一律回退 CPU：着色器没加载 / 小画幅 / 纹理分配失败 / 回读失败。
// 回退路径的画面与 3.1.48 逐字节相同，所以这是一个「只会变快、不会变错」的开关。
//
// 3.1.52 起这是**兜底**路径：段区间钩子（beauty_chain_hook）优先，它返回 0
// 时引擎才回头问这个单段钩子。BEAUTY_CHAIN_PROD=0 可以强制只走这条。

// SEH 防崩兜底：RTX 5060（Blackwell）上 Phase 3.5 的 23 次 draw 曾在特定纹理
// 格式/驱动下触发 device-lost，表现为整进程 闪退且无日志。这里用结构化异常
// 捕获把「崩」变成「本帧回退 CPU + 调用方卸载钩子」，保证无论如何 OBS 不闪退、
// 且留下明确日志供排查。函数体内只有 POD（指针/整型/标志），无 C++ 对象析构，
// 可安全使用 __try/__except。
static gs_texture_t *beauty_gpu_smooth_safe(struct beauty_filter *f,
					    gs_texture_t *in_tex,
					    uint32_t cx, uint32_t cy,
					    BeautyBandDiag *bp, bool *crashed)
{
	gs_texture_t *t = NULL;
	*crashed = false;
	__try {
		t = beauty_gpu_smooth(f, in_tex, cx, cy, bp);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*crashed = true;
		t = NULL;
	}
	return t;
}

static bool beauty_smooth_hook(void *user, const unsigned char *src,
			       unsigned char *dst, int w, int h,
			       const unsigned char *mask,
			       const unsigned char *protect,
			       const BeautyBandDiag &bp)
{
	struct beauty_filter *f = (struct beauty_filter *)user;
	if (!f || !src || !dst || !f->stage || w <= 0 || h <= 0)
		return false;
	// 本会话已确认 GPU 磨皮会崩/算错：永久回退 CPU 磨皮，不再尝试。
	if (f->gpu_smooth_broken)
		return false;

	if (!beauty_ensure_gray(f, (uint32_t)w, (uint32_t)h))
		return false;

	// 3.1.51：准备段的两段计时（frame / mask）由 beauty_hook_prepare 报告。
	// 3.1.50 时这段时间只能用「磨皮 seg − sm 三段和」倒推，拆不开就不知道该
	// 优化哪一头。
	double prep_f = 0.0, prep_m = 0.0;
	gs_texture_t *in = beauty_hook_prepare(f, src, w, h, mask, protect,
					       &prep_f, &prep_m);
	if (!in)
		return false;

	// ---- 跑链 ----------------------------------------------------------
	// 3.1.50：三段各自计时，见 struct 里 prof_sm_* 的说明。t_draw 量的只是
	// **提交**时间 —— D3D11 延迟执行，真正的 GPU 计算要到 map 才被迫同步，
	// 所以「draw 很小、map 很大」是正常形态，不要误读成 GPU 算得慢。
	const double t_draw0 = beauty_now_ms();
	BeautyBandDiag cp = bp;
	bool smooth_crashed = false;
	gs_texture_t *gpu_tex =
		beauty_gpu_smooth_safe(f, in, (uint32_t)w, (uint32_t)h, &cp,
				       &smooth_crashed);
	const double t_draw1 = beauty_now_ms();
	if (smooth_crashed) {
		// device-lost / 驱动异常被 SEH 捕获：标记 broken、卸载钩子，
		// 本会话后续帧与重启前都回退 CPU 磨皮，绝不二次闪退。
		f->gpu_smooth_broken = true;
		blog(LOG_ERROR,
		     "[" PLUGIN_NAME "] GPU 磨皮生产路径触发异常（疑似 device-lost），"
		     "卸载磨皮钩子、本会话永久回退 CPU 磨皮；排查可设 "
		     "BEAUTY_GPU_SMOOTH=1 跑对拍日志");
		beautyCoreSetSmoothHook(NULL, NULL);
		return false;
	}
	if (!gpu_tex)
		return false;

	// ---- 回读 ----------------------------------------------------------
	// gs_stagesurface_map 会把 GPU 队列排空并等拷贝完成，这一段才是 GPU 侧
	// 真实代价的落点，换掉的是 CPU 磨皮的 8.8 ms。生产路径上它会计进引擎
	// 耗时（seg 的「磨皮」项）。
	gs_stage_texture(f->stage, gpu_tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->stage, &ptr, &linesize) || !ptr) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] GPU 磨皮生产：回读失败，本帧回退 CPU");
		return false;
	}
	const double t_map1 = beauty_now_ms();
	const size_t row = (size_t)w * 4;
	if (linesize == row) {
		memcpy(dst, ptr, row * (size_t)h);
	} else {
		for (int y = 0; y < h; y++)
			memcpy(dst + (size_t)y * row,
			       ptr + (size_t)y * linesize, row);
	}
	gs_stagesurface_unmap(f->stage);
	const double t_copy1 = beauty_now_ms();

	f->prof_sm_prep += prep_f + prep_m;
	f->prof_sm_mask += prep_m;
	f->prof_sm_draw += t_draw1 - t_draw0;
	f->prof_sm_map += t_map1 - t_draw1;
	f->prof_sm_copy += t_copy1 - t_map1;
	f->prof_sm_n++;
	return true;
}

// ---------------------------------------------------------------------------
// Phase 5a：段区间钩子（3.1.52）
// ---------------------------------------------------------------------------
//
// 与 3.1.49 的单段钩子的差别只有一件事：**认领的段从一段变成一段区间**。
// 磨皮 → 色调 → 细节 全部在显存里走完，才回读一次。
//
// 每个阶段的量化口径必须与 CPU 逐段对齐（这是 79.0 分不降的前提）：
//   · smooth.hlsl 的 Comp 已经把值 floor(+0.5)/255 写进 RGBA8；
//   · tone.hlsl 同样是 floor(+0.5)/255；
//   · detail.hlsl 的 Refill 用 **floor**（CPU 那边是 `(unsigned char)v` 截断）。
// 所以链上每一段交接的都是「和 CPU 一模一样的 8bit 图」，不会越算越偏。
//
// 细节段为什么能留在显存里：它的输入是「原图亮度」+「当前颜色」。原图在
// `in`（钩子准备段上传的那张），当前颜色是上一段的输出纹理 —— 两者都在显存，
// 所以中间不需要回读。CPU 版的 detailRefill 也是从 src 取亮度、往 dst 上加，
// 语义完全相同。

// 加载 detail.hlsl。参数自检的理由与 smooth 那边一致：DLL 与 data 目录里的
// 着色器版本必须配套，缺参数时 gs_effect_set_* 是**静默跳过**的 —— 采样器会
// 绑到空纹理上，表现为「锐化完全没效果」，而对拍还会因为「两边都没做」判通过。
static gs_effect_t *beauty_load_detail_effect(struct beauty_filter *f)
{
	if (f->detail_fx)
		return f->detail_fx;
	if (f->detail_tried)
		return NULL;
	f->detail_tried = true;

	char *path = NULL;
	const char *sdir = getenv("BEAUTY_SHADER_DIR");
	if (sdir && sdir[0]) {
		char *u8 = beauty_env_path_to_utf8(sdir);
		if (u8) {
			const size_t n = strlen(u8) + 16;
			path = (char *)bmalloc(n);
			if (path)
				snprintf(path, n, "%s/detail.hlsl", u8);
			bfree(u8);
		}
	} else {
		path = obs_module_file("shaders/detail.hlsl");
	}
	if (!path) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] detail.hlsl 未找到（data 目录缺 "
		     "shaders/detail.hlsl）：细节段回填留在 CPU");
		return NULL;
	}

	char *errors = NULL;
	f->detail_fx = gs_effect_create_from_file(path, &errors);
	if (!f->detail_fx) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] detail.hlsl 编译失败：%s",
		     errors ? errors : "(no detail)");
		bfree(errors);
	} else {
		static const char *kNeed[] = {"image", "image2", "image3",
					      "skinMask", "noMask"};
		static const char *kTech[] = {"LumTech", "HistTech", "BoxHTech",
					      "BoxVTech", "RefillTech"};
		bool ok = true;
		for (size_t i = 0; i < sizeof(kNeed) / sizeof(kNeed[0]); i++) {
			if (!gs_effect_get_param_by_name(f->detail_fx, kNeed[i])) {
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] detail.hlsl 缺少参数 `%s`："
				     "着色器与 DLL 版本不匹配，细节段回填留在 CPU。",
				     kNeed[i]);
				ok = false;
				break;
			}
		}
		for (size_t i = 0; ok && i < sizeof(kTech) / sizeof(kTech[0]); i++) {
			if (!gs_effect_get_technique(f->detail_fx, kTech[i])) {
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] detail.hlsl 缺少 technique "
				     "`%s`：细节段回填留在 CPU。",
				     kTech[i]);
				ok = false;
			}
		}
		if (!ok) {
			gs_effect_destroy(f->detail_fx);
			f->detail_fx = NULL;
		} else {
			blog(LOG_INFO, "[" PLUGIN_NAME "] detail.hlsl 已加载（%s）",
			     path);
		}
	}
	bfree(path);
	return f->detail_fx;
}

// 细节段的两张中间平面 + 亮度历史 + 色调输出。
//
// 平面格式：先试 GS_RGBA16F（0..255 的浮点值在 16F 下 ulp≈0.125 灰阶，
// 对 3/7 盒式足够），失败再退 GS_RGBA32F（磨皮那套已证可用，代价是 4K 下
// 每张 132 MB）。都失败就返回 false，细节段整段留在 CPU。
// 链共用的中间渲染目标（chain_rt）：色调的输出目标；色调未被认领时，细节
// 回填也拿它当输出（见 beauty_chain_detail 里 out_rt 的选择）。
//
// 为什么单独一个 ensure 而不是塞进 beauty_ensure_detail_rt：链钩子里的执行
// 顺序是 磨皮 → **色调** → 五官 → **细节** → 美形，而 chain_rt 原先只由
// beauty_ensure_detail_rt 创建（即细节段才建）→ 跑到色调时 `f->chain_rt`
// 还是 NULL，`beauty_chain_tone` 第一道判断就 return NULL。后果是三重的：
//   ① 色调永远认领不了，每帧回落 CPU（720p 实测 1.03 ms / 4K 更贵）；
//   ② tone_ok=false 把细节段一起拦下（再回落 1.00 ms）；
//   ③ claim 覆盖不到全部 flags → chain_direct 为假 → 每帧多一次全帧回读
//      （720p 实测 map 1.92 + copy 0.34 ms）。
// 三项合计约 4.3 ms/帧 @720p。此前一直没暴露，是因为历次 A/B 场景的
// 清晰/美白/红润都是 0（TONE 不进 flags），README 的 T2 也注明了这点。
static bool beauty_ensure_chain_rt(struct beauty_filter *f, uint32_t cx,
				   uint32_t cy)
{
	if (f->chain_rt && f->chain_w == cx && f->chain_h == cy)
		return true;

	if (f->chain_rt) {
		gs_texrender_destroy(f->chain_rt);
		f->chain_rt = NULL;
	}
	f->chain_w = f->chain_h = 0;

	f->chain_rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!f->chain_rt) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 链中间渲染目标创建失败 %ux%u", cx, cy);
		return false;
	}
	f->chain_w = cx;
	f->chain_h = cy;
	return true;
}

static bool beauty_ensure_detail_rt(struct beauty_filter *f, uint32_t cx,
				    uint32_t cy)
{
	if (f->det_w == cx && f->det_h == cy && f->det_a && f->det_b &&
	    f->det_hist)
		return true;

	// 注意：chain_rt 不在这里建、也不在这里销毁 —— 它由 beauty_ensure_chain_rt
	// 独占管理（链钩子入口就建好，色调段等着用）。早年它在这张表里，结果色调
	// 因拿不到它而恒定认领失败，见 beauty_ensure_chain_rt 的说明。
	gs_texrender_t **slots[] = {&f->det_a, &f->det_b, &f->det_hist};
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		if (*slots[i]) {
			gs_texrender_destroy(*slots[i]);
			*slots[i] = NULL;
		}
	}
	f->det_w = f->det_h = 0;
	f->det_hist_ok = false;

	f->det_a = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->det_b = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	if (!f->det_a || !f->det_b) {
		if (f->det_a) {
			gs_texrender_destroy(f->det_a);
			f->det_a = NULL;
		}
		if (f->det_b) {
			gs_texrender_destroy(f->det_b);
			f->det_b = NULL;
		}
		f->det_a = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
		f->det_b = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] GS_RGBA16F 不能作渲染目标，细节平面改用 "
		     "GS_RGBA32F");
	}
	// 历史用 RGBA8 存「灰阶 / 255」：只需要 1 个通道，但 RGBA 是最保险的
	// 可渲染格式（R8 单通道渲染目标在部分后端上不可用，而这里不值得赌）。
	f->det_hist = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!f->det_a || !f->det_b || !f->det_hist) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 细节段渲染目标创建失败 %ux%u", cx, cy);
		return false;
	}
	f->det_w = cx;
	f->det_h = cy;
	return true;
}

// 色调 pass：in → chain_rt。掩码直接用磨皮准备段已经上传好的 skin_tex，
// 不再单独传一份（诊断路径 beauty_gpu_tone 用的是 mask_tex，两者互不影响）。
static gs_texture_t *beauty_chain_tone(struct beauty_filter *f,
				       gs_texture_t *in, uint32_t cx, uint32_t cy,
				       float clarity, float whiten, float rosy)
{
	gs_effect_t *fx = beauty_load_tone_effect(f);
	if (!fx || !f->chain_rt || !beauty_mask_skin(f))
		return NULL;

	if (!smooth_rt_begin(f->chain_rt, cx, cy))
		return NULL;

	smooth_bind_tex(fx, "image", in);
	smooth_bind_tex(fx, "mask", beauty_mask_skin(f));
	smooth_bind_float(fx, "clarity", clarity);
	smooth_bind_float(fx, "whiten", whiten);
	smooth_bind_float(fx, "rosy", rosy);
	smooth_draw(fx, "ToneTech", in, cx, cy);
	smooth_rt_end(f->chain_rt);

	// 不要恢复投影（见 smooth_rt_begin 上方的说明）
	return gs_texrender_get_texture(f->chain_rt);
}

// 细节回填：src（亮度源）+ color（当前颜色）→ 回填后的颜色纹理。
//
// 四趟 + 一次历史更新，见 detail.hlsl 文件头的编排表。
// 输出目标是「不是 color 的那一张」：color 可能是 sm_out（色调没被认领时），
// 采样和渲染不能是同一张纹理，所以按需切到 chain_rt。
static gs_texture_t *beauty_chain_detail(struct beauty_filter *f,
					 gs_texture_t *src_tex,
					 gs_texture_t *color_tex,
					 uint32_t cx, uint32_t cy,
					 const BeautyDetailDiag &d,
					 bool no_mask, bool hist_reset)
{
	gs_effect_t *fx = beauty_load_detail_effect(f);
	if (!fx || !beauty_ensure_detail_rt(f, cx, cy))
		return NULL;

	// emaMode：0 不做 EMA / 1 只用当前帧重置历史 / 2 与历史混合。
	// 判据与 CPU 的 copyOnly 完全相同（reset || beta>=0.999），这样「运动帧」
	// 在两边都是「历史=当前帧」而不是「跟着旧画面拖影」。
	int ema_mode = 0;
	if (d.emaOn)
		ema_mode = (hist_reset || d.beta >= 0.999f) ? 1 : 2;
	int b256 = (int)(d.beta * 256.0f + 0.5f);

	// ⚠ **每一趟 draw 之前都必须把全部参数重绑一遍** —— 这是 3.1.52 / 3.1.53
	// 两次都栽在同一件事上的根源：
	//
	//   `gs_technique_begin_pass()` 在提交参数之后会把该参数已设的值消耗掉，
	//   于是「绑一次、连画五趟」只有第一趟是有效的，后面每一趟都算「未设置」，
	//   各报一条 `Not all shader parameters were set`。
	//
	//   实测证据（同一台机器、同一集合）：
	//     · 3.1.52 把 dFine 那 11 个绑在第 5 趟之前 → 前四趟报 → 3517 条
	//     · 3.1.53 把它们挪到入口绑一次    → 第一趟 Lum 吃掉 → 后三趟报 → 1539 条
	//   3517 : 1539 ≈ 4 : 3，正好对上「几趟没绑」，这就把「绑得晚」与
	//   「绑得少」排除掉了 —— 真正的机制是**参数是一次性的**。
	//
	//   这也解释了为什么 `image` 从来没出过问题：它本来就是每趟重绑的。
	//
	// 代价是每帧多 4 × 19 次参数设置（约几 µs），换来日志干净 —— 而日志
	// 干净是后面每一次真机排查的前提，3.1.52 那 77 KB 噪声已经把性能行埋过一次了。
	auto bind_all = [&](gs_texture_t *img, gs_texture_t *img2,
			    gs_texture_t *img3) {
		smooth_bind_vec2(fx, "texel", 1.0f / (float)cx, 1.0f / (float)cy);
		smooth_bind_int(fx, "emaMode", ema_mode);
		smooth_bind_int(fx, "emaB256", b256);
		smooth_bind_int(fx, "emaIB256", 256 - b256);
		smooth_bind_float(fx, "dFine", d.fine);
		smooth_bind_float(fx, "dMid", d.mid);
		smooth_bind_float(fx, "dGFine", d.globalFine);
		smooth_bind_float(fx, "dGMid", d.globalMid);
		smooth_bind_float(fx, "dThrFine", d.thrFine);
		smooth_bind_float(fx, "dThrMid", d.thrMid);
		smooth_bind_float(fx, "dThrFineG", d.thrFineGlobal);
		smooth_bind_float(fx, "dThrMidG", d.thrMidGlobal);
		smooth_bind_float(fx, "dKneeFine", d.kneeFine);
		smooth_bind_float(fx, "dKneeMid", d.kneeMid);
		// 无掩码时 m 必须按 0 算（与磨皮相反），见 detail.hlsl 里 noMask 的注释。
		smooth_bind_int(fx, "noMask", no_mask ? 1 : 0);
		smooth_bind_tex(fx, "image", img);
		smooth_bind_tex(fx, "image2", img2);
		smooth_bind_tex(fx, "image3", img3);
		smooth_bind_tex(fx, "skinMask",
				beauty_mask_skin(f) ? beauty_mask_skin(f) : img);
	};

	// 1) Lum：原图 → 亮度平面（含 EMA）
	// 首帧 det_hist 还没 begin 过，它的纹理是 NULL；emaMode 此时必为 1
	// （hist_reset 成立），PS_Lum 不采样 image2，但仍然要给它一张真纹理，
	// 否则这个参数就算「未设置」。
	gs_texture_t *hist = f->det_hist ? gs_texrender_get_texture(f->det_hist)
					 : NULL;
	bind_all(src_tex, hist ? hist : src_tex, src_tex);
	if (!smooth_rt_begin(f->det_a, cx, cy))
		return NULL;
	smooth_draw(fx, "LumTech", src_tex, cx, cy);
	smooth_rt_end(f->det_a);
	gs_texture_t *plane = gs_texrender_get_texture(f->det_a);
	if (!plane)
		return NULL;

	// 2) Hist：把新历史写回历史纹理（下一帧 Lum 读它）
	if (ema_mode != 0) {
		// ⚠ image2 必须换成别的纹理，不能留成历史纹理本身：这一趟要渲染
		// 进历史纹理，而上一趟它正是被当 SRV 采样的那张。同一张资源在同一
		// draw 里既当 SRV 又当 RTV 是 D3D11 的未定义行为（debug layer 报
		// "resource still bound"），实测可能只是「没写进去」，表现为 EMA
		// 静默失效 —— 最坏的那种。解绑到掩码而不是 NULL：NULL 在部分后端
		// 上会变成「未绑定」并让 draw 被跳过，等于同样静默。
		bind_all(plane, beauty_mask_skin(f) ? beauty_mask_skin(f) : plane, plane);
		if (!smooth_rt_begin(f->det_hist, cx, cy))
			return NULL;
		smooth_draw(fx, "HistTech", plane, cx, cy);
		smooth_rt_end(f->det_hist);
	}

	// 3) BoxH：亮度平面 → det_b（.r box3h, .g box7h, .b 亮度透传）
	bind_all(plane, plane, plane);
	if (!smooth_rt_begin(f->det_b, cx, cy))
		return NULL;
	smooth_draw(fx, "BoxHTech", plane, cx, cy);
	smooth_rt_end(f->det_b);
	gs_texture_t *bh = gs_texrender_get_texture(f->det_b);
	if (!bh)
		return NULL;

	// 4) BoxV：det_b → det_a（此刻 det_a 的亮度已经由 .b 带进 det_b，可以覆写）
	bind_all(bh, bh, bh);
	if (!smooth_rt_begin(f->det_a, cx, cy))
		return NULL;
	smooth_draw(fx, "BoxVTech", bh, cx, cy);
	smooth_rt_end(f->det_a);
	gs_texture_t *plane2 = gs_texrender_get_texture(f->det_a);
	if (!plane2)
		return NULL;

	// 5) Refill：颜色 + 平面 + 掩码 → 输出（目标不能等于 color_tex）
	//
	// ⚠ 这里必须比较 **texrender 对象**，绝不能比较 gs_texrender_get_texture()。
	// `chain_rt` 在第一次 begin 之前 texture 字段是 NULL，用它当「这张能不
	// 能用」的判据会把一张完好的目标误判成不可用，然后 return NULL 把整段
	// 细节交回 CPU。3.1.52 真机就是这么栽的：色调没开 → chain_rt 从头到尾
	// 没 begin → 细节段恒定失败，CPU 侧 1.16 ms 一分没省，而日志里只表现
	// 为「磨皮接管了、细节没有」＋每帧四条参数警告。
	//
	// 两张目标各自对应一条来路：
	//   · 色调被认领 → color_tex 就是 chain_rt 的纹理 → 写 sm_out（里面是
	//     磨皮的原始输出，已被色调读走合成过，覆写安全）；
	//   · 色调没开 → color_tex 就是 sm_out 的纹理 → 写 chain_rt（本帧首
	//     begin，纹理由 begin 现建）。
	gs_texrender_t *out_rt = f->sm_out;
	if (gs_texrender_get_texture(out_rt) == color_tex)
		out_rt = f->chain_rt;
	if (!out_rt)
		return NULL;

	// 这一趟 image3 换成真平面（box3/box7/亮度），image2 换成原图（PS_Refill
	// 目前没用到它，但参数必须是一次性的、每趟都要给值）。
	bind_all(color_tex, src_tex, plane2);

	if (!smooth_rt_begin(out_rt, cx, cy))
		return NULL;
	smooth_draw(fx, "RefillTech", color_tex, cx, cy);
	smooth_rt_end(out_rt);
	// 不要恢复投影（见 smooth_rt_begin 上方的说明）

	gs_texture_t *out_tex = gs_texrender_get_texture(out_rt);
	if (!out_tex) {
		// begin 之后还拿不到纹理，说明这张 rt 根本没被渲染（格式不支持等）。
		// 每实例只报一次，免得又变成刷屏。
		if (!f->detail_err_reported) {
			f->detail_err_reported = true;
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 细节回填输出纹理不可用：本实例细节段"
			     "留在 CPU（不影响画面，只是没提速）");
		}
		return NULL;
	}
	return out_tex;
}

// ===========================================================================
// 5c：GPU 美形（3.1.62）
// ===========================================================================
//
// 与前面几段的关系：美形在整条链的**最末端**（CPU 侧顺序 磨皮→色调→五官→
// 细节→美形），所以它拿到的 `src` 就是前面所有段的最终输出纹理，做完直接
// 成为链的最终结果 —— 这正是 5b-1「链尾不回读」需要的形态：只要美形也被
// 认领，本帧 flags 就全满，可以跳过 map+copy。
//
// 为什么这一段值得做（3.1.61 之前一直没做）：原计划把它排在 5b-2 后面，
// 理由是「CPU 720p 只花 0.3 ms」。这个数字只对 720p 成立 —— 4K 下位移场
// 要重采样约 8 倍的像素，实测同比例推算 ≈18 ms。更要紧的是：只要美形滑块
// 非零，claim 就到不了 WARP，**5b-1 整段失效**，4K 从 13 ms 掉回 30~35 ms。
// 也就是说 5c 不是「再省一点」，而是「美形用户能不能用上 5b-1 的前提」。

// 加载 warp.hlsl。参数自检理由与 detail/smooth 一致：缺参数时 gs_effect_set_*
// 是**静默跳过**的，采样器会绑到空纹理 —— 表现为「滑块没反应」，而对拍还会
// 因为「两边都没做」判通过。
static gs_effect_t *beauty_load_warp_effect(struct beauty_filter *f)
{
	if (f->warp_fx)
		return f->warp_fx;
	if (f->warp_tried)
		return NULL;
	f->warp_tried = true;

	char *path = NULL;
	const char *sdir = getenv("BEAUTY_SHADER_DIR");
	if (sdir && sdir[0]) {
		char *u8 = beauty_env_path_to_utf8(sdir);
		if (u8) {
			const size_t n = strlen(u8) + 16;
			path = (char *)bmalloc(n);
			if (path)
				snprintf(path, n, "%s/warp.hlsl", u8);
			bfree(u8);
		}
	} else {
		path = obs_module_file("shaders/warp.hlsl");
	}
	if (!path) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] warp.hlsl 未找到（data 目录缺 "
		     "shaders/warp.hlsl）：美形段留在 CPU，5b-1 在美形开启时失效");
		return NULL;
	}

	char *errors = NULL;
	f->warp_fx = gs_effect_create_from_file(path, &errors);
	if (!f->warp_fx) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] warp.hlsl 编译失败：%s",
		     errors ? errors : "(no warp)");
		bfree(errors);
	} else {
		static const char *kNeed[] = {"image", "texSize", "fA", "fB",
					      "fC", "fD", "roi", "wFlags"};
		bool ok = true;
		for (size_t i = 0; i < sizeof(kNeed) / sizeof(kNeed[0]); i++) {
			if (!gs_effect_get_param_by_name(f->warp_fx, kNeed[i])) {
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] warp.hlsl 缺少参数 `%s`："
				     "着色器与 DLL 版本不匹配，美形段留在 CPU。",
				     kNeed[i]);
				ok = false;
				break;
			}
		}
		if (ok && !gs_effect_get_technique(f->warp_fx, "WarpTech")) {
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] warp.hlsl 缺少 technique "
			     "`WarpTech`：美形段留在 CPU。");
			ok = false;
		}
		if (!ok) {
			gs_effect_destroy(f->warp_fx);
			f->warp_fx = NULL;
		} else {
			blog(LOG_INFO, "[" PLUGIN_NAME "] warp.hlsl 已加载（%s）",
			     path);
		}
	}
	bfree(path);
	return f->warp_fx;
}

// 逐脸 ping-pong 的两张目标。
//
// 格式必须是 **GS_RGBA（8bit）**，不能沿用细节段的 16F/32F：
//   · CPU 侧就是 8bit 落笔（`(unsigned char)(v + 0.5)`），中间放浮点等于
//     引入 CPU 根本没有的量化误差，逐像素对拍永远对不上；
//   · 更关键的是 ROI 外的像素要**原样透传**：8bit → 8bit 往返是恒等的，
//     16F 往返虽然也保精度，但一旦某处有量化就会把整帧背景都改掉。
static bool beauty_ensure_warp_rt(struct beauty_filter *f, uint32_t cx,
				  uint32_t cy)
{
	if (f->warp_w == cx && f->warp_h == cy && f->warp_a && f->warp_b)
		return true;

	gs_texrender_t **slots[] = {&f->warp_a, &f->warp_b};
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		if (*slots[i]) {
			gs_texrender_destroy(*slots[i]);
			*slots[i] = NULL;
		}
	}
	f->warp_w = f->warp_h = 0;

	f->warp_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->warp_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!f->warp_a || !f->warp_b) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 美形段渲染目标创建失败 %ux%u", cx, cy);
		return false;
	}
	f->warp_w = cx;
	f->warp_h = cy;
	return true;
}

// 逐脸跑一趟。返回链的新末尾纹理；失败返回 NULL（调用方据此不认领 WARP，
// 整段交回 CPU —— 引擎在美形段前有 `claimed & BEAUTY_SEG_WARP` 守卫）。
//
// 为什么一张脸一趟而不是一趟画完：CPU 是**串行**的（face[0] 就地改完，
// face[1] 读到的是改过的数据）。单趟要复现这个语义只能假设各脸 ROI 完全
// 不重叠，而 CPU 侧的原话只是「不会**大面积**交叠」。串行 + ping-pong 把这个
// 假设彻底去掉，代价是多几趟（现实中 1~2 张脸）。
//
// ra/rb 由调用方给：生产链用 f->warp_a / f->warp_b，对拍必须用**自己的一对**
// scratch —— 生产链在 5b-1 直绘模式下 chain_out_tex 就是 warp_a 的纹理，
// 对拍若往同一张里画，等于把即将上屏的画面覆盖掉（而且用的是原图当输入，
// 与生产链的输入根本不是同一张）。
static gs_texture_t *beauty_chain_warp_into(struct beauty_filter *f,
					    gs_texture_t *src, uint32_t cx,
					    uint32_t cy,
					    const BeautyWarpDiag &wd,
					    gs_texrender_t *ra,
					    gs_texrender_t *rb)
{
	if (!src || wd.count <= 0 || !ra || !rb)
		return NULL;

	gs_effect_t *fx = beauty_load_warp_effect(f);
	if (!fx)
		return NULL;

	int n = wd.count;
	if (n > BEAUTY_WARP_MAX_FACES)
		n = BEAUTY_WARP_MAX_FACES;

	gs_texture_t *cur = src;
	for (int i = 0; i < n; i++) {
		const BeautyWarpFace &g = wd.faces[i];
		gs_texrender_t *rt = (i & 1) ? rb : ra;

		// 目标不能是源（D3D11 同一资源既当 SRV 又当 RTV 是未定义行为）。
		// 正常情况不可能撞上：src 来自 sm_out / chain_rt，ping-pong 只在
		// warp_a / warp_b 之间换。留着这条判断是为了「撞上时宁可交回 CPU，
		// 也不要静默写出错画面」。
		if (gs_texrender_get_texture(rt) == cur)
			return NULL;

		struct vec4 va, vb, vc, vd, vr;
		vec4_set(&va, g.cx, g.cy, g.halfW, g.eyeR);
		vec4_set(&vb, g.eyeRx, g.eyeRy, g.eyeLx, g.eyeLy);
		vec4_set(&vc, g.eyeAmp, g.thinYc, g.thinSig, g.thinK);
		vec4_set(&vd, g.chinYm, g.chinSpan, g.chinK, 0.0f);
		vec4_set(&vr, (float)g.roi[0], (float)g.roi[1],
			 (float)g.roi[2], (float)g.roi[3]);

		gs_eparam_t *p;
		if ((p = gs_effect_get_param_by_name(fx, "fA")))
			gs_effect_set_vec4(p, &va);
		if ((p = gs_effect_get_param_by_name(fx, "fB")))
			gs_effect_set_vec4(p, &vb);
		if ((p = gs_effect_get_param_by_name(fx, "fC")))
			gs_effect_set_vec4(p, &vc);
		if ((p = gs_effect_get_param_by_name(fx, "fD")))
			gs_effect_set_vec4(p, &vd);
		if ((p = gs_effect_get_param_by_name(fx, "roi")))
			gs_effect_set_vec4(p, &vr);
		smooth_bind_vec2(fx, "texSize", (float)cx, (float)cy);
		smooth_bind_int(fx, "wFlags", (int)g.flags);
		smooth_bind_tex(fx, "image", cur);

		if (!smooth_rt_begin(rt, cx, cy))
			return NULL;
		smooth_draw(fx, "WarpTech", cur, cx, cy);
		smooth_rt_end(rt);

		gs_texture_t *t = gs_texrender_get_texture(rt);
		if (!t)
			return NULL;
		cur = t;
	}
	return cur;
}

// 生产路径的入口：确保 ping-pong 目标存在后转调上面那个。
static gs_texture_t *beauty_chain_warp(struct beauty_filter *f,
				       gs_texture_t *src, uint32_t cx,
				       uint32_t cy, const BeautyWarpDiag &wd)
{
	if (!src || wd.count <= 0)
		return NULL;
	if (!beauty_ensure_warp_rt(f, cx, cy))
		return NULL;
	return beauty_chain_warp_into(f, src, cx, cy, wd, f->warp_a, f->warp_b);
}

// ===========================================================================
// 5b-2 切片 2：GPU 掩码对拍（3.1.64，纯诊断 —— 生产掩码仍然是 CPU 的）
// ===========================================================================
//
// 这一步只回答一个问题：**mask.hlsl 算出来的掩码，和 CPU 的 mask_/protect_
// 是不是逐像素相同（允许 1 灰阶）？** 答案出来之前不动生产链。
//
// 为什么非要先问这个：掩码是磨皮与细节的**输入**，它差 1 灰阶，下游全歪；
// 而 mask.hlsl 里有 6 个肤色系数、4 个亮度门限、羽化公式、整数右移 ——
// 全是「抄错照样编译、画面只是略变」的量（T1 的 `.b=0` 就这样藏了一版）。
//
// 几何为什么由引擎递：CPU 的边界是整数截断（(int)(cx-hw)、x1-x、featherRamp
// (y-y0+1, fe)）。宿主按浮点重算一遍必然与 MSVC 分叉，所以引擎用
// BeautyMaskDiag 把算好的整数原样交过来（见 beauty_core.h 的注释）。

// 加载 mask.hlsl。参数自检的理由与 warp/detail 一致：缺参数时 gs_effect_set_*
// 是**静默跳过**的，采样器会绑到空纹理 —— 表现是掩码全 0 或全 255，
// 而对拍还会因为「两边都不对」显得很像通过。
static gs_effect_t *beauty_load_mask_effect(struct beauty_filter *f)
{
	if (f->mask_fx)
		return f->mask_fx;
	if (f->mask_tried)
		return NULL;
	f->mask_tried = true;

	char *path = NULL;
	const char *sdir = getenv("BEAUTY_SHADER_DIR");
	if (sdir && sdir[0]) {
		char *u8 = beauty_env_path_to_utf8(sdir);
		if (u8) {
			const size_t n = strlen(u8) + 16;
			path = (char *)bmalloc(n);
			if (path)
				snprintf(path, n, "%s/mask.hlsl", u8);
			bfree(u8);
		}
	} else {
		path = obs_module_file("shaders/mask.hlsl");
	}
	if (!path) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] mask.hlsl 未找到（data 目录缺 "
		     "shaders/mask.hlsl）：GPU 掩码对拍跳过（生产不受影响）");
		return NULL;
	}

	char *errors = NULL;
	f->mask_fx = gs_effect_create_from_file(path, &errors);
	if (!f->mask_fx) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] mask.hlsl 编译失败：%s",
		     errors ? errors : "(no mask)");
		bfree(errors);
	} else {
		static const char *kNeed[] = {
			"image", "size", "texel", "faceRect", "faceGeom",
			"faceFe", "neckRect", "neckEll", "neckFe", "maskShape",
			"skinEllipse", "eye0", "eye1"};
		bool ok = true;
		for (size_t i = 0; i < sizeof(kNeed) / sizeof(kNeed[0]); i++) {
			if (!gs_effect_get_param_by_name(f->mask_fx, kNeed[i])) {
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] mask.hlsl 缺少参数 `%s`："
				     "着色器与 DLL 版本不匹配，GPU 掩码对拍跳过。",
				     kNeed[i]);
				ok = false;
				break;
			}
		}
		if (ok && !gs_effect_get_technique(f->mask_fx, "MaskSkinTech")) {
			blog(LOG_WARNING, "[" PLUGIN_NAME "] mask.hlsl 缺少 "
					  "`MaskSkinTech`：跳过。");
			ok = false;
		}
		if (ok && !gs_effect_get_technique(f->mask_fx, "MaskEyeTech")) {
			blog(LOG_WARNING, "[" PLUGIN_NAME "] mask.hlsl 缺少 "
					  "`MaskEyeTech`：跳过。");
			ok = false;
		}
		if (!ok) {
			gs_effect_destroy(f->mask_fx);
			f->mask_fx = NULL;
		} else {
			blog(LOG_INFO, "[" PLUGIN_NAME "] mask.hlsl 已加载（%s）",
			     path);
		}
	}
	bfree(path);
	return f->mask_fx;
}

// 对拍专用资源：两张 RGBA scratch + 一张 RGBA 暂存面。
//
// 为什么**不用** GS_R8：切片 3 才需要论证 R8 当渲染目标在各后端都成立；
// 这一步只验算法，用 RGBA 最稳（值写进 R 通道，其余通道写常数）。
// 也**不复用** f->stage：那是生产下载用的，对拍不该抢它。
static bool beauty_ensure_mask_rt(struct beauty_filter *f, uint32_t cx,
				  uint32_t cy)
{
	if (f->maskcmp_w == cx && f->maskcmp_h == cy && f->mask_rt_a &&
	    f->mask_rt_b && f->mask_stage)
		return true;

	if (f->mask_rt_a) {
		gs_texrender_destroy(f->mask_rt_a);
		f->mask_rt_a = NULL;
	}
	if (f->mask_rt_b) {
		gs_texrender_destroy(f->mask_rt_b);
		f->mask_rt_b = NULL;
	}
	if (f->mask_stage) {
		gs_stagesurface_destroy(f->mask_stage);
		f->mask_stage = NULL;
	}
	f->maskcmp_w = f->maskcmp_h = 0;

	f->mask_rt_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->mask_rt_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->mask_stage = gs_stagesurface_create(cx, cy, GS_RGBA);
	if (!f->mask_rt_a || !f->mask_rt_b || !f->mask_stage) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 掩码对拍渲染目标创建失败 %ux%u ",
		     cx, cy);
		return false;
	}
	f->maskcmp_w = cx;
	f->maskcmp_h = cy;
	return true;
}

// 把一张脸的掩码画进 scratch：skin → rt_skin，eye → rt_eye。
// 切片 1 只支持单脸（多脸时上层直接跳过对拍，不做 blend 合并）。
static bool beauty_gpu_mask_into(struct beauty_filter *f, gs_texture_t *src,
				 uint32_t cx, uint32_t cy,
				 const BeautyMaskDiag &md)
{
	if (!src || md.count != 1)
		return false;
	gs_effect_t *fx = beauty_load_mask_effect(f);
	if (!fx || !beauty_ensure_mask_rt(f, cx, cy))
		return false;

	const BeautyMaskFace &g = md.faces[0];

	struct vec4 vr, vg, vn, vne, vell, ve0, ve1, vezero;
	vec4_zero(&vezero);
	vec4_set(&vr, (float)g.x0, (float)g.y0, (float)g.x1, (float)g.y1);
	vec4_set(&vg, g.cx, g.cy, g.a, g.b);
	vec4_set(&vn, (float)g.nx0, (float)g.nyLo, (float)g.nx1,
		 (float)g.nyHi);
	vec4_set(&vne, g.neckA, g.neckB, g.neckCy, 0.0f);
	vec4_set(&vell, md.cb, md.cr, md.ru, md.rv);
	vec4_set(&ve0, g.eye0x, g.eye0y, g.eyeW, g.eyeH);
	vec4_set(&ve1, g.eye1x, g.eye1y, g.eyeW, g.eyeH);

	// **每趟 draw 之前都要重绑全部参数**（E2，3.1.52 踩过，3.1.64 又踩一次）：
	// `gs_technique_begin_pass()` 会把参数值消耗掉。绑一次连画两趟时，第二趟会报
	// `device_draw: Not all shader parameters were set` 并**静默跳过** ——
	// 表现是「第二张纹理全 0」。2026-11-01 07:07 的 M1 眼睛掩码全零正是这个。
	auto bind_all = [&]() {
		gs_eparam_t *p;
		if ((p = gs_effect_get_param_by_name(fx, "faceRect")))
			gs_effect_set_vec4(p, &vr);
		if ((p = gs_effect_get_param_by_name(fx, "faceGeom")))
			gs_effect_set_vec4(p, &vg);
		if ((p = gs_effect_get_param_by_name(fx, "neckRect")))
			gs_effect_set_vec4(p, &vn);
		if ((p = gs_effect_get_param_by_name(fx, "neckEll")))
			gs_effect_set_vec4(p, &vne);
		if ((p = gs_effect_get_param_by_name(fx, "skinEllipse")))
			gs_effect_set_vec4(p, &vell);
		// hasEyes == 0 时把两个椭圆设成退化尺寸（rx=0）：shader 里 <0.5 直接
		// 返回 0，与 CPU「不画保护」同义。
		if ((p = gs_effect_get_param_by_name(fx, "eye0")))
			gs_effect_set_vec4(p, g.hasEyes ? &ve0 : &vezero);
		if ((p = gs_effect_get_param_by_name(fx, "eye1")))
			gs_effect_set_vec4(p, g.hasEyes ? &ve1 : &vezero);
		smooth_bind_vec2(fx, "size", (float)cx, (float)cy);
		smooth_bind_vec2(fx, "texel", 1.0f / (float)cx, 1.0f / (float)cy);
		smooth_bind_float(fx, "faceFe", (float)g.fe);
		smooth_bind_float(fx, "neckFe", (float)g.feNeck);
		smooth_bind_int(fx, "maskShape", md.shape);
		smooth_bind_tex(fx, "image", src);
	};

	if (!smooth_rt_begin(f->mask_rt_a, cx, cy))
		return false;
	bind_all();
	smooth_draw(fx, "MaskSkinTech", src, cx, cy);
	smooth_rt_end(f->mask_rt_a);

	if (!smooth_rt_begin(f->mask_rt_b, cx, cy))
		return false;
	bind_all();
	smooth_draw(fx, "MaskEyeTech", src, cx, cy);
	smooth_rt_end(f->mask_rt_b);
	return true;
}

// 把一张 RGBA 读回并与 CPU 的 8-bit 缓冲逐像素比。
// 直接边读边比：不在 CPU 上再占一份 w*h*4（4K 是 33 MB）。
static void beauty_mask_diff_stage(struct beauty_filter *f, gs_texture_t *tex,
				   const unsigned char *cpu, uint32_t cx,
				   uint32_t cy, int *maxdiff_out,
				   double *avg_out, uint64_t *diffpx_out,
				   int bbox[4], uint8_t *dump_out)
{
	*maxdiff_out = 0;
	*avg_out = 0.0;
	*diffpx_out = 0;
	if (bbox) {
		bbox[0] = (int)cx;
		bbox[1] = (int)cy;
		bbox[2] = -1;
		bbox[3] = -1;
	}
	if (!tex || !f->mask_stage)
		return;

	gs_stage_texture(f->mask_stage, tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->mask_stage, &ptr, &linesize) || !ptr)
		return;

	uint64_t sum = 0, diffpx = 0;
	int maxd = 0;
	int bx0 = (int)cx, by0 = (int)cy, bx1 = -1, by1 = -1;
	for (uint32_t y = 0; y < cy; y++) {
		const uint8_t *row = ptr + (size_t)y * linesize;
		uint8_t *drow = dump_out ? (dump_out + (size_t)y * cx) : NULL;
		for (uint32_t x = 0; x < cx; x++) {
			const int gv = (int)row[(size_t)x * 4];      // R 通道
			const int cv = cpu ? (int)cpu[(size_t)y * cx + x] : 0;
			if (drow)
				drow[x] = (uint8_t)gv;
			int d = gv - cv;
			if (d < 0)
				d = -d;
			if (d > maxd)
				maxd = d;
			if (d) {
				sum += (uint64_t)d;
				diffpx++;
				if ((int)x < bx0) bx0 = (int)x;
				if ((int)x > bx1) bx1 = (int)x;
				if ((int)y < by0) by0 = (int)y;
				if ((int)y > by1) by1 = (int)y;
			}
		}
	}
	gs_stagesurface_unmap(f->mask_stage);

	*maxdiff_out = maxd;
	*diffpx_out = diffpx;
	*avg_out = diffpx ? (double)sum / (double)diffpx : 0.0;
	if (bbox && bx1 >= 0) {
		bbox[0] = bx0;
		bbox[1] = by0;
		bbox[2] = bx1;
		bbox[3] = by1;
	}
}

// 把一块 8-bit 掩码写成裸文件（M1 失败时离线定位用）。
static void beauty_mask_dump(const char *dir, const char *name,
			     const uint8_t *buf, uint32_t cx, uint32_t cy)
{
	if (!dir || !dir[0] || !buf)
		return;
	char path[520];
	snprintf(path, sizeof(path), "%s/%s_%ux%u.raw", dir, name, cx, cy);
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return;
	fwrite(buf, 1, (size_t)cx * cy, fp);
	fclose(fp);
	blog(LOG_INFO, "[" PLUGIN_NAME "] 掩码 dump 已写出：%s", path);
}

// 把一张 RGBA 纹理读回成 CPU 字节（M1 定位「输入像素是否同一份」用）。
// 尺寸必须与 stage 面一致，否则跳过 —— 宁可少 dump，也不要踩 gs_stage_texture 的尺寸假设。
static bool beauty_readback_rgba(struct beauty_filter *f, gs_texture_t *tex,
				 uint8_t *dst, uint32_t cx, uint32_t cy)
{
	if (!tex || !f->mask_stage || !dst)
		return false;
	if (gs_texture_get_width(tex) != cx || gs_texture_get_height(tex) != cy)
		return false;
	gs_stage_texture(f->mask_stage, tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->mask_stage, &ptr, &linesize) || !ptr)
		return false;
	const size_t row = (size_t)cx * 4;
	for (uint32_t y = 0; y < cy; y++)
		memcpy(dst + (size_t)y * row, ptr + (size_t)y * linesize, row);
	gs_stagesurface_unmap(f->mask_stage);
	return true;
}

// 把一张掩码纹理（mask.hlsl 的输出，权重写在 R 通道）读回成 1 字节/像素。
// 2.8.3（P0-7）：geomOnly 帧没有 CPU 掩码，五官对拍要拿 **GPU 刚画好的那份**
// 当 CPU 基准的掩码 —— 只有这样才是「同一份掩码 + 两种算法」。采样口径与之
// 一致：region.hlsl 的 skinMask 是 Point 采样、坐标在像素中心，所以读回来的
// R 通道就是 shader 眼里的那个值（UNORM8 → /255，与 CPU 的 mrow[x]/255 同）。
// 复用掩码对拍的 stage：只在诊断路径调用，不与生产抢 f->stage。
static bool beauty_mask_readback8(struct beauty_filter *f, gs_texture_t *tex,
				  uint8_t *dst, uint32_t cx, uint32_t cy)
{
	if (!tex || !f->mask_stage || !dst)
		return false;
	if (gs_texture_get_width(tex) != cx || gs_texture_get_height(tex) != cy)
		return false;
	gs_stage_texture(f->mask_stage, tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->mask_stage, &ptr, &linesize) || !ptr)
		return false;
	for (uint32_t y = 0; y < cy; y++) {
		const uint8_t *row = ptr + (size_t)y * linesize;
		uint8_t *d = dst + (size_t)y * cx;
		for (uint32_t x = 0; x < cx; x++)
			d[x] = row[(size_t)x * 4];
	}
	gs_stagesurface_unmap(f->mask_stage);
	return true;
}

// M1 一次性对拍：GPU 掩码 vs 引擎当帧的 mask/protect。
static void beauty_gpu_mask_selfcheck(struct beauty_filter *f,
				      gs_texture_t *in_tex,
				      const BeautyChainJob &job)
{
	static const int kMaskWaitMax = 600;
	const BeautyMaskDiag &md = job.maskGeom;

	if (!in_tex || !job.mask || job.w <= 0 || job.h <= 0)
		return;
	// 没脸：没有可比对象（CPU 那边也没有掩码）。
	if (md.count <= 0) {
		if (f->mask_wait < kMaskWaitMax)
			f->mask_wait++;
		return;
	}
	// 切片 1 只支持单脸；多脸要 blend 合并，尚未实现 —— 明确说出来，
	// 否则日志里「一直没有对拍结果」会被误读成「对拍没生效」。
	if (md.count > 1) {
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] GPU 掩码对拍跳过：本帧 %d 张脸，"
		     "切片 1 只支持单脸（等单人画面会自动重试）",
		     md.count);
		if (f->mask_wait < kMaskWaitMax)
			f->mask_wait++;
		// 多脸时每帧都会说一句没有意义，报过就收手。
		if (f->mask_wait > 30)
			f->gpu_mask_reported = true;
		return;
	}
	// 掩码必须是**本帧**刚换进来的：否则 CPU 掩码来自更早的一帧，而 GPU 掩码
	// 是按当前帧算的 —— 人脸只要动了几像素，两边就该不同，比了也是假的。
	// 这一条是 2026-11-01 07:04 那次 15% 差异的根因（当时掩码是复用的旧帧）。
	if (!md.fresh) {
		if (f->mask_wait < kMaskWaitMax)
			f->mask_wait++;
		return;
	}
	if (md.shape & 4) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] GPU 掩码对拍跳过：maskShape=%d 含 "
		     "bit2（脖子逐行扫描），GPU 侧未实现",
		     md.shape);
		f->gpu_mask_reported = true;
		return;
	}

	const uint32_t cx = (uint32_t)job.w;
	const uint32_t cy = (uint32_t)job.h;
	if (!beauty_gpu_mask_into(f, in_tex, cx, cy, md))
		return;

	// M1 失败时靠这两样东西定位：差异 bbox（落在哪个矩形/椭圆里）
	// 与原始掩码 dump（离线画分布）。正常通过时 dump 不产生任何文件。
	const char *dumpdir = getenv("BEAUTY_M1_DUMP_DIR");
	const size_t npix = (size_t)cx * (size_t)cy;
	uint8_t *d_skin = dumpdir && dumpdir[0] ? (uint8_t *)malloc(npix) : NULL;
	uint8_t *d_eye = dumpdir && dumpdir[0] ? (uint8_t *)malloc(npix) : NULL;

	// 顺带把「GPU 采到的那张图」和「CPU 掩码所用的那张图」都 dump 出来。
	// 掩码公式两边是同一份代码思路，唯一可能分叉的就是**输入像素**：
	// 一处是 `in_tex`（可能来自 OBS 的源纹理），一处是 `job.src`（buf_in 字节）。
	// 只要它们不是逐字节相同，皮肤权重就会在同一个像素上给出不同分值。
	if (dumpdir && dumpdir[0]) {
		uint8_t *gsrc = (uint8_t *)malloc(npix * 4);
		if (gsrc && beauty_readback_rgba(f, in_tex, gsrc, cx, cy))
			beauty_mask_dump(dumpdir, "gpu_src_rgba", gsrc, cx * 4,
					 cy);
		free(gsrc);
		if (job.src)
			beauty_mask_dump(dumpdir, "cpu_src_rgba", job.src, cx * 4,
					 cy);
	}

	int bb_skin[4] = {0, 0, 0, 0}, bb_eye[4] = {0, 0, 0, 0};
	int md_skin = 0, md_eye = 0;
	double avg_skin = 0.0, avg_eye = 0.0;
	uint64_t px_skin = 0, px_eye = 0;
	beauty_mask_diff_stage(f, gs_texrender_get_texture(f->mask_rt_a),
			       job.mask, cx, cy, &md_skin, &avg_skin, &px_skin,
			       bb_skin, d_skin);
	beauty_mask_diff_stage(f, gs_texrender_get_texture(f->mask_rt_b),
			       job.protect, cx, cy, &md_eye, &avg_eye, &px_eye,
			       bb_eye, d_eye);

	if (d_skin) {
		beauty_mask_dump(dumpdir, "gpu_skin", d_skin, cx, cy);
		beauty_mask_dump(dumpdir, "cpu_skin", job.mask, cx, cy);
	}
	if (d_eye) {
		beauty_mask_dump(dumpdir, "gpu_eye", d_eye, cx, cy);
		if (job.protect) {
			beauty_mask_dump(dumpdir, "cpu_eye", job.protect, cx, cy);
		} else {
			memset(d_eye, 0, npix);
			beauty_mask_dump(dumpdir, "cpu_eye", d_eye, cx, cy);
		}
	}
	free(d_skin);
	free(d_eye);

	const double npx = (double)cx * (double)cy;
	const bool pass = (md_skin <= 1) && (md_eye <= 1);
	const BeautyMaskFace &g0 = md.faces[0];

	char msg[1100];
	snprintf(msg, sizeof(msg),
		 "GPU 掩码对拍 %ux%u：skin maxdiff=%d 平均偏差=%.6f "
		 "差异像素=%llu(%.4f%%) bbox=(%d,%d)-(%d,%d) | eye maxdiff=%d "
		 "差异像素=%llu bbox=(%d,%d)-(%d,%d) | faces=%d shape=%d "
		 "rect=(%d,%d)-(%d,%d) fe=%d neck=(%d,%d)-(%d,%d) feN=%d "
		 "eyes=%d eye0=(%.1f,%.1f,%.1f,%.1f) eye1=(%.1f,%.1f,%.1f,%.1f) "
		 "判定=%s"
		 "（mask.hlsl 只跑诊断，生产掩码仍是 CPU 的）",
		 cx, cy, md_skin, avg_skin,
		 (unsigned long long)px_skin,
		 npx > 0 ? 100.0 * (double)px_skin / npx : 0.0,
		 bb_skin[0], bb_skin[1], bb_skin[2], bb_skin[3],
		 md_eye, (unsigned long long)px_eye,
		 bb_eye[0], bb_eye[1], bb_eye[2], bb_eye[3],
		 md.count, md.shape,
		 g0.x0, g0.y0, g0.x1, g0.y1, g0.fe,
		 g0.nx0, g0.nyLo, g0.nx1, g0.nyHi, g0.feNeck,
		 g0.hasEyes, g0.eye0x, g0.eye0y, g0.eyeW, g0.eyeH,
		 g0.eye1x, g0.eye1y, g0.eyeW, g0.eyeH,
		 pass ? "通过（≤1 灰阶）" : "失败");
	blog(pass ? LOG_INFO : LOG_WARNING, "[" PLUGIN_NAME "] %s", msg);

	f->gpu_mask_reported = true;
}

// ===========================================================================
// 4d：五官椭圆族的 GPU 对拍（R1-lite，3.1.70）
// ===========================================================================
//
// 只验「主椭圆」一族（立体 8 个 + 眼周 3 个）：加法增益、无邻域、无顺序耦合，
// 最适合先把管道打通。法令纹（邻域 max + 顺序耦合）与祛斑随后。
//
// CPU 基准怎么来：几何快照（job.regionGeom）已经把**同一份**椭圆交给了宿主，
// 而 `beautycore::applyEllipses` 本来就是公开 API —— 宿主拿同一帧、同一份掩码
// 直接调它，得到的就是 CPU 会算出的结果。**不需要给引擎加新的导出函数**。

static gs_effect_t *beauty_load_region_effect(struct beauty_filter *f)
{
	if (f->region_fx)
		return f->region_fx;
	if (f->region_tried)
		return NULL;
	f->region_tried = true;

	char *path = NULL;
	const char *sdir = getenv("BEAUTY_SHADER_DIR");
	if (sdir && sdir[0]) {
		char *u8 = beauty_env_path_to_utf8(sdir);
		if (u8) {
			const size_t n = strlen(u8) + 16;
			path = (char *)bmalloc(n);
			if (path)
				snprintf(path, n, "%s/region.hlsl", u8);
			bfree(u8);
		}
	} else {
		path = obs_module_file("shaders/region.hlsl");
	}
	if (!path) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] region.hlsl 未找到：五官对拍跳过");
		return NULL;
	}

	char *errors = NULL;
	f->region_fx = gs_effect_create_from_file(path, &errors);
	if (!f->region_fx) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] region.hlsl 编译失败：%s",
		     errors ? errors : "(no region)");
		bfree(errors);
	} else {
		static const char *kNeed[] = {"image", "skinMask", "ellipseTex",
					      "ellipseCount", "texSize"};
		bool ok = true;
		for (size_t i = 0; i < sizeof(kNeed) / sizeof(kNeed[0]); i++) {
			if (!gs_effect_get_param_by_name(f->region_fx, kNeed[i])) {
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] region.hlsl 缺少参数 `%s`",
				     kNeed[i]);
				ok = false;
				break;
			}
		}
		if (ok && !gs_effect_get_technique(f->region_fx,
						   "RegionEllipseTech")) {
			blog(LOG_WARNING, "[" PLUGIN_NAME "] region.hlsl 缺少 "
					  "`RegionEllipseTech`");
			ok = false;
		}
		if (!ok) {
			gs_effect_destroy(f->region_fx);
			f->region_fx = NULL;
		} else {
			blog(LOG_INFO, "[" PLUGIN_NAME "] region.hlsl 已加载（%s）",
			     path);
		}
	}
	bfree(path);
	return f->region_fx;
}

static bool beauty_ensure_region_rt(struct beauty_filter *f, uint32_t cx,
				    uint32_t cy)
{
	if (f->region_w == cx && f->region_h == cy && f->region_rt[0] &&
	    f->region_rt[3] && f->region_stage && f->region_param)
		return true;
	for (int i = 0; i < 4; i++) {
		if (f->region_rt[i]) {
			gs_texrender_destroy(f->region_rt[i]);
			f->region_rt[i] = NULL;
		}
	}
	if (f->region_stage) {
		gs_stagesurface_destroy(f->region_stage);
		f->region_stage = NULL;
	}
	if (f->region_param) {
		gs_texture_destroy(f->region_param);
		f->region_param = NULL;
	}
	f->region_w = f->region_h = 0;

	// 64×1 RGBA32F：21 个椭圆 × 3 texel = 63，留一个余量。
	f->region_param = gs_texture_create(64, 1, GS_RGBA32F, 1, NULL,
					    GS_DYNAMIC);
	// 4 张 scratch：主输出 / 中间1 / 中间2 / 末态。
	// 每一趟都不能读自己写的 RT（D3D11 同一资源既当 SRV 又当 RTV 是未定义行为）。
	// ⚠ 中间两张必须是**浮点**：祛斑的横向和最大到 13×255=3315，除以 255 是 13，
	// 存进 8 位 UNORM 会直接饱和成 1 —— 均值全错（2026-11-01 实测 2.8% 像素、
	// 差异铺满整个 ROI，就是这个）。max/LR 用不到那么大的动态范围，但共用这两张。
	f->region_rt[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	// 用单通道 float：够放 13 倍的和，4K 下每张 16.6 MB 而不是 RGBA32F 的 66 MB。
	f->region_rt[1] = gs_texrender_create(GS_R32F, GS_ZS_NONE);
	f->region_rt[2] = gs_texrender_create(GS_R32F, GS_ZS_NONE);
	f->region_rt[3] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->region_stage = gs_stagesurface_create(cx, cy, GS_RGBA);
	// 读回复用掩码对拍的 stage（beauty_readback_rgba 依赖它）
	if (!beauty_ensure_mask_rt(f, cx, cy) || !f->region_param ||
	    !f->region_rt[0] || !f->region_rt[1] || !f->region_rt[2] ||
	    !f->region_rt[3] || !f->region_stage) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] 五官对拍资源创建失败 %ux%u",
		     cx, cy);
		return false;
	}
	f->region_w = cx;
	f->region_h = cy;
	return true;
}

// 把一段椭圆写进参数纹理（**转成绝对像素**，与 CPU 在 applyEllipses 里做的一致）
static void beauty_region_upload_els(struct beauty_filter *f,
				     const BeautyRegionGeom &rg,
				     const BeautyRegionGeom::Face &fc, int beg,
				     int n)
{
	if (n <= 0 || n > 21)
		return;
	float *pb = (float *)calloc(64 * 4, sizeof(float));
	if (!pb)
		return;
	for (int i = 0; i < n; i++) {
		const BeautyRegionEllipse &e = rg.els[beg + i];
		float *A = pb + (size_t)(i * 3 + 0) * 4;
		A[0] = fc.fx + e.cx * fc.fw;
		A[1] = fc.fy + e.cy * fc.fh;
		A[2] = e.rx * fc.fw;
		A[3] = e.ry * fc.fh;
		float *B = pb + (size_t)(i * 3 + 1) * 4;
		B[0] = cosf(e.rot);
		B[1] = sinf(e.rot);
		B[2] = e.gain;
		B[3] = e.feather;
		float *C = pb + (size_t)(i * 3 + 2) * 4;
		C[0] = e.shade;
	}
	gs_texture_set_image(f->region_param, (const uint8_t *)pb, 64 * 16,
			     false);
	/* 2.8.2 诊断：确认「交到 shader 手里的是什么」。geomOnly 帧里五官效果
	 * 整个消失，链却照样跑完 pass —— 先看参数表本身有没有内容。 */
	if (getenv("BEAUTY_REGION_DBG")) {
		static int up_dbg = 0;
		if ((++up_dbg % 120) == 1)
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] [region-dbg] 上传 n=%d："
			     "A=(%.1f,%.1f,%.2f,%.2f) B=(%.3f,%.3f,%.3f,%.3f) "
			     "C.shade=%.3f",
			     n, pb[0], pb[1], pb[2], pb[3], pb[4], pb[5], pb[6],
			     pb[7], pb[8]);
	}
	free(pb);
}

// 跑一趟 region 的某个 technique：src -> rt[dst]，可选第二张输入（LR / 均值）。
//
// 三个坑（2026-11-01 全踩过，全部只有真跑才现形）：
//  1. **不要自己套 technique/pass** —— smooth_draw() 内部已经 begin/begin_pass/end，
//     外面再套就是嵌套 pass，输出直接全黑（首跑 maxdiff=255、100% 像素）。
//  2. **每趟都要重绑全部标量**（E2：begin_pass 消耗参数），漏一个就刷
//     `device_draw (D3D11): Not all shader parameters were set` 且**该趟被静默跳过**。
//     漏过的是 ellipseCount（它随趟变化：主 14 / 法令纹 2 / 下瓣 2 / 祛斑 0）。
//  3. 参数表每趟都要按那一族的子集重新上传（见 beauty_region_upload_els）。
static gs_texture_t *beauty_region_pass(struct beauty_filter *f,
					gs_effect_t *fx, const char *tech,
					gs_texture_t *src, gs_texture_t *aux,
					const char *aux_name, uint32_t cx,
					uint32_t cy, int dst,
					const BeautyRegionGeom &rg,
					const BeautyRegionGeom::Face &fc,
					int ec)
{
	if (!smooth_rt_begin(f->region_rt[dst], cx, cy))
		return NULL;
	smooth_bind_tex(fx, "image", src);
	smooth_bind_tex(fx, "skinMask", beauty_mask_skin(f));
	smooth_bind_tex(fx, "ellipseTex", f->region_param);
	smooth_bind_vec2(fx, "texSize", (float)cx, (float)cy);
	smooth_bind_int(fx, "ellipseCount", ec);
	smooth_bind_float(fx, "fSlope", fc.nasoSlope);
	smooth_bind_float(fx, "fMaxLift", rg.nasoMaxLift);
	smooth_bind_float(fx, "fGateLo", rg.nasoGateLo);
	smooth_bind_float(fx, "fGateHi", rg.nasoGateHi);
	smooth_bind_int(fx, "lrRadius", rg.nasoLrRadius);
	smooth_bind_float(fx, "sK", rg.spotK);
	smooth_bind_float(fx, "sThrLo", rg.spotThrLo);
	smooth_bind_float(fx, "sThrHi", rg.spotThrHi);
	smooth_bind_int(fx, "sRadius", rg.spotRadius);
	// 祛斑的作用域：CPU 的盒均值在 ROI 边界 clamp，不是画面边界
	smooth_bind_vec2(fx, "roiMin", (float)rg.roiX0, (float)rg.roiY0);
	smooth_bind_vec2(fx, "roiMax", (float)rg.roiX1, (float)rg.roiY1);
	if (aux && aux_name)
		smooth_bind_tex(fx, aux_name, aux);
	smooth_draw(fx, tech, src, cx, cy);
	smooth_rt_end(f->region_rt[dst]);
	return gs_texrender_get_texture(f->region_rt[dst]);
}

// 5b-2/五官：按 CPU 的次序把五官三族跑到 GPU 上（生产与对拍共用这一份）。
// 顺序：主椭圆 → 法令纹（max H/V + 施加）→ 下瓣 → 祛斑（行和/列均值 + 施加）。
// 返回最终的纹理；任何一步失败返回 NULL（调用方不得认领 REGION，整帧交回 CPU）。
static gs_texture_t *beauty_region_chain(struct beauty_filter *f,
					 gs_texture_t *in_tex, uint32_t cx,
					 uint32_t cy, const BeautyChainJob &job)
{
	const BeautyRegionGeom &rg = job.regionGeom;
	/* 2.8.1：掩码来源有两条 —— CPU 上传的（`job.mask` 非空）与 GPU 按几何重画的
	 * （S4a 的 geomOnly 帧，`mask_skin_use` 非空）。链做的是把
	 * `beauty_mask_skin(f)`（即 `mask_skin_use ?: skin_tex`）绑到 `skinMask` 上，
	 * 所以这里的判据应该是「本帧有没有掩码纹理」，而不是「有没有 CPU 指针」。
	 *
	 * 原来的写法只认 `job.mask`，后果是：S7 静止降频后绝大多数帧是 geomOnly，
	 * `job.mask == nullptr` → 五官链每帧在第一行就返回 NULL → REGION 不认领 →
	 * 细节/美形被 `region_blocks` 连带挡住 → 整帧交回 CPU + 整帧回读。
	 * 4K 开五官 54~58 ms 的主因在这里，不是"五官段没有 GPU 版本"（见 §9.17）。 */
	if (!in_tex || (!job.mask && !f->mask_skin_use) || rg.faceCount != 1)
		return NULL;
	const BeautyRegionGeom::Face &fc = rg.faces[0];
	gs_effect_t *fx = beauty_load_region_effect(f);
	if (!fx || !beauty_ensure_region_rt(f, cx, cy))
		return NULL;

	/* 2.8.2 诊断（BEAUTY_REGION_DBG=1，每 120 帧一次）：geomOnly 帧认领了
	 * REGION，但画面里五官效果整个消失 —— 这条日志用来分清「几何是空的」与
	 * 「几何有值但 pass 白跑」。链尾再打一条 `cur == in_tex` 说明有没有真处理。 */
	static int rg_dbg_n = 0;
	static bool rg_rb_done = false;
	const bool rg_dbg = getenv("BEAUTY_REGION_DBG") != NULL;
	if (rg_dbg && (++rg_dbg_n % 120) == 1)
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] [region-dbg] mask=%d skin_use=%d "
		     "geomskip=%d faces=%d els=%d naso=%d tail=%d spot=%d",
		     job.mask ? 1 : 0, f->mask_skin_use ? 1 : 0, (int)job.maskGeomOnly,
		     rg.faceCount, fc.elsCount, fc.nasoCount, fc.tailCount,
		     rg.spotOn);

	gs_texture_t *cur = in_tex;
	if (fc.elsCount > 0) {
		beauty_region_upload_els(f, rg, fc, fc.elsBeg, fc.elsCount);
		cur = beauty_region_pass(f, fx, "RegionEllipseTech", in_tex, NULL,
					 NULL, cx, cy, 0, rg, fc, fc.elsCount);
		if (!cur)
			return NULL;
	}

	if (fc.nasoCount > 0 && rg.nasoMaxLift > 0.0f) {
		beauty_region_upload_els(f, rg, fc, fc.nasoBeg, fc.nasoCount);
		gs_texture_t *h = beauty_region_pass(f, fx, "RegionFoldMaxHTech",
						     cur, NULL, NULL, cx, cy, 1, rg, fc,
						     fc.nasoCount);
		gs_texture_t *v = h ? beauty_region_pass(f, fx, "RegionFoldMaxVTech",
							 h, NULL, NULL, cx, cy, 2, rg,
							 fc, fc.nasoCount)
				    : NULL;
		if (!v)
			return NULL;
		cur = beauty_region_pass(f, fx, "RegionFoldApplyTech", cur, v,
					 "lrTex", cx, cy, 3, rg, fc,
					 fc.nasoCount);
		if (!cur)
			return NULL;
	}

	if (fc.tailCount > 0) {
		beauty_region_upload_els(f, rg, fc, fc.tailBeg, fc.tailCount);
		cur = beauty_region_pass(f, fx, "RegionEllipseTech", cur, NULL,
					 NULL, cx, cy, 0, rg, fc, fc.tailCount);
		if (!cur)
			return NULL;
	}

	if (rg.spotOn) {
		gs_texture_t *h = beauty_region_pass(f, fx, "RegionSpotSumHTech",
						     cur, NULL, NULL, cx, cy, 1, rg,
						     fc, 0);
		gs_texture_t *v = h ? beauty_region_pass(f, fx, "RegionSpotMeanVTech",
							 h, NULL, NULL, cx, cy, 2, rg, fc, 0)
				    : NULL;
		if (!v)
			return NULL;
		cur = beauty_region_pass(f, fx, "RegionSpotApplyTech", cur, v,
					 "meanTex", cx, cy, 3, rg, fc, 0);
		if (!cur)
			return NULL;
	}
	if (rg_dbg && (rg_dbg_n % 120) == 1)
		blog(LOG_INFO, "[" PLUGIN_NAME "] [region-dbg] 出口：%s",
		     (cur == in_tex) ? "**返回原图，一个 pass 都没跑**"
				     : "已跑 pass（像素应当改变）");
	/* 一次性读回：pass 跑过 ≠ 像素变过。读回链输出与输入逐像素比，
	 * 结果分三种 —— maxdiff=0 说明 shader 没改动任何像素（输入为空/掩码为 0）；
	 * 有差异但很小说明只施加了一部分；差异正常说明链本身没问题，是后面丢了。 */
	if (rg_dbg && !rg_rb_done && cur && cur != in_tex) {
		rg_rb_done = true;
		const size_t npix = (size_t)cx * (size_t)cy;
		uint8_t *pa = (uint8_t *)malloc(npix * 4);
		uint8_t *pb = (uint8_t *)malloc(npix * 4);
		if (pa && pb && beauty_readback_rgba(f, in_tex, pa, cx, cy) &&
		    beauty_readback_rgba(f, cur, pb, cx, cy)) {
			long dn = 0;
			int dmx = 0;
			for (size_t i = 0; i < npix; i++) {
				int w = 0;
				for (int c = 0; c < 3; c++) {
					int dv = (int)pa[i * 4 + c] -
						 (int)pb[i * 4 + c];
					if (dv < 0)
						dv = -dv;
					if (dv > w)
						w = dv;
				}
				if (w) {
					dn++;
					if (w > dmx)
						dmx = w;
				}
			}
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] [region-dbg] 链输出 vs 输入："
			     "maxdiff=%d 差异像素=%ld(%.4f%%)",
			     dmx, dn, npix ? 100.0 * (double)dn / (double)npix
					   : 0.0);
		}
		free(pa);
		free(pb);
	}
	return cur;
}

static void beauty_gpu_region_selfcheck(struct beauty_filter *f,
					gs_texture_t *in_tex,
					const BeautyChainJob &job)
{
	const BeautyRegionGeom &rg = job.regionGeom;
	// 2.8.3（P0-7）：这里原来写的是 `!job.mask` 就返回，而 S7 静止降频之后
	// 绝大多数帧是 geomOnly（`job.mask` 为空，掩码由 GPU 按几何重画）—— 对拍
	// 几乎从不触发，真机上三次采集只碰巧中了一次。判据改成「有没有掩码可用」。
	if (!in_tex || job.w <= 0 || job.h <= 0)
		return;
	if (rg.faceCount != 1)
		return;
	const BeautyRegionGeom::Face &fc = rg.faces[0];
	// 主椭圆可以为 0（例如只开法令纹/只开祛斑的场景），那时从输入直接开始串。
	if (fc.elsCount < 0 || fc.elsCount > 21)
		return;

	const uint32_t cx = (uint32_t)job.w;
	const uint32_t cy = (uint32_t)job.h;
	const size_t npix = (size_t)cx * (size_t)cy;

	gs_effect_t *fx = beauty_load_region_effect(f);
	// 顺带把 mask_stage 的尺寸钉到本帧（掩码读回复用它）。
	if (!fx || !beauty_ensure_region_rt(f, cx, cy))
		return;

	/* ---- 掩码来源 -------------------------------------------------------
	 * · 引擎给了 CPU 掩码（非 geomOnly 帧）→ 直接用，与旧行为等价。
	 * · geomOnly 帧 → 把 GPU 本帧刚画好的生产掩码读回，当 CPU 基准的掩码。
	 *   这才是 P0-7 要的判据：**同一份掩码 + 两种算法**。旧写法拿的是
	 *   `job.mask`（引擎的上一次残留，`beauty_core.cpp:2634/2816`），两路
	 *   本来就不是同一份掩码 —— 那时量到的差异含「掩码不同」这一项。 */
	const uint8_t *mask = job.mask;
	std::vector<uint8_t> mask_gpu;
	const char *mask_src = "CPU";
	if (!mask) {
		if (!f->mask_skin_use)
			return;
		mask_gpu.resize(npix);
		if (!beauty_mask_readback8(f, f->mask_skin_use, mask_gpu.data(), cx,
					   cy))
			return;
		mask = mask_gpu.data();
		mask_src = "GPU";
	}

	/* 掩码不能是空的：空掩码下两条算法都不碰任何像素，maxdiff 恒为 0 ——
	 * 这是最典型的假通过（§9.18 的 `--nospot` 实验就是这么废掉的：前后截图像素
	 * 完全相同，还以为找对了方向）。所以先量覆盖率，覆盖率 0 直接不算样本。 */
	{
		uint64_t msum = 0, mnz = 0;
		for (size_t i = 0; i < npix; i++) {
			msum += mask[i];
			if (mask[i])
				mnz++;
		}
		if (mnz == 0) {
			if (!f->region_skip_logged) {
				f->region_skip_logged = true;
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] 五官对拍等待中：本帧"
				     "掩码来源=%s 但全为 0（没脸/脸太偏），跳过",
				     mask_src);
			}
			return;
		}
		f->region_skip_logged = false;
		f->region_mask_pct = npix ? 100.0 * (double)mnz / (double)npix : 0.0;
		f->region_mask_avg = npix ? (double)msum / (double)npix : 0.0;
	}

	// 预热帧不算样本（见 region_cmp_delay 的初始化处）。
	if (f->region_cmp_delay > 0) {
		f->region_cmp_delay--;
		return;
	}

	// 与生产走**同一个**函数：对拍验的就是真的那一份。
	gs_texture_t *cur = beauty_region_chain(f, in_tex, cx, cy, job);
	if (!cur)
		return;

	// ---- CPU 基准：同一帧、同一掩码、同一几何、同一顺序 ------------------
	const size_t bytes = npix * 4;
	uint8_t *src = (uint8_t *)malloc(bytes);
	uint8_t *cpu = (uint8_t *)malloc(bytes);
	uint8_t *gpu = (uint8_t *)malloc(bytes);
	if (!src || !cpu || !gpu) {
		free(src); free(cpu); free(gpu);
		return;
	}
	if (!beauty_readback_rgba(f, in_tex, src, cx, cy)) {
		free(src); free(cpu); free(gpu);
		return;
	}
	memcpy(cpu, src, bytes);

	auto to_els = [&](int beg, int n, std::vector<beautycore::Ellipse> &v) {
		v.resize((size_t)n);
		for (int i = 0; i < n; i++) {
			const BeautyRegionEllipse &e = rg.els[beg + i];
			beautycore::Ellipse o;
			o.cx = e.cx; o.cy = e.cy; o.rx = e.rx; o.ry = e.ry;
			o.rot = e.rot; o.gain = e.gain;
			o.shade = e.shade; o.feather = e.feather;
			v[(size_t)i] = o;
		}
	};
	std::vector<beautycore::Ellipse> e1, e2, e3;
	to_els(fc.elsBeg, fc.elsCount, e1);
	beautycore::applyEllipses(cpu, (int)cx, (int)cy, mask, fc.fx, fc.fy,
				  fc.fw, fc.fh, e1.data(), fc.elsCount, 1);
	if (fc.nasoCount > 0 && rg.nasoMaxLift > 0.0f) {
		to_els(fc.nasoBeg, fc.nasoCount, e2);
		beautycore::applyFoldRemoval(cpu, (int)cx, (int)cy, mask, fc.fx,
					     fc.fy, fc.fw, fc.fh, e2.data(),
					     fc.nasoCount, fc.nasoSlope,
					     rg.nasoMaxLift, rg.nasoLrRadius,
					     rg.nasoGateLo, rg.nasoGateHi, 1);
	}
	if (fc.tailCount > 0) {
		to_els(fc.tailBeg, fc.tailCount, e3);
		beautycore::applyEllipses(cpu, (int)cx, (int)cy, mask, fc.fx,
					  fc.fy, fc.fw, fc.fh, e3.data(),
					  fc.tailCount, 1);
	}
	if (rg.spotOn) {
		beautycore::RoiRect roi;
		roi.x0 = rg.roiX0; roi.y0 = rg.roiY0;
		roi.x1 = rg.roiX1; roi.y1 = rg.roiY1;
		std::vector<float> scratch;
		beautycore::applySpotFix(cpu, (int)cx, (int)cy, mask, roi,
					 rg.spotK, rg.spotRadius, rg.spotThrLo,
					 rg.spotThrHi, 1, scratch);
	}

	if (!beauty_readback_rgba(f, cur, gpu, cx, cy)) {
		free(src); free(cpu); free(gpu);
		return;
	}

	int maxd = 0;
	uint64_t diffpx = 0, sum = 0;
	int bx0 = (int)cx, by0 = (int)cy, bx1 = -1, by1 = -1;
	for (uint32_t y = 0; y < cy; y++) {
		for (uint32_t x = 0; x < cx; x++) {
			const size_t k = ((size_t)y * cx + x) * 4;
			int worst = 0;
			for (int c = 0; c < 3; c++) {
				int dv = (int)gpu[k + c] - (int)cpu[k + c];
				if (dv < 0)
					dv = -dv;
				if (dv > worst)
					worst = dv;
			}
			if (worst > maxd)
				maxd = worst;
			if (worst) {
				diffpx++;
				sum += (uint64_t)worst;
				if ((int)x < bx0) bx0 = (int)x;
				if ((int)x > bx1) bx1 = (int)x;
				if ((int)y < by0) by0 = (int)y;
				if ((int)y > by1) by1 = (int)y;
			}
		}
	}
	const bool pass = (maxd <= 1);
	// 失败时落盘：离线还要看「差异落在椭圆的什么位置、掩码在那儿是多少」。
	// 只有显式给了 BEAUTY_M1_DUMP_DIR 才写（4K 一次 100 MB 量级）。
	// ⚠ 放在 free 之前：src 要在这里被重新读回来当源图 dump。
	const char *dumpdir = getenv("BEAUTY_M1_DUMP_DIR");
	if (!pass && dumpdir && dumpdir[0]) {
		if (beauty_readback_rgba(f, in_tex, src, cx, cy))
			beauty_mask_dump(dumpdir, "region_src_rgba", src, cx * 4,
					 cy);
		beauty_mask_dump(dumpdir, "region_mask", mask, cx, cy);
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] 五官对拍失败，已落盘 src/mask（%ux%u，"
		     "掩码来源=%s）",
		     cx, cy, mask_src);
	}
	free(src); free(cpu); free(gpu);

	f->region_cmp_done++;
	blog(pass ? LOG_INFO : LOG_WARNING,
	     "[" PLUGIN_NAME "] 五官整段对拍 %ux%u（样本 %d/%d）："
	     "maxdiff=%d 差异像素=%llu(%.4f%%) bbox=(%d,%d)-(%d,%d) "
	     "平均偏差=%.6f | 掩码来源=%s 覆盖率=%.1f%% 均值=%.1f | "
	     "faces=1 ellipses=%d+%d+%d spot=%d 判定=%s",
	     cx, cy, f->region_cmp_done, f->region_cmp_n, maxd,
	     (unsigned long long)diffpx,
	     npix ? 100.0 * (double)diffpx / (double)npix : 0.0, bx0, by0, bx1,
	     by1, diffpx ? (double)sum / (double)diffpx : 0.0, mask_src,
	     f->region_mask_pct, f->region_mask_avg, fc.elsCount, fc.nasoCount,
	     fc.tailCount, rg.spotOn,
	     pass ? "通过（≤1 灰阶）" : "失败");

	if (f->region_cmp_done >= f->region_cmp_n)
		f->region_reported = true;
}

// 段区间钩子本体。返回认领的段（必须构成执行顺序上的前缀）。
static unsigned beauty_chain_hook(void *user, const BeautyChainJob &job)
{
	struct beauty_filter *f = (struct beauty_filter *)user;
	if (f) {
		f->chain_claim = 0;
		f->diag_detail_ok = false;   // 每帧重置：只有本帧真认领 DETAIL 才置位
		f->diag_warp_ok = false;     // 同理：只有本帧真认领 WARP 才有几何可比
	}
	if (!f || !f->stage || !job.src || !job.dst || job.w <= 0 || job.h <= 0)
		return 0;
	// 磨皮是这一段的入口：做不了就是整段做不了（色调/细节都需要磨皮的结果
	// 在显存里，否则得先把 dst 传上去，比走 CPU 还慢）。
	if (!(job.flags & BEAUTY_SEG_SMOOTH))
		return 0;

	const uint32_t cx = (uint32_t)job.w;
	const uint32_t cy = (uint32_t)job.h;
	if (!beauty_ensure_gray(f, cx, cy) || !beauty_ensure_chain_rt(f, cx, cy))
		return 0;

	double prep_f = 0.0, prep_m = 0.0;

	// ==== 5b-2 切片 3：GPU 掩码进生产链（BEAUTY_GPU_MASK=1）================
	// 判据与切片 1/2 完全一致，因为「能不能用」取决于同一组信息：
	//   · 掩码必须是本帧刚换进来的（否则几何与画面不是同一帧）
	//   · 单脸（切片 1 的边界）、且不含 maskShape bit2（脖子逐行扫描未实现）
	// 任一不满足 → 整帧回到今天的路径（CPU 掩码 + 上传）。**只会变慢，不会变错。**
	const BeautyMaskDiag &mg = job.maskGeom;
	// S4a（2.6.0）：`job.maskGeomOnly` 表示本帧引擎只给了几何、mask 缓冲里没有内容。
	// 两处判据因此放宽，但都保持与老路径**语义等价**：
	//   · 「有掩码可用」由 `job.mask || geomOnly` 表示 —— 几何也算一种掩码来源；
	//   · 新鲜度由 `mg.fresh || geomOnly` 表示 —— 老路径在 fresh=0 时上传的是
	//     「上一轮的 CPU 掩码」，这里用「上一轮的几何 + 当前帧纹理」重画，滞后一模一样。
	// 条件不满足（多脸 / shape bit2 / GPU 关）时引擎照常画了 CPU 掩码，geomOnly=0，
	// 走的还是原来的上传路径 —— 所以这条放宽不存在「两边都没有掩码」的状态。
	const bool geom_only = f->gpu_mask_chain && job.maskGeomOnly;
	const bool gpu_mask_ok = f->gpu_mask_chain &&
				 (job.mask || job.maskGeomOnly) &&
				 (mg.fresh || job.maskGeomOnly) &&
				 mg.count == 1 && !(mg.shape & 4);

	gs_texture_t *ring = NULL;
	if (gpu_mask_ok && f->gpu_ring_ok)
		ring = beauty_ring_get(f, job.srcTag);
	/* S4b：缩略图帧的 job.src 是**上一帧的旧像素**（本帧没有回读全帧）。
	 * 环没命中时绝不能落到 beauty_hook_prepare 的「从 job.src 上传」那条路 ——
	 * 那会把上一帧的画面送进链，动态场景直接错帧。整帧不认领，让宿主跳过。 */
	if (f->thumb_frame && !ring) {
		f->an_thumbskip++;
		if (!f->thumb_skip_logged) {
			f->thumb_skip_logged = true;
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 缩略图帧在帧环里找不到 tag=%llu "
			     "的纹理 → 本帧跳过｜环=%llu/%llu/%llu "
			     "frame_tag=%llu gpu_mask_ok=%d geom_only=%d "
			     "count=%d shape=%d fresh=%d maskGeomOnly=%d "
			     "mask=%p",
			     (unsigned long long)job.srcTag,
			     (unsigned long long)f->ring_tag[0],
			     (unsigned long long)f->ring_tag[1],
			     (unsigned long long)f->ring_tag[2],
			     (unsigned long long)f->frame_tag,
			     gpu_mask_ok ? 1 : 0, geom_only ? 1 : 0, mg.count,
			     (int)mg.shape, (int)mg.fresh,
			     job.maskGeomOnly ? 1 : 0, (const void *)job.mask);
		}
		return 0;
	}

	gs_texture_t *in = ring ? ring
			       : beauty_hook_prepare(f, job.src, job.w, job.h,
						     job.mask, job.protect,
						     &prep_f, &prep_m,
						     gpu_mask_ok);
	if (!in)
		return 0;

	// 掩码指针只在本次钩子调用期间有效；任何 return 都要清掉，
	// 否则下一帧会拿着一张已经不属于本帧的 RT 纹理算 —— 用析构器兜住所有出口。
	struct MaskClear {
		struct beauty_filter *ff;
		explicit MaskClear(struct beauty_filter *p) : ff(p) {}
		~MaskClear()
		{
			ff->mask_skin_use = NULL;
			ff->mask_eye_use = NULL;
		}
	};
	MaskClear guard(f);   // 无论走哪条分支、从哪 return，退出时都会清空

	// ==== 4d：五官几何快照（一次性日志，BEAUTY_REGION_GEOM=1）=============
	// 只证明「引擎把三组椭圆 + 祛斑参数交到了宿主手里」，不参与任何渲染。
	{
		// 优化（3.1.76）：getenv 每帧调用是热路径上的浪费（走进程环境块查找），
		// 缓存成成员，只在第一次查。
		if (!f->rg_env_checked) {
			f->rg_env_checked = true;
			f->rg_env_on = getenv("BEAUTY_REGION_GEOM") ? 1 : 0;
		}
		const char *rg_env = f->rg_env_on ? "1" : NULL;
		if (rg_env && rg_env[0] == '1' && job.regionGeom.faceCount > 0 &&
		    f->region_geom_reported == 0) {
			f->region_geom_reported = 1;
			const BeautyRegionGeom &rg = job.regionGeom;
			int els = 0, naso = 0, tail = 0;
			for (int i = 0; i < rg.faceCount; i++) {
				els += rg.faces[i].elsCount;
				naso += rg.faces[i].nasoCount;
				tail += rg.faces[i].tailCount;
			}
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 五官几何快照：faces=%d ellipses=%d "
			     "（主 %d / 法令纹 %d / 下瓣 %d）spot=%d k=%.2f r=%d "
			     "thr=[%.1f,%.1f] 首脸框=(%.1f,%.1f,%.1f,%.1f) "
			     "nasoSlope=%.3f",
			     rg.faceCount, rg.ellipseCount, els, naso, tail,
			     rg.spotOn, rg.spotK, rg.spotRadius, rg.spotThrLo,
			     rg.spotThrHi, rg.faces[0].fx, rg.faces[0].fy,
			     rg.faces[0].fw, rg.faces[0].fh,
			     rg.faces[0].nasoSlope);
		}
	}

	// ==== T9：抽搐诊断计数（每帧，不改画面）===============================
	if (!job.maskGeom.fresh)
		f->an_nofresh++;
	if (!job.mask)
		f->an_masknull++;
	if (!f->det_hist_ok)
		f->an_histrst++;

	// ==== 5b-2 诊断：GPU 掩码对拍（一次性，不碰生产纹理）=================
	// 放在 GPU 掩码建图**之前**：两者共用同一对 scratch RT，先比完再建生产用的那份。
	// 这样即使默认走 GPU 掩码，M1 仍然每进程能出一次判定（诊断能力不随默认值丢失）。
	if (f->gpu_mask_cmp && !f->gpu_mask_reported)
		beauty_gpu_mask_selfcheck(f, in, job);

	if (gpu_mask_ok) {
		if (!beauty_gpu_mask_into(f, in, cx, cy, mg)) {
			// S4a（2.6.0）：本帧引擎只给了几何，**没有 CPU 掩码可以退**。
			// 整帧跳过处理（原样输出）—— 宁可这一帧不美颜，也绝不能
			// 按「无掩码 = 全皮肤」把整张画面磨掉。计数进 an_geomskip。
			if (geom_only) {
				f->an_geomskip++;
				obs_source_skip_video_filter(f->context);
			}
			return 0;   // 画不出来就整帧交回 CPU（画面正确，只是慢一帧）
		}
		f->mask_skin_use = gs_texrender_get_texture(f->mask_rt_a);
		f->mask_eye_use = gs_texrender_get_texture(f->mask_rt_b);
		if (geom_only)
			f->an_geomonly++;   // S4a：本帧用的是「几何 + GPU 重画」
		if (!f->mask_skin_use || !f->mask_eye_use) {
			if (geom_only) {
				f->an_geomskip++;
				obs_source_skip_video_filter(f->context);
			}
			return 0;
		}
	} else if (geom_only) {
		/* 防御分支：按上面的判据，geom_only 为真时 gpu_mask_ok 必然为真，
		 * 这里理论上不可达（除非将来改了判据）。留着的理由与上一处相同：
		 * 真走到这儿时，`job.mask == NULL` 会被当成全皮肤，必须拦住。 */
		f->an_geomskip++;
		obs_source_skip_video_filter(f->context);
		return 0;
	}

	// ==== 4d：五官椭圆族的对拍（一次性，纯诊断）=========================
	// ⚠ 必须放在「建 GPU 掩码之后」：对拍要采样本帧的皮肤掩码，放在建掩码之前
	// 会拿上一帧的 —— 2026-11-01 第一次跑就是这么差的 6%（不是公式错）。
	if (f->region_cmp && !f->region_reported)
		beauty_gpu_region_selfcheck(f, in, job);

	const double t_draw0 = beauty_now_ms();
	BeautyBandDiag cp = job.band;
	// 诊断覆盖（默认关）：把趟数锁死，量「趟数 → 耗时」斜率用。见
	// beauty_smooth_passes_force() 上方的说明。放在这里而不是引擎里的原因：
	// GPU draw 与 CPU 对拍基准都从这里取 cp，覆盖一次两边一致。
	if (const int fp = beauty_smooth_passes_force())
		cp.passes = fp;
	gs_texture_t *cur = beauty_gpu_smooth(f, in, cx, cy, &cp);
	const double t_draw1 = beauty_now_ms();
	if (!cur)
		return 0;

	unsigned claim = BEAUTY_SEG_SMOOTH;
	double t_tone = 0.0, t_detail = 0.0, t_warp = 0.0;

	// ---- 色调 ---------------------------------------------------------
	if (job.flags & BEAUTY_SEG_TONE) {
		const double t0 = beauty_now_ms();
		gs_texture_t *t = beauty_chain_tone(f, cur, cx, cy,
						    job.tone.clarity,
						    job.tone.whiten,
						    job.tone.rosy);
		t_tone = beauty_now_ms() - t0;
		if (t) {
			cur = t;
			claim |= BEAUTY_SEG_TONE;
		}
		// 色调失败就不再往后走：细节段的输入是「着色后的颜色」，
		// 少了这一段等于把色调整段丢掉（画面会变），宁可只认领磨皮。
	}

	// ---- 细节 ---------------------------------------------------------
	// 只在「区间连续」时做：五官段会插在色调与细节之间，它没 GPU 版本，
	// ---- 五官 ---------------------------------------------------------
	// 顺序与 CPU 一致：磨皮 → 色调 → **五官** → 细节 → 美形。
	// 三族全部跑成功才认领 REGION；任何一步失败都不认领，引擎会把五官连同
	// 依赖它的细节/美形一起交回 CPU（画面正确，只是这一帧慢）。
	double t_region = 0.0;
	if ((job.flags & BEAUTY_SEG_REGION) && f->gpu_region) {
		const double t_r0 = beauty_now_ms();
		gs_texture_t *t = beauty_region_chain(f, cur, cx, cy, job);
		t_region = beauty_now_ms() - t_r0;
		if (t) {
			cur = t;
			claim |= BEAUTY_SEG_REGION;
		}
	}

	// 一旦本帧要跑五官，细节就只能留给 CPU（否则前缀不成立，引擎会整份丢弃）。
	const bool region_blocks =
		(job.flags & BEAUTY_SEG_REGION) && !(claim & BEAUTY_SEG_REGION);
	const bool tone_ok =
		!(job.flags & BEAUTY_SEG_TONE) || (claim & BEAUTY_SEG_TONE);
	if ((job.flags & BEAUTY_SEG_DETAIL) && !region_blocks && tone_ok) {
		const bool hist_reset = !f->det_hist_ok;
		const double t0 = beauty_now_ms();
		gs_texture_t *t = beauty_chain_detail(f, in, cur, cx, cy,
						      job.detail, job.mask == NULL,
						      hist_reset);
		t_detail = beauty_now_ms() - t0;
		if (t) {
			cur = t;
			claim |= BEAUTY_SEG_DETAIL;
			f->det_hist_ok = true;
			// T1 诊断：暂存本帧引擎算好的细节参数，供自检复用。
			// 宿主不能自己重算 —— fine/mid 取决于磨皮质感与运动 EMA 的 beta。
			if (f->gpu_detail_diag && !f->gpu_detail_reported) {
				f->diag_detail = job.detail;
				f->diag_detail_ok = true;
			}
		} else {
			f->det_hist_ok = false;
		}
	}

	// ---- 美形（5c / 3.1.62）-------------------------------------------
	// 美形是执行顺序的最后一段（磨皮→色调→五官→细节→美形），所以认领它要求
	// **前面每一段都已认领** —— 否则 claim 不是 flags 的前缀，引擎会整份丢弃。
	//
	// 已知边界：五官段（REGION）插在色调与细节之间。它出现前缀就断在这里的
	// 两种情形是：① `BEAUTY_GPU_REGION=0`（五官链不跑）；② 五官链本帧失败
	// （`rg.faceCount != 1` 多脸、着色器/RenderTarget 建不出来）。这两种情况
	// 美形只能留给 CPU —— 认领必须是 flags 的**前缀**，断了就是断了。
	//
	// 3.1.72 起五官段本身**有** GPU 版本（`beauty_region_chain`），所以别再按
	// 「五官没有 GPU 版本」去理解这条边界（旧注释就是这么写的，2026-11-03 更正）；
	// 2.8.1 又补掉了「geomOnly 帧拿不到掩码就整段放弃」这一条假边界。
	const bool warp_prefix_ok =
		!region_blocks && tone_ok &&
		(!(job.flags & BEAUTY_SEG_DETAIL) || (claim & BEAUTY_SEG_DETAIL));
	if ((job.flags & BEAUTY_SEG_WARP) && warp_prefix_ok) {
		const double t0 = beauty_now_ms();
		gs_texture_t *t = beauty_chain_warp(f, cur, cx, cy, job.warp);
		t_warp = beauty_now_ms() - t0;
		if (t) {
			cur = t;
			claim |= BEAUTY_SEG_WARP;
			// 5c 对拍：把引擎算好的几何暂存下来，稍后 CPU 按**同一份几何**
			// 重跑一遍做逐像素比较。宿主不能自己重算几何（关键点只在引擎里）。
			if (f->gpu_warp_diag && !f->gpu_warp_reported) {
				f->diag_warp = job.warp;
				f->diag_warp_ok = true;
			}
		}
	}

	// ---- 5b-1：能不回读就不回读 -----------------------------------------
	// 判据只有一条：本帧 flags 是否**全部**被认领。引擎在每个 CPU 段前都有
	// `claimed & SEG` 守卫（beauty_core.cpp:1892 色调 / 1902 五官 / 后面还有
	// 细节与美形），全认领就意味着 dst 一个字节都不会被写 —— 这份回读没人
	// 消费，可以直接把显存里的最终纹理交给主机画。
	//
	// 三条附加条件都是「主机侧还要用 buf_out」的情形，必须走老路：
	//   · !f->verified —— 首帧的上传自检要 stage out_tex 与 buf_out 对拍
	//   · f->mirror    —— 镜像翻转是 CPU 整帧操作，只作用在 buf_out 上
	// 3.1.62 起美形也能被认领，所以「美形开启 → 退回回读」这条限制没有了；
	// 现在只剩**五官**（REGION）会让 claim 覆盖不到 flags。
	const bool chain_direct = f->chain_out &&
				  (claim & job.flags) == job.flags &&
				  f->verified && !f->mirror;
	if (chain_direct) {
		f->chain_out_tex = cur;
		f->chain_out_valid = true;
		// 统计口径与老路一致，只是 map/copy 这两项本帧确实是 0。
		f->prof_sm_prep += prep_f + prep_m;
		f->prof_sm_mask += prep_m;
		f->prof_sm_draw += t_draw1 - t_draw0;
		f->prof_ch_tone += t_tone;
		f->prof_ch_detail += t_detail;
		f->prof_ch_warp += t_warp;
		f->prof_sm_n++;
		f->chain_claim = claim;
		return claim;
	}

	// ---- 回读 ---------------------------------------------------------
	// 整条链到这一行才第一次碰 CPU 内存。map 会把 GPU 队列排空，所以这里
	// 等的是「磨皮 + 色调 + 细节」的全部 GPU 时间，不只是拷贝。
	gs_stage_texture(f->stage, cur);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->stage, &ptr, &linesize) || !ptr) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 段区间钩子：回读失败，本帧回退 CPU");
		f->det_hist_ok = false;
		return 0;   // dst 未被写满，必须整份交回引擎
	}
	const double t_map1 = beauty_now_ms();
	const size_t row = (size_t)job.w * 4;
	if (linesize == row) {
		memcpy(job.dst, ptr, row * (size_t)job.h);
	} else {
		for (int y = 0; y < job.h; y++)
			memcpy(job.dst + (size_t)y * row,
			       ptr + (size_t)y * linesize, row);
	}
	gs_stagesurface_unmap(f->stage);
	const double t_copy1 = beauty_now_ms();

	f->prof_sm_prep += prep_f + prep_m;
	f->prof_sm_mask += prep_m;
	f->prof_sm_draw += t_draw1 - t_draw0;
	f->prof_sm_map += t_map1 - t_draw1;
	f->prof_sm_copy += t_copy1 - t_map1;
	f->prof_ch_tone += t_tone;
	f->prof_ch_detail += t_detail;
	f->prof_ch_warp += t_warp;
	f->prof_sm_n++;
	f->chain_claim = claim;
	// T9：认领不满 = 本帧有段落回落到 CPU（抽搐的候选成因之一）
	if ((claim & job.flags) != job.flags)
		f->an_claimdrop++;

	return claim;
}

// ---------------------------------------------------------------------------
// T1：GPU 细节对拍（BEAUTY_GPU_DETAIL=1，诊断，不改画面）
// ---------------------------------------------------------------------------
//
// 与色调/磨皮自检同形态，但细节段有两处不同，决定了这里怎么做：
//   1) detailRefill 是**在 dst 上做加法**、亮度取自 src —— 对拍两边必须同起点。
//      这里让两边都从「原图」起（GPU image=in_tex、CPU dst=memcpy(src)），
//      与 beautyCoreSmoothCpu 的隔离口径一致：只比算子本身，不比链式整合。
//   2) 细节段带**时序 EMA**：GPU 用 det_hist、CPU 用 detailLumT_，两份历史
//      互不相通。直接比会把「历史不同」误算成「算法错」。所以对拍帧强制
//      emaOn=0（GPU 跳过 HistTech、CPU 传 lumT=nullptr），两边都无状态。
//
// dp 由链钩子递出并暂存（f->diag_detail）—— 宿主不能自己重算：fine/mid 取决于
// 磨皮质感与运动 EMA 的 beta，重算必漂。
static void beauty_gpu_detail_selfcheck(struct beauty_filter *f, gs_texture_t *in_tex,
					uint32_t cx, uint32_t cy)
{
	static const int kDetailWaitMax = 600;
	static const double kDetailMinCover = 3.0;

	if (!f->buf_in || !f->stage)
		return;
	// 只有本帧链钩子真的认领过 DETAIL 才有 dp 可比（4K 下 D 位可能不认领）。
	if (!f->diag_detail_ok)
		return;

	// 强效果门槛：与磨皮同一套（必须检出人脸 + 掩码有实质面积）。
	int mask_px = -1, faces = 0;
	beautyCoreMaskStat(&mask_px, &faces);
	const double cover = (mask_px > 0 && cx && cy)
				     ? 100.0 * (double)mask_px / ((double)cx * (double)cy)
				     : 0.0;
	const bool strong = (faces >= 1) && (cover >= kDetailMinCover);
	if (!strong && f->detail_wait < kDetailWaitMax) {
		f->detail_wait++;
		return;
	}

	// 增益全 0 = 这一帧细节段本就空转，比了也是「两边都不动」，证明力为零。
	const BeautyDetailDiag &d0 = f->diag_detail;
	if (d0.fine <= 0.0f && d0.mid <= 0.0f && d0.globalFine <= 0.0f &&
	    d0.globalMid <= 0.0f) {
		if (f->detail_wait < kDetailWaitMax)
			f->detail_wait++;
		return;
	}

	// 与磨皮自检同一坑：mask_buf 由 beauty_ensure_mask 分配，只调 ensure_gray
	// 不够 —— 漏了它，细节对拍会悄悄依赖「先前跑过色调自检」。
	if (!beauty_ensure_gray(f, cx, cy) || !beauty_ensure_mask(f, cx, cy))
		return;
	if (!beautyCoreCopyMask(f->mask_buf, (int)cx, (int)cy))
		return;   // 无掩码时对拍会「假通过」，下一帧再来
	gs_texture_set_image(f->skin_tex, f->mask_buf, cx, false);
	f->hook_eye_zero = false;

	// 单独跑一趟**无状态** detail（emaOn=0，base=原图）。与生产链无关：链已在
	// 本次 render 里回读完 buf_out，这里的 det_*/chain_rt/sm_out 都只是 scratch。
	BeautyDetailDiag dd = f->diag_detail;
	dd.emaOn = 0;
	gs_texture_t *gpu_tex = beauty_chain_detail(f, in_tex, in_tex, cx, cy, dd,
						    false, true);
	if (!gpu_tex)
		return;

	const size_t px = (size_t)cx * (size_t)cy;
	const size_t bytes = px * 4;
	uint8_t *gpu_buf = (uint8_t *)malloc(bytes);
	uint8_t *cpu_buf = (uint8_t *)malloc(bytes);
	if (!gpu_buf || !cpu_buf) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	bool got = false;
	gs_stage_texture(f->stage, gpu_tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(f->stage, &ptr, &linesize) && ptr) {
		const size_t row = (size_t)cx * 4;
		if (linesize == row) {
			memcpy(gpu_buf, ptr, bytes);
		} else {
			for (uint32_t y = 0; y < cy; y++)
				memcpy(gpu_buf + (size_t)y * row,
				       ptr + (size_t)y * linesize, row);
		}
		gs_stagesurface_unmap(f->stage);
		got = true;
	}
	if (!got) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] GPU 细节对拍：输出回读失败");
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	// CPU 基准：同一帧、同一份掩码、同一组参数（dst 由内部 memcpy(src) 起）。
	if (!beautyCoreDetailCpu(f->buf_in, cpu_buf, (int)cx, (int)cy, f->mask_buf,
				 dd)) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	int maxdiff = 0;
	uint64_t sum_abs = 0;
	uint64_t diff_px = 0;
	uint64_t gpu_sum = 0;
	uint64_t cpu_sum = 0;
	uint64_t gpu_chg = 0;
	uint64_t cpu_chg = 0;
	for (size_t k = 0; k < px; k++) {
		const uint8_t *gp = gpu_buf + k * 4;
		const uint8_t *cp = cpu_buf + k * 4;
		const uint8_t *op = f->buf_in + k * 4;
		int worst = 0;
		for (int ch = 0; ch < 3; ch++) {
			int d = (int)gp[ch] - (int)cp[ch];
			if (d < 0)
				d = -d;
			if (d > worst)
				worst = d;
			sum_abs += (uint64_t)d;
			gpu_chg += (uint64_t)((int)gp[ch] > op[ch] ? gp[ch] - op[ch]
								  : op[ch] - gp[ch]);
			cpu_chg += (uint64_t)((int)cp[ch] > op[ch] ? cp[ch] - op[ch]
								  : op[ch] - cp[ch]);
		}
		if (worst)
			diff_px++;
		if (worst > maxdiff)
			maxdiff = worst;
		gpu_sum += (uint64_t)gp[0] + gp[1] + gp[2];
		cpu_sum += (uint64_t)cp[0] + cp[1] + cp[2];
	}

	uint64_t masked = 0;
	for (size_t k = 0; k < px; k++)
		if (f->mask_buf[k])
			masked++;

	const double n3 = (double)px * 3.0;
	const double avg_abs = n3 > 0 ? (double)sum_abs / n3 : 0.0;
	const bool pass = (maxdiff <= 2) && (avg_abs < 0.05);

	std::string msg = "GPU 细节对拍 ";
	msg += std::to_string(cx) + "x" + std::to_string(cy);
	msg += "（emaMode=0 无状态 参数 fine=" + std::to_string(dd.fine) +
	       " mid=" + std::to_string(dd.mid) +
	       " gFine=" + std::to_string(dd.globalFine) +
	       " gMid=" + std::to_string(dd.globalMid) + "）";
	msg += "：maxdiff=" + std::to_string(maxdiff);
	msg += " 平均偏差=" + std::to_string(avg_abs);
	msg += " 差异像素=" + std::to_string((double)diff_px * 100.0 / (double)px) + "%";
	msg += " 亮度 gpu=" + std::to_string((double)gpu_sum / n3);
	msg += " cpu=" + std::to_string((double)cpu_sum / n3);
	msg += " 改动量 gpu=" + std::to_string((double)gpu_chg / n3);
	msg += " cpu=" + std::to_string((double)cpu_chg / n3);
	msg += " 掩码覆盖=" + std::to_string((double)masked * 100.0 / (double)px) + "%";
	msg += " faces=" + std::to_string(faces);
	msg += " 等脸=" + std::to_string(f->detail_wait) + "帧";
	msg += pass ? " 判定=通过" : " 判定=失败";
	if (!strong)
		msg += "【弱效果兜底：等满 " + std::to_string(f->detail_wait) +
		       " 帧仍无人脸，改动量偏小，证明力有限】";
	msg += "（detail.hlsl 只跑诊断，画面仍是生产链结果）";
	blog(pass ? LOG_INFO : LOG_WARNING, "[" PLUGIN_NAME "] %s", msg.c_str());

	free(gpu_buf);
	free(cpu_buf);
	f->gpu_detail_reported = true;
}

// ---------------------------------------------------------------------------
// 5c：GPU 美形对拍（BEAUTY_GPU_WARP=1，一次性，诊断，不改画面）
// ---------------------------------------------------------------------------
//
// 与 T1 细节对拍同形态，但有一处根本不同，值得写清楚：
//   · 细节对拍要**造**一个无状态场景（emaOn=0）才能比，因为 EMA 历史两边不通；
//   · 美形**本来就是无状态**的 —— 位移场只取决于当前帧的几何与像素。
//     所以这里不需要任何「对拍专用参数」，直接拿生产链当帧用的那份几何
//     （f->diag_warp）重跑一遍就行。这也让判据更强：比的就是生产路径本身。
//
// 输入用 `in_tex`（原图）而不是链的中间结果：美形是链尾，它的输入在生产里
// 是「色调+细节之后」的颜色。但这里要比的是**算子**，不是链式整合 ——
// 两边从同一张图起（CPU 侧 beautyCoreWarpCpu 内部 memcpy(src)），
// 与 beautyCoreSmoothCpu / beautyCoreDetailCpu 的口径完全一致。
//
// 判据：maxdiff ≤ 1（3.1.62 定案）。给 1 而不是 0 的原因：位移场是 4 点浮点
// 加权，MSVC 与 fxc 对 `a*b + c*d` 是否合成 fma 各有取舍，末位舍入可能翻 1
// 灰阶。这不是「差不多就行」，而是把判据定在**浮点末位**这个真实误差源上；
// 真出错时（权重写错、少钳一次、漏一段位移）差异是几十到 255，与 1 不会混淆。
static void beauty_gpu_warp_selfcheck(struct beauty_filter *f, gs_texture_t *in_tex,
				      uint32_t cx, uint32_t cy)
{
	static const int kWarpWaitMax = 600;

	if (!f->buf_in || !f->stage || !in_tex)
		return;
	// 只有本帧链钩子真的认领过 WARP 才有几何可比（五官开启 / 着色器缺失时不认领）。
	if (!f->diag_warp_ok)
		return;

	const BeautyWarpDiag &wd = f->diag_warp;
	if (wd.count <= 0)
		return;

	// 强效果门槛：位移量必须真的非零。滑块全 0 时「两边都不动」，
	// 对拍必然通过但证明力为零 —— 那正是 3.1.60 在 5b-1 上踩过的坑。
	bool active = false;
	long long roi_px = 0;
	for (int i = 0; i < wd.count; i++) {
		const BeautyWarpFace &g = wd.faces[i];
		if ((g.flags & BEAUTY_WARP_F_THIN) && g.thinK > 1e-3f)
			active = true;
		if ((g.flags & BEAUTY_WARP_F_EYE) && g.eyeAmp > 1e-3f)
			active = true;
		if ((g.flags & BEAUTY_WARP_F_CHIN) && g.chinK > 1e-3f)
			active = true;
		const long long rw = (long long)g.roi[2] - (long long)g.roi[0];
		const long long rh = (long long)g.roi[3] - (long long)g.roi[1];
		if (rw > 0 && rh > 0)
			roi_px += rw * rh;
	}
	if (!active) {
		if (f->warp_wait < kWarpWaitMax)
			f->warp_wait++;
		return;
	}

	// 自己的一对 scratch：生产链在 5b-1 直绘模式下 chain_out_tex 可能就是
	// warp_a 的纹理，往同一张里画等于把即将上屏的画面覆盖掉。
	gs_texrender_t *ra = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_t *rb = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!ra || !rb) {
		if (ra)
			gs_texrender_destroy(ra);
		if (rb)
			gs_texrender_destroy(rb);
		return;
	}

	gs_texture_t *gpu_tex = beauty_chain_warp_into(f, in_tex, cx, cy, wd, ra, rb);
	if (!gpu_tex) {
		gs_texrender_destroy(ra);
		gs_texrender_destroy(rb);
		return;
	}

	const size_t px = (size_t)cx * (size_t)cy;
	const size_t bytes = px * 4;
	uint8_t *gpu_buf = (uint8_t *)malloc(bytes);
	uint8_t *cpu_buf = (uint8_t *)malloc(bytes);
	if (!gpu_buf || !cpu_buf) {
		free(gpu_buf);
		free(cpu_buf);
		gs_texrender_destroy(ra);
		gs_texrender_destroy(rb);
		return;
	}

	bool got = false;
	gs_stage_texture(f->stage, gpu_tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(f->stage, &ptr, &linesize) && ptr) {
		const size_t row = (size_t)cx * 4;
		if (linesize == row) {
			memcpy(gpu_buf, ptr, bytes);
		} else {
			for (uint32_t y = 0; y < cy; y++)
				memcpy(gpu_buf + (size_t)y * row,
				       ptr + (size_t)y * linesize, row);
		}
		gs_stagesurface_unmap(f->stage);
		got = true;
	}
	gs_texrender_destroy(ra);
	gs_texrender_destroy(rb);
	if (!got) {
		blog(LOG_WARNING, "[" PLUGIN_NAME "] GPU 美形对拍：输出回读失败");
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	// CPU 基准：同一份几何（wd 原样传入，不重算）。
	if (!beautyCoreWarpCpu(f->buf_in, cpu_buf, (int)cx, (int)cy, wd)) {
		free(gpu_buf);
		free(cpu_buf);
		return;
	}

	int maxdiff = 0;
	int maxdiff_rgb = 0;
	int maxdiff_a = 0;
	uint64_t sum_abs = 0;
	uint64_t diff_px = 0;
	uint64_t diff_in_roi = 0;
	for (size_t k = 0; k < px; k++) {
		const uint8_t *gp = gpu_buf + k * 4;
		const uint8_t *cp = cpu_buf + k * 4;
		int worst = 0;
		for (int ch = 0; ch < 3; ch++) {
			int d = (int)gp[ch] - (int)cp[ch];
			if (d < 0)
				d = -d;
			if (d > worst)
				worst = d;
			sum_abs += (uint64_t)d;
		}
		int da = (int)gp[3] - (int)cp[3];
		if (da < 0)
			da = -da;
		if (worst > maxdiff_rgb)
			maxdiff_rgb = worst;
		if (da > maxdiff_a)
			maxdiff_a = da;
		if (worst > maxdiff)
			maxdiff = worst;
		if (worst)
			diff_px++;
	}

	// ROI 内差异计数：ROI 外本该逐字节相同，一旦出现差异说明「透传」写错了。
	for (int i = 0; i < wd.count; i++) {
		const BeautyWarpFace &g = wd.faces[i];
		for (int y = g.roi[1]; y < g.roi[3]; y++) {
			if (y < 0 || y >= (int)cy)
				continue;
			for (int x = g.roi[0]; x < g.roi[2]; x++) {
				if (x < 0 || x >= (int)cx)
					continue;
				const size_t k = (size_t)y * cx + (size_t)x;
				if (gpu_buf[k * 4] != cpu_buf[k * 4])
					diff_in_roi++;
			}
		}
	}

	const double n3 = (double)px * 3.0;
	const double avg_abs = n3 > 0 ? (double)sum_abs / n3 : 0.0;
	const bool pass = (maxdiff <= 1);

	char msg[640];
	snprintf(msg, sizeof(msg),
		 "GPU 美形对拍 %ux%u：maxdiff=%d（RGB %d / alpha %d）"
		 " 平均偏差=%.6f 差异像素=%llu(%.4f%%) ROI内R差异=%llu"
		 " | faces=%d 面板 thin=%.2f eye=%.2f chin=%.2f"
		 " 位移 eyeAmp=%.3f thinK=%.3f chinK=%.3f"
		 " ROI占比=%.2f%% 等强效果=%d帧 判定=%s"
		 "（warp.hlsl 只跑诊断，画面仍是生产链结果）",
		 cx, cy, maxdiff, maxdiff_rgb, maxdiff_a, avg_abs,
		 (unsigned long long)diff_px,
		 px ? 100.0 * (double)diff_px / (double)px : 0.0,
		 (unsigned long long)diff_in_roi, wd.count, (double)wd.thin,
		 (double)wd.eye, (double)wd.chin,
		 wd.count > 0 ? (double)wd.faces[0].eyeAmp : 0.0,
		 wd.count > 0 ? (double)wd.faces[0].thinK : 0.0,
		 wd.count > 0 ? (double)wd.faces[0].chinK : 0.0,
		 px ? 100.0 * (double)roi_px / (double)px : 0.0, f->warp_wait,
		 pass ? "通过（≤1 灰阶，浮点末位舍入）" : "失败");
	blog(pass ? LOG_INFO : LOG_WARNING, "[" PLUGIN_NAME "] %s", msg);

	free(gpu_buf);
	free(cpu_buf);
	f->gpu_warp_reported = true;
}

// ---------------------------------------------------------------------------
// 5b-1：绘制路径对拍（BEAUTY_CHAINOUT_CMP=1，一次性）
// ---------------------------------------------------------------------------
// 为什么要对拍「绘制」而不是「数据」：直绘用的纹理和回读出来的字节本来就是
// 同一份（都来自那张 texrender），比内容永远相等、毫无意义。真正会变的是
// 绘制这一段 —— 采样器、纹理格式、sRGB 开关、混合状态。所以这里把两条路
// 各渲染进一张 scratch RT 再逐像素比：**RGB 相等**就证明直绘与老路在
// 画面等价（alpha 故意不同，见下面的判定段）。
static bool chainout_render_stage(struct beauty_filter *f, gs_texture_t *tex,
				  bool opaque, uint8_t *out, uint32_t cx,
				  uint32_t cy)
{
	if (!f->stage || !tex || !out)
		return false;
	gs_texrender_t *rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!rt)
		return false;

	bool ok = false;
	if (gs_texrender_begin(rt, cx, cy)) {
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		// 与真实绘制段保持一致：同一个 base effect / technique / sRGB 处理。
		// 唯一的差别是混合状态（直绘不透明、老路 SRC_ALPHA），这正是我们要
		// 验证的两个变量之一。
		const bool linear_srgb = gs_get_linear_srgb();
		const bool prev_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear_srgb);
		gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
		gs_eparam_t *img = gs_effect_get_param_by_name(eff, "image");
		if (tech && img) {
			if (linear_srgb)
				gs_effect_set_texture_srgb(img, tex);
			else
				gs_effect_set_texture(img, tex);
			gs_blend_state_push();
			if (opaque)
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			else
				gs_blend_function(GS_BLEND_ONE,
						  GS_BLEND_INVSRCALPHA);
			const size_t passes = gs_technique_begin(tech);
			for (size_t i = 0; i < passes; i++) {
				gs_technique_begin_pass(tech, i);
				gs_draw_sprite(tex, 0, cx, cy);
				gs_technique_end_pass(tech);
			}
			gs_technique_end(tech);
			gs_blend_state_pop();
		}
		gs_texrender_end(rt);
		gs_enable_framebuffer_srgb(prev_srgb);

		gs_texture_t *t = gs_texrender_get_texture(rt);
		if (t) {
			gs_stage_texture(f->stage, t);
			uint8_t *ptr = NULL;
			uint32_t ls = 0;
			if (gs_stagesurface_map(f->stage, &ptr, &ls) && ptr) {
				const size_t row = (size_t)cx * 4;
				for (uint32_t y = 0; y < cy; y++)
					memcpy(out + (size_t)y * row,
					       ptr + (size_t)y * ls, row);
				gs_stagesurface_unmap(f->stage);
				ok = true;
			}
		}
	}
	gs_texrender_destroy(rt);
	return ok;
}

static void beauty_chainout_cmp(struct beauty_filter *f, uint32_t cx, uint32_t cy)
{
	if (!f->chain_out_tex || !f->out_tex || !f->buf_out || !f->stage)
		return;
	const size_t need = (size_t)cx * cy * 4;
	uint8_t *a = (uint8_t *)malloc(need);
	uint8_t *b = (uint8_t *)malloc(need);
	if (!a || !b) {
		free(a);
		free(b);
		return;
	}

	// A：直绘路径（链纹理 + 不透明混合）
	bool ok = chainout_render_stage(f, f->chain_out_tex, true, a, cx, cy);

	// B：老路 —— 回读 → 强制 alpha → set_image → 按 SRC_ALPHA 绘制。
	//    刻意复用 buf_out / out_tex：本帧走的是直绘，它们本来就不会被用到。
	if (ok) {
		gs_stage_texture(f->stage, f->chain_out_tex);
		uint8_t *ptr = NULL;
		uint32_t ls = 0;
		ok = gs_stagesurface_map(f->stage, &ptr, &ls) && ptr;
		if (ok) {
			const size_t row = (size_t)cx * 4;
			if (ls == row) {
				memcpy(f->buf_out, ptr, need);
			} else {
				for (uint32_t y = 0; y < cy; y++)
					memcpy(f->buf_out + (size_t)y * row,
					       ptr + (size_t)y * ls, row);
			}
			gs_stagesurface_unmap(f->stage);
			uint32_t *p = (uint32_t *)(void *)f->buf_out;
			for (size_t k = 0; k < (size_t)cx * cy; k++)
				p[k] |= 0xFF000000u;
			gs_texture_set_image(f->out_tex, f->buf_out, cx * 4, false);
			ok = chainout_render_stage(f, f->out_tex, false, b, cx, cy);
		}
	}

	if (ok) {
		// 3.1.59：RGB 与 alpha 必须分开判 —— 这两件事问的不是同一个问题。
		//   RGB   —— 绘制路径（采样器/格式/sRGB/混合/矩阵）是否等价。这是门禁
		//            真正要断言的东西，必须是 0。
		//   alpha —— 两条路**故意**不同：老路把 alpha 强制成 0xFF，直绘透传源帧
		//            alpha。混在一起判会让任何带透明区的场景永远红（4K 实测
		//            75% 透明区，6,220,800 字节差异全部来自 alpha），门禁就废了。
		// 实测依据（3.1.58 21:19 那趟）：720p 相机铺满整帧 → 全不透明 →
		// maxdiff=0；4K 滤镜挂在场景源上、相机只占 25% → 75% 透明 → 只剩
		// alpha 差异。两条路 RGB 在构造上就相同（out_tex 是 chain_out_tex 的
		// memcpy 再 |=0xFF000000，只动 alpha 字节）。
		size_t maxd_rgb = 0, maxd_a = 0;
		size_t diffpx_rgb = 0, diffpx_a = 0, bad_b_alpha = 0;
		double sum_rgb = 0.0;
		uint32_t bx0 = cx, by0 = cy, bx1 = 0, by1 = 0;
		for (uint32_t y = 0; y < cy; y++) {
			const uint8_t *pa = a + (size_t)y * cx * 4;
			const uint8_t *pb = b + (size_t)y * cx * 4;
			for (uint32_t x = 0; x < cx; x++) {
				const uint8_t *qa = pa + (size_t)x * 4;
				const uint8_t *qb = pb + (size_t)x * 4;
				size_t d = 0;
				for (int c = 0; c < 3; c++) {
					const size_t dc = qa[c] > qb[c]
								  ? (size_t)(qa[c] - qb[c])
								  : (size_t)(qb[c] - qa[c]);
					if (dc > d)
						d = dc;
				}
				if (d) {
					diffpx_rgb++;
					sum_rgb += (double)d;
					if (d > maxd_rgb)
						maxd_rgb = d;
					if (x < bx0)
						bx0 = x;
					if (y < by0)
						by0 = y;
					if (x > bx1)
						bx1 = x;
					if (y > by1)
						by1 = y;
				}
				const size_t da = qa[3] > qb[3]
							  ? (size_t)(qa[3] - qb[3])
							  : (size_t)(qb[3] - qa[3]);
				if (da) {
					diffpx_a++;
					if (da > maxd_a)
						maxd_a = da;
				}
				// 不变量：B 是「回读 + 强制 alpha=0xFF」那条路，它的 alpha
				// 必须恒为 255。这条一旦破了说明 B 自己没按设计走，两条路
				// 的比较也就失去意义 —— 所以并入判定。
				if (qb[3] != 255)
					bad_b_alpha++;
			}
		}

		const size_t total_px = (size_t)cx * cy;
		const bool pass = (maxd_rgb == 0) && (bad_b_alpha == 0);
		blog(pass ? LOG_INFO : LOG_WARNING,
		     "[" PLUGIN_NAME "] 5b-1 绘制路径对拍 %ux%u：RGB maxdiff=%d "
		     "差异像素=%d (%.4f%%) | alpha 差异像素=%d (maxdiff=%d) | "
		     "B 非 255 像素=%d | 判定=%s"
		     "（A=直绘链纹理+不透明混合，B=回读+上传+SRC_ALPHA）",
		     cx, cy, (int)maxd_rgb, (int)diffpx_rgb,
		     total_px ? 100.0 * (double)diffpx_rgb / (double)total_px : 0.0,
		     (int)diffpx_a, (int)maxd_a, (int)bad_b_alpha,
		     pass ? "通过（RGB 逐像素一致）" : "失败");

		if (diffpx_rgb) {
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 5b-1 RGB 差异 bbox=(%u,%u)-(%u,%u) "
			     "平均偏差=%.6f —— RGB 不一致就是绘制路径有问题，"
			     "先别采信速度数字",
			     bx0, by0, bx1, by1,
			     diffpx_rgb ? sum_rgb / (double)diffpx_rgb : 0.0);
		} else if (diffpx_a) {
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 5b-1 alpha 差异属预期：直绘透传源帧 "
			     "alpha，老路强制 0xFF。本帧 %d 像素（%.2f%%）alpha 不同，"
			     "RGB 完全一致 → 绘制路径等价",
			     (int)diffpx_a,
			     total_px ? 100.0 * (double)diffpx_a / (double)total_px
				      : 0.0);
		}
	} else {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 5b-1 绘制路径对拍：渲染/stage 失败，"
		     "未得出结论（不影响画面，只是没验证到）");
	}
	free(a);
	free(b);
	f->chainout_reported = true;
}

// ---------------------------------------------------------------------------
// 投影矩阵作用域守卫（2.2.1）
// ---------------------------------------------------------------------------
// 为什么必须守住入口和出口：OBS 调滤镜的 video_render 时，栈上的投影是**画布**
// 投影（obs-video.c set_render_size → gs_ortho(0, 画布宽, 0, 画布高)），源像素到
// 画布像素的换算由场景项的 draw_transform 矩阵负责；obs-source.c 的
// render_filter_tex() 画最终结果时也**不设**投影，直接用 (0..w, 0..h) 的
// gs_draw_sprite。所以函数内部任何一次 gs_ortho(0,cx,0,cy) 只要没被弹回去，
// 就会让本帧剩余绘制（包括我们自己的第 5 步）按 画布/cx 放大，锚点是画布左上角。
// 画布 1920x1080 + 相机 1280x720 → 1.5 倍，正是用户报的「磨皮 0 正常、
// 磨皮 > 0 整幅放大」。详见 smooth_rt_begin() 上方的长注释。
//
// 有了这个守卫，即使将来某段新增代码又写了一次 gs_ortho 忘记还原，交回给 OBS
// 的投影也一定与进来时一致 —— 把「必须成对」从口头约定变成构造上成立。
struct beauty_projection_guard {
	beauty_projection_guard() { gs_projection_push(); }
	~beauty_projection_guard() { gs_projection_pop(); }
	beauty_projection_guard(const beauty_projection_guard &) = delete;
	beauty_projection_guard &operator=(const beauty_projection_guard &) = delete;
};

// ---------------------------------------------------------------------------
// S3（Q3）：单张暂存面的「map → memcpy → unmap」
// ---------------------------------------------------------------------------
// 抽出来是因为双缓冲回读要在**两张面**上轮流跑同一段逻辑（上一次提交的那张、
// 本帧新提交的那张）。顺带保住 2.4.2 起的两段计时：等待段（排队等 GPU 把
// 那趟拷贝做完）与拷贝段（memcpy 整帧，省不掉）。
// 失败返回 false —— 调用方必须当作「本帧没有新画面」处理，绝不能拿旧 buf_in 顶替。
static bool beauty_stage_readback(struct beauty_filter *f, gs_stagesurf_t *st,
				  uint32_t cx, uint32_t cy)
{
	if (!st)
		return false;
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	const double t_map = f->prof_on ? beauty_now_ms() : 0.0;
	if (!gs_stagesurface_map(st, &ptr, &linesize) || !ptr)
		return false;
	if (f->prof_on)
		f->prof_dl_wait += beauty_now_ms() - t_map;
	const double t_cp = f->prof_on ? beauty_now_ms() : 0.0;
	const size_t row = (size_t)cx * 4;
	if (linesize == row) {
		memcpy(f->buf_in, ptr, row * cy);
	} else {
		for (uint32_t y = 0; y < cy; y++)
			memcpy(f->buf_in + (size_t)y * row, ptr + (size_t)y * linesize, row);
	}
	if (f->prof_on)
		f->prof_dl_copy += beauty_now_ms() - t_cp;
	gs_stagesurface_unmap(st);
	return true;
}

// ---------------------------------------------------------------------------
// S4b（2.7.0）：GPU 侧 letterbox 缩略图
// ---------------------------------------------------------------------------
// 要产出的是**与 FaceDetector::letterboxRgb 同源**的一张 S×S 缩略图。三个参数
// 必须照抄那个函数的算式，因为引擎反算 full=(t-ox)/scale 就是它的逆：
//     scale = min(S/w, S/h)
//     nw    = (int)(w*scale + 0.5f)     nh = (int)(h*scale + 0.5f)
//     ox    = (S-nw)/2                  oy = (S-nh)/2
// 这里差 1 像素，到全帧上就是 1/scale（4K 下 6 像素）的整体偏移 —— 所以
// 这四个数**算完要回传给引擎**，不能让它自己猜。
//
// 采样用 OBS 内置 default effect（Linear + Clamp），与 CPU 的双线性同一族；
// 唯一的差异是 GPU 在（sRGB 格式下）线性空间插值、CPU 在 sRGB 空间插值，
// 表现为高对比边缘上的几个灰阶 —— 对检测是噪声级，验收时按「人脸框差 ≤1 px
// 缩略图尺度」判，不按逐像素相等判。
//
// 检测输入边长固定 640：YuNet 模型把输入声明成固定 640×640（见 beauty_core.cpp
// 的 kFaceInputDefault），换别的尺寸 ORT 直接拒绝，所以这里不暴露配置。
static const uint32_t kThumbSide = 640;

static bool beauty_ensure_thumb(struct beauty_filter *f, uint32_t side,
				gs_color_format fmt)
{
	if (f->thumb_rt && f->thumb_stage && f->thumb_buf &&
	    f->thumb_w == side && f->thumb_h == side && f->thumb_fmt == fmt)
		return true;
	if (f->thumb_rt) {
		gs_texrender_destroy(f->thumb_rt);
		f->thumb_rt = NULL;
	}
	if (f->thumb_stage) {
		gs_stagesurface_destroy(f->thumb_stage);
		f->thumb_stage = NULL;
	}
	if (f->thumb_buf) {
		bfree(f->thumb_buf);
		f->thumb_buf = NULL;
	}
	f->thumb_rt = gs_texrender_create(fmt, GS_ZS_NONE);
	f->thumb_stage = gs_stagesurface_create(side, side, fmt);
	f->thumb_buf = (unsigned char *)bzalloc((size_t)side * side * 4);
	f->thumb_w = f->thumb_h = side;
	f->thumb_fmt = fmt;
	if (!f->thumb_rt || !f->thumb_stage || !f->thumb_buf) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 检测缩略图资源不可用：本帧回退全帧回读"
		     "（不影响画面，只是慢一点）");
		return false;
	}
	return true;
}

// 2.11.0（方案C）：运动采样的边长。64 让引擎的 16×16 网格正好是「每格 4×4 像素」，
// 每格 16 个采样点 —— 与老路径的 256 个点采样同量级，但每一格是区域平均而不是
// 单点命中，噪声更低。回读量 64×64×4 = 16 KB，是 640×640 那张的 1/100。
static const uint32_t kMotionSide = 64;

static bool beauty_ensure_mot(struct beauty_filter *f)
{
	if (f->mot_rt && f->mot_stage && f->mot_stage_alt && f->mot_buf &&
	    f->mot_side == kMotionSide)
		return true;
	if (f->mot_rt) {
		gs_texrender_destroy(f->mot_rt);
		f->mot_rt = NULL;
	}
	if (f->mot_stage) {
		gs_stagesurface_destroy(f->mot_stage);
		f->mot_stage = NULL;
	}
	if (f->mot_stage_alt) {
		gs_stagesurface_destroy(f->mot_stage_alt);
		f->mot_stage_alt = NULL;
	}
	if (f->mot_buf) {
		bfree(f->mot_buf);
		f->mot_buf = NULL;
	}
	// 格式固定 RGBA8：运动判定只要亮度，不需要与源同格式（不做 sRGB 对称，
	// 因为这里比的是「相邻两帧的差」，任何单调映射都不改变判据的分离度）。
	f->mot_rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->mot_stage = gs_stagesurface_create(kMotionSide, kMotionSide, GS_RGBA);
	f->mot_stage_alt = gs_stagesurface_create(kMotionSide, kMotionSide, GS_RGBA);
	f->mot_buf = (unsigned char *)bzalloc((size_t)kMotionSide * kMotionSide * 4);
	f->mot_side = kMotionSide;
	f->mot_primed = false;
	f->mot_ping = 0;
	if (!f->mot_rt || !f->mot_stage || !f->mot_stage_alt || !f->mot_buf) {
		blog(LOG_WARNING,
		     "[" PLUGIN_NAME "] 运动采样资源不可用：本帧回退「每帧回读缩略图」"
		     "（不影响画面，只是没省到那 0.6~1.5 ms）");
		return false;
	}
	return true;
}

// 把 640×640 缩略图再缩到 kMotionSide×kMotionSide 并回读（16 KB），交给引擎算
// 运动网格。**只回读这一张小图**，那张 1.6 MB 的等 beautyCoreDetectDue() 说要才读。
static bool beauty_motion_sample(struct beauty_filter *f)
{
	static bool failed = false;
	auto fail = [](const char *why) {
		if (!failed) {
			failed = true;
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 运动采样不可用：%s → 退回每帧回读缩略图",
			     why);
		}
		return false;
	};
	if (!beauty_ensure_mot(f))
		return fail("ensure resources");
	gs_texture_t *src = gs_texrender_get_texture(f->thumb_rt);
	if (!src)
		return fail("thumb texrender texture null");
	gs_effect_t *fx = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = fx ? gs_effect_get_technique(fx, "Draw") : NULL;
	if (!tech)
		return fail("default effect Draw technique missing");
	const uint32_t S = f->mot_side;
	if (!smooth_rt_begin(f->mot_rt, S, S))
		return fail("texrender begin");
	gs_eparam_t *img = gs_effect_get_param_by_name(fx, "image");
	if (img)
		gs_effect_set_texture(img, src);
	// 与 beauty_thumb_build 同一形态：手动跑一遍 technique（外层可能已有
	// active technique，gs_effect_loop 会直接退出）。
	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(src, 0, S, S);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
	smooth_rt_end(f->mot_rt);

	gs_texture_t *tex = gs_texrender_get_texture(f->mot_rt);
	if (!tex)
		return fail("motion texrender texture null");
	/* 双缓冲：本帧的样本先**只排队**拷进 cur（gs_stage_texture 不等 GPU），
	 * 真正 map 的是上一帧那张 —— 那时它早已拷完，map 几乎不等待。
	 *
	 * 为什么非要这样：第一版是「本帧拷、本帧 map」，实测 tdl 从 0.85 掉到 0.05，
	 * 但**下载总量几乎不动**（720p 1.10→0.90、4K 2.08→2.03）。说明 0.7~0.9 ms
	 * 里占大头的是 map 的同步等待（要等 GPU 把本帧之前排的活干完），不是那
	 * 1.6 MB 的 memcpy（≈0.1 ms）。所以字节数不是瓶颈，同步点才是。
	 *
	 * 代价：运动信息晚一帧。判据「运动起始滞后 ≤+1 帧」因此可能变成 +2，
	 * 必须用运动素材实测（dper / 对齐%）再决定能不能用。 */
	gs_stagesurf_t *cur = f->mot_ping ? f->mot_stage_alt : f->mot_stage;
	gs_stagesurf_t *prev = f->mot_ping ? f->mot_stage : f->mot_stage_alt;
	gs_stage_texture(cur, tex);
	f->mot_ping ^= 1;
	if (!f->mot_primed) {
		// 首帧没有「上一份」可比 —— 交给调用方走全帧回读，一帧的代价。
		f->mot_primed = true;
		return false;
	}
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(prev, &ptr, &linesize) || !ptr)
		return fail("stage map");
	const size_t row = (size_t)S * 4;
	if (linesize == row) {
		memcpy(f->mot_buf, ptr, row * S);
	} else {
		for (uint32_t y = 0; y < S; y++)
			memcpy(f->mot_buf + (size_t)y * row,
			       ptr + (size_t)y * linesize, row);
	}
	gs_stagesurface_unmap(prev);
	beautyCoreSetMotionImage(f->mot_buf, (int)S, (int)S);
	// 2.11.1（诊断）：前 40 张存盘，便于离线看清「引擎到底吃到了什么」。
	if (f->mot_dump_dir[0] && f->mot_dump_i < 40) {
		char path[600];
		snprintf(path, sizeof(path), "%s/mot_%03d.raw",
			 f->mot_dump_dir, f->mot_dump_i);
		FILE *fp = fopen(path, "wb");
		if (fp) {
			fwrite(f->mot_buf, 1, row * S, fp);
			fclose(fp);
			f->mot_dump_i++;
		}
	}
	return true;
}

// 把当前 thumb_rt 的内容回读到 f->thumb_buf（S×S×4 = 1.6 MB）。
// 2.11.0（方案C）把这一步从 beauty_thumb_build 里拆出来：第一阶段只画不读，
// 第二阶段由宿主在「引擎说本帧要提交检测」之后单独调它。
static bool beauty_thumb_readback(struct beauty_filter *f)
{
	static bool failed = false;
	auto fail = [](const char *why) {
		if (!failed) {
			failed = true;
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 缩略图回读失败：%s → 本帧回退全帧路径",
			     why);
		}
		return false;
	};
	if (!f->thumb_rt || !f->thumb_stage || !f->thumb_buf)
		return fail("resources");
	const uint32_t S = f->thumb_w;
	const double t0 = f->prof_on ? beauty_now_ms() : 0.0;
	gs_texture_t *tex = gs_texrender_get_texture(f->thumb_rt);
	if (!tex)
		return fail("texrender texture null");
	gs_stage_texture(f->thumb_stage, tex);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(f->thumb_stage, &ptr, &linesize) || !ptr)
		return fail("stage map");
	const size_t row = (size_t)S * 4;
	if (linesize == row) {
		memcpy(f->thumb_buf, ptr, row * S);
	} else {
		for (uint32_t y = 0; y < S; y++)
			memcpy(f->thumb_buf + (size_t)y * row,
			       ptr + (size_t)y * linesize, row);
	}
	gs_stagesurface_unmap(f->thumb_stage);
	/* 取证用：BEAUTY_THUMB_DUMP=1 时把这张缩略图原样落盘一份（RGBA 裸数据）。
	 * 缩略图一旦有翻转 / 通道错 / 黑边偏移，检测会检出一堆假脸，但日志只报个
	 * 数字 —— 只有把图拿出来看才能一次定位。 */
	if (!f->thumb_dumped && getenv("BEAUTY_THUMB_DUMP")) {
		f->thumb_dumped = true;
		// 路径必须是纯 ASCII：fopen 走 ANSI 代码页，中文目录在这里直接失败。
		FILE *fp = fopen("C:/Users/Administrator/AppData/Local/Temp/thumb_dump.rgba",
				 "wb");
		if (fp) {
			fwrite(f->thumb_buf, 1, row * S, fp);
			fclose(fp);
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 缩略图已 dump：%ux%u scale=%.4f "
			     "ox=%d oy=%d",
			     S, S, f->thumb_scale, f->thumb_ox, f->thumb_oy);
		}
	}
	if (f->prof_on)
		f->prof_dl_thumb += beauty_now_ms() - t0;
	return true;
}
static bool beauty_thumb_build(struct beauty_filter *f, gs_texture_t *src,
			       uint32_t cx, uint32_t cy, bool read_back = true)
{
	// 失败原因只报一次：这条路每帧都会试一次，不收敛的理由会刷屏。
	auto fail = [](const char *why) {
		static bool once = false;
		if (!once) {
			once = true;
			blog(LOG_WARNING, "[" PLUGIN_NAME "] 缩略图不可用：%s", why);
		}
		return false;
	};
	if (!src || !cx || !cy)
		return fail("no src");
	const uint32_t S = kThumbSide;
	const gs_color_format fmt = gs_texture_get_color_format(src);
	// 只支持 32-bit 单像素格式（RGBA8 / BGRA8 及其 sRGB 变体）：
	// 16F/32F 回读出来是浮点，而引擎侧按 8-bit RGBA 读。
	// ⚠ gs_get_format_bpp 返回的是**位**不是字节（GS_RGBA = 32）。
	if (gs_get_format_bpp(fmt) != 32)
		return fail("format bpp != 32");
	if (!beauty_ensure_thumb(f, S, fmt))
		return fail("ensure resources");

	// 2.10.0 方案 B：输入旋转。检测缩略图按 input_rot 旋转后 letterbox 进 S×S，
	// 使检测在「旋转后的人脸」上跑；引擎在 buildMaskInto 出口把 FaceBox/kps 反
	// 变换回源坐标系。渲染/输出尺寸不变，用户对显示方向的场景项旋转照旧。
	const int rot = ((f->input_rot % 360) + 360) % 360;
	const bool swap = (rot == 90 || rot == 270);
	const uint32_t Wr = swap ? cy : cx;   // 旋转后全帧宽
	const uint32_t Hr = swap ? cx : cy;   // 旋转后全帧高
	const float scale = fminf((float)S / (float)Wr, (float)S / (float)Hr);
	const int nw = (int)((float)Wr * scale + 0.5f);
	const int nh = (int)((float)Hr * scale + 0.5f);
	if (nw <= 0 || nh <= 0 || nw > (int)S || nh > (int)S)
		return fail("bad letterbox rect");
	const int ox = ((int)S - nw) / 2;
	const int oy = ((int)S - nh) / 2;

	gs_effect_t *fx = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = fx ? gs_effect_get_technique(fx, "Draw") : NULL;
	if (!tech)
		return fail("default effect Draw technique missing");

	const double t0 = f->prof_on ? beauty_now_ms() : 0.0;
	// smooth_rt_begin 会把整张 S×S 清成黑 —— letterbox 的黑边就是这一步来的，
	// 不要另外再画一个黑底（多一趟 draw 没意义）。
	if (!smooth_rt_begin(f->thumb_rt, S, S))
		return fail("texrender begin");
	gs_viewport_push();
	gs_set_viewport(ox, oy, nw, nh);
	gs_ortho(0.0f, (float)nw, 0.0f, (float)nh, -100.0f, 100.0f);
	gs_eparam_t *img = gs_effect_get_param_by_name(fx, "image");
	if (img)
		gs_effect_set_texture(img, src);
	// 与 smooth_draw 同一形态：手动跑一个 technique，不用 gs_effect_loop
	// （外层可能已有 active technique，loop 会直接退出）。
	const size_t passes = gs_technique_begin(tech);
	const float sw = (float)cx * scale;   // 源按字母缩放后的宽
	const float sh = (float)cy * scale;   // 源按字母缩放后的高
	const float rad = (float)rot * (3.141592653589793f / 180.0f);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		if (rot == 0) {
			gs_draw_sprite(src, 0, (uint32_t)nw, (uint32_t)nh);
		} else {
			// 绕 letterbox 矩形中心旋转源（旋转后铺满 nw×nh），本段 letterbox
			// 计算用的就是旋转后尺寸 (Wr,Hr)，与上面 nw/nh 一致。
			gs_matrix_push();
			gs_matrix_translate3f((float)nw * 0.5f, (float)nh * 0.5f, 0.0f);
			// libobs 32.1.1 没有 gs_matrix_rotate3d / GS_AXIS_Z；绕 Z 轴旋转
			// 只有 gs_matrix_rotaa4f(x,y,z,angle)，angle 是弧度（matrix4_rotate_aa4f）。
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, rad);
			gs_matrix_translate3f(-sw * 0.5f, -sh * 0.5f, 0.0f);
			gs_draw_sprite(src, 0, (uint32_t)(sw + 0.5f), (uint32_t)(sh + 0.5f));
			gs_matrix_pop();
		}
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
	gs_viewport_pop();
	smooth_rt_end(f->thumb_rt);

	// 映射参数每帧都要更新（引擎反算几何要用），与「读不读」无关。
	f->thumb_scale = scale;
	f->thumb_ox = ox;
	f->thumb_oy = oy;
	// 画的那一段耗时先记；回读那一段由 beauty_thumb_readback 自己记。
	if (f->prof_on)
		f->prof_dl_thumb += beauty_now_ms() - t0;
	/* 2.11.0（方案C）：第一阶段只画不读 —— RT 里已是本帧缩略图，运动采样直接
	 * 拿它当输入；那 1.6 MB 的回读等 beautyCoreDetectDue() 说「要」再做。 */
	if (!read_back)
		return true;
	return beauty_thumb_readback(f);
}

static void beauty_filter_render(void *data, gs_effect_t *effect)
{
	struct beauty_filter *f = (struct beauty_filter *)data;
	obs_source_t *target = obs_filter_get_target(f->context);
	static int dbg = 0;

	// 投影守卫必须在**所有** return 之前构造：下面每一个 obs_source_skip_video_filter
	// 都会让父源用当前投影自己画一遍，投影被改坏就同样会放大。
	beauty_projection_guard proj_guard;

	// 进程级检测选项（识别模式 / 人物个数）。这两个 setter 在引擎里
	// 是进程全局的，只能从渲染线程调；UI 线程只置脏。
	if (f->dirty) {
		f->dirty = false;
		beautyEngine().setDetectOptions(0, f->landmark_q, f->max_faces);
		// 3.1.75：这两个下拉以前是空实现（引擎不存值），接上之后打一行留证。
		// 语义见 beauty_core.cpp 的 setDetectOptions：标准 conf=0.6 / 高精度 0.45。
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] 识别选项已下发：人物个数=%d 识别模式=%s"
		     "（conf=%.2f）",
		     f->max_faces, f->landmark_q ? "高精度" : "标准",
		     f->landmark_q ? 0.45 : 0.60);
	}

	if (!f->enabled || f->slot < 0 || !target) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);
	if (!cx || !cy) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	// ---- T7 定性（3.1.63）：同一个视频 tick 里本滤镜被调用了几次 --------
	// 为什么需要它：2026-11-01 05:19 那趟（美形开启）日志窗口从 6.000 s 变成
	// 3.000 s、`对齐` 从 100% 掉到 50%。两种解释都成立且处理完全不同：
	//   (a) 同一个 tick 渲染了两次（预览 + 输出，或某处重复调用）→ 白干一倍活；
	//   (b) 真的按 60 fps 出帧 → 那 30 fps 相机只喂得上一半的掩码。
	// `obs_get_total_frames()` 是 libobs 的全局已渲染帧计数：**同一个 tick 内的
	// 两次调用读到的是同一个值**。所以「按 tick 值分组，看一组里有几次调用」
	// 直接把 (a)/(b) 分开 —— 不用猜，也不依赖时间戳阈值。
	// 只在 BEAUTY_PROFILE=1 时记（它是 prof 窗口的一部分），平时零成本。
	if (f->prof_on) {
		const uint32_t tf = obs_get_total_frames();
		f->tick_calls++;
		// 首调用也要算一个 tick：tick_last 初值 0 可能与真实帧号 0 相等。
		if (tf != f->tick_last || f->tick_calls == 1) {
			f->tick_ticks++;
			f->tick_last = tf;
			f->tick_cur = 1;
		} else {
			f->tick_cur++;
		}
		if (f->tick_cur > f->tick_max)
			f->tick_max = f->tick_cur;
	}

	if (dbg < 3) {
		blog(LOG_INFO, "[" PLUGIN_NAME "] render#%d target=%s %ux%u (v%s, slot=%d)", dbg,
		     obs_source_get_name(target), cx, cy, PLUGIN_VERSION, f->slot);
		dbg++;
	}

	// ---- T2：同会话 A/B 切臂 -----------------------------------------------
	// 相位设计：chain_ab_total 与 prof_frames 在**同一处**递增（见帧末），
	// 窗口长 180 帧，所以 arm = (total/180)&1 在整个窗口内恒定 —— 打印那一
	// 行时绝不会跨臂。
	// 注意 `want` 在第一个窗口是 **false**，所以第一臂是「关」的那条
	// （1seg / rb）。生产态那条从第二个窗口才计入，反而避开了首窗口的
	// 预热偏差 —— 实测日志也确实是 1seg 开头（20-49-59.txt）。
	//
	// 只翻段区间钩子，**单段磨皮钩子两条臂都保持挂着**：1seg 臂就是靠它接管
	// 磨皮的，把它也摘掉就变成「3.1.48 全 CPU」了，那不是我们要比的 A/B。
	if (f->chain_ab) {
		const bool want = ((f->chain_ab_total / 180) & 1) != 0;
		if (f->chain_ab_out) {
			// 5b-1 的 A/B：只翻「链尾不回读」，段区间钩子两条臂都开着。
			// 钩子本身不用重挂，改个布尔即可，所以没有切换成本。
			f->chain_out = want;
		} else if (want != f->chain_prod) {
			f->chain_prod = want;
			beautyCoreSetChainHook(want ? beauty_chain_hook : NULL,
					       want ? f : NULL);
			// 切臂那一帧 chain_claim 留 0 → 帧末 `det_hist_ok` 自动置
			// false → 切回 chain 的首帧走 copyOnly 重置亮度历史。这条兜底
			// 是 3.1.52 就写好的（见 process 之后那段注释），这里不必额外
			// 清 det_hist，否则反而会多丢一帧的 EMA 历史。
		}
	}

	// (re)allocate on size change
	if (f->w != cx || f->h != cy || !f->tr) {
		f->w = cx;
		f->h = cy;
		free(f->buf_in);
		free(f->buf_out);
		f->buf_in = (uint8_t *)malloc((size_t)cx * cy * 4);
		f->buf_out = (uint8_t *)malloc((size_t)cx * cy * 4);
		if (f->stage) {
			gs_stagesurface_destroy(f->stage);
			f->stage = NULL;
		}
		if (f->stage_alt) {
			gs_stagesurface_destroy(f->stage_alt);
			f->stage_alt = NULL;
		}
		// S3：只有开着异步回读才建第二张面（4K 33 MB），并让 ping-pong 从
		// 「没有上一帧」的干净状态开始；尺寸一变，旧面里的内容一定不能再用。
		f->stage_submit = -1;
		// S4b：分辨率变了，上一帧的「全认领」判断不能再沿用（掩码几何、缩略图
		// 映射全按旧尺寸算的），第一帧老实回读全帧。
		f->thumb_prev_ok = false;
		if (f->tr)
			gs_texrender_destroy(f->tr);
		f->tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		f->stage = gs_stagesurface_create(cx, cy, GS_RGBA);
		if (f->stage_async)
			f->stage_alt = gs_stagesurface_create(cx, cy, GS_RGBA);
		if (f->out_tex) {
			gs_texture_destroy(f->out_tex);
			f->out_tex = NULL;
		}
		if (!f->tr || !f->stage || !f->buf_in || !f->buf_out) {
			blog(LOG_ERROR, "[" PLUGIN_NAME "] alloc failed %ux%u", cx, cy);
			obs_source_skip_video_filter(f->context);
			return;
		}
		f->verified = false;
		f->warm = false;   // 尺寸变了，引擎资源要按新尺寸重新预热
		if (!f->out_tex) {
			/* GS_DYNAMIC is MANDATORY here.
			 * gs_texture_set_image() is implemented on top of
			 * gs_texture_map(); the D3D11 backend maps via
			 * ID3D11DeviceContext::Map(), which only succeeds for
			 * D3D11_USAGE_DYNAMIC textures (created with GS_DYNAMIC).
			 * A plain DEFAULT texture makes Map() fail *silently*
			 * (no blog output), so gs_texture_set_image() becomes a
			 * no-op and the output texture keeps its uninitialised
			 * contents -> completely black video. */
			f->out_tex = gs_texture_create(cx, cy, GS_RGBA, 1, NULL, GS_DYNAMIC);
		}
	}

	// ==== 引擎资源预热（2.1.1，BUG 4「美颜开启瞬间抽搐」修复）=============
	// A 方案：把引擎资源的分配与「参数是否非 0」解耦。尺寸就绪（上面刚分配好
	// tr/stage/buf_in/buf_out）的**第一帧**就把资源建好 —— 哪怕这一帧参数全
	// 0、走的是透传。这样用户开启美颜的那帧只剩检测推理 + 正常处理，
	// 不再同时承担纹理创建，渲染线程不会被卡住几十毫秒。
	// 覆盖：skin/eye 纹理 + mask_buf（gray/mask）、磨皮四层 + sm_out、
	// 细节三层、链中间 rt、GPU 掩码对拍 rt —— 全是链路径上懒加载的大件。
	// 帧环不在此预热：它 opt-in，且每帧 put 前都自行 ensure（格式那时才拿得到）。
	// 2.2.0 增补：六个 effect（tone/smooth/detail/warp/mask/region）也在此加载。
	// gs_effect_create_from_file 在创建时就把 HLSL 编译完（编译失败在创建时
	// 就报错，不会拖到首次 draw），所以这一步同时把「首次 draw 的着色器编译
	// 耗时」从开启首帧挪走了。任一加载失败只 Warning，运行路径自带重试。
	// 代价：滤镜一启用就常驻一份显存（720p 约 45 MB / 4K 约 240 MB）。
	// 失败不阻断渲染：ensure 幂等，后续帧会自然重试。
	if (beautyEngine().supportsGpu() && !f->warm) {
		f->warm = true;   // 每尺寸只主动试一次；失败留给运行路径重试
		const bool tex_ok = beauty_ensure_gray(f, cx, cy) &&
				    beauty_ensure_mask(f, cx, cy) &&
				    beauty_ensure_smooth_rt(f) &&
				    beauty_ensure_detail_rt(f, cx, cy) &&
				    beauty_ensure_chain_rt(f, cx, cy) &&
				    beauty_ensure_mask_rt(f, cx, cy);
		const bool fx_ok = beauty_load_tone_effect(f) &&
				   beauty_load_smooth_effect(f) &&
				   beauty_load_detail_effect(f) &&
				   beauty_load_warp_effect(f) &&
				   beauty_load_mask_effect(f) &&
				   beauty_load_region_effect(f);
		if (!tex_ok || !fx_ok)
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 引擎资源预热未完全成功（textures=%d effects=%d）"
			     "；effects=0 表示装机 data 目录缺 shaders/*.hlsl，"
			     "GPU 各段将整段回落 CPU（约 12 ms/帧 @720p）",
			     (int)tex_ok, (int)fx_ok);
		else
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 引擎资源已预热 %ux%u（开启美颜不再卡首帧）",
			     cx, cy);
	}

	// 1) capture the upstream frame into our texrender.
	//
	//    This mirrors libobs obs_source_process_filter_begin_with_color_space()
	//    (obs-source.c) exactly. Two details matter and are easy to get wrong:
	//
	//    a) the blend state + gs_clear before drawing the source;
	//    b) WHICH render call to use.  obs_source_default_render() begins the
	//       "Draw" technique first and works for synchronous sources;
	//       asynchronous sources (cameras, capture cards) must keep using
	//       obs_source_video_render().
	gs_texrender_reset(f->tr); // texrender is one-shot: clear 'rendered' before re-begin
	const bool tr_ok = gs_texrender_begin(f->tr, cx, cy);
	if (tr_ok) {
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
					   GS_BLEND_INVSRCALPHA);

		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		obs_source_t *parent = obs_filter_get_parent(f->context);
		const uint32_t parent_flags = parent ? obs_source_get_output_flags(parent) : 0;
		const bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		const bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_blend_state_pop();
		gs_texrender_end(f->tr);
	}
	gs_texture_t *in_tex = gs_texrender_get_texture(f->tr);
	if (!in_tex || !f->out_tex) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	// 只在本帧 process 期间有效：GPU 磨皮回调靠它判断「引擎递来的帧是不是
	// 就是 in_tex 里那张」，是的话省一次整帧上传。帧末清掉。
	f->hook_cur_in = in_tex;

	// ==== GPU 渲染路径接缝（方案 A / Phase 2+）============================
	// 当 beautyEngine().supportsGpu() 为真时，本段「下载 → 引擎 → 上传」往返
	// 应被替换为：直接在 D3D11 着色器上对 in_tex / out_tex 计算，画面全程留
	// 显存，彻底消灭 GPU↔CPU 拷贝与同步等待（即 BEAUTY_PROFILE 的「下载/上传」项）。
	// 当前 Phase 1 的 GpuEngine 仍走下方 CPU 路径（process 是直通壳），仅标记
	// supportsGpu()=true 用于验证开关；真正着色器在 Phase 2–5 接入后启用此分支。
	// =======================================================================

	// S1 (Q1): 全 0 参数直通短路。verified 预热完成、未开镜像、且开关未关时，
	// 整段跳过 GPU→CPU→GPU 往返，最终直接画输入纹理 in_tex（等价于"滤镜禁用"）。
	BeautySettings s;
	beauty_filter_settings(f, s);
	const bool skip_sdk = s.allZero();
	const char *azs = getenv("BEAUTY_ALLZERO_SKIP");
	const bool allow_allzero_skip = !(azs && azs[0] == '0');
	const bool passthrough = allow_allzero_skip && skip_sdk && f->verified && !f->mirror;

	// 2) download to CPU  —— 直通时整段跳过
	// S3（Q3）：cpu_delayed = 本帧真的走了 ping-pong（CPU 侧因此晚一帧）。
	bool cpu_delayed = false;
	if (!passthrough) {
	bool got = false;
	const double t_dl = f->prof_on ? beauty_now_ms() : 0.0;
	/* S4b（2.7.0）：只回读缩略图这条路。四个条件缺一不可：
	 *   · detect_small   —— 开关（BEAUTY_DETECT_SMALL=0 回退全帧）
	 *   · gpu_mask_chain —— 掩码由 GPU 按几何重画，否则掩码就是 CPU 全帧画的，
	 *                       那些像素只存在于 buf_in 里，缩略图顶不上
	 *   · thumb_prev_ok  —— **上一帧全认领**。这是唯一的预测依据：本帧的 flags
	 *                       要等 process() 内部钩子跑完才知道，而回读发生在那之前
	 *   · !stage_async   —— S3 的延迟路径下引擎手上是 n−1 帧，而缩略图来自本帧，
	 *                       两者错一帧；两个 opt-in 开关互斥，见 9.5.3
	 * f->verified 是自检已完成（缩略图模式下 buf_in 是旧的，自检不能跑）。 */
	/* S4b：上一轮检测必须是**单脸**。缩略图模式下没有全帧像素，多脸时引擎
	 * 画不出那张跨脸的全帧掩码（buildMaskInto 直接返回 false）。不预判的话
	 * 就会「试一帧 → 跳过 → 回退全帧 → 再试」，画面每两帧掉一帧。 */
	int mkpx0 = -1, mkfaces0 = 0;
	beautyCoreMaskStat(&mkpx0, &mkfaces0);
	const bool want_thumb = f->detect_small && f->gpu_mask_chain &&
				f->gpu_ring_ok && f->thumb_prev_ok &&
				!f->stage_async && !f->mirror && f->verified &&
				mkfaces0 == 1;
	f->thumb_frame = false;
	f->nopix_frame = false;   // 2.11.0（方案C）：每帧复位，见下方两段式分支
	// S4b 一次性判据快照：缩略图路径「为什么会/不会生效」只看这一行。
	if (!f->thumb_dbg_done && f->verified) {
		f->thumb_dbg_done = 1;
		blog(LOG_INFO,
		     "[" PLUGIN_NAME "] S4b 判据：detect_small=%d mask_chain=%d "
		     "ring=%d prev_full=%d async=%d mirror=%d verified=%d",
		     f->detect_small ? 1 : 0, f->gpu_mask_chain ? 1 : 0,
		     f->gpu_ring_ok ? 1 : 0, f->thumb_prev_ok ? 1 : 0,
		     f->stage_async ? 1 : 0, f->mirror ? 1 : 0,
		     f->verified ? 1 : 0);
	}
	/* 2.11.0（方案C）：两段式。第一阶段只画缩略图（GPU，不读回）+ 采样 64×64
	 * 运动小图（16 KB），问引擎「本帧要不要检测输入」；要才回读那 1.6 MB。
	 * 不要的帧一个像素都不回读 —— buf_in 与今天的缩略图帧同语义（旧的），所以
	 * 照样要求「上一帧全认领」，认领不全就整帧跳过，代价与今天完全一样。 */
	bool thumb_ready = false;
	if (want_thumb && f->motion_gpu) {
		if (beauty_thumb_build(f, in_tex, cx, cy, /*read_back=*/false) &&
		    beauty_motion_sample(f)) {
			const bool due = beautyCoreDetectDue();
			beautyCoreClearMotionImage();
			if (due) {
				// 回读失败 → 落到下面的全帧路径；引擎那边已经把本帧
				// 记成「要提交」，全帧像素照样能喂给它，不会漏检测。
				thumb_ready = beauty_thumb_readback(f);
			} else {
				thumb_ready = true;
				f->nopix_frame = true;
				f->an_nopix++;
				f->mot_skipped++;
			}
		} else {
			beautyCoreClearMotionImage();
		}
	} else if (want_thumb) {
		thumb_ready = beauty_thumb_build(f, in_tex, cx, cy);
	}
	if (thumb_ready) {
		got = true;
		f->thumb_frame = true;
		f->an_thumb++;
		/* S4b1（2.7.1）：缩略图帧必须在 **process 之前**进显存环 ——
		 * 这是 2.7.0 那次 20 min 长跑里 tskip 的真正成因，不是抖动。
		 *
		 * 引擎给宿主的取帧帧号默认是**当前帧**（beauty_core.cpp：
		 * `srcTagThisFrame = curTag`），只有「掩码刚到」的那一帧才改成
		 * heldTag_（上一帧）。而掩码晚到（worker 没跟上）的帧上
		 * srcTag 就等于 curTag，同时 S4a 的放宽让 maskGeomOnly 顶过新鲜度
		 * 检查、gpu_mask_ok 仍为真 —— 于是钩子在 process 内部去环里取
		 * **当前帧**的纹理。2.7.0 把进环留在 process 之后，这一帧必然取不到，
		 * 落进「找不到纹理 → 整帧跳过」，跳过又把 thumb_prev_ok 置假，
		 * 下一帧再赔一次全帧回读。实测 tick=198 的窗口丢 18 帧（9%）。
		 *
		 * 挪到前面之后，这类帧与全帧路径**语义完全等价**：当前帧画面 +
		 * 上一帧几何，不跳、不拖影。拷贝总数不变 —— 原来「提交了新检测才
		 * 拷」，默认 maskEveryAsync=0 是每帧都提交，缩略图帧本来就每帧拷
		 * 一次，这里只是把顺序提前（ring_always 那档已在上面无条件拷过）。
		 * ⚠ frame_tag 要到下面「5b-2 切片 4」才 ++，此刻用 +1 预取本帧帧号；
		 * 缩略图路径要求 !stage_async，所以 cpu_tag 一定等于 frame_tag。 */
		if (f->gpu_ring_ok && !f->ring_always &&
		    beauty_ensure_ring(f, gs_texture_get_width(in_tex),
				       gs_texture_get_height(in_tex),
				       gs_texture_get_color_format(in_tex)))
			beauty_ring_put(f, f->frame_tag + 1, in_tex);
	} else {
		if (want_thumb && !f->thumb_fail_logged) {
			f->thumb_fail_logged = true;
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] 缩略图构建失败：本帧回退全帧回读"
			     "（画面正确，只是没省到那 2~3 ms）");
		}
		/* 2.10.1 的「输入方向不生效」WARN 已在 2.13.1 删除：P0-11 阶段 2 把旋转
		 * 挪进全帧检测路径的 letterbox 重采样之后，缩略图与全帧两条路都转了，
		 * 这个分支不再意味着失效。留着会变成假警报（开五官参数的家庭用户
		 * 每会话都会看到一条「旋转不会生效」）。 */
	/* S3：上一帧的暂存面在上一次 render 里就已提交，到这一刻那趟拷贝早已完成，
	 * 先 map 它几乎不等待；映射成功后再把本帧提交到**另一张**面。
	 * 顺序不能倒：gs_stagesurface_map 会排空命令队列 —— 先提交本帧的话，等到
	 * 的就是自己刚排进去的那趟拷贝（4K ≈2.5 ms），等于白做。 */
	gs_stagesurf_t *prev = NULL, *next = NULL;
	if (f->stage_async && f->stage_alt && f->stage_submit >= 0) {
		prev = f->stage_submit ? f->stage_alt : f->stage;
		next = f->stage_submit ? f->stage : f->stage_alt;
	}
	if (prev && next) {
		got = beauty_stage_readback(f, prev, cx, cy);
		if (got) {
			gs_stage_texture(next, in_tex);
			f->stage_submit = (next == f->stage_alt) ? 1 : 0;
			cpu_delayed = true;
		} else {
			f->stage_submit = -1;   // 读失败：下一帧退回同步路径
		}
	} else {
		gs_stage_texture(f->stage, in_tex);
		got = beauty_stage_readback(f, f->stage, cx, cy);
		f->stage_submit = (f->stage_async && f->stage_alt) ? 0 : -1;
	}
	}   // end S4b：走缩略图时不碰 f->stage

	/* 回读失败就直接直通，**不要**再喂一个上一帧的旧 buf_in 给引擎：
	 * 既有画面正确性问题（引擎会照着旧帧做人脸跟踪），也白白浪费一次
	 * 整帧处理。放在引擎调用之前判断。 */
	if (!got) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	if (f->prof_on)
		f->prof_dl += beauty_now_ms() - t_dl;
	}  // end if (!passthrough)：下载/回读/计时整段跳过

	// 5b-2 切片 4：帧号 + 把本帧原样拷进显存环。
	// 顺序要紧：帧号必须在 process 之前设好（引擎要把它和送检测的那一帧绑定）。
	// S2b：拷贝时机改成「默认 process 之后、且仅提交帧」——见 5050 附近的块。
	uint64_t ring_sub0 = 0;   // 本帧开始前的检测提交次数（判「本帧是否被送去检测」）
	unsigned long long cpu_tag = 0;   // 送给引擎的帧号（异步时 = frame_tag − 1）
	if (!passthrough) {
	f->frame_tag++;
	/* S3：异步回读时引擎手上是**上一帧**的像素，帧号必须跟着它走 ——
	 * 否则引擎会把「n−1 的掩码」盖在「n 的帧」上，heldTag_ 也指向错误的帧，
	 * 环取出来的纹理与掩码错位一帧（动态画面直接变拖影）。 */
	cpu_tag = cpu_delayed ? f->frame_tag - 1 : f->frame_tag;
	beautyCoreSetFrameTag(cpu_tag);
	/* S4b：把本帧的缩略图交给引擎 —— 像素来自 GPU 缩略图，几何仍是全帧。
	 * 必须在 process() 之前设、之后立刻清：任何一帧「忘了清」都会让下一帧
	 * 回退全帧时仍去读上一帧的缩略图，几何直接错一帧（4K 下 ≈6 px）。 */
	/* 2.11.0（方案C）：nopix 帧没有全帧像素、也没有缩略图，引擎在掩码到货那一帧
	 * 仍需知道「这不是有像素但没用上」——否则帧号对齐（3.1.10）会被跳过，
	 * 掩码与画面差一帧（静态看不出，动起来就是拖影）。 */
	beautyCoreSetNoPixelFrame(f->nopix_frame);
	if (f->thumb_frame && !f->nopix_frame)
		beautyCoreSetThumbSource(f->thumb_buf, (int)f->thumb_w,
					 (int)f->thumb_h, (int)cx, (int)cy,
					 f->thumb_scale, f->thumb_ox,
					 f->thumb_oy);
	else
		beautyCoreClearThumbSource();
	ring_sub0 = beautyCoreMaskSubmits();
	// 回退路径（BEAUTY_RING_ALWAYS=1）：保持 2.4.0 的老顺序，process 前无条件拷。
	if (f->ring_always && f->gpu_ring_ok &&
	    beauty_ensure_ring(f, gs_texture_get_width(in_tex),
			       gs_texture_get_height(in_tex),
			       gs_texture_get_color_format(in_tex)))
		beauty_ring_put(f, f->frame_tag, in_tex);
	} else if (f->gpu_ring_ok) {
		/* S2：直通期间不写环，但槽位里还留着**直通前那几帧**，且 tag 仍然
		 * 匹配。参数一旦从全 0 拉起来，引擎要的是更早那一帧（heldTag_），而
		 * frame_tag 在直通期间是冻结的 —— 旧帧会被当成那一帧取走，掩码区
		 * 闪一帧陈旧画面（真实摄像头下可见）。
		 * 把 tag 全清掉，让 beauty_ring_get() 必然落空，回退到「上传当前帧」
		 * 这条一直存在的安全路径；环在随后几帧内自动重新填满。零成本。 */
		for (int i = 0; i < BEAUTY_RING_SLOTS; i++)
			f->ring_tag[i] = 0;
	}
	if (passthrough) {
		/* S3：直通帧没有提交任何暂存面拷贝，ping-pong 的状态必须作废 ——
		 * 否则恢复非直通的第一帧会把「直通前那一帧」当成上一帧交给引擎
		 * （画面直接跳回几秒前），帧号也会跟着错一帧。下一帧走同步路径
		 * 重新起步，代价仅一帧。 */
		f->stage_submit = -1;
		/* S4b：直通期间环的 tag 被清空，恢复的第一帧必然取不到纹理。这里同时
		 * 把「上一帧全认领」作废，让恢复后的第一帧先老实回读一次全帧、把环
		 * 重新填满 —— 否则那一帧会因为 ring 落空而整帧跳过（白丢一帧）。 */
		f->thumb_prev_ok = false;
	}

	/* S3：CPU 侧晚一帧时，链要画的必须是**引擎手上那一帧**的纹理，也就是环里
	 * tag=cpu_tag 的那张。取不到就把 hook_cur_in 置空，让 beauty_hook_prepare
	 * 走「从 buf_in 上传」的老路 —— 慢几毫秒，但内容一定对（宁可慢，不可错帧）。 */
	if (cpu_delayed) {
		f->hook_cur_in = beauty_ring_get(f, cpu_tag);
		if (!f->async_logged) {
			f->async_logged = true;
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 暂存面双缓冲回读生效：CPU 侧整体晚一帧"
			     "（BEAUTY_ASYNC_STAGE=0 回退同步路径）");
		}
	}

	// 3) 引擎处理（自研核）。
	//
	// 全 0 参数时引擎输出与输入逐字节一致，直接透传，省掉一整个 CPU 往返
	// （含一次 BGRA 双换和一次 render 调用）。
	// S1 (Q1): s / skip_sdk / passthrough 已在下载前入口分支（见上方 ~4791 行）算好，
	// 此处不再重复定义，直接复用 skip_sdk 决定是否跳过引擎 process。
	const double t_proc = f->prof_on ? beauty_now_ms() : 0.0;

	if (!skip_sdk) {
		// 3.1.52：帧前清认领标记。钩子在本帧 process() 内部写它，process()
		// 返回后据此判断「细节段的亮度历史还连不连续」。
		f->chain_claim = 0;
		// 5b-1：每帧重置。只有钩子本帧真的走了不回读路径才会置位，
		// 否则下面一律走「回读 + 上传」的老路（画面与 3.1.57 逐字节相同）。
		f->chain_out_valid = false;
		f->chain_out_tex = NULL;
		const bool ok = beautyEngine().process(f->slot, f->buf_in, f->buf_out,
						   (int)cx, (int)cy, s);
		if (!ok) {
			// 每实例只报一次：恢复后清零，下次故障还会再报。
			if (!f->err_reported) {
				f->err_reported = true;
				blog(LOG_ERROR, "[" PLUGIN_NAME "] 引擎处理失败：%s（本实例直通）",
				     beautyEngine().lastErr());
			}
			obs_source_skip_video_filter(f->context);
			return;
		}
		f->err_reported = false;
		// S4b：缩略图的生命周期只到 process() 为止（异步路径已在 submitAsync
		// 里 memcpy 进输入槽）。留着不清会让下一次回退全帧的帧读错几何。
		beautyCoreClearThumbSource();
		/* S4b：走了缩略图，但本帧 flags 没被全部认领 —— 有 CPU 段要跑，而
		 * buf_in 里是上一帧的旧像素（本帧根本没回读全帧）。这时不能上传、
		 * 也不能画 buf_out 里的半成品：整帧跳过，画面原样。下一帧
		 * thumb_prev_ok 自动为假 → 回退全帧回读，代价只有这一帧。 */
		if (f->thumb_frame && !f->chain_out_valid) {
			f->an_thumbskip++;
			if (!f->thumb_skip_logged) {
				f->thumb_skip_logged = true;
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME "] 缩略图帧没被全认领："
				     "chain_claim=0x%x chain_out=%d → 本帧跳过",
				     f->chain_claim, f->chain_out ? 1 : 0);
			}
			/* 必须在这里作废「上一帧全认领」：本帧一条 CPU 段都没认领成功，
			 * 说明缩略图这条路当前不可用（典型是多脸）。不作废的话下一帧
			 * 还会照样只回读缩略图、再跳一次 —— 画面每两帧掉一帧。 */
			f->thumb_prev_ok = false;
			obs_source_skip_video_filter(f->context);
			return;
		}
		/* S4b：下一帧能不能只回读缩略图，取决于**本帧**是否被全部认领。
		 * 放在这里是因为 chain_out_valid 到此已经定稿（钩子写、process 后不再变）。 */
		f->thumb_prev_ok = f->chain_out_valid;
		// 细节段这一帧没跑在 GPU 上（面板没开锐化 / 回退 CPU）→ 显存里的
		// 亮度历史已经落后，下一帧必须用当前帧重置。漏了这一步，回退那一帧
		// 之后的画面会带着几帧前的亮度做 EMA，表现为细纹理轻微「拖影」。
		if (!(f->chain_claim & BEAUTY_SEG_DETAIL))
			f->det_hist_ok = false;

		// C20：肤色自适应首次估计收效后补打一行 skin=。init 那行永远是
		// "(center=default, not yet estimated)"，真机自查「第 2 帧以后真的
		// 在估计」全靠这一行 —— worker 一次性置位，这里取走即清，之后每帧零开销。
		if (beautyCoreSkinAnnounce())
			blog(LOG_INFO, "[" PLUGIN_NAME "] %s", beautyCoreSkinInfo());
	} else if (!passthrough) {
		memcpy(f->buf_out, f->buf_in, (size_t)cx * cy * 4);
	}
	if (f->prof_on) {
		f->prof_proc += beauty_now_ms() - t_proc;

		// 分项计时（3.1.47）。紧跟在引擎耗时之后取，保证与同一次 process() 对齐；
		// 六项之和略小于 prof_proc，差额是分支判断与循环骨架。
		double seg[6];
		beautyCoreSegMs(seg);
		for (int i = 0; i < 6; i++)
			f->prof_seg[i] += seg[i];

		// 掩码统计（3.1.44）。beautyCoreMaskStat 是纯读、O(1)：两个数本来就在
		// 引擎里（同步与异步两条路径都会写），所以这里既不扫掩码也不加锁。
		// px = -1 表示这一帧没有有效掩码（没检出脸 / 模型未就绪），不计入覆盖率
		// 分母之外的任何处理 —— 只是把「有脸帧」判为 0。
		int mkpx = -1, mkfaces = 0;
		beautyCoreMaskStat(&mkpx, &mkfaces);
		if (mkpx > 0 && cx > 0 && cy > 0) {
			double cov = (double)mkpx / ((double)cx * (double)cy);
			if (cov > 1.0) cov = 1.0;   // 掩码尺寸与帧尺寸短暂不一致时夹住
			f->prof_cover += cov;
		}
		if (mkfaces > 0)
			f->prof_face_frames++;

		// ROI 包围盒占比（3.1.48）。同一个 O(1) 探针的第三个返回值：它回答的是
		// 「surface3RgbBand 收了 roi 却不用」到底浪费了多少 —— 理论可省上限 = 1-本值。
		// -1 的帧（无掩码）整帧跳过，连分母都不进，否则平均值会被拉低。
		const double bcov = beautyCoreBoundsCover();
		if (bcov >= 0.0) {
			double b = bcov;
			if (b > 1.0) b = 1.0;
			f->prof_bounds += b;
			f->prof_bounds_n++;
		}
	}

	// ==== Phase 2 gated 自检：GPU 色调对拍（不改 buf_out / out_tex）==========
	// BEAUTY_GPU_TONE=1 且引擎宣称 supportsGpu() 时才进来，且只跑一次。
	// 放在这里的原因：buf_in（原图）还在、引擎本帧的掩码刚到货、而上传回
	// out_tex 之前 —— 三个条件同时满足的唯一位置。
	if (f->gpu_tone_diag && !f->gpu_tone_reported && !cpu_delayed)
		beauty_gpu_tone_selfcheck(f, in_tex, cx, cy);
	// ==== Phase 3 gated 自检：GPU 磨皮对拍（同样不改 buf_out / out_tex）======
	// 位置与色调自检完全相同，理由也相同：buf_in 未被动过、本帧掩码已到货、
	// 结果还没上传回 out_tex。磨皮对拍还多一个条件 —— 引擎本帧必须真的走了
	// band 路径（beautyCoreBandDiag 返回 true），否则没有可比的对象。
	if (f->gpu_smooth_diag && !f->gpu_smooth_reported && !cpu_delayed)
		beauty_gpu_smooth_selfcheck(f, in_tex, cx, cy);
	// ==== T1 gated 自检：GPU 细节对拍（同样不改 buf_out / out_tex）==========
	// 位置同上：buf_in 未动、本帧掩码已到货、结果还没上传回 out_tex。
	// 额外条件：本帧链钩子必须认领过 DETAIL（f->diag_detail_ok），否则没有 dp。
	if (f->gpu_detail_diag && !f->gpu_detail_reported && !cpu_delayed)
		beauty_gpu_detail_selfcheck(f, in_tex, cx, cy);
	// ==== 5c gated 自检：GPU 美形对拍（同样不改 buf_out / out_tex）==========
	// 位置同上。额外条件：本帧链钩子必须认领过 WARP（f->diag_warp_ok），
	// 否则手上没有引擎算好的几何 —— 宿主自己推不出来（关键点只在引擎里）。
	// 它用的是自己的一对 scratch 纹理，不碰生产链的 warp_a/warp_b：
	// 5b-1 直绘时 chain_out_tex 可能就是 warp_a，覆盖它等于改画面。
	if (f->gpu_warp_diag && !f->gpu_warp_reported && !cpu_delayed)
		beauty_gpu_warp_selfcheck(f, in_tex, cx, cy);
	// =======================================================================

	// 4) 上传（5b-1 直绘 / S1 全0直通 时整段跳过：直通帧直接画 in_tex）
	//
	// 老路把 buf_out 传进 out_tex 再画。这条路径顺带干三件与画质相关的事
	// （强制不透明、首帧上传自检、镜像翻转），所以只在「本帧链没交出纹理」
	// 时才走。
	//
	// 新路（5b-1）：链钩子已经把最终结果留在显存里，且本帧 flags 全被认领
	// —— dst 一个字节都没写，buf_out 还是上一帧的残留。这里什么都不做，
	// 直接画那张纹理。省掉 map + memcpy + 整帧 alpha 遍历 + set_image。
	const bool chain_direct = f->chain_out_valid && f->chain_out_tex != NULL;
	// 一次性绘制路径对拍（BEAUTY_CHAINOUT_CMP=1）。**必须只在直绘帧跑**：
	// 它拿 buf_out / out_tex 当老路的临时容器，而直绘帧这两者本来就不参与
	// 绘制，所以不会污染画面。
	//
	// 3.1.60：加 `frame_id >= 180` 的下限。5b-1 转默认后，直绘帧从开机第
	// 1~2 帧就成立，而那时人脸还没检测出来、掩码是空的 —— 链退化成近似
	// 直通，两条路比出来的东西几乎没有信息量，门禁会**平凡通过**。等到
	// 180 帧（≈3 s @60fps，正好一个日志窗口）再比，那时链已经真的在算，
	// 比较才有意义。带 A/B 跑时这条不起作用（第一臂本来就到 180 帧才切）。
	if (chain_direct && f->chainout_cmp && !f->chainout_reported &&
	    f->frame_id >= 180)
		beauty_chainout_cmp(f, cx, cy);
	const double t_up = f->prof_on ? beauty_now_ms() : 0.0;
	double detail_in_check = 0.0, detail_out_check = 0.0;
	if (!chain_direct && !passthrough) {   // 直通帧：直接画 in_tex，跳过整段上传
		// the engine may leave alpha at 0 in areas it did not touch; force opaque
		//
		// 3.1.21：改成按 32 位整像素写。原来是每隔 4 字节写一次的单字节循环，
		// 1080p 是 200 万次 strided 写，每帧白花 2~3 ms；整像素 OR 之后访存变成
		// 连续，同样的结果只花 0.4 ms。RGBA 里 A 是第 4 个字节，小端 32 位读出来的
		// 就是 0xAABBGGRR —— 所以置位用的是最高字节 0xFF000000。
		//
		// 5b-1 直绘时这趟遍历也一并省掉：链的最终 pass 写出的 alpha 就是
		// 源帧的 alpha（smooth.hlsl / detail.hlsl 都是 `return float4(..., src.a)`），
		// 所以直绘改用「不透明混合」来复现这里强制 0xFF 的效果（见下面的 draw）。
		{
			uint32_t *p = (uint32_t *)(void *)f->buf_out;
			const size_t px = (size_t)cx * cy;
			for (size_t k = 0; k < px; k++)
				p[k] |= 0xFF000000u;
		}

		// The first self-check compares the engine output before display transforms.
		// Save both metrics before buf_in is reused as the rotation scratch buffer.
		if (!f->verified && !skip_sdk) {
			detail_in_check = frame_detail(f->buf_in, cx, cy);
			detail_out_check = frame_detail(f->buf_out, cx, cy);
		}

		if (f->mirror)
			frame_mirror(f->buf_out, cx, cy);

		gs_texture_set_image(f->out_tex, f->buf_out, cx * 4, false);
	}
	// ==== S2b：帧环拷贝按需（4K 每帧省一次 33 MB 的 gs_copy_texture）========
	// 只有「本帧被送去检测」才需要把这一帧留在环里：对齐换帧要的 heldFrame_
	// 必然来自 submitAsync 的那一帧（beauty_core.cpp 里用当时的 g_frameTag 盖戳），
	// 其余帧链直接采样 in_tex（beauty_hook_prepare：src==buf_in → hook_cur_in），
	// 环里那份从来没被读过 —— 2.4.0 之前是每帧白拷一次。
	// 必须放在 process 之后：提交动作发生在 process 内部，之前无从判断。
	// 判空落空时的代价只是「从 CPU 上传那一帧」（4K ≈3 ms），画面不受影响。
	//
	// S3 例外：异步回读时链要画的是**上一帧**，那张纹理只能来自环 ——
	// 于是每帧都必须留一份（按需优化在这里退化为「全部需要」）。这一趟
	// 拷贝是排进 GPU 队列的，宿主不会等它（下一帧 map 时它早已完成），
	// 所以它不体现在往返里；换来的是上面省掉的 map 排队等待。
	if (!passthrough && f->gpu_ring_ok && !f->ring_always && !f->thumb_frame &&
	    (f->stage_async || beautyCoreMaskSubmits() != ring_sub0)) {
		if (beauty_ensure_ring(f, gs_texture_get_width(in_tex),
				       gs_texture_get_height(in_tex),
				       gs_texture_get_color_format(in_tex)))
			beauty_ring_put(f, f->frame_tag, in_tex);
		if (!f->ring_skip_logged && !f->stage_async) {
			f->ring_skip_logged = true;
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 帧环拷贝改按需：仅检测提交帧留环，"
			     "其余帧跳过整帧拷贝（BEAUTY_RING_ALWAYS=1 回退老行为）");
		}
	}
	f->hook_cur_in = NULL;   // 仅供本帧 process 期间的磨皮回调复用

	// 性能埋点输出（BEAUTY_PROFILE=1）。每 180 帧（60fps 下 3 秒）报一次，
	// 免得刷爆日志。这三个数决定了「要不要整体改走 GPU」：
	// 下载那一项含 stagesurface map 的同步等待，是 GPU→CPU 的真实代价。
	if (f->prof_on) {
		f->prof_up += beauty_now_ms() - t_up;
		f->prof_frames++;
		// S7（2.8.0）：按窗口取极值。周期用 max（静止 8 / 运动 1，max 能证明
		// 静止段真的降了频），运动量也用 max（能标定阈值够不够）。
		{
			const int per = beautyCoreMaskPeriod();
			if (per > (int)f->dper_max)
				f->dper_max = (uint32_t)per;
			const int mo = beautyCoreMaskMotion();
			if (mo > f->dmot_max)
				f->dmot_max = mo;
		}
		// T2：与 prof_frames 同处递增 —— 这是切臂相位不漂的唯一保证。
		// 放在 prof 块内是刻意的：prof 关着就没有日志窗口，A/B 也无从统计，
		// 此时计数不涨、臂恒为 chain（= 生产行为），是个安全的退化。
		if (f->chain_ab)
			f->chain_ab_total++;
		if (f->prof_frames >= 180) {
			const double n = (double)f->prof_frames;
			const double dl = f->prof_dl / n;
			const double pr = f->prof_proc / n;
			const double up = f->prof_up / n;
			// 掩码↔帧对齐比率（3.1.10）：探针返回的是启动以来的全量值，
			// 取差值才是本窗口的。首个窗口（基线还是 0）显示的是从启动
			// 到现在的累计比率 —— 含义一样，只是窗口更长。
			uint64_t al_used = 0, al_total = 0;
			beautyCoreAlignStat(&al_used, &al_total);
			const uint64_t al_du = al_used - f->prof_align0_used;
			const uint64_t al_dt = al_total - f->prof_align0_total;
			f->prof_align0_used = al_used;
			f->prof_align0_total = al_total;
			// 3.1.48：ROI 包围盒占比追加在**行尾**。刻意不插在「掩码」和「有脸」
			// 之间 —— 05_验证/phase0_parse_log.py 的 MASK_RE 是按「掩码 X% 有脸 Y%」
			// 紧邻匹配的，插进中间会让那两列静默变成 None（不报错，只是数据缺失，
			// 这种失真最难发现）。追加到末尾则对旧正则完全透明。
			char btxt[32];
			if (f->prof_bounds_n > 0)
				snprintf(btxt, sizeof(btxt), "%.1f%%",
					 f->prof_bounds * 100.0 / (double)f->prof_bounds_n);
			else
				snprintf(btxt, sizeof(btxt), "--");
			// 3.1.57 / T2：行尾再追加 `ab=...`（本窗口跑的哪条臂）。
			// 仍然遵守「新 token 只准追加在行尾」的规矩：BOUNDS_RE 是
			// `边框\s+([\d.]+)%`，后缀再多也不影响它。
			// 3.1.58：多了 out / rb 这一对（BEAUTY_AB_TARGET=out 时翻 5b-1）。
			// 3.1.62 修正口径：`ab=` 问的是「本窗口属于哪条**臂**」，不是
			// 「走的哪条**路**」。A/B 没开时根本不存在两条臂，必须打 `--`；
			// 3.1.57~3.1.61 打的是 `chain`（因为那只是 chain_prod 的值），
			// 解析脚本会把它当成 A/B 窗口，然后报告「只有单臂、无法比较」——
			// 结论碰巧是对的，但那是**误判出来的对**，换一天就会骗人。
			const char *abtxt =
				(!f->gpu_smooth_prod || !f->chain_ab)
					? "--"
					: f->chain_ab_out
						  ? (f->chain_out ? "out" : "rb")
						  : (f->chain_prod ? "chain" : "1seg");
			// 3.1.63 / T7：再追加 `tick=调用数/tick数 dup=单tick最多调用数`。
			// 排在 ab= 之后是为了不打断 AB_RE（`ab=(chain|1seg|out|rb)`）和
			// 一切按行尾取 ab 的旧脚本；`tick=` 自己也是新 token，只追加。
			// 读法：dup=1 且 调用数≈tick数 → 一个 tick 只渲染一次（正常）；
			//       dup=2 → 同一个 tick 被渲染两次，(a) 成立；
			//       调用数≈tick数 但窗口时间减半 → 真的按 60 fps 出帧，(b) 成立。
			blog(LOG_INFO,
			     "[" PLUGIN_NAME "] 性能 %ux%u：下载 %.2f + 引擎 %.2f + 上传 %.2f"
			     " = %.2f ms/帧（%d 帧平均）掩码 %.2f%% 有脸 %.0f%%"
			     " 对齐 %llu/%llu（%.0f%%）边框 %s ab=%s"
			     " tick=%llu/%llu dup=%d"
			     " anom=nofresh%u/masknull%u/histrst%u/claimdrop%u"
			     " dlw=%.2f dlm=%.2f gonly=%u gskip=%u tsm=%u tskip=%u tdl=%.2f"
			     " dper=%u dmot=%d",
			     cx, cy, dl, pr, up, dl + pr + up, f->prof_frames,
			     f->prof_cover * 100.0 / n,
			     (double)f->prof_face_frames * 100.0 / n,
			     (unsigned long long)al_du, (unsigned long long)al_dt,
			     al_dt ? (double)al_du * 100.0 / (double)al_dt : 100.0, btxt,
			     abtxt, (unsigned long long)f->tick_calls,
			     (unsigned long long)f->tick_ticks, f->tick_max,
			     f->an_nofresh, f->an_masknull, f->an_histrst,
			     f->an_claimdrop,
			     f->prof_dl_wait / n, f->prof_dl_copy / n,
			     f->an_geomonly, f->an_geomskip,
			     f->an_thumb, f->an_thumbskip, f->prof_dl_thumb / n,
			     f->dper_max, f->dmot_max);
			// 打印完清零，下一窗口重新统计
			f->an_nofresh = f->an_masknull = 0;
			f->an_histrst = f->an_claimdrop = 0;
			f->an_geomonly = f->an_geomskip = 0;
			f->an_thumb = f->an_thumbskip = 0;
			f->dper_max = 0;
			f->dmot_max = 0;
			f->prof_dl_thumb = 0.0;
			/* 2.11.0（方案C）：本窗口「预判不提交、因此没回读检测输入」的帧数。
			 * 单独一行 + 字符串拼接：不动上面那条 printf 格式串（blog 不受
			 * 编译器格式检查保护，3.1.4 就是因为多一个 %s 崩了两次）。 */
			if (f->motion_gpu) {
				int mhot = 0, mcalls = 0;
				beautyCoreMotionStat(&mhot, &mcalls);
				const uint64_t sub_now = beautyCoreMaskSubmits();
				const uint64_t sub_win =
					(sub_now >= f->an_sub0) ? sub_now - f->an_sub0 : 0;
				f->an_sub0 = sub_now;
				std::string mm = "方案C 窗口 " + std::to_string(cx) +
						 "x" + std::to_string(cy) +
						 "：nopix=" +
						 std::to_string(f->an_nopix) + "/" +
						 std::to_string(f->prof_frames) +
						 " 判动=" + std::to_string(mhot) + "/" +
						 std::to_string(mcalls) +
						 // 2.12.0（P1-1c）：口径必须可见。同一个
						 // 「判动=3/180」在 max 与 rms 下含义完全不同，
						 // 不打出来就没法判断这一次跑的是哪一档。
						 " 聚合=" +
						 (beautyCoreMotionAggRms() ? "rms" : "max") +
						 " 提交=" + std::to_string(sub_win) +
						 " 累计省 " +
						 std::to_string(
							 (unsigned long long)f->mot_skipped) +
						 " 帧";
				blog(LOG_INFO, "[" PLUGIN_NAME "] %s", mm.c_str());
				f->an_nopix = 0;
			}

			// 分项单独一行：把它和总耗时写在同一行会让本来就长的性能行更难读，
			// 而这两行是配套看的（差值应该很小，否则说明埋点位置错位）。
			{
				std::string sm = "seg 分项 " + std::to_string(cx) + "x" +
						 std::to_string(cy) + "：";
				double seg_sum = 0.0;
				for (int i = 0; i < 6; i++) {
					const double v = f->prof_seg[i] / n;
					seg_sum += v;
					if (i)
						sm += " + ";
					sm += beautyCoreSegLabel(i);
					char b[32];
					snprintf(b, sizeof(b), " %.2f", v);
					sm += b;
				}
				char tot[128];
				snprintf(tot, sizeof(tot),
					 " = %.2f ms（引擎 %.2f，差 %.2f）", seg_sum, pr,
					 pr - seg_sum);
				sm += tot;
				// Phase 3.5：GPU 磨皮接管率（本窗口差值）。calls 只在引擎真的
				// 走到 band 磨皮段的帧上涨，所以面板关掉磨皮时这里是 0/0。
				// 用 ASCII "gpusm=" 做锚点，与 "seg " 同一套理由（装机脚本只能
				// 认 ASCII token）。
				{
					long long hh = 0, hc = 0;
					beautyCoreSmoothHookStat(&hh, &hc);
					const long long dh = hh - f->hook_hit0;
					const long long dc = hc - f->hook_call0;
					f->hook_hit0 = hh;
					f->hook_call0 = hc;
					if (dc > 0) {
						char gb[64];
						snprintf(gb, sizeof(gb),
							 " gpusm=%lld/%lld(%.0f%%)",
							 dh, dc,
							 (double)dh * 100.0 / (double)dc);
						sm += gb;
					}
				}
				// 3.1.52：段区间钩子的认领分布。`chain=命中/询问` 之后跟五段
				// 的窗口增量（顺序：磨皮 色调 五官 细节 美形）。
				// 健康形态：磨皮=色调=细节=命中数，五官/美形=0（这两段还没有
				// GPU 版本，是后续 Phase 的事）。若磨皮明显小于命中数，说明
				// 引擎判了「非前缀」把整份认领丢弃了 —— 那时画面仍然正确，
				// 只是白跑一趟 GPU，看这一行就能发现。
				{
					long long hh = 0, hc = 0;
					unsigned long long sg[5] = {0, 0, 0, 0, 0};
					beautyCoreChainStat(&hh, &hc, sg);
					const long long dh = hh - f->chain_hit0;
					const long long dc = hc - f->chain_call0;
					f->chain_hit0 = hh;
					f->chain_call0 = hc;
					if (dc > 0) {
						unsigned long long d0[5];
						for (int i = 0; i < 5; i++) {
							d0[i] = sg[i] - f->chain_seg0[i];
							f->chain_seg0[i] = sg[i];
						}
						char cb[128];
						snprintf(cb, sizeof(cb),
							 " chain=%lld/%lld S/T/R/D/W=%llu/%llu/"
							 "%llu/%llu/%llu",
							 dh, dc, d0[0], d0[1], d0[2], d0[3],
							 d0[4]);
						sm += cb;
					}
				}
				// 3.1.50：磨皮段三段细分 `sm=提交/回读/拷贝`。同样追加在行尾，
				// 对既有正则透明。n= 是三段都成功的帧数 —— 它应当接近 gpusm
				// 的命中数，若明显偏小说明有帧在 map 阶段失败回退了。
				//
				// ⚠ 3.1.52 起如果 chain 生效，`map` 等的是**整条链**的 GPU
				// 时间（磨皮+色调+细节），不再只是磨皮回读；`draw` 仍是磨皮
				// 那 23 次提交。两条路径的 sm= 不能直接横比，必须看 chain=。
				if (f->prof_sm_n > 0) {
					const double sn = (double)f->prof_sm_n;
					char sb[160];
					// 3.1.51：追加 `prep=准备/掩码`。准备段 = 整帧上传（对齐
					// 换帧时才有）+ 掩码两张纹理上传；掩码那项是准备段里的一
					// 部分，单独列出来是为了验证「去掉 CPU 交错」到底省了多少。
					// 3.1.52：再追加 `ch=色调/细节`（链上后两段的**提交**时间，
					// 与 draw 一样只量 CPU 侧建命令，GPU 计算计入 map）。
					// 3.1.62：追加 `wr=`（美形各趟的提交时间）。刻意**不动**
					// ch= 的字段数：03_装机 与 05_验证 的解析脚本都按
					// `ch=a/b` 抓，改成 a/b/c 会让老脚本静默少抓一个数。
					snprintf(sb, sizeof(sb),
						 " sm=%.2f/%.2f/%.2f(n=%d) prep=%.2f/%.2f"
						 " ch=%.2f/%.2f wr=%.2f",
						 f->prof_sm_draw / sn,
						 f->prof_sm_map / sn,
						 f->prof_sm_copy / sn, f->prof_sm_n,
						 f->prof_sm_prep / sn,
						 f->prof_sm_mask / sn,
						 f->prof_ch_tone / sn,
						 f->prof_ch_detail / sn,
						 f->prof_ch_warp / sn);
					sm += sb;
				}
				// 行首的 "seg " 是**故意**的 ASCII 锚点：03_装机 下的 .ps1 必须
				// 纯 ASCII（PS 5.1 按 ANSI 解码无 BOM 文件，中文会乱码），所以
				// 脚本只能靠这个 ASCII token 来定位分项行。
				blog(LOG_INFO, "[" PLUGIN_NAME "] %s", sm.c_str());
			}

			f->prof_frames = 0;
			f->prof_dl = 0.0;
			f->prof_dl_wait = 0.0;
			f->prof_dl_copy = 0.0;
			f->prof_proc = 0.0;
			f->prof_up = 0.0;
			f->prof_cover = 0.0;
			f->prof_face_frames = 0;
			f->prof_bounds = 0.0;
			f->prof_bounds_n = 0;
			f->prof_sm_draw = 0.0;
			f->prof_sm_map = 0.0;
			f->prof_sm_copy = 0.0;
			f->prof_sm_prep = 0.0;
			f->prof_sm_mask = 0.0;
			f->prof_sm_n = 0;
			f->prof_ch_tone = 0.0;
			f->prof_ch_detail = 0.0;
			f->prof_ch_warp = 0.0;
			// 3.1.63 / T7：tick 计数随窗口归零。tick_last 故意**不**清 ——
			// 窗口边界若切在同一个 tick 中间，清了会把同一帧算成两个 tick。
			f->tick_calls = 0;
			f->tick_ticks = 0;
			f->tick_cur = 0;
			f->tick_max = 0;
			for (int i = 0; i < 6; i++)
				f->prof_seg[i] = 0.0;
		}
	}

	// One-shot end-to-end self check.
	//
	//  1) copy the uploaded texture back through a stagesurface and measure its
	//     brightness. mean ~= 0 means the upload silently did nothing (e.g. a
	//     non-GS_DYNAMIC texture) and the picture would be black.
	//  2) compare the detail (mean |Laplacian|) of the frame we received with
	//     the frame the engine returned. A working skin smoother must lower it;
	//     if the two are equal the blur parameters had no effect at all.
	//
	// f->stage is free here - its contents already went to buf_in.
	if (!f->verified) {
		f->verified = true;
		gs_stage_texture(f->stage, f->out_tex);
		uint8_t *vp = NULL;
		uint32_t vls = 0;
		if (gs_stagesurface_map(f->stage, &vp, &vls) && vp) {
			uint64_t sum = 0;
			uint64_t n = 0;
			for (uint32_t y = 0; y < cy; y += 16) {
				const uint8_t *row = vp + (size_t)y * vls;
				for (uint32_t x = 0; x < cx; x += 16) {
					sum += row[(size_t)x * 4] + row[(size_t)x * 4 + 1] +
					       row[(size_t)x * 4 + 2];
					n += 3;
				}
			}
			gs_stagesurface_unmap(f->stage);
			IBeautyEngine &e = beautyEngine();
			blog(LOG_INFO,
			     "[" PLUGIN_NAME
			     "] self-check: out_tex mean=%.1f (0.0 = upload broken) | detail in=%.3f "
			     "out=%.3f (%+.1f%%) | blur_type=%d blur=%.0f skin_detect=%d faces=%d "
			     "engine=%s region=%s core=%.1fms",
			     n ? (double)sum / (double)n : 0.0, detail_in_check, detail_out_check,
			     detail_in_check > 0.0
				     ? (detail_out_check / detail_in_check - 1.0) * 100.0
				     : 0.0,
			     f->blur_type, f->v[BEAUTY_IDX_BLUR], f->skin_detect, e.lastFaces(),
			     e.id(), region_state(e), e.lastCoreMs());
		} else {
			blog(LOG_WARNING, "[" PLUGIN_NAME "] self-check: could not map out_tex");
		}
	}

	if (f->frame_id <= 3) {
		IBeautyEngine &e = beautyEngine();
		blog(LOG_INFO,
		     "[" PLUGIN_NAME
		     "] frame#%d drawn %ux%u faces=%d engine=%s region=%s skip_sdk=%d noface_skip=%d noface=%llu",
		     f->frame_id, cx, cy, e.lastFaces(), e.id(), region_state(e),
		     skip_sdk ? 1 : 0, beautyCoreNoFaceSkip(),
		     (unsigned long long)beautyCoreNoFaceFrames());
	}
	f->frame_id++;

	// 5) draw -- mirrors libobs render_filter_tex() (obs-source.c).
	//    Uses gs_technique_begin/begin_pass instead of gs_effect_loop():
	//    gs_effect_loop() bails out with "An effect is already active" when
	//    OBS has already begun a technique around our video_render call.
	gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(draw_effect, "Draw");
	gs_eparam_t *image = gs_effect_get_param_by_name(draw_effect, "image");
	if (!tech) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const bool linear_srgb = gs_get_linear_srgb();
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);

	gs_texture_t *draw_tex = passthrough ? in_tex
					   : (chain_direct ? f->chain_out_tex : f->out_tex);

	// === DIAG A：验证"最终绘制纹理是否为半分辨率／陈旧 RT"（方案 A）===
	//   仅当环境变量 BEAUTY_DIAG_DIMS=1 时启用（2.2.1：默认关 —— 原来判据写成
	//   "未设 BEAUTY_DIAG_OFF 就开"，与本注释自相矛盾，导致生产环境每 180 帧
	//   刷一条 WARNING）。这条诊断的假设（半分辨率纹理被当全分辨率画）已被
	//   证伪：真因是投影矩阵泄漏，见 smooth_rt_begin() 上方。留着它只作为
	//   "纹理尺寸是否对得上"的一次性排查工具。
	//   判读：若 draw 或 chain_out_tex 的 WxH = 960x540（即 1920x1080 的一半），
	//   而 target=1920x1080，那才是"2× 中心放大"。尺寸对得上而画面仍放大 → 查投影。
	{
		static int diag_init = 0;
		static int diag_on = 0;
		static long long diag_frames = 0;
		if (!diag_init) {
			const char *dv = getenv("BEAUTY_DIAG_DIMS");
			diag_on = (dv && dv[0] == '1') ? 1 : 0;
			diag_init = 1;
		}
		if (diag_on) {
			diag_frames++;
			if (diag_frames % 180 == 0) {
				uint32_t tw = gs_texture_get_width(draw_tex);
				uint32_t th = gs_texture_get_height(draw_tex);
				uint32_t cw = f->chain_out_tex
						      ? gs_texture_get_width(f->chain_out_tex)
						      : 0;
				uint32_t ch = f->chain_out_tex
						      ? gs_texture_get_height(f->chain_out_tex)
						      : 0;
				uint32_t ow = f->out_tex
						      ? gs_texture_get_width(f->out_tex)
						      : 0;
				uint32_t oh = f->out_tex
						      ? gs_texture_get_height(f->out_tex)
						      : 0;
				blog(LOG_WARNING,
				     "[" PLUGIN_NAME
				     "] DIAG_DIMS draw=%ux%u target=%ux%u chain_direct=%d "
				     "chain_out_tex=%ux%u out_tex=%ux%u",
				     tw, th, (uint32_t)cx, (uint32_t)cy,
				     chain_direct ? 1 : 0, cw, ch, ow, oh);
			}
		}
	}

	if (image) {
		if (linear_srgb)
			gs_effect_set_texture_srgb(image, draw_tex);
		else
			gs_effect_set_texture(image, draw_tex);
	}

	gs_blend_state_push();
	// 5b-1：直绘用 ONE/ZERO（源覆盖），精确复现老路「先把 alpha 强制成 0xFF
	// 再按 SRC_ALPHA 混合」的结果 —— 那条路径下 dst = src×1 + dst×0，与源覆盖
	// 完全等价。不能沿用 INVSRCALPHA：链的最终 pass 透传源帧 alpha，若源是
	// 带透明的素材，直绘会真的半透明，与老路（强制不透明）不一致。
	if (chain_direct)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	else
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	const size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(draw_tex, 0, cx, cy);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

// ------------------------------------------------------------------
// Module
// ------------------------------------------------------------------
static struct obs_source_info beauty_filter_info = {};

OBS_DECLARE_MODULE()

static void obs_module_unload(void)
{
	// 释放引擎 / GL / 语义区域资源。滤镜实例此时已被 OBS 全部销毁
	// （source info 的 destroy 先于模块卸载），槽位都已归还。
	beautyEngine().release();
	blog(LOG_INFO, "[" PLUGIN_NAME "] engine released");
}

MODULE_EXPORT const char *obs_module_description(void);
const char *obs_module_description(void)
{
	return "OBS美颜滤镜 (OBS Beauty, standalone core) v" PLUGIN_VERSION;
}

bool obs_module_load(void)
{
	// 方案 A 便捷开关：BEAUTY_GPU=1 等价于 BEAUTY_ENGINE_IMPL=gpu。
	// ⚠ 必须放在本函数**最前面**，早于下面第一次 beautyEngine()：
	// 引擎是单例，preheat() 一调用就按当时的 env 选定实现，之后再改 env 无效。
	// 3.1.40 及之前这段映射只写在 beauty_filter_create() 里，而模块加载阶段
	// 早就把引擎定成 core 了 —— 于是 BEAUTY_GPU=1 从来没生效过（日志恒为
	// engine=core）。3.1.41 修：真机实测 engine=gpu/2.0.0 才第一次出现。
	{
		const char *gp = getenv("BEAUTY_GPU");
		const char *ei = getenv("BEAUTY_ENGINE_IMPL");
		if (gp && gp[0] == '1' && (!ei || !ei[0]))
			_putenv_s("BEAUTY_ENGINE_IMPL", "gpu");
	}

	// 引擎日志接进 OBS 日志系统；不接的话引擎消息会写到 stderr（OBS 里看不到）。
	beauty_set_log_fn(beauty_obs_log);

	// 预热：不等第一个滤镜创建，让模块加载就完成那 ~2 秒的引擎初始化，
	// 与 1.x 行为一致（首帧不卡）。失败不阻塞 —— 滤镜会直通并每实例报一次错。
	std::string err;
	bool didInit = false;
	if (!beautyEngine().preheat(err, didInit)) {
		blog(LOG_ERROR,
		     "[" PLUGIN_NAME "] engine init failed (%s) - filter passes video through: %s",
		     beautyEngineId(), err.c_str());
	} else if (didInit) {
		IBeautyEngine &e = beautyEngine();
		// 刻意用字符串拼接而不是 printf 格式化：这一行有 6 个值要打，是全文件最
		// 容易被改的一行，而 blog() 不受编译器格式检查保护（见文件头的警告）。
		// 3.1.4 就是在这里多加了一个 %s 少配一个参数，把 OBS 崩了两次。
		std::string msg = "init OK (v";
		msg += PLUGIN_VERSION;
		msg += ", engine=";
		msg += e.id() ? e.id() : "?";
		msg += "/";
		msg += e.version() ? e.version() : "?";
		msg += ", region=";
		msg += region_state(e);
		msg += ", item=";
		msg += std::to_string(e.itemId());
		msg += ", owner_thread=";
		msg += std::to_string(e.ownerThread());
		msg += ")";
		blog(LOG_INFO, "[" PLUGIN_NAME "] %s", msg.c_str());
		// C20：皮肤判据快照。真机「磨皮没效果」的头号原因是肤色掩码被白平衡
		// 压低（实测真机帧掩码只剩 34%），而这一点光看画面看不出来。打一行
		// 让用户能自查自适应有没有开、椭圆中心在哪。
		blog(LOG_INFO, "[" PLUGIN_NAME "] %s", beautyCoreSkinInfo());

		// 3.1.7 起默认引擎是自研核，它靠 YuNet 拿人脸掩码。掩码拿不到**不会崩**，
		// 但会静默退化成「全帧磨皮」—— CPU 约 2.5 倍，而且清晰 / 美白 / 红润 /
		// 五官五项 / 脸内锐化全部不执行。3.1.4 的用户看到的就是「CPU 22% + 个别
		// 功能能用」，查了一整轮才发现是模型没加载。所以这里必须留一条 WARN，
		// 不能让同样的事再悄悄发生一次。
		if (e.id() && strcmp(e.id(), "core") == 0 && !e.regionActive()) {
			blog(LOG_WARNING,
			     "[" PLUGIN_NAME "] core engine has NO face mask: %s. "
			     "Falling back to full-frame smoothing (higher CPU, most effects "
			     "disabled). Check onnxruntime.dll + models\\yunet.onnx next to "
			     "the plugin dll.",
			     (e.lastErr() && e.lastErr()[0]) ? e.lastErr() : "(no detail)");
		}
	}

	beauty_filter_info.id = BEAUTY_FILTER_ID;
	beauty_filter_info.type = OBS_SOURCE_TYPE_FILTER;
	beauty_filter_info.output_flags = OBS_SOURCE_VIDEO;
	beauty_filter_info.get_name = beauty_filter_name;
	beauty_filter_info.create = beauty_filter_create;
	beauty_filter_info.destroy = beauty_filter_destroy;
	beauty_filter_info.get_defaults = beauty_filter_get_defaults;
	beauty_filter_info.get_properties = beauty_filter_properties;
	beauty_filter_info.update = beauty_filter_update;
	beauty_filter_info.video_render = beauty_filter_render;

	obs_register_source_s(&beauty_filter_info, sizeof(beauty_filter_info));

	blog(LOG_INFO, "[" PLUGIN_NAME "] filter registered as '%s' (v%s)",
	     BEAUTY_FILTER_ID, PLUGIN_VERSION);
	return true;
}
