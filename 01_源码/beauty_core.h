#pragma once
// ---------------------------------------------------------------------------
// beauty_core —— 美颜引擎抽象层
//
// 目的：把「谁来做美颜」从 OBS 胶水层里解耦出去。
//
//   obs-beauty.cpp  ──>  IBeautyEngine  ──>  CoreEngine        （默认：自研）
//                                       └─>  LegacyAdapter       （回退：参考实现驱动，仅离线基准）
//                                       └─>  PassthroughEngine （验证用直通）
//
// 接口严格按 LegacyEngine 的现有用法设计（RGBA 进 / RGBA 出 + BeautySettings +
// 每实例 slot），所以替换实现时 OBS 侧不需要懂任何引擎内部细节。
//
// 选择方式（进程内只解析一次）：
//   * 不设 / 未知值 → CoreEngine（3.1.7 起的默认）
//   * BEAUTY_ENGINE_IMPL=legacy       → LegacyAdapter（仅离线基准 DLL 编译）
//   * BEAUTY_ENGINE_IMPL=core         → CoreEngine（显式写出来，便于脚本自证）
//   * BEAUTY_ENGINE_IMPL=passthrough  → 直通，用于验证开关与降级
//
// 为什么要留一个直通引擎：它是「开关真的生效」的最小可验证证据 —— 装上之后
// 若画面完全没变化，说明开关没接上；若画面变成原图，说明链路走通了。它同时
// 也是「新引擎出错时的安全降级目标」。
// ---------------------------------------------------------------------------

#include <string>
#include <cstdint>
#include "beauty_params.h"   // BeautySettings / P_* 的唯一出处，不自造一份

// ---------------------------------------------------------------------------
// 磨皮参数快照（Phase 3 诊断用，**只读**）
//
// 为什么必须由引擎导出、而不是让 GPU 侧自己按 g_cfg 重算一遍：
// 本帧真正生效的 I / e / T 不是常数 —— 3.1.18 起它们由「档位 t」查表
// （lvI / lvE / lvT）得到，中间还夹着 lvLerp5、s3EFix 覆盖、夹取。OBS 侧若
// 复制这套映射，就变成两份实现：改一处忘另一处的那天，对拍会报一个假 bug，
// 而且没人能立刻看出是「着色器错了」还是「参数算错了」。
//
// 语义与 beautycore::BandParams 一一对应，但刻意不复用它：那是个内部头，
// 让 OBS 胶水层 include beauty_smooth.h 会把整条 CPU 实现头拖进外层。
// ---------------------------------------------------------------------------
struct BeautyBandDiag {
    // 盒宽占半分辨率宽度的比例（默认 5/960、9/960、5/960）
    float f1, f2, fa;
    int   passes;      // 盒式次数（默认 3）
    float T;           // 振幅门控阈值（灰阶）
    float gwp;         // 门控陡度指数
    int   halfRes;     // 1 = 半分辨率处理
    float I;           // 强度 0..1
    float e;           // evenfactor
    float protectK;    // 眼睛保护 0..1（CPU 侧的 s3EyeK/100 并夹取后）
};

// ---------------------------------------------------------------------------
// 引擎接口
//
// 方法语义与 LegacyEngine 的同名方法一一对应，注释见 legacy_engine.h。
// 这里只说明接口层面的差异与约定：
//   * process() 允许 src == dst；实现方必须自己准备输出暂存缓冲。
//   * acquireSlot()/releaseSlot() 保持「每实例一个上下文」的模型。自研引擎
//     若不需要槽位，返回一个恒定的合法值即可（PassthroughEngine 返回 0）。
//   * 所有方法都可能被渲染线程调用；线程安全由实现方负责。
// ---------------------------------------------------------------------------
class IBeautyEngine {
public:
    virtual ~IBeautyEngine() {}

    // 短标识，用于日志与 A/B 对照（"legacy" / "passthrough" / "core"）。
    virtual const char* id() const = 0;
    virtual const char* version() const = 0;

    virtual bool ensureReady(std::string& err) = 0;
    virtual bool preheat(std::string& err, bool& didInit) = 0;
    virtual bool ready() const = 0;

    virtual int  acquireSlot(std::string& err) = 0;
    virtual void releaseSlot(int slot) = 0;
    virtual void setDetectOptions(int rotate, int landmarkQuality, int maxFaces) = 0;

    // advanceFrame：是否推进引擎内部的帧序号（人脸跟踪的时序状态）。
    // 默认 true（正常渲染）；测试里要「同一帧喂两次、比对两条实现路径」时
    // 传 false，否则第二次调用会看到已被前一次更新过的跟踪状态。
    virtual bool process(int slot, const unsigned char* src, unsigned char* dst,
                         int w, int h, const BeautySettings& s,
                         bool advanceFrame = true) = 0;

    // ---- 只读诊断，供日志使用（可以返回保守值，不影响画面）----
    virtual int          lastFaces() const = 0;
    virtual bool         regionActive() const = 0;
    virtual double       lastCoreMs() const = 0;
    // 3.1.47 诊断：上一帧的六段耗时（毫秒），索引含义见 beautyCoreSegLabel()。
    // 默认实现全部填 0 = 「本引擎不提供分项」，调用方据此决定打不打印，
    // 因此给默认实现而不是纯虚，避免破坏 LegacyEngine / GpuEngine。
    virtual void lastSegMs(double* out6) const
    {
        if (!out6) return;
        for (int i = 0; i < 6; i++) out6[i] = 0.0;
    }
    virtual const char*  lastErr() const = 0;
    virtual int          itemId() const = 0;
    virtual unsigned long ownerThread() const = 0;

    // 是否走 GPU 渲染路径（方案 A）。默认 false（纯 CPU 引擎）；GpuEngine 覆写为
    // true，obs-beauty.cpp 据此决定「跳过下载/上传往返、改跑着色器」。Phase 1 的
    // GpuEngine 虽标记 true，但 process() 仍是 CPU 直通，实际 GPU 计算在 Phase 2+ 接入。
    virtual bool supportsGpu() const { return false; }

    // 诊断：把**当前正在用的**皮肤掩码拷出（w*h 字节，0..255）。
    // 默认没有掩码 → 返回 false。
    //
    // 为什么不用 beautyCoreComputeMask()：那个函数是**离线对拍**入口，内部会
    // stopAsync() 并把 asyncOn_ 永久置 false（为了哈希确定性），一旦在渲染线程
    // 每帧调用，此后每一帧都会在渲染线程上同步跑一次人脸检测 —— CPU 直接顶满。
    // 本方法只做一次持锁 memcpy，不改任何引擎状态。
    virtual bool copyMaskTo(unsigned char* out, int w, int h)
    {
        (void)out;
        (void)w;
        (void)h;
        return false;
    }

    // 诊断：当前掩码的置位像素数与同源人脸数（见 beautyCoreMaskStat 的说明）。
    // 默认实现表示「没有掩码」（px = -1），不改任何状态，可安全在渲染线程调用。
    virtual void maskStat(int* pixelsOut, int* facesOut) const
    {
        if (pixelsOut) *pixelsOut = -1;
        if (facesOut)  *facesOut = 0;
    }

    // 诊断（3.1.48）：本帧磨皮 ROI 包围盒（人脸框并集）占整帧的面积比，0..1。
    // 返回 **-1** 表示本帧没有可用包围盒（无掩码 / 包围盒为空），不参与统计。
    //
    // 这个数为一个具体决策服务：`surface3RgbBand` 的 roi 形参目前完全没被使用
    // —— 函数体内它只出现在签名和 `roi.empty()` 早退判断里，掩码只覆盖 14% 时
    // 仍然跑满全帧 18 趟盒式。ROI 裁剪的理论收益上限就是 1 − 本值，所以先量它、
    // 再决定要不要动那个函数，比直接改结构（GPU 翻转）便宜得多。
    // 默认实现返回 -1；只在渲染线程读，不重算、不加锁。
    virtual double boundsCover() const { return -1.0; }

    // 诊断：把**当前正在用的**五官保护掩码（眼睛）拷出（w*h 字节，0..255）。
    // 与 copyMaskTo 同一套语义：只持锁 memcpy，不重算、不碰跟踪状态。
    // 磨皮的合成式要用它做 `out = mix(C, res, 1 - eye)`，缺了它 GPU 侧只能
    // 传全 0，对拍会在眼周出现一条系统性差异 —— 那种差异最容易被误读成
    // 「着色器合成式写错了」。
    virtual bool copyProtectTo(unsigned char* out, int w, int h)
    {
        (void)out;
        (void)w;
        (void)h;
        return false;
    }

    // 诊断：本帧磨皮实际生效的参数。默认 false = 本帧没走磨皮（或该实现不支持）。
    // 只在 BEAUTY_GPU_SMOOTH=1 的对拍帧读一次。
    virtual bool bandDiag(BeautyBandDiag* out) const
    {
        (void)out;
        return false;
    }

    // 宿主卸载插件时调用；之后本进程内不再使用。
    virtual void release() = 0;
};

// ---------------------------------------------------------------------------
// 自研核参数（仅供离线对拍 / 调参使用）
//
// 生产路径不调用它：默认值就是离线对拍得出的最佳组合（见
// 05_文档/S2_磨皮核对拍结论.md）。离线扫描参数时用这个入口改，避免为了调参
// 去改源码重编译。
// ---------------------------------------------------------------------------
void beautyCoreSetSmoothParams(int radius, float sigmaSpace,
                               float sigmaColor, int down);

// 性能开关（仅供离线对照实验）
//   threads <= 0 自动（硬件并发数，上限 8）；1 = 强制单线程
//   useRoi  0 = 关掉 ROI 裁剪，退回全帧磨皮
//
// 生产路径不调用它。关 ROI 只用于测量「全帧成本/ROI 节省了多少」，
// 生产必须保持开启 —— 关掉后 93% 的算力会花在随后被丢弃的像素上。
void beautyCoreSetPerf(int threads, int useRoi);

// Sobel 边缘门控灵敏度（仅供离线调参）。0 = 关闭。
// 灵敏度系数取自 wuxiwuhen/obs-beauty-filter（MIT）的 PSComposite，原值 ×10.0。
// 门控会在「亮度梯度大」的地方（眉毛、眼镜框、发际线）取消磨皮。
void beautyCoreSetEdgeK(int k);

// 五官保护强度 0..100（仅供离线调参）。0 = 关闭。
// 眼睛和嘴巴上的磨皮会乘 (1 - protect × k/100)，做法同 lumina 的 uSmoothProtect。
void beautyCoreSetProtectK(int k);

// 阈值软膝（立体感）0..255，0 = 关闭。|src-smoothed| 超过它会按比例减少磨皮，
// 从而保住鼻梁侧、下颌线这类大幅度的明暗过渡。做法同 lumina 的 uSmoothThreshold。
void beautyCoreSetLimitThr(int thr);

// 磨皮算法切换（离线 A/B 用）：
//   mode  1 = 高低频分离（默认），0 = 旧的「双边滤波 + 线性混合」
//   sigma 低频尺度（高斯 sigma，半径自动取 3σ）
//   magLo/magHi  幅度门控区间（0..255 尺度）：小于 lo 全磨，大于 hi 不动
void beautyCoreSetFreq(int mode, float sigma, float magLo, float magHi);

// 颗粒回填量 0..1（离线调参）。抑制双边迭代的阶梯感。
void beautyCoreSetFreqGrain(float g);

// 低频提取方式（离线 A/B）：mode 0 = 双边迭代，1 = 引导滤波；radius/eps 只对后者生效。
void beautyCoreSetGuide(int mode, int radius, float eps);

// 低频层路线（离线 A/B）：path 0 = 旧 uint8 双边迭代，1 = 浮点双边，
// 2 = 浮点双边 + 低频参照（默认）。refRadius <= 0 表示不做参照。
void beautyCoreSetLowPath(int path, int refRadius);

// 清晰（clarity）离线标定：面板 100 档映射到 0..maxGain；radius 是盒式半径
// （12 → 25px 窗口）；kneeLo/kneeHi 是软膝区间（防边缘光晕）。
void beautyCoreSetClarity(float maxGain, int radius, float kneeLo, float kneeHi);

// ---------------------------------------------------------------------------
// 异步掩码提交周期的当前值（帧，0 = 每帧）。**只用于日志**，不影响行为。
// 为什么要暴露：3.1.8 起生产默认每帧检测，离线台架仍是每 4 帧（同步路径），
// 两者数字不可直接比。日志里打出实际节奏，省得下次又对不上账。
int beautyCoreMaskCadence();

// 累计提交给检测 worker 的帧数（诊断用）。配合帧数就能算出实际节奏：
//   提交数 / 帧数 ≈ 1/(周期+1)。周期 0 → ≈1.0，周期 4 → ≈0.2。
// 这是「每帧检测」唯一能直接测到的证据 —— 它不体现在渲染线程的毫秒数上。
uint64_t beautyCoreMaskSubmits();

// 3.1.40（移植自 CPU 版 v3.1.39）：累计「确认本帧没人 → 直接透传跳过」的帧数。
// 这是验证无脸跳过修复是否生效的唯一直接指标；配合总帧数可得跳过占比。
uint64_t beautyCoreNoFaceFrames();

// 3.1.40：无脸透传开关状态（1 = 开 / 0 = 旧的全帧兜底行为）。
// 日志/UI 用它表明当前是否处于修复后的行为。
int beautyCoreNoFaceSkip();

// 掩码↔帧对齐计数（3.1.10，诊断用）：used = 用了对齐帧处理的帧数。
// ratio = used / total（total 由 beautyCoreAlignCount 的出参给）。
// 30fps 下 ≈1 说明 worker 每帧都到货、掩码与画面逐像素同源；
// 明显 < 1 说明检测追不上渲染，拖影会按比例回来。
uint64_t beautyCoreAlignUsed();

// 3.1.10 探针：出参带回 (对齐帧数, 处理总帧数)。
int beautyCoreAlignStat(uint64_t* used, uint64_t* total);

// 掩码诊断（3.1.44，**只给日志用**，不改任何引擎状态）。
//   * pixelsOut = 当前掩码里被置位的像素数；-1 表示此刻没有有效掩码
//   * facesOut  = 与这份掩码**同源**的人脸数（0 = 本帧没检出脸）
//
// 为什么要单独暴露这两项：Phase 0 真机实测在**同一分辨率、同一参数**下，
// 引擎耗时会差 2.3 倍（1080×1920 掩码覆盖 21% → 21.3 ms，把脸放大 1.78 倍
// 后覆盖不变但像素同样多 → 48.7 ms）。差异的唯一来源是「掩码覆盖了多少像素」
// 和「有没有脸触发美形三项」。性能日志不带这两个数，两次测量之间就无法判断
// 差异来自分辨率、来自覆盖，还是来自这次压根没检出脸 —— 基线也就不可复现。
//
// 值本身就存在引擎里（maskPixels_ / lastFaces_，同步与异步两条路径都会更新），
// 所以调用是 O(1)，**不需要扫描掩码**，放在每帧渲染路径上也不会增加功耗。
void beautyCoreMaskStat(int* pixelsOut, int* facesOut);

// ROI 包围盒面积占比（3.1.48，只读诊断）
//
// 返回本帧磨皮包围盒面积 / 整帧面积，0..1；**-1 表示本帧无包围盒**（无掩码或
// 掩码为空），调用方应跳过这一帧而不是当成 0% 计入平均。
//
// 存在理由：`surface3RgbBand` 收了 roi 形参却从未使用，所以包围盒大小目前
// 对耗时**毫无影响**。想知道「修 ROI 裁剪」值不值得做，必须先知道包围盒实际
// 占多大 —— 如果它本来就占 80%，那改这个函数就是白干。与 maskStat 一样是
// O(1) 读取，不扫像素。
double beautyCoreBoundsCover();

// 分项计时（3.1.47，只读诊断）
//
// 背景：4K 场景引擎 65 ms、掩码覆盖率却只有 2%。没有分项数据，就没法判断该优化
// 哪一段 —— 「磨皮占大头」只是从「2% 掩码仍跑全帧双尺度带通」推出来的猜想。
//
// out6  六个 double，单位毫秒，索引含义见 beautyCoreSegLabel()：
//       0 掩码更新 / 1 磨皮 / 2 色调 / 3 五官 / 4 细节 / 5 美形
// 六项之和**略小于** lastCoreMs()，差值是该分支自身的判断开销。
// 引擎不提供分项时全部填 0（GpuEngine 目前转发给内部 CPU 引擎，因此有值）。
void beautyCoreSegMs(double* out6);

// 分段名，供日志打印。i 越界时返回 "?"。
const char* beautyCoreSegLabel(int i);

// 掩码导出（仅供离线对拍）
//
// 为什么要这个：自研引擎只磨「肤色 ∩ 人脸框」那 5% 左右，而 参考实现的改动区有
// 13%（含脖子/肩）。拿参考实现改动区当分母去算细节保留率，会把「只磨了脸」读成
// 「几乎没磨」（保留率 99%）。对拍的分母必须是**自研掩码本身**。
//
// src   RGBA8（w*h*4），会被强制重算一次掩码（不吃缓存）
// outMask w*h 字节，0 或 255
// 返回 false 表示本帧没有可用掩码（无脸 / 模型不可用），此时 outMask 全 0。
// 只对 core 引擎有效；其它实现返回 false。
// ---------------------------------------------------------------------------
// 皮肤判据快照（C20，只用于日志）
//
// 为什么要暴露：真机上「磨皮没效果」的头号原因是肤色掩码被白平衡压低，
// 而这个值光看画面看不出来。日志里打一行，一眼能确认自适应有没有开、
// 当前椭圆中心漂到哪了。返回指向静态缓冲的指针，非线程安全但只在启动时读。
// ---------------------------------------------------------------------------
const char* beautyCoreSkinInfo();

// 「肤色自适应估计首次收效」的一次性通知（C20 真机自查）。引擎 worker 在
// 第 2 帧以后第一次把估计出的中心写进全局时置位；取走返回 true 并清零。
// 渲染线程拿到 true 后紧跟一行 beautyCoreSkinInfo() —— init 时那行只有
// (center=default, not yet estimated)，这里才是自适应真正生效的证据。
// 返回的指针指向静态缓冲，只在本线程打日志用。
bool beautyCoreSkinAnnounce();

bool beautyCoreComputeMask(const unsigned char* src, int w, int h,
                           unsigned char* outMask);

// 拷贝「当前正在用的」皮肤掩码（诊断用，**只读**）。
//
// 与 beautyCoreComputeMask 的关键区别：不重算、不停异步 worker、不改
// asyncOn_、不动人脸跟踪状态，只做一次持锁 memcpy。GPU 色调诊断（Phase 2）
// 每帧都要拿掩码去上传，必须用这个 —— 用 computeMask 会让此后每一帧都在
// 渲染线程同步跑人脸检测。
//
// outMask  w*h 字节，0..255。返回 false = 当前无可用掩码（无脸 / 模型未加载 /
// 尺寸不符），此时 outMask 已被清零。
bool beautyCoreCopyMask(unsigned char* outMask, int w, int h);

// 诊断：对 src 单独跑一遍 CPU 端色调（清晰 / 美白 / 红润），结果写 rgbaOut。
// 内部就是 beauty_freq.cpp 的 toneColor()（全帧 ROI、单线程），用于和 GPU
// 着色器逐像素对拍。不推进人脸跟踪、不改引擎状态。
// clarity/whiten/rosy 与引擎内部同尺度：面板值 / 100，夹 [0,1]。
bool beautyCoreToneCpu(const unsigned char* src, int w, int h,
                       const unsigned char* mask, float clarity, float whiten,
                       float rosy, unsigned char* rgbaOut);

// ---- Phase 3 磨皮诊断（全部为纯新增，零行为变化）-------------------------

// 本帧磨皮实际生效的参数。false = 这一帧没走 band 路径（磨皮为 0 / 走的别的
// 模型 / 引擎不是 core），此时 *_out 内容未定义。
bool beautyCoreBandDiag(BeautyBandDiag* out);

// 五官保护掩码只读拷贝（语义同 beautyCoreCopyMask）。false = 无可用掩码。
bool beautyCoreCopyProtect(unsigned char* outMask, int w, int h);

// CPU 端磨皮基准：内部就是 beautycore::surface3RgbBand()，单线程、全帧 ROI。
// 刻意只跑磨皮（不跑色调/美形），这样与 GPU 对拍的两边「算什么」完全一致，
// 差异只可能来自「怎么算」。
// src/dst 均为 RGBA8；mask/protect 可为 nullptr（等价于全皮肤 / 无保护）。
bool beautyCoreSmoothCpu(const unsigned char* src, unsigned char* dst, int w, int h,
                         const unsigned char* mask, const unsigned char* protect,
                         const BeautyBandDiag& p);

// ---- 磨皮执行钩子（3.1.49 / Phase 3.5）-------------------------------------
//
// 引擎把「本帧真正要磨的那一份 src」连同当帧生效的 mask/protect 一起交给
// 宿主；宿主在渲染线程里用 GPU 算完、把结果**写满 dst** 后返回 true。
// 返回 false = 没做成（着色器缺失 / 回读失败 / 小画幅），引擎自动回退 CPU
// 路径，画面与 3.1.48 逐字节相同，只是慢一点。
//
// 为什么必须是「引擎内回调」，而不是让插件层在主路径外侧先把 GPU 磨皮做完：
//   1) 3.1.10 的掩码↔帧对齐会在 process() **内部**把 src 换成上一帧的
//      heldFrame_（真机日志实测 71% 的帧如此）。插件层在调用前无从预判，
//      在外侧磨皮必然与后续色调/美形错帧，画面会闪。
//   2) mask / protect 必须是引擎当帧真正生效的那两份（可能来自 held 帧），
//      拷贝接口 beautyCoreCopyMask 只能拿到「当前」的一份。
//   3) 回调运行在渲染线程的 GS 上下文里，宿主可以直接建纹理、开 texrender。
//
// 线程约束：只在渲染线程、且在该帧的 gs_texrender 已 end 之后被调用。
// 一个进程只挂一个钩子（与 beauty_set_log_fn 同样的单例约定）。
typedef bool (*BeautySmoothHook)(void* user,
                                 const unsigned char* src, unsigned char* dst,
                                 int w, int h,
                                 const unsigned char* mask,
                                 const unsigned char* protect,
                                 const BeautyBandDiag& bp);

// fn 传 nullptr 卸载钩子（回到纯 CPU 磨皮）。
void beautyCoreSetSmoothHook(BeautySmoothHook fn, void* user);

// 钩子命中率探针（启动以来累计）。calls 只统计真的走到 band 磨皮段的帧，
// 面板把磨皮关掉时两个数都不涨。插件层用差值算每个日志窗口的比率。
void beautyCoreSmoothHookStat(long long* hits, long long* calls);

// ---- 段区间钩子（3.1.52 / Phase 5a）---------------------------------------
//
// 3.1.49 的 BeautySmoothHook 只让宿主接管**一段**（磨皮），于是链上剩下的
// 色调 / 五官 / 细节 / 美形仍在 CPU 上跑，中间结果必须落回 CPU buffer：
// 720p 实测磨皮段 1.74 ms，其中 GPU 计算只占 0.04，剩下 1.70 全是
// 「上传 + 回读 + 拷贝」。要再降一个数量级，宿主必须能一次接管**连续多段**，
// 让中间结果一直留在显存里，只在认领区间的末尾回读一次。
//
// 契约（与光滑钩子的三条硬约束一致，只是把「一段」换成「一段区间」）：
//   1) 引擎按 SMOOTH → TONE → REGION → DETAIL → WARP 的固定顺序执行；
//      钩子返回的位掩码必须是**这个顺序上的前缀**（认领了 DETAIL 却不要
//      REGION 是非法的，引擎会整份丢弃、退回全 CPU）。这条校验在引擎里做，
//      宿主写错只会变慢，不会算错。
//   2) 认领范围内，宿主必须把 **最后一段** 的结果写满 dst；引擎从下一段
//      继续在 CPU 上跑（若还有）。
//   3) 只能认领 job.flags 里置位的段 —— 那些是本帧真正要跑的段。
//
// 失败一律返回 0（或比预期短的合法前缀）：引擎回退 CPU，画面与 3.1.51
// 逐字节相同，只是慢一点。所以这是一个「只会变快、不会变错」的开关。
//
// 为什么任务参数由引擎填：色调/细节的生效值不是面板值 —— I/e/T 是查表+夹取
// 出来的，细节的 fine/mid 还要按磨皮质感与运动自适应重算。宿主自己再算一遍
// 必然与 CPU 路径漂移，所以引擎把**算好的**参数递出去（BeautyChainJob）。
enum {
    BEAUTY_SEG_SMOOTH = 1u << 0,
    BEAUTY_SEG_TONE   = 1u << 1,
    BEAUTY_SEG_REGION = 1u << 2,
    BEAUTY_SEG_DETAIL = 1u << 3,
    BEAUTY_SEG_WARP   = 1u << 4,
};

// 色调三项权重（与引擎内部同尺度：面板值 / 100，夹到 [0,1]）。
struct BeautyToneDiag {
    float clarity;
    float whiten;
    float rosy;
};

// 细节段的生效参数（逐字对应 beautycore::DetailParams + DetailEma）。
// emaOn 为 0 时 beta 无意义；deltaRefill 目前 GPU 侧不支持（引擎不会置位）。
struct BeautyDetailDiag {
    float fine;
    float mid;
    float globalFine;
    float globalMid;
    float thrFine;
    float thrMid;
    float thrFineGlobal;
    float thrMidGlobal;
    float kneeFine;
    float kneeMid;
    float beta;        // 时序 EMA 的新帧权重
    int   emaOn;       // 1 = 需要推进/读取亮度历史
    int   deltaRefill; // 1 = delta 模式（当前 GPU 不认领）
    int   roi[4];      // x0,y0,x1,y1；只用于日志，GPU 侧按全帧算
};

// ---------------------------------------------------------------------------
// 5c：美形段的逐脸几何（3.1.62）
//
// 为什么由引擎递出来而不是让宿主自己算：位移场的常量（eyeR / thinYc /
// thinSig / ROI ...）来自人脸框与关键点，宿主手上没有这些，重新推导必然
// 与 CPU 漂移 —— 与色调（I/e/T 查表夹取）、细节（fine/mid 自适应重算）
// 是同一条理由。
//
// 字段与 beautycore::WarpGeom 逐项对应。那边是引擎内部头（beauty_warp.h），
// 不能塞进这个公共接口，所以 beauty_core.cpp 里做一次逐字段搬运 —— 重复的
// 只有搬运，算式只有一份。
// ---------------------------------------------------------------------------
enum {
    BEAUTY_WARP_MAX_FACES = 8,   // 与面板「人物个数」上限一致
};

// flags 位定义必须与 beautycore 的 WARP_F_* 一致。
enum {
    BEAUTY_WARP_F_EYE  = 1u << 0,
    BEAUTY_WARP_F_THIN = 1u << 1,
    BEAUTY_WARP_F_CHIN = 1u << 2,
    BEAUTY_WARP_F_KPS  = 1u << 3,
};

struct BeautyWarpFace {
    float cx, cy;                 // 脸框中心
    float halfW;                  // 脸框宽/2
    float eyeRx, eyeRy;           // 右眼（关键点或比例估算）
    float eyeLx, eyeLy;           // 左眼
    float eyeR;                   // 大眼作用半径（已夹到 [0.10fw, 0.35fw]）
    float eyeAmp;                 // 大眼满档中心放大系数（p.eye × 0.55）
    float thinYc, thinSig, thinK; // 瘦脸：垂直高斯中心/sigma/满档外移
    float chinYm, chinSpan, chinK;// 下巴：压缩起点/跨度/满档压缩
    int   roi[4];                 // x0,y0,x1,y1（已钳到画面内）
    unsigned flags;               // BEAUTY_WARP_F_*
};

struct BeautyWarpDiag {
    float thin, eye, chin;        // 面板值/100，仅日志与自检用
    int   count;                  // 本帧要变形的脸数；0 = 本帧不做美形
    BeautyWarpFace faces[BEAUTY_WARP_MAX_FACES];
};

// ---- 5b-2：掩码几何快照（诊断用，纯新增）---------------------------------
//
// 为什么需要它：GPU 掩码要逐像素复刻 buildMaskInto，而它的边界全是**整数截断**
// （(int)(cx-hw)、x1-x、featherRamp(y-y0+1, fe)）。让着色器自己从浮点算这些
// 边界，等于开了一个「MSVC 截断 vs HLSL 截断」的分叉口。所以几何一律在引擎里
// 按原式算好、原样交给宿主 —— 宿主只负责把它塞进常量缓冲。
//
// 这就是 5c 的 BeautyWarpDiag 的同一套做法，理由也一样：**同一份来源**。
struct BeautyMaskFace {
    int   x0, y0, x1, y1;         // 脸框（已 clamp 到画面内）
    int   fe;                     // 脸羽化像素
    int   feNeck;                 // 脖子羽化像素
    // 脖子的**循环上下界**，随 maskShape 变：
    //   矩形分支 = [y1, cy+hh+f.h*len)；椭圆分支 = [clamp(cyN−B), clamp(cyN+B))
    // 后者会伸进脸框区域，所以两条分支不能共用同一个下界。
    int   nx0, nyLo, nx1, nyHi;
    float cx, cy, a, b;           // 脸椭圆：中心 + 半轴
    float neckA, neckB, neckCy;   // 脖子椭圆：A、B、cyN(=y1)
    // 眼睛保护椭圆（只画眼，不画嘴 —— 与参考实现结构一致）。
    // CPU 只在 kpsSane() 通过时才画；不通过时整张保护掩码是全 0。
    int   hasEyes;
    float eye0x, eye0y, eye1x, eye1y;  // 两只眼的中心（关键点）
    float eyeW, eyeH;                  // 椭圆半轴（CPU: f.w*0.12 / f.h*0.075）
};

struct BeautyMaskDiag {
    int   count;                  // 有效脸数；0 = 本帧没有可用掩码
    int   shape;                  // g_maskShape 位（bit2 = 脖子逐行，GPU 不支持）
    // 1 = 这份掩码是**本帧**刚换进来的（掩码与 job.src 同一帧）。
    // 0 = 掩码是旧的、只是还在复用 —— 那时 GPU 掩码（按当前帧算）与 CPU 掩码
    //     （按更早的帧算）本来就不该相等，任何逐像素比较都没有意义。
    int   fresh;
    float cb, cr, ru, rv;         // 肤色椭圆（C20 自适应结果）
    BeautyMaskFace faces[BEAUTY_WARP_MAX_FACES];
};

// ---- 五官段的几何快照（4d 用）-------------------------------------------
//
// 为什么要把几何单独交出去：GPU 侧必须与 CPU 用**同一份**椭圆（关键点 → 归一化
// → 乘脸宽/高 → 加偏移，多层链），宿主自己重算必然分叉（5c 的教训）。
// 所以引擎在**决定 claim 之前**先跑一遍「只收集、不施加」的 applyFaceRegions，
// 把每张脸的三组椭圆原样塞进 job。
struct BeautyRegionEllipse {
    float cx, cy, rx, ry, rot, gain, shade, feather;  // 相对脸框的归一化值
};

struct BeautyRegionGeom {
    int faceCount;
    struct Face {
        float fx, fy, fw, fh;      // 这张脸的脸框（椭圆坐标相对它归一化）
        float nasoSlope;
        int elsBeg, elsCount;      // 主椭圆（立体 + 眼周）
        int nasoBeg, nasoCount;    // 法令纹上瓣（暗度驱动）
        int tailBeg, tailCount;    // 法令纹下瓣（常量增益）
    } faces[8];
    int ellipseCount;
    BeautyRegionEllipse els[128];
    // 法令纹的三个全局参数（CPU 在 applyFoldRemoval 里读 g_cfg，宿主拿不到，一并交出）
    float nasoMaxLift;
    float nasoGateLo, nasoGateHi;
    int   nasoLrRadius;
    // 祛斑：作用于 bounds_，与单张脸无关
    int   spotOn;
    float spotK, spotThrLo, spotThrHi;
    int   spotRadius;
    // applySpotFix 要一个 RoiRect（哪一块参与）。宿主没有 bounds_，所以一并交出去。
    int   roiX0, roiY0, roiX1, roiY1;
};

struct BeautyChainJob {
    const unsigned char* src;      // 本帧真正要处理的源（可能来自掩码对齐换帧）
    unsigned char* dst;            // 引擎的输出缓冲；认领区间末端必须写满
    int w;
    int h;
    const unsigned char* mask;     // w*h，0..255；nullptr = 无掩码（= 全皮肤）
    const unsigned char* protect;  // w*h，0..255；nullptr = 无五官保护
    BeautyBandDiag band;           // 磨皮生效参数（含 I/e/T/gwp/protectK）
    BeautyToneDiag tone;           // 色调三项
    BeautyDetailDiag detail;       // 细节段
    BeautyWarpDiag warp;           // 美形段（5c）
    BeautyMaskDiag maskGeom;       // 5b-2：生成当帧 mask 用的几何（诊断/接线用）
    BeautyRegionGeom regionGeom;   // 五官：本帧三组椭圆 + 祛斑参数（4d）
    // 5b-2 切片 4（帧环）：`src` 对应的**宿主帧号**。宿主每帧自增一个计数并
    // 事先通过 beautyCoreSetFrameTag() 告诉引擎；引擎把它和送进检测 worker 的
    // 那一帧绑在一起，对齐换帧时把**被换进来那一帧的帧号**填到这里。
    // 宿主据此在显存帧环里找到同一帧的纹理 —— 这是「跳过整帧上传」而不改语义的前提。
    unsigned long long srcTag;
    unsigned flags;                // 本帧真正要跑的段（BEAUTY_SEG_*）
};

// 返回认领的段（BEAUTY_SEG_* 位或）。0 = 一段都不认领。
typedef unsigned (*BeautyChainHook)(void* user, const BeautyChainJob& job);

// fn 传 nullptr 卸载。装上之后**优先于** BeautySmoothHook；两者都不装时
// 引擎全程走 CPU（3.1.48 的行为）。
void beautyCoreSetChainHook(BeautyChainHook fn, void* user);

// 5b-2 切片 4：宿主每帧告诉引擎「这一帧的帧号」（单调自增）。
// 引擎把它与送进检测 worker 的帧绑定；对齐换帧时 job.srcTag 给出**被换进来那一帧**
// 的帧号，宿主据此在显存帧环里取同一帧的纹理，从而跳过整帧上传。
// 不调用时帧号恒为 0，宿主自然拿不到环命中 → 退回上传（默认行为不变）。
void beautyCoreSetFrameTag(unsigned long long tag);

// 段区间钩子探针：hits/calls 语义同上（calls 只在磨皮段真的到位时涨），
// segCounts 是五段的累计认领帧数，索引与 BEAUTY_SEG_* 的位序一致
// （0 磨皮 / 1 色调 / 2 五官 / 3 细节 / 4 美形）。用于日志确认「到底谁在跑 GPU」。
void beautyCoreChainStat(long long* hits, long long* calls,
                         unsigned long long* segCounts5);

// ---- T1：GPU 细节对拍（诊断，纯新增、零行为变化）---------------------------
//
// CPU 端细节基准：内部就是 beautycore::detailRefill()，单线程、全帧 ROI。
// 用于和 detail.hlsl 逐像素对拍，补上「细节段至今没有数值对拍」的空白。
//
// 两个刻意的选择：
//   1) dst 由调用方预置为**对拍起点**（本实现直接 memcpy(src, dst)），
//      让 GPU（image=原图）与 CPU 从同一张图起 —— 与 beautyCoreSmoothCpu
//      的隔离口径一致：只比算子本身，不比链式整合。
//   2) 时序 EMA 强制关（ema.lumT=nullptr）—— GPU 侧对拍帧同样 emaOn=0，
//      两边都无状态。GPU 的 det_hist 与 CPU 的 detailLumT_ 是两份互不相通
//      的历史，不关 EMA 会把「历史不同」误算成「算法错」。
bool beautyCoreDetailCpu(const unsigned char* src, unsigned char* dst, int w, int h,
                         const unsigned char* mask, const BeautyDetailDiag& d);

// ---- 5c：GPU 美形对拍（诊断，纯新增、零行为变化）---------------------------
//
// CPU 端美形基准：dst 先置成 src，然后对 wd 里的每张脸串行调用
// warpFaceWithGeom（多脸就地修改，与生产路径的调用顺序一致）。
//
// 几何**原样取自** wd，不重算 —— 这里要问的是「warp.hlsl 有没有照着这份几何
// 做对」，不是「几何算得对不对」。后者由离线黄金基准的 mixed 用例（美形三项
// 各 10 档）负责，两者互补；混成一条判据就分不清是几何错还是着色器错。
bool beautyCoreWarpCpu(const unsigned char* src, unsigned char* dst, int w, int h,
                       const BeautyWarpDiag& wd);

//
//   legacy      LegacyAdapter（仅离线基准 DLL） —— 与本次改动前逐位相同的行为
//   passthrough          直通，用于验证开关与降级
//   core                 自研引擎（T2 的落点，已实现磨皮）
//
// 环境变量只在第一次调用时解析一次（与 skipGlLock 等既有开关同一套写法）。
// 未设置时行为与改动前完全一致：不读环境、不打印、零开销。
// ---------------------------------------------------------------------------
IBeautyEngine& beautyEngine();
const char* beautyEngineId();
