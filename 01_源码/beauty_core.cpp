// beauty_core.cpp —— 引擎抽象层 + 三个实现的容器
//
//   1) PassthroughEngine：直通，用来证明「开关真的生效」；
//   2) CoreEngine：自研引擎，3.1.7 起是唯一的实现；
//   3) beautyEngine()：返回选定的实现（默认 core）。
//
// 3.1.21：旧的参考实现驱动适配层（LegacyAdapter）**已从插件里移除**。
// 它只在离线基准 DLL（ref_golden.dll）里保留 —— 那份 DLL 的作用是「量出参考
// 实现的行为」，不随插件发布。插件的编译单元里不再有任何参考实现二进制名。

#include "beauty_core.h"
#include "beauty_seg.h"    /* 方案 E / P0：整帧分割影子层 */
#include "beauty_face.h"
#include "beauty_freq.h"
#include "beauty_guide.h"
#include "beauty_log.h"
#include "beauty_onnx.h"
#include "beauty_pool.h"
#include "beauty_region.h"
#include "beauty_smooth.h"
#include "beauty_warp.h"   /* 3.1.27：美形三项的几何变形 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef BEAUTY_WITH_LEGACY_ENGINE
#include "legacy_engine.h"
#endif

// ---- 磨皮执行钩子（3.1.49 / Phase 3.5）------------------------------------
// 定义在文件顶部是因为 CoreEngine::process 在 1500 行附近就要用它，而对外
// 的 beautyCoreSetSmoothHook() 放在文件末尾的诊断区。
//
// 3.0.0：原注释写「宿主只有一个（OBS 插件）」—— 实机证明一个 OBS 场景可以
// 同时给多个 source 挂滤镜。这一批名字仍在文件顶部定义（兼容保留、不再被
// 引用），但 1050 行之后的引用已由宏重定向到 g_slots[t_slot]，按引擎槽各存
// 一份；配置类全局（g_cfg 等）仍进程级共享。详见 1040 行起的多实例说明。
static BeautySmoothHook g_smoothHook = nullptr;
static void* g_smoothUser = nullptr;
static long long g_smoothHookHit = 0;    // 钩子真的接管了的帧数
static long long g_smoothHookCall = 0;   // 钩子被问过的帧数（含回退）

// 段区间钩子（3.1.52）。与光滑钩子同一条约定：一个进程只挂一个，优先于它。
// 分开存放而不是合并成一个结构，是为了让「只装光滑钩子」这条已验收的路径
// 一行都不用改 —— 出问题时把段区间钩子摘掉就是 3.1.51 的行为。
static BeautyChainHook g_chainHook = nullptr;
static void* g_chainUser = nullptr;
static long long g_chainHit = 0;
static long long g_chainCall = 0;
static unsigned long long g_chainSeg[5] = {0, 0, 0, 0, 0};

// ---- 方案 E / P0：分割影子层的工作区与节流计数 --------------------------
// 1 MB（512×512×4）放全局而不是 worker 栈上（默认 1 MB 栈，放两份会挤爆）。
// 只在 BEAUTY_SEG=1 时被触碰；默认路径一行都不执行。
static beautycore::SegFrame g_segWork;
static int g_segTick = 0;
static int g_segEvery = 0;      // 0 = 首次使用时从环境变量取

// 跑一次分割。**两条路径共用这一份**：以前只有 async worker 里调，于是
// BEAUTY_ASYNC_DETECT=0（离线台、以及任何走同步检测的配置）下 BEAUTY_SEG=1
// 根本不执行 —— 分割结果永远是空的，而 segMix 一旦非 0 就会把整张掩码乘成 0，
// 表现为「开了分割美颜直接没了」。抽出来两边一起调，节流也只有一个计数器。
static void runSegIfDue(const unsigned char* src, int w, int h,
                        int geomW, int geomH, float mapScale, int mapOx, int mapOy,
                        const std::vector<beautycore::FaceBox>& boxes) {
    if (!beautycore::skinsegEnabled()) return;
    if (g_segEvery <= 0) {
        // 默认 12 帧：512 输入在 4 线程下约 95 ms/次、256 输入约 45 ms/次
        // （实测），30 fps 下每 12 帧≈400 ms，占用约 1/4 个核；调小会直接把
        // worker 压满，掩码更新被拖慢。
        const char* ev = std::getenv("BEAUTY_SEG_EVERY");
        int n = ev ? std::atoi(ev) : 12;
        g_segEvery = (n > 0 && n < 600) ? n : 12;
    }
    if (++g_segTick < g_segEvery) return;
    g_segTick = 0;
    // 取本帧最高分人脸做裁切框；没有就传空 → 内部退回整帧。
    const beautycore::FaceBox* bestFace = nullptr;
    for (const auto& fb : boxes)
        if (fb.w > 0.5f && fb.h > 0.5f)
            if (!bestFace || fb.score > bestFace->score) bestFace = &fb;
    // 计时与发布都在 skinsegRun 内完成（发布时 ms 必须已填好）。
    // 注意两套坐标：像素是缩略图（w×h），人脸框是全帧几何（geomW/H），
    // 换算靠 mapScale/mapOx/mapOy —— 漏传就会裁到错误的位置（2026-11-05 踩过）。
    (void)beautycore::skinsegRun(src, w, h, geomW, geomH, mapScale, mapOx, mapOy,
                                 bestFace, &g_segWork);
}

// 认领是否构成「执行顺序上的前缀」。
//
// 为什么必须校验：段区间钩子的收益来自「中间结果不出显存」，前提是认领的段
// 在链上连续。若宿主认领了 DETAIL 却跳过 REGION，引擎就会在 REGION 段拿
// 一份还没回读的 dst 去跑 CPU 算子 —— 结果是「用了上一帧的图」，画面静默错帧。
// 所以非法前缀直接整份丢弃（退回全 CPU），让错误只表现为「变慢」。
static bool chainClaimIsPrefix(unsigned claim, unsigned flags) {
    static const unsigned kOrder[5] = {BEAUTY_SEG_SMOOTH, BEAUTY_SEG_TONE,
                                       BEAUTY_SEG_REGION, BEAUTY_SEG_DETAIL,
                                       BEAUTY_SEG_WARP};
    bool gap = false;
    for (int i = 0; i < 5; i++) {
        const unsigned b = kOrder[i];
        if (!(flags & b))
            continue;           // 本帧不跑这一段，跳过不影响连续性
        if (claim & b) {
            if (gap)
                return false;   // 前面已经有没认领的段了 → 不连续
        } else {
            gap = true;
        }
    }
    return true;
}


namespace {

#ifdef BEAUTY_WITH_LEGACY_ENGINE
// ---------------------------------------------------------------------------
// LegacyAdapter —— 旧链路适配器（仅离线基准 DLL 编译，插件不编译这段）
// ---------------------------------------------------------------------------
class LegacyAdapter : public IBeautyEngine {
public:
    const char* id() const override { return "legacy"; }
    const char* version() const override { return LegacyEngine::inst().version(); }

    bool ensureReady(std::string& err) override { return LegacyEngine::inst().ensureReady(err); }
    bool preheat(std::string& err, bool& didInit) override {
        return LegacyEngine::inst().preheat(err, didInit);
    }
    bool ready() const override { return LegacyEngine::inst().ready(); }

    int acquireSlot(std::string& err) override { return LegacyEngine::inst().acquireSlot(err); }
    void releaseSlot(int slot) override { LegacyEngine::inst().releaseSlot(slot); }
    void setDetectOptions(int rotate, int landmarkQuality, int maxFaces) override {
        LegacyEngine::inst().setDetectOptions(rotate, landmarkQuality, maxFaces);
    }

    bool process(int slot, const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautySettings& s,
                 bool advanceFrame = true) override {
        return LegacyEngine::inst().process(slot, src, dst, w, h, s, advanceFrame);
    }

    int lastFaces() const override { return LegacyEngine::inst().lastFaces(); }
    bool regionActive() const override { return LegacyEngine::inst().regionActive(); }
    double lastCoreMs() const override { return LegacyEngine::inst().lastCoreMs(); }
    const char* lastErr() const override { return LegacyEngine::inst().lastErr(); }
    int itemId() const override { return LegacyEngine::inst().itemId(); }
    unsigned long ownerThread() const override { return LegacyEngine::inst().ownerThread(); }

    void release() override { LegacyEngine::inst().release(); }
};

#endif  // BEAUTY_WITH_LEGACY_ENGINE

// ---------------------------------------------------------------------------
// PassthroughEngine —— 直通
//
// 用途有两个：
//   * 证明 A/B 开关确实接上了（切过去画面应当变成完全没美颜的原图）；
//   * 作为自研引擎出错时的安全降级目标（比「滤镜整体直通」更安全：OBS 侧的
//     尺寸/纹理/自检逻辑仍然照常跑一遍，不会因为降级而悄悄失效）。
//
// 刻意**不**做任何图像处理：一旦它产出与输入不一致的像素，就说明骨架接错了。
// ---------------------------------------------------------------------------
class PassthroughEngine : public IBeautyEngine {
public:
    const char* id() const override { return "passthrough"; }
    const char* version() const override { return "0"; }

    bool ensureReady(std::string& err) override { err.clear(); return true; }
    bool preheat(std::string& err, bool& didInit) override {
        err.clear();
        didInit = false;
        return true;
    }
    bool ready() const override { return true; }

    int acquireSlot(std::string& err) override { err.clear(); return 0; }
    void releaseSlot(int) override {}
    void setDetectOptions(int, int, int) override {}

    bool process(int, const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautySettings&,
                 bool = true) override {
        if (!src || !dst || w <= 0 || h <= 0) return false;
        if (dst != src) memcpy(dst, src, (size_t)w * (size_t)h * 4);
        return true;
    }

    int lastFaces() const override { return 0; }
    bool regionActive() const override { return false; }
    double lastCoreMs() const override { return 0.0; }
    const char* lastErr() const override { return ""; }
    int itemId() const override { return 0; }
    unsigned long ownerThread() const override { return (unsigned long)GetCurrentThreadId(); }

    void release() override {}
};

// smoothstep 的 C++ 版（OBS 的 effect 里没有内置，这里自己写）
inline float smoothStep(float a, float b, float x) {
    if (b <= a) return (x < a) ? 0.0f : 1.0f;
    float t = (x - a) / (b - a);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// 约束到 [0,1]。面板档位是 0..100 的 double，面板外（离线脚本）可能给越界值。
inline float clampi01(double v) {
    const float f = (float)v;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// 五锚点查表（锚点对应面板 0/25/50/75/100），中间线性插值。
// 用它而不是幂函数：产品曲线在低档几乎是硬拐点（25 档 ≈ 满档的 0.5%），
// 任何单项幂函数都拟合不出「0.005 / 0.211 / 0.588 / 1.0」这条形状。
inline float lvLerp5(const float tbl[5], float t) {
    if (t <= 0.0f) return tbl[0];
    if (t >= 1.0f) return tbl[4];
    const float u = t * 4.0f;
    const int i = (int)u;
    const float f = u - (float)i;
    if (i >= 4) return tbl[4];
    return tbl[i] + (tbl[i + 1] - tbl[i]) * f;
}

// "a,b,c,d,e" → 5 个 float。缺项保持原值，便于只覆盖其中几个锚点。
inline void parse5(const char* s, float out[5]) {
    if (!s || !*s) return;
    const char* p = s;
    for (int i = 0; i < 5 && p && *p; i++) {
        out[i] = (float)atof(p);
        const char* q = strchr(p, ',');
        p = q ? (q + 1) : nullptr;
    }
}

// 自研核参数：CoreEngine 与离线调参入口共用。默认值 = 离线对拍最佳组合。
struct SmoothCfg {
    int radius = beautycore::kSmoothRadius;
    float sigmaSpace = beautycore::kSmoothSigmaSpace;
    float sigmaColor = beautycore::kSmoothSigmaColor;
    int down = 1;
    // 0 = 自动（按硬件并发数，上限 8）；1 = 强制单线程（调参复现用）
    int threads = 0;
    // 只在掩码包围盒内做滤波。关掉它只能用于「测量全帧成本」这类对照实验，
    // 生产路径必须保持开启：关掉后 93% 的算力会花在随后被丢弃的像素上。
    bool roi = true;
    // Sobel 边缘门控的灵敏度。0 = 关闭（退回纯掩码混合）。
    // 灵敏度取自 wuxiwuhen/obs-beauty-filter（MIT）的 PSComposite：
    //   edge = saturate(|∇luma| * k),  k 默认 10（该仓库用 ×10.0）
    // 实测：k=10 时高梯度区磨皮量降到 81.8%，低梯度区 100.0% 不变。
    // 调参见 05_文档/C2_边缘门控与保边核.md
    int edgeK = 10;
    // 五官保护强度 0..100。磨皮权重会乘 (1 - protect × protectK/100)，
    // 0 = 不保护，100 = 五官区域完全不磨。做法同 lumina 的 uSmoothProtect。
    //
    // 80 → 40 是 C 方案的一部分：原来唇细纹理 96.7%（参考实现89.3%），保护压太死。
    // 40 的取舍依据是**眼区**：protectK 0/20/40/60/80 对应眼区
    // 90.3/90.9/91.7/92.6/93.5，而参考实现是 91.3 —— 40 恰好落在参考实现线上，
    // 再往下眼睛就被磨得比参考实现还狠了（见 C15 §3）。
    int protectK = 40;
    // 阈值软膝：|src - smoothed| 超过它就开始减少磨皮，到 2.5 倍时完全不磨。
    //
    // **默认关闭（0）**：实测在我们的实现里几乎没有效果。原因是我们用的是
    // r=2 的双边滤波，本身就很保边 —— 全帧平均 |src-sm| 只有 0.064/255，
    // 连 15 的阈值都够不到，所以 thr=15/30/50 时高差值区的磨皮量都在 99.8%
    // 以上，等于没动。这条在 lumina 那边有意义，是因为它用的是大半径高斯，
    // |src-sm| 本来就大。
    //
    // 保留这个能力是为「以后换成更大半径的核」留口子，生产不开。
    int limitThr = 0;

    // ---- 高低频磨皮（频率分离）----
    // 1 = 用高低频（默认），0 = 退回旧的「双边滤波 + 线性混合」。
    // 旧方式的问题：dst = src·(1-t) + blur·t 会把**所有频段**一起削弱 t 倍，
    // 包括脸部的低频结构（明暗层次、骨骼起伏）→ 强度一大就"糊"。
    // 高低频只压「幅度小的高频」（毛孔/痘印），低频从不动。
    int freqMode = 1;
    // 低频尺度 = radius × iters。迭代比放大半径便宜：r=2 迭代 3 次 ≈ 半径 6 的
    // 保边模糊，成本却只有 r=6 的约 1/9（面积比）。
    // lowPath=3（盒式）时这一项不生效，尺度由 boxRadius/boxIters 决定。
    int freqIters = 2;
    // 幅度门控。magHi 从 30 抬到 60 是 C 方案的一部分：换成不保边的盒式低通后，
    // 高频层的幅度整体变大，30 会把痘印/鼻翼阴影一起当成「结构」放过去 ——
    // 实测 magHi 30→60 让额头 90.4%→81.0%、鼻翼 93.4%→85.4%（见 C15）。
    // 眉毛/唇线这些真边缘的幅度远大于 60，仍然被拦住（眼区实测 92.3%，参考实现 91.3%）。
    float freqMagLo = 6.0f;  // 幅度小于它：全按强度磨
    float freqMagHi = 60.0f; // 幅度大于它：完全不动（眉毛、鼻梁边、嘴唇轮廓）
    // 颗粒回填：把 4 邻域尺度的极细颗粒扣回一部分。
    // 0.5 → 0.10 也是 C 方案的一部分：它原本是给**双边迭代的阶梯感**打的补丁，
    // 盒式低通没有阶梯；而实测它是「细纹理保留率」的最大单一来源
    // （脸颊 60.2% → 80.2%，见 C15），留着就把刚追上的差距又吐回去了。
    float freqGrain = 0.10f;
    // 低频提取：0 = 双边迭代（快，有阶梯），1 = 引导滤波（无阶梯，O(1)）
    int freqLowMode = 0;
    int guideRadius = 12;
    float guideEps = 400.0f;

    // 低频层的第二条路线（与 freqLowMode 互斥，>0 时优先）：
    //   0 = 旧的 uint8 双边迭代（绝对颜色差、中间结果压回 uint8）—— 默认
    //   1 = 浮点双边（只换精度）
    //   2 = 浮点双边 + 低频参照（权重只看局部起伏）
    //
    // **默认 0 是实测结论，不是保守**：2026-10-28 用 golden 对三条路线做了
    // 逐项对照（05_文档/C8_低频层对照_浮点与参照.md），把 lumina 那套
    // 「(tap-center)-(refTap-refCenter)」搬到我们 r=2 的核上：
    //   小幅度高频保留 106.0% → 105.4%（几乎不变）
    //   大幅度保留        99.1% → 98.8%（更差）
    //   脸颊台阶像素      59.4% → 66.8%（更差，磨出更多平台边）
    //   成本              6.6 ms → 10.5 ms @640×394（+60%）
    // 四条都更差或持平，所以不投产。代码保留是因为：① 换更大半径的核时它可能
    // 才起作用（lumina 等效半径 ≈12px，我们只有 2）；② 它同时也是「ROI 局部
    // 浮点平面」这套基础设施的唯一使用者，删了要重写。
    // 3 = 盒式迭代低通（C 方案，见 C14/C15）：不保边，保边交给幅度门控。
    // **默认 3**（2026-10-28 起）：实测脸颊细纹理保留 86.0% → 64.9%（参考实现63.1%），
    // 四区域平均绝对差从 18.6pp 降到 2.0pp。旧的 0/1/2 三条双边路线全部保留，
    // 用 BEAUTY_LOW_PATH 切回去做对照。
    int lowPath = 3;
    // 参照半径（盒式）。8 → 17 像素窗口，约等于「大尺度」而不是「皮肤纹理」。
    int refRadius = 8;
    // lowPath=3 的尺度：等效 σ² = boxIters × ((2·boxRadius+1)² − 1) / 12。
    //
    // 默认 r=2 / iters=2 → σ≈2（两步 5×5 盒式 ≈ 9×9 高斯）。这个值是**按三频带**
    // 选的，不是按细频带：只看细频带的话 r=4/it=2 更贴近 参考实现（脸颊 65.0 vs 63.1），
    // 但它的中频/阴影会掉到 77.4/94.0，而参考实现是 83.8/96.8 —— 中频削太多就是
    // 「脸塌」。r=2/it=2 的三频带平均绝对差 1.92pp，r=4/it=2 是 4.49pp（C15 §3）。
    //
    // 扫参时最反直觉的一条：**r 越大磨得越少**。低通越糊，|src−low| 幅度越大，
    // 越容易被 magHi 门控拦住。别再靠加大半径去追强度。
    int boxRadius = 2;
    int boxIters = 2;
    // 磨皮档位的饱和系数（见 process() 里的 t_eff 公式）。0 = 退回线性。
    float blurSat = 0.16f;

    // ---- 磨皮核模型（3.1.14 / C22）----
    // 0 = legacy：盒式高低频 + 幅度门控 + blendWithMask（3.1.12/3.1.13 默认）
    // 1 = surface3：三层表面模糊，按参考实现 GLSL 结构重写（见 C22）
    // **3.1.17 起默认 1**（配合 s3Band=1 组成生产路径）。
    // 依据：真机帧 1920×1080 / 脸宽 529px / 磨皮 100 离线对拍「参考实现真值」
    // （加密 bundle、不下发 blur_type = bt2）：
    //     legacy     RMSE 3.251  改动量 1.480  空间相关 0.312  ← 装机旧默认，严重欠磨
    //     band T=14  RMSE 2.280  改动量 2.740  空间相关 0.769
    //     band T=8.3 RMSE ~1.63  改动量 ~1.98  空间相关 ~0.823  ← 新默认
    //     产品真值    —          改动量 2.034
    // 回退：BEAUTY_SMOOTH_MODEL=legacy
    int smoothModel = 1;
    // surface3 的两层半径 / 迭代 / 颜色阈值。默认值只是起点，由离线拟合定。
    // 半径**随档位增长**（C22 §4：真机 50 档等效 r≈6、100 档 r≈10）：
    //     r(t) = s3R0 + (s3R1|s3R2 − s3R0)·t
    int s3R1 = 14, s3It1 = 1;
    int s3R2 = 14, s3It2 = 1;
    int s3R0 = 2;                 // t=0 时的半径（两层共用）
    float s3Thr1 = 0.0f, s3Thr2 = 0.0f;
    // 档位曲线（3.1.14 起改用 GLSL 原式，不再拟合）：
    //     intensity  = mix(t·K, 1.0, step(0.7, t))   ← step 的自变量是**乘之前**的 t
    //     evenfactor = mix(E0, intensity − E0, step(0.6, intensity))
    // 三段形态：[0, 0.42] 恒 0.3；(0.42, 0.70] 线性升到 0.7；[0.70, 1] 恒 0.7。
    // （0.42 = 0.6/1.4286，即 intensity 刚够到 EThr 的档位）
    // 关键含义：t ≥ 0.7 之后 intensity 恒为 1，**75→100 档的差别不可能来自这里**，
    // 只能来自第一级预模糊 —— 这是第 2 批必须补上 pre_blur 的直接原因。
    // 旧的线性拟合式 e = eLo + (eHi−eLo)·t（0.45→0.60）已删除：它是阶段 3 扫参的
    // 产物，与源码结构不符（见 00_先读我.md §4.3、C23 §2.3）。
    float s3K = 1.4286f;          // 强度饱和系数（= 1/0.7，与 step 阈值配套）
    float s3ESat = 0.7f;          // step(0.7, t) 的阈值，用乘之前的 t
    float s3EThr = 0.6f;          // step(0.6, intensity) 的阈值
    float s3E0 = 0.3f;            // evenfactor 的下限，同时也是减数
    // >=0 时强制 evenfactor 取该值（离线扫参／诊断用；生产保持 -1 走原式）。
    // 为什么需要：e 只由 I 决定，于是 I<0.6 的档位 e 一律 0.3，低档纹理
    // 磨削量被压在平坦区的 1/2，而产品的低档反而是纹理先动。
    float s3EFix = -1.0f;
    // 眼睛保护的排除强度（%，100 = 完全按 protect 掩码排除）。
    // 3.1.14 起 protect 只有眼睛：参考实现没有 lip 通道，嘴唇当皮肤磨。
    float s3EyeK = 100.0f;

    // ---- 25 tap 表面模糊（第 2 批 #5/#6/#7，C23 §3）----
    // 0 = 旧的盒式两层（boxRgbRoi，半径随档位 + 按脸宽 fw/532 归一化）
    // 1 = 25 tap 表面模糊，固定 UV 半轴，串行两趟 yFactor 40→20
    // 用 BEAUTY_S3_TAP25=1 打开。**定稿前不翻默认**（默认仍是 0，legacy 与
    // 现有 surface3 的输出都不动）。
    int s3Tap25 = 0;
    // UV 半轴【实测】0a″ 光栅实验：四画幅 σx = 1.95/3.85/7.72/11.20 px，
    // 拟合 σx ∝ W^0.98 → 核定义在 UV 空间。这两个常量**不是拟合值**，别按
    // 画幅或脸宽去改它们 —— 改了就退回拟合路线（C23 §8.4 明令禁止）。
    float s3Sx = 0.002640845f;
    float s3Sy = 0.0046875f;
    // 两趟的值域权重系数【实测·标定，非源码】。容限 Δg = 1/sqrt(yFactor)：
    //   pass1 110 → 0.0953；pass2 55 → 0.1348
    //
    // 怎么定的（重要，别当成拟合）：C23 原来记的是 40/20【重建·待证】。用光栅法
    // 量自研引擎的等效核宽，40/20 得到 σx=4.67px，而参考实现实测 σx=3.85px —— 核宽
    // 了 21%，过磨的直接原因。扫 yFactor 得到 σx：40→4.67、80→4.10、160→3.54，
    // 对 log2(yFactor) 线性插值到 σ=3.85 得 yFactor≈109。取 110。
    // 验证（不是调参）：像素级改动量 2.73 灰阶 vs参考实现 2.71，几乎完全对上；
    // 皮肤内部 RMSE 2.795（盒式）→ 2.417。
    // 注意 yFactor 再往上（160/80）RMSE 只再降 0.056，但在噪声内，且幅度掉到
    // 2.46 明显欠磨 —— 按 RMSE 去挑就是拿金标拟合，不做。
    float s3YF1 = 110.0f;
    float s3YF2 = 55.0f;
    // 半分辨率处理【实测】。UV 与分辨率无关，所以它不改变核的物理尺寸。
    int s3Half = 1;

    // ---- 双尺度带通 + 振幅门控（第 2 批-B，C23 §11/§12）----
    // 0 = 关（25 tap / 盒式两条老路径）
    // 1 = surface3RgbBand：B1 窄、B2 宽两个不同尺度的模糊 + 振幅门控
    // **3.1.17 起默认 1**（与 smoothModel=1 配套 = 生产路径）。
    // 为什么要换掉 25 tap：§10.4 频响实测是双凹陷（稀疏环核的旁瓣），§11.4
    // 振幅依赖只有 1.05 倍（参考实现2.0 倍）。根因见 §11.5：B1/B2 必须尺度不同。
    // 回退：BEAUTY_S3_BAND=0
    int s3Band = 1;
    // 振幅门控阈值 T（灰阶）。w = 1/(1+(a/T)²)，a = blur(|C.g − B2.g|)。
    // 【3.1.17 重定】原值 14 是按「改动量 2.704」标的，而那个 2.704 来自我们自己
    // 的 ref_golden.dll 显式下发 bt=1 的金标 —— 不是参考实现产品。参考实现实测
    // **从不下发 blur_type**，生效档是 bundle 默认 bt2，同帧改动量只有 2.034。
    // 按产品真值重扫（同帧、磨皮 100）：T=14→2.740 / 12→2.529 / 10→2.279 /
    // 9→2.138 / 8→1.979 / 7→1.803。
    // 【3.1.17】先取 8.3：插值后改动量 ≈2.03、RMSE ≈1.63、相关 ≈0.823，与产品
    // 真值 2.034 相差 <0.3%。
    // 【3.1.19】改取 **6.5**：8.3 只对上了总量，对不上分布 —— 产品「越平越磨、
    // 越有结构越保守」，平坦/纹理比 1.39，而 8.3 只有 1.09。T 是唯一能改这个
    // 比值的旋钮（I/e 只等比缩放），降到 6.5 后比值 1.31，掉的总量由 e 补回。
    // 注意方向：T 越小 → gw=1/(1+(a/T)²) 整体越小 → 磨得越轻、纹理保留越多。
    float s3BandT = 6.5f;

    // ---- 门控陡度指数（3.1.19）------------------------------------------
    // gw = 1/(1+(a/T)^gwp)。p=2 是原式；p>2 让「越平越磨、越有结构越保守」
    // 的非对称变强 —— 平坦/纹理比是产品最明显的分布特征（产品 1.39，我方
    // p=2 时只有 1.10）。T 单独调不动这个矛盾：它同时缩放两个分区。
    float s3Gwp = 2.0f;

    // ---- 档位 → (I, T) 查表（3.1.18）------------------------------------
    // 为什么必须查表：band 路径原先只有 I 一条随档位变化的腿，而 I 在 t≥0.7
    // 就饱和到 1（s3ESat），于是 70~100 档输出逐字节相同（死区），且低档
    // 起步过强。产品五档实测（同帧离线、加密 bundle、不下发 blur_type）：
    //     档位  25      50      75      100
    //     改动  0.010   0.402   1.122   1.908     ← 凸曲线，低档几乎不动
    // 我方旧映射（T=8.3）：0.459 / 1.120 / 1.956 / 1.956 —— 低档强 46 倍、
    // 高段压死。归一化后产品的强度占比是 0.005/0.211/0.588/1.0，所以把
    // 强度项改成**按档位查表分段线性**，锚点直接取产品反解值。
    // T 不随档位变：100 档我方 flat/tex = 1.08 而产品 1.06，选择性本来
    // 就对得很齐（8 帧那次的 1.05 vs 1.39 是分类器口径不同造成的误判）。
    // 表可在运行时被 BEAUTY_S3_LV_I / BEAUTY_S3_LV_T 覆盖（离线扫参用）。
    int   lvTable = 1;
    float lvI[5] = {0.000f, 0.020f, 0.240f, 0.577f, 0.976f};
    // 3.1.19：T 由 8.3 → 6.5。T 是唯一能改「平坦/纹理相对强度」的旋钮
    // （I 与 e 只等比缩放）：8.3 时平坦/纹理比 1.09，产品是 1.39。T 降到
    // 6.5 把拐点前移，代价是总量掉 15%，由 e 补回 —— 见下。
    float lvT[5] = {6.5f, 6.5f, 6.5f, 6.5f, 6.5f};
    // evenfactor 也按档位查表。
    // 为什么把 e 从 I 上摘下来：旧式 e 由 I 决定，I<0.6 的档位 e 恒为 0.3，
    // 于是低档只磨平坦区、纹理几乎不动（同帧实测 50 档 flat/tex = 2.17）；
    // 而产品 50 档是 0.96（纹理与平坦一起磨）。e 固定后我方变为
    // 1.18/1.08/1.08，与产品的 0.96/1.11/1.06 同量级。
    // 3.1.19：0.70 → 0.95，与 T=6.5 配对。e 控制「向 B2 混合」的量，也就是
    // 低频压平（平坦区的主要改动来源）；T 压掉纹理后用它把总量补回 1.91。
    float lvE[5] = {0.95f, 0.95f, 0.95f, 0.95f, 0.95f};

    // ---- 清晰（clarity）：大半径局部对比 ----
    // 面板「清晰」100 档线性映射到 0..clarityMax。
    // 上限取 0.5 而不是 1.0：gain=1 时明暗差被原样翻倍，脸会显得像 HDR 过曝。
    // 0.30 是离线扫参的结果：面板 25/50/75/100 对应中频 100.2/101.1/103.0/104.7，
    // 大幅度带 99.7~100.0（不涨 = 边缘没被加亮）。
    float clarityMax = 0.30f;
    // 盒式半径 → (2r+1) 像素窗口。12 = 25px，大于磨皮核（≤8px）才叫「层次」。
    int clarityRadius = 12;
    // ---- 异步检测 ----
    // 1 = 检测跑在 worker 线程（默认），0 = 老的同线程同步检测。
    // 关掉它只为离线对拍：异步会让「第几帧用哪份掩码」依赖线程调度，
    // 逐字节哈希的测试会不稳定。
    bool asyncDetect = true;
    // 3.1.40（从 CPU 版 v3.1.39 移植）：无脸帧是否跳过整段处理。
    // 默认 true。曾检测到过脸、之后人走开时，直接逐字节透传，不再退回
    // 「无掩码 = 全帧磨皮」的兜底路径。BEAUTY_NOFACE_SKIP=0 逐字节回到
    // 旧行为（无掩码 → 全帧兜底磨皮）。
    bool noFaceSkip = true;
    // 异步路径的提交周期（帧）。0 = 每帧都提交。
    //
    // 为什么异步路径要每帧、而同步路径仍然是每 4 帧（见 kMaskEverySync）：
    //   · 异步时检测在 worker 上，渲染线程一分钱不花，只多烧一个核的一部分 ——
    //     20 核机器上 14.1ms/33.3ms ≈ 42% 单核，整机 +1.7 个百分点；
    //   · 同步时检测压在渲染线程上，每帧跑就是每帧 +14ms，那是掉帧，不是省不省的问题。
    // 3.1.8 之前两条路共用 4 帧，真机表现为「掩码拖在脸后面」：
    // 提交周期 5 帧 = 167ms，滞后从 1 帧锯齿漂到 5 帧，而掩码边界是脸框硬切边，
    // 拖影非常显眼。实测 YuNet 只要 14.1ms，慢的从来不是推理，是节流。
    int maskEveryAsync = 0;
    // 2.8.0（S7）：提交周期按「画面是否在动」自适应。
    //
    // 为什么需要它：maskEveryAsync=0 的语义是「每帧提交」，实测**画面完全静止**
    // 时这一条就常驻 0.476 核（720p，见 05_验证/cpu_probe.py），按每次推理 15.4 ms
    // CPU 反算，30fps 下等于每秒白烧 0.46 核。直播里主持人坐着不动的时间占比不低，
    // 这部分开销换不到任何画面：脸没动，掩码就不该重算。
    //
    // 做法：16×16 亮度网格与上一帧比，差异超阈值 → 立刻回每帧；否则拉到
    // detAdaptStill 帧。所以「人在动」时与 2.7.1 逐帧等价，只有静止时才降频 ——
    // 拖影风险被限制在「静止的那几帧」，而那几帧本来就没有拖影可看。
    // BEAUTY_DET_ADAPT=0 逐字节回到 2.7.1 的固定周期。
    bool detAdapt = true;
    int  detAdaptStill = 8;   // 静止时的提交周期（帧）= 3.75 次/秒 @30fps
    int  detAdaptThr = 6;     // 运动阈值：16 个块（4×4 格/块）里最大块平均差，0..255
    int  detAdaptHot = 3;     // 运动结束后继续每帧提交的帧数（吸收尾影与抖动）
    // 2.12.0（P1-1c）：16 个块怎么合并成一个数。
    //   false = 取最大（2.11.x 默认，逐字节不变）
    //   true  = 均方根 sqrt(Σm²/16)
    // 「取最大」对**局部**微动极敏感：真人说话时脸上任何一小块动就把整帧判成动，
    // 可人脸位置根本没变、检测结果完全没过期 —— 真素材实测判动 52.5%，
    // 方案C 的收益因此从 −60% 掉到 −18%（PROGRESS §9.32）。换成 RMS 后
    // 同一素材判动 10.2%，而真运动素材仍是 100%（p50 12.2，离阈值 6 有 2 倍余量）、
    // 人脸进出的过渡帧判动率一字不变（峰值 41.7 对 102.9，余量 7 倍）。
    // 阈值**不跟着改**：RMS 口径下 6 已经两头都对，扫过 5/6/7/8/10/12（calib_agg.py）。
    bool detAdaptAggRms = false;
    // 软膝：|mid| ≤ lo 全增益，≥ hi 完全不加。
    // 8/22 是扫出来的：软膝放到 20/45 时大幅度带会涨到 105.9（= 边缘被加亮，
    // 也就是光晕）。收到 8/22 后稳定在 100.0 以内，边缘平均改动只有 0.26 灰阶。
    float clarityKneeLo = 8.0f;
    float clarityKneeHi = 22.0f;

    // ---- 细节回填（锐化 / 全局锐化）----
    // 中频带增益基准，与参考实现路径的 kMidGain 取同一个值（legacy_engine.cpp）。
    // 这样「锐化 50」在我们的引擎里和在参考实现里指的是同一条曲线：细频带 100% 回填、
    // 中频带推到原图的 1.65 倍。数值对照见 05_文档/C11_锐化与全局锐化.md。
    float detailMidGain = 3.30f;
    // 细频带软阈值：幅值低于它的高频直接不回填。相机噪点与毛孔在幅值上重叠
    // （±2~6 vs 2~8），只能从最细的一端削掉一点；3.0 是「削噪但不动毛孔」的
    // 折中值，靠 BEAUTY_DETAIL_THR 可现场改。
    float detailThrFine = 3.0f;
    // 3.1.35：磨皮=0 时的「纯锐化」参数（脸内锐化专用，与磨皮>0 的回填路径隔离）。
    // 磨皮>0 时脸内锐化是「回填被磨掉的高频」：dst 已失高频，加回 src 高频 =
    // 把高频补回原值量级，增益走 detailMidGain 那条曲线（沿用历史值，不动）。
    // 磨皮=0 时 dst≈src，语义变成「纯锐化增强」，机制上有两个坑：
    //   (1) 中频带(mid)在磨皮=0 时会对全脸皮肤产生一份固定基底偏移
    //       （实测 100 档 chg 2.74、且 25 档就已有 0.62 的与档位无关的基底），
    //       所以纯锐化模式**关掉中频带**（mid=0），只留细频带。
    //   (2) 细频带默认 thrFine=3.0 的软阈值会把磨皮=0 时幅度本就很小的高频
    //       几乎全削掉（实测纯细频+默认阈值时 100 档 chg 仅 0.078、dret 1.05，
    //       等于没锐化），所以纯锐化模式把阈值放开到 faceSharpThrFine（默认 0）。
    // 放开后细频带才能产生 SDK 原生锐化那种「细节翻倍」的观感（dret→1.6+）。
    // 离线用 BEAUTY_FACE_SHARP_GAIN / BEAUTY_FACE_SHARP_THR 扫参，生产初值 1.0 / 0.0。
    float faceSharpGain = 1.0f;
    float faceSharpThrFine = 0.0f;

    // ---- 全局锐化自己的旋钮（3.1.22 新增，从 detailMidGain / detailThrFine 拆出）----
    // 为什么拆：原来全局锐化与脸内锐化共用同一条中频曲线与同一个细频阈值，
    // 改一个就会动到另一个 —— 两边的档位语义根本不同，没法各自标定。
    // 参照路径（legacy_engine.cpp 的 applySharpen）里它们本来就是两条独立曲线：
    //     脸内：  am = 锐化/100,          gm = min(锐化/100 × kMidGain, 3.0)
    //     掩码外：aG = 全局锐化/100,     gG = min(全局锐化/100 × kMidGain, 3.0)
    // 拆开后全局这一路就能单独扫参（见 06_离线验证/全项对齐_20261030/
    // sweep_gsharpen.json）。
    float globalMidGain = 3.30f;    // 全局锐化的中频带基准增益（参照 kMidGain）
    float globalFineGain = 1.00f;   // 全局锐化的细频带基准增益（面板 100 = 全额回填）
    // 全局锐化的细频软阈值。生产值 2.00（3.1.22 扫参定档）：
    //   阈值 3.0 → 25 档改动量 −8.1%、100 档 dret 2.478
    //   阈值 2.0 → 25 档改动量 −7.5%、100 档 dret 2.494（参照 2.837）
    //   阈值 1.0 → 25 档改动量 −6.2%、但 100 档改动量 +3.1%（开始过量）
    // 阈值 0 看着 dret 最接近（2.826 vs 2.837），但改动量过 10%（4.623 vs 4.194），
    // 净效果更差 —— 说明我方的细频带本来就比参照强一点，不能再往上加。
    float globalThrFine = 2.00f;    // 全局锐化的细频软阈值（与脸内分开标定）
    float globalThrMid = 0.00f;     // 全局锐化的中频软阈值（默认不削）

    // ---- 磨皮后的「基准细频回填」（3.1.22 新增）----------------------------
    // 为什么要它：单开磨皮 100 档时我方细节保留 dret=0.734，参照=0.834 ——
    // 我方比参照多削了 12% 的高频；同时我方总改动量 2.009 还比参照 1.908 高。
    // 也就是说：我方把能量挪到了**中频**，而不是像参照那样留在低频。
    // I / e / T / gwp 四个旋钮都改不动这个形状（它们让「总量」与「细节保留」
    // 永远反向，见 06_离线验证/全项对齐_20261030/sweep_blur.json）。
    // 真正的解法是把**细频带**按比例还回来：磨皮少了多少高频，就还回去多少。
    // 这不会额外增加改动量 —— 还回去的高频本来就是原图的一部分，
    // 补得越多反而越接近原图（chg 下降）。
    // 只在「用户没开锐化、也没开清晰」时生效（开了就用用户自己的档位）。
    // 生产默认值 0.06（3.1.22 实测定档）。
    // 扫参结论（100 档 / 3 帧均值，参照 chg=1.905 / dret=0.835）：
    //   refill 0.05 → chg 1.914  rmse 1.57  dret 0.838
    //   refill 0.10 → chg 1.913  rmse 1.58  dret 0.839
    //   refill 0.15 → chg 1.913  rmse 1.58  dret 0.841
    //   refill 0.30 → chg 1.913  rmse 1.60  dret 0.850
    //   refill 0.40 → chg 1.916  rmse 1.62  dret 0.858（已超参照，RMSE 变差）
    // 0.05~0.10 是量化吸收后的平台区（dret 基本不动），0.10 起单调上升。
    // 取 0.06：落在平台区中部，dret≈0.838 贴参照 0.835，RMSE 1.57 优于基线 1.58。
    float blurRefill = 0.06f;
    // 回填只在磨皮强度 ≥ 这个值时启用（I 是查表强度，0/25/50/75/100 档
    // 对应 0.00/0.08/0.24/0.58/0.98）。0.3 大致落在 50 与 75 档之间。
    float blurRefillMinI = 0.3f;
    // 1 = 用 delta 模式（加回「原图细频 − dst 细频」）。默认 0，
    // 见 wantBlurRefill 处的实测对比。
    int blurRefillDelta = 0;
    // 基准回填专用的细频软阈值（灰阶）。比锐化那套（3.0）低得多：
    // 磨皮要还的就是「小幅值细频」，锐化要防的却是「噪点被放大」，
    // 两者的取舍正好相反，所以不能共用一个阈值。
    float blurRefillThr = 0.8f;

    // ---- 细节层时序 EMA（0.7.2）----
    // 锐化会把相机白噪一起放大，而噪点帧间独立 → 静止的脸呈「沸腾」式闪动。
    // 把算高通用的亮度平面做一次运动自适应 EMA：静止强平滑、头一动立刻放开。
    // 常量照抄参考实现侧已标定过的一组（legacy_engine.cpp 的 kDetailBetaStill 等）：
    // 静止 beta 0.20（噪声 ×0.38），运动判据 0.15/0.40 对应 rawChg 0.79/1.44。
    //
    // 我们的运动判据换成「全帧亮度帧间差均值（4×4 抽样，单位=灰阶）」，比 参考实现的
    // rawChg 更直接：相机静止 + 传感器噪声实测在 1~3 之间，真人头动在 8 以上。
    // 阈值 2.5 / 8.0 是这两段之间取的空隙，两侧都有余量。
    bool  emaOn = true;
    float emaStill = 2.5f;       // 帧间差 <= 它 → 按静止（强平滑）
    float emaFull = 8.0f;        // 帧间差 >= 它 → 完全不平滑（零拖影）
    float emaBetaStill = 0.20f;  // 静止时新帧的权重

    // ---- 人脸局部五项（五官立体 / 亮眼 / 去黑眼圈 / 去法令纹 / 祛斑痘）----
    // 增益单位是「灰阶」：面板拉满时椭圆中心处的亮度增量（可用负值=压暗）。
    // 每个值都是先量参考实现再定的 —— 用 ref_golden.dll 把该项单独拉满、与该项为 0
    // 的输出相减，取连通域的峰值。所以这些数字是「对齐观感」而不是猜的：
    //     五官立体 峰值 +7.3 / -4.7    亮眼 峰值 +44（眼白本就接近饱和）
    //     去黑眼圈 峰值 +7.7           去法令纹 峰值 +3.3
    // 数据与差分图见 06_离线验证/goldens/_probe/ 与 05_文档/C12_人脸局部五项.md
    // 上面那组是**峰值**，直接当增益用会过头 —— 峰值是连通域里最亮的几个点，
    // 而椭圆中心一大片都会拿到满增益。下面这版是按「脸内 dY 均值 + p95」对拍
    // 回调过的（core vs 参考实现：立体 +2.97/+6.0 → 目标 +0.64/+5.1，依此类推）。
    // ---- 五官局部三项：3.1.22 按参照差分图重建后的旋钮 --------------------
    //
    // 【亮眼】参照在这批素材上几乎不动（峰值 < 1 灰阶），保持原样不动它。
    float regEyeGain = 12.0f;
    //
    // 【五官立体】= 8 个锚在关键点上的宽椭圆（见 applyFaceRegions 的 threed 表）。
    // 三条关键结论（都是实测出来的，不是猜的）：
    //   1. 参照的立体是**解剖/网格驱动**的：额头恒亮、脖子恒暗，与画面自身
    //      明暗无关。之前用「低频明暗场」路线实测 corr≈0.05（头发/背景污染），
    //      已废弃。
    //   2. 参照的**面部中线 = 双眼中点**，不是 YuNet 的鼻尖 —— 侧脸时鼻尖
    //      会偏 0.12 脸宽，参照的中线不偏。所有中线特征（鼻梁/鼻底/额头）
    //      都锚在 midX/midY 上。
    //   3. 增益分「亮部 / 暗部」两组 feather（亮部 0.705 平台大、暗部 0.498），
    //      因为参照的提亮区是大片平顶、压暗区是窄带。
    //   regThreedGain : 整体比例因子（离线对拍用）。
    float regThreedGain = 1.0f;
    // 【去黑眼圈】参照：两个小块在眼下方，峰值 17.3~18.5，**与暗度无关**
    //   （corr(原亮度, Δ) ≈ +0.1），是纯几何区域 → 双层椭圆（亮核 + 淡晕）。
    //   转头时近侧眼袋在画面上被拉宽 → 用 yaw（鼻尖相对眼中点的偏移）做
    //   透视缩短修正，rx *= 1 − regPouchYawK·yaw·side。
    float regPouchYawK = 1.5611f;     // 3.1.23 重新拟合值（原 2.0）
    float regPouchScale = 1.0f;      // 黑眼圈双层椭圆的整体增益比例
    // 【去法令纹】参照：峰值恒为 **+52**（封顶），且 corr(原亮度, Δ) = −0.4
    //   → 它不是「固定形状 + 固定亮度」，而是**把暗线填平到周围皮肤亮度**：
    //   Δ = min(maxLift, slope·max(0, LR − Y))，LR = 邻域最大亮度。
    //   3.1.23：加**暗线闸门**（gateLo/gateHi）把响应收窄到褶皱线上；
    //   限制因子在 applyFoldRemoval 内部改成 max(皮肤掩码, 区域权重)。
    //   3.1.23 第二轮（双瓣模型 naso6，8 帧统一拟合 fit_naso6.json）：
    //   上瓣参数整体收紧（k 0.60→0.45、闸门 8.5/15.0→19.2/29.9、
    //   LR 半径 6→4、封顶 60→57.5）—— 旧值让上瓣过冲到封顶，
    //   而参照的峰只有 48。
    float regNasoSlope = 0.4500f;    // 每 1 灰阶的「暗差」提亮多少（3.1.23 双瓣重拟合）
    float regNasoMaxLift = 57.5000f; // 封顶（双瓣拟合值；参照实测峰值 51~52）
    int regNasoLrRadius = 4;         // LR = 半径 4 的邻域最大亮度（双瓣拟合值）
    float regNasoGateLo = 19.1526f;   // 暗差 < gateLo → 不起作用
    float regNasoGateHi = 29.8535f;   // 暗差 > gateHi → 完全起作用
    // 祛斑痘：拉回量 = k × 偏离局部均值的幅度。
    //
    // 3.1.64（2026-11-05，用户定方案 A）：0.25 → 0.75。理由与代价：
    //   · 0.25 是「参考实现的 dY p5/p95 = -3.6/+3.5」反推出来的，但那个幅度
    //     对应的是**合成的、对比度很高的**斑点图；真人 720p 摄像头上痘印的
    //     dev 中位只有 1~2 灰阶，乘 0.25 后 0.25~0.5，lroundf 之后多数是 0
    //     —— 满档实测只有 1.30% 像素变化、maxdiff 7，滑块观感等于没有。
    //   · 实测 k=0.5/0.8/1.0 的满档 maxdiff 分别是 15/24/24，0.75 落在
    //     「肉眼可辨」这一档。
    //   · 代价（历史实测，必须记住）：k=0.55 时 core 的 dY 到 -11/+7，暗斑
    //     被打得太狠，痘印会变成一块块**发亮的补丁**。因此 0.75 只在
    //     「dev 判据仍然是双向 ±」的前提下成立；若发现补丁，先回到 0.5 或
    //     改判据（只压 dev<0），不要继续往上加。
    //   · BEAUTY_REG_SPOT 可逐帧覆盖，装机后要 A/B 就用它，别改源码重编。
    float regSpotK = 0.75f;
    int regSpotRadius = 2;          // 5×5 窗口。2~4 是斑点尺度，再大就吃到结构
    float regSpotThrLo = 1.4f;      // 小于它算皮肤细纹理，不动
    float regSpotThrHi = 30.0f;     // 大于它算真边缘（睫毛/鼻孔/唇线），不动
};

// 关键点是否可信。
//
// 遮挡（手挡脸、口罩、低头）或大侧脸时，YuNet 仍会输出 5 个点，但位置可能
// 离谱到人脸框外面。直接用它们画五官保护区，椭圆会跑到画面别处，把不该保护
// 的地方保护掉（或者该保护的没保护）。
// 判据：五个点都必须落在人脸框外扩 50% 的范围里 —— 正常侧脸也就贴边，
// 超出这个范围说明这组关键点不可信，宁可这次不做五官保护。
static bool kpsSane(const beautycore::FaceBox& f) {
    if (!f.hasKps) return false;
    const float mx0 = f.x - f.w * 0.5f, mx1 = f.x + f.w * 1.5f;
    const float my0 = f.y - f.h * 0.5f, my1 = f.y + f.h * 1.5f;
    for (int p = 0; p < 5; p++) {
        const float x = f.kps[p * 2], y = f.kps[p * 2 + 1];
        if (x < mx0 || x > mx1 || y < my0 || y > my1) return false;
    }
    return true;
}

// 在缓冲上画一个软边椭圆，权重 0..255，取较大值。
//
// 公式抄 dinjufen/lumina-beauty 的 kComposeShader（核心代码为 MIT）：
//     weight = (1 - smoothstep(0.35, 1.0, du² + dv²)) × strength
// 注意它把**平方距离**直接喂给 smoothstep（不是 sqrt），这里保持一致。
void paintProtectEllipse(unsigned char* buf, int w, int h,
                         float cx, float cy, float rx, float ry) {
    if (!buf || rx < 0.5f || ry < 0.5f) return;
    int x0 = (int)(cx - rx), x1 = (int)(cx + rx) + 1;
    int y0 = (int)(cy - ry), y1 = (int)(cy + ry) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    const float irx = 1.0f / rx, iry = 1.0f / ry;
    for (int y = y0; y < y1; y++) {
        const float dv = (y + 0.5f - cy) * iry;
        unsigned char* row = buf + (size_t)y * w;
        for (int x = x0; x < x1; x++) {
            const float du = (x + 0.5f - cx) * irx;
            const float q = du * du + dv * dv;
            if (q >= 1.0f) continue;
            const float t = smoothStep(0.35f, 1.0f, q);
            const int v = (int)((1.0f - t) * 255.0f + 0.5f);
            if (v > (int)row[x]) row[x] = (unsigned char)v;
        }
    }
}
SmoothCfg g_cfg;

// 异步掩码的提交计数（诊断用，只增不减）。
// 为什么需要它：提交周期从 4 帧改成每帧，从「每帧毫秒」上完全看不出来 ——
// 检测跑在 worker 上，渲染线程一分钱不多花。唯一直接可测的就是「多少帧提交了几次」。
uint64_t g_maskSubmits = 0;

// 2.8.0（S7）：自适应检测周期的两个瞬时诊断量（每次 updateMask 刷新）。
//   g_detPeriod —— 最近一帧算出的提交周期，1 = 每帧（与 2.7.1 等价）。
//   g_detDiff   —— 最近一帧的 16×16 网格平均亮度差，阈值是 detAdaptThr。
// 为什么放全局：与 g_maskSubmits / g_frameTag 同一个理由 —— 一个进程只有一份
// 引擎，宿主只认「引擎」这一个对象，全局量最不容易在包装层传丢。
int g_detPeriod = 0;
int g_detDiff = 0;
// 2.11.1：运动判定本身的命中率（本窗口「判成在动」的帧数 / 判定次数）。
// 为什么需要它：dper/dmot 都是**瞬时值**（窗口最后一帧 / 窗口最大值），
// 无法回答「这个窗口里到底几帧被判成在动」。2.11.0 在现场就是被这一点
// 骗过去的 —— `nopix=0/180` 同时可以由「每帧都在动」和「分支根本没进」
// 两种原因造成，光看瞬时值分不开。见 PROGRESS §9.31。
int g_motHot = 0;
int g_motCalls = 0;

// 无脸透传帧数（3.1.40，诊断用）。
// noface=N 才能证明「本窗口真的有 N 帧是因为没人而跳过的」，是验证
// 3.1.40 修复是否生效的唯一直接指标（移植自 CPU 版 v3.1.39）。
uint64_t g_noFaceFrames = 0;

// 掩码↔帧对齐的计数（3.1.10，诊断用）。
// 为什么需要它：对齐有没有生效、真机上是不是常态，日志一个字都看不出来。
// ratio = alignUsed / alignTotal，30fps 下应 ≈1；worker 追不上时会掉。
uint64_t g_alignUsed = 0;
uint64_t g_alignTotal = 0;

// 5b-2 切片 4：宿主帧号（单调自增，宿主每帧设一次）。
// 为什么不放引擎成员里：GpuEngine 是 CoreEngine 的壳，process 直接转发，
// 宿主也只认「引擎」这一个对象；一个进程内只有一份引擎，全局量足够且最不容易传丢。
unsigned long long g_frameTag = 0;

// 2.7.0（S4b）：检测输入缩略图。宿主在 GPU 上把整帧 letterbox 到 S×S（默认
// 640×640）后回读，引擎拿它跑检测 —— 全帧回读（4K 33 MB）从此只在「认领不全、
// 需要 CPU 全帧像素」的帧上发生。
//
// 为什么不塞进 Engine 成员：与 g_frameTag 同一个理由，宿主只认「引擎」这一个
// 对象，而 updateMask 是 Engine 的私有方法，全局量是最短的一条线。
//
// 语义（三个都必须给对，否则几何会系统性偏移）：
//   · p / tw / th   缩略图像素本体（RGBA8，stride = tw*4）；
//   · fullW / fullH 几何坐标系（全帧尺寸）—— 掩码几何/五官椭圆都在这个系里算；
//   · scale/ox/oy   宿主做 letterbox 时用的那三个数，必须与 FaceDetector 内部
//                   的 letterboxRgb 同一套算法（scale=min(S/w,S/h) 等），因为
//                    反算 full=(t-offset)/scale 是它的逆。
// p 为 nullptr 即「本帧没有缩略图」，行为与 2.6.0 逐字节相同。
static const unsigned char* g_thumbPx = nullptr;
static int g_thumbW = 0, g_thumbH = 0;
static int g_thumbFullW = 0, g_thumbFullH = 0;
static float g_thumbScale = 0.0f;
static int g_thumbOx = 0, g_thumbOy = 0;

// 2.11.0（方案C）：运动判定输入。宿主每帧喂一张小图（默认 64×64 RGBA），
// 引擎在 detectPeriod 里把它切成 16×16 格，每格取「格内逐像素 |Δ| 的最大值」，
// 得到与原来同样形状的 16×16 网格（**不是**格内先平均 —— 那样会抹平运动信号，
// 实测对齐从 180/180 掉到 22/180，见 detectPeriod 里的注释）。
// 置空 = 回到「从检测输入点采样」的老路径（逐字节不变）。
static const unsigned char* g_motPx = nullptr;
static int g_motW = 0, g_motH = 0;
// 本帧的提交预判（beautyCoreDetectDue 写、updateMask 读并清）。
// 只在宿主真的调过预判的帧上有效；不调就照旧由 updateMask 自己判定。
static bool g_duePending = false;
static bool g_dueValue = false;
// 本帧有没有全帧像素（缩略图帧 / nopix 帧 = 没有）。对齐那一段靠它区分
// 「有像素但没用上」与「本来就没有像素」—— 两者该做的事完全不同。
static bool g_noPixFrame = false;

// 掩码参数：人脸框外扩倍数。S1 对拍扫出来 IoU 峰值在 1.15
// （IoU 0.727 / 精确率 0.934 / 召回率 0.767），见 05_文档/S1_掩码对拍结论.md。
const float kFaceBoxGrow = 1.15f;

// 人脸检测的推理输入边长默认值。
//
// 必须保持 640：我们的 YuNet 2023mar ONNX 把输入声明成固定 640x640，ORT 会
// 直接拒绝其它尺寸（`Got: 320 Expected: 640`）。「降到 320 省 4 倍算力」这条
// 路在当前模型上走不通 —— 详见 beauty_face.h 里 setInputSize 的说明。
// 检测提速改走「推理多线程」而不是「降分辨率」。
const int kFaceInputDefault = 640;

// 脖子肤色通道（2026-10-28 新增）
//
// 背景：S1 当时放弃补脖子，理由是「磨皮场景下漏磨脖子比误磨背景更可接受」。
// 用户 2026-10-28 要求「对齐 OBS美颜摄像头」，而参考实现的掩码覆盖到脖子
// （face_src 上到 y≈332，只磨脸的话停在 232），所以补回来。
//
// 三条硬约束（都是 S1 已经验证过的，别放宽）：
//   * **不能放大矩形框** —— grow 到 2.2 时精确率从 0.934 掉到 0.393，
//     矩形会把背景整块框进来；
//   * 横向只能收窄：半宽取脸宽的 0.30（总宽 0.60 脸宽），比脸框窄一截，
//     这样左右两侧的背景不会被拉进来；
//   * 纵向只到脸高的 0.55，再往下就是衣服；
//   * 依然要求 isSkin —— 白衣服（R≈G≈B，不满足 R>G）和灰背景靠这条挡。
// 参数怎么定的：`06_离线验证/neck_sweep.py` 在 S1 的 8 个有效场景上扫网格，
// 以参考实现的掩码当真值（192×256 口径，与 S1 可直接比）：
//     只磨脸           IoU 0.6981
//     半宽0.30/长0.55  IoU 0.7934（初版估值）
//     半宽0.45/长0.65  IoU 0.8442  ← 峰值，取这组
//     半宽0.60/长0.75  IoU 0.7701（横向放到 1.2 倍脸宽开始框进背景）
// 曲面在峰值附近很平（0.40~0.45 × 0.55~0.75 都在 0.83 以上），不是过拟合尖点。
const float kNeckHalfW = 0.45f;   // 半宽 = 脸宽 × 0.45（总宽 0.90 脸宽，仍窄于脸框）
const float kNeckLen = 0.65f;     // 长度 = 脸高 × 0.65

// 脖子「跟随肤色」时的探索范围（3.1.20，g_maskShape 的 bit 4）
//
// 两个数都是**允许范围**而不是**裁剪位置**：边界最终落在肤色真正结束的地方
// （脖子侧面、头发、衣领），这两个数只决定「最多允许探多远」和「探不到时兜底多宽」。
//   * maxHalf 0.70 脸宽：比旧矩形（0.45）宽，用于覆盖真实脖子的完整宽度；
//     仍设上限是因为肤色背景（v_skinbg 场景）会把掩码拉爆，S1 已验证放大矩形
//     精确率会掉到 0.393。
//   * minHalf 0.30 脸宽：肤色被阴影/胡须/镜框打断时的兜底，避免掩码被切碎。
const float kNeckSkinMaxHalf = 0.70f;
const float kNeckSkinMinHalf = 0.30f;

// 掩码边界羽化（3.1.9，修「硬边拖影」；3.1.10 收窄 + 依赖帧对齐）
//
// 问题：异步检测最快也有 1 帧滞后（33ms，worker 忙时会到 2 帧）。肤色那层本来
// 就是软边，但**人脸框是硬裁**（下面 buildMaskInto 直接截 x0..x1 / y0..y1），
// 头部一动，脸后就会留下一条硬边——观感上就是「拖影」。
//
// 做法：在框边缘 F 像素内把权重平滑降到 0。F 是 smoothstep 而不是线性 ——
// 线性斜坡在两端有折角（一阶不连续），大脸近距离看能看到一条「淡等值线」；
// smoothstep 两端导数为 0，看不到折点。
//
// **羽化治不了滞后本身**：滞后 33ms 时边界每帧都在脸后扫动，羽化只是把"硬边扫过"
// 变成"软边扫过"，拖影带还在。3.1.10 的掩码↔帧对齐（见 updateMask / collectAsync）
// 才是把滞后从「掩码落后画面」改成「整帧落后 33ms」—— 对齐之后边界不再扫动，
// 羽化只负责软化矩形裁剪的硬角，所以才能收窄到 8%。
//
// 为什么不做运动预测：预测在急停/反向时会把掩码甩到脸前面（过冲），那是另一种
// 更难看的东西。帧对齐 + 软边比它便宜且稳。
//
// 为什么是 0.08 而不是 0.12：3.1.9 第一版跟了 wtnl-zhi 的默认 12%，真机反馈
// 「整体效果弱了较多」—— 我们的掩码不只是脸的椭圆，还有脖子通道（横向 0.90 脸宽、
// 一直伸到领口），12% 脸宽的带子打进脖子的横向范围就是 13% 半宽，脸框四条边各削
// 一条带子加起来实测把掩码总能量削掉 10%，而 face_src 离线图上脸不贴框，这个数字
// 被低估了。8% 回到自定的第一版，配合 3.1.10 的「掩码与帧对齐」够用 —— 对齐之后
// 边界不再每帧移动，羽化只需要把矩形裁剪的硬角软化，不再需要补偿滞后。
// （wtnl-zhi 的 12% 是骑在椭圆边界上内外各一半的，我们只能向内，等价于更厚的边。）
const float kMaskFeatherRatio = 0.08f;

// -1 = 按脸宽自适应；0 = 关闭（与 3.1.8 逐字节相同，A/B 用）；>0 = 固定像素
int g_maskFeatherPx = -1;

// 2.13.0：把**检测几何**整体平移 (dx,dy) 像素，纯诊断，默认 0 = 不介入。
//
// 为什么需要它：2.12.0 的 RMS 聚合让掩码的平均年龄从 1 帧涨到 3 帧，日志里
// 只能看到 `align%` 68.5%→23.5% 和「掩码偏了 16.8 px（p90）」。但「偏 17 像素」
// 不是结论 —— 结论是**画面上值多少灰阶**。这个开关就是把「偏 d 像素」这件事
// 做成一帧可以被逐字节对拍的输入：静态素材、两个 d、两张截图相减。
//
// 只平移掩码几何、不动交给五官段的那份（`fboxOut` 在平移前就拷走了）：
// 这样测出来的是「磨皮区域错位」的代价，不掺五官/美形的位移。
// 生产路径永不设置它，因此默认值下逐字节等于 2.12.0。
int g_detOffX = 0;
int g_detOffY = 0;

// 掩码几何形状（3.1.20）。
//
// 问题：旧版把掩码画成**两个矩形**——脸框一个、脖子通道一个。羽化只能把硬边变成
// 软边，边本身还是**直的**：脖子左右两条竖线永远「切」在脖子皮肤上，线外一刀不磨。
// 头部快速摆动时这两条直边在脖子上来回扫，观感就是「遮罩残影」（用户 2026-10-30 反馈）。
//
// 做法：把裁剪边界从矩形换成**椭圆距离场**，边界沿 (x/a)²+(y/b)²=1 连续收敛，
// 没有一段是直的；羽化仍然沿用 kMaskFeatherRatio 的斜坡，所以只是「形状」变了，
// 「软硬」不变。
//
// 位掩码（可用 BEAUTY_MASK_SHAPE 覆盖，离线 A/B 用）：
//   0 = 矩形（3.1.19 及以前的旧行为）
//   1 = 脸用椭圆
//   2 = 脖子用椭圆
//   3 = 两者都用椭圆
//
// 2.15.0 起默认 3（椭圆）。理由不是审美：**矩形边是直的**，而人脸相对画面
// 一歪（歪头、或源本身被转了 90°）那条直边就横切在皮肤上，用户看到的正是
// 「长方形美白遮罩」。椭圆边界处处是曲线，配合 g_maskRoll 的转角，边界
// 始终贴着脸自身轮廓 —— 两件事必须一起做，只开椭圆也救不了 90° 躺着的脸
// （那时脸框的宽高是互换的，轴对齐椭圆的半轴长度整好差一个身位）。
int g_maskShape = 3;

// 2.15.0：掩码几何跟随人脸滚转角（见 faceRoll()）。
//
// 0 = 关闭，几何永远轴对齐（2.14.1 及以前的行为，逐字节一致，用于 A/B 与
//     回归对拍）；1 = 跟随。默认 1。
int g_maskRoll = 1;

// 2.15.3：掩码几何的时间平滑系数（0..255 的 α，按 1/256 解释）。
//
// 为什么需要它：检测每跑一次，框中心抖 1~3 px、滚转角抖 1~3°。单帧看不出来，
// 但脸一大（怼镜头）或歪着头，这点抖动摊到掩码边界上就是「抽风闪」。
// 90/256 ≈ 0.35 是折中：抖动量降到约 0.45 倍，同时肉眼看不到跟手变慢
// （大跳会直接采纳，见 emaMaskFace，所以快动不拖影）。
// 0 = 关闭（逐字节回到 2.15.2 的掩码几何），可用 BEAUTY_MASK_EMA 覆盖。
int g_maskEma = 90;

// 2.15.3：掩码几何时间平滑的状态（见 emaMaskFace），按「脸的序号」存。
// 放文件作用域而不是函数内 static：MSVC 不接受「函数内 static + 局部类型」。
struct MaskGeomTrack {
    float cx = 0.0f, cy = 0.0f, w = 0.0f, h = 0.0f;
    float rc = 1.0f, rs = 0.0f;
    bool valid = false;
};
MaskGeomTrack g_maskTrack[BEAUTY_WARP_MAX_FACES];

// 2.6.0（S4a）：只出几何、不画全帧 CPU 掩码。
//
// 前提是宿主有 GPU 掩码链（beauty_gpu_mask_into）：那条路是**按几何参数**
// 在 4K 全分辨率上重画的，从头到尾不读 mask_ 一个像素。于是这张 w*h 的 CPU 掩码
// 在 GPU 路径下纯属白算 —— 4K 实测 `seg 掩码` 2.27~2.82 ms，其中逐像素画掩码
// 是大头（检测推理 + letterbox 只占一部分）。
//
// 默认 false（= 3.1.x 以来一直的行为）。由宿主通过 beautyCoreSetMaskGeomOnly()
// 打开；BEAUTY_MASK_GEOM_ONLY=0 可强制关回去做 A/B。
bool g_maskGeomOnly = false;

// 羽化斜坡查表：ramp[dist * 65 + fe] = smoothstep(dist/fe) × 256。
//
// 为什么查表：羽化在 worker 上逐像素跑，1080p 脸框约 40 万像素，每像素至少一次
// 整数除法就是 40 万次（约 10ms），能把这层优化整个吃掉。表只有 65×65 个 int
// （17KB，常驻 L1/L2），首次使用时算一次，之后每像素只剩一次乘法寻址。
const int kFeatherMax = 64;          // 与 buildMaskInto 里的 clamp 上限一致
int g_featherRamp[(kFeatherMax + 1) * (kFeatherMax + 1)];
bool g_featherReady = false;

void initFeatherRamp() {
    for (int fe = 0; fe <= kFeatherMax; fe++) {
        for (int d = 0; d <= kFeatherMax; d++) {
            int v;
            if (fe <= 0 || d >= fe) {
                v = 256;
            } else if (d <= 0) {
                v = 0;
            } else {
                const float t = (float)d / (float)fe;
                v = (int)(t * t * (3.0f - 2.0f * t) * 256.0f + 0.5f);
            }
            g_featherRamp[d * (kFeatherMax + 1) + fe] = v;
        }
    }
    g_featherReady = true;
}

// 距边界 dist 像素处的羽化系数：0（边界）→ 256（dist >= fe）。
// 必须在 initFeatherRamp() 之后调用（ensureReady 里初始化，早于任何掩码构建）。
inline int featherRamp(int dist, int fe) {
    if (fe <= 0) return 256;
    if (!g_featherReady) return 256;   // 表没建好就当没羽化，退回 3.1.8 行为
    if (fe > kFeatherMax) fe = kFeatherMax;
    if (dist <= 0) return 0;
    if (dist >= fe) return 256;
    if (dist > kFeatherMax) dist = kFeatherMax;
    return g_featherRamp[dist * (kFeatherMax + 1) + fe];
}

// 脖子通道参数（离线扫参用）。生产路径不设这两个环境变量，走上面的默认值。
// 与 BEAUTY_FACE_MODEL 同一套写法：只在 ensureReady 里读一次。
struct NeckCfg {
    float halfW = kNeckHalfW;
    float len = kNeckLen;
};
NeckCfg g_neck;

// 自身 DLL 所在目录（找模型用）
std::string selfDir() {
    char path[MAX_PATH] = {0};
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&beautyCoreSetSmoothParams, &hm);
    if (hm) GetModuleFileNameA(hm, path, MAX_PATH);
    std::string s(path);
    const size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? std::string(".") : s.substr(0, p);
}

// YCbCr 肤色判据（与 legacy_engine.cpp 的 applyWhiten 同一套阈值）
// A/B 开关：true = 旧的 YCbCr 矩形窗 + 硬 0/255（改造前行为），
// false = 椭圆 + 软边 + 亮度门控（默认）。只用于离线对比，生产不设环境变量。
bool g_skinHard = false;

// 肤色椭圆的**中心与半径**（C20）。
//
// 原来这两个中心是写死的常量（抄自 MIT 版 lumina-beauty）。它是相机在标准
// 白平衡下统计出来的，可摄像头不保证白平衡 —— C20 实测：真机帧平均 RGB
// 151/118/134（B > G，白平衡偏冷），整张脸被推出椭圆，皮肤掩码只剩 41%，
// 磨皮强度掉到 参考实现的 1/8。而离线素材 face_src 在同一判据下是 99%。
// 同一个核参数只因素材白平衡不同就差 2.8 倍，此前所有离线调参都建立在
// 「掩码 ≈ 95%」这个失真前提上。
//
// 修法：中心改为**逐帧从画面本身估计**（脸框中心区色度中位数），半径保留常量。
// 关键性质：在 face_src 这种标准白平衡素材上，估计出的中心 (−0.063, 0.096)
// 与默认 (−0.065, 0.095) 几乎重合 —— 所以开启自适应不会推翻此前结论。
float g_skinCb = -0.065f;         // 椭圆中心（逐帧估计，这里是初值/兜底值）
float g_skinCr = 0.095f;
float g_skinRu = 0.075f;          // 椭圆半径（C20 方案 B2 的兜底旋钮）
float g_skinRv = 0.070f;
bool  g_skinAdapt = true;         // 自适应中心开关（BEAUTY_SKIN_ADAPT=0 关闭）
float g_skinAdaptEma = 1.0f;      // 帧间平滑系数，**默认 1.0 = 纯函数无状态**
bool  g_skinAdaptValid = false;   // 上一帧是否取得有效中心（EMA 初始化用）

// 「估计首次收效」的一次性上报标志（C20 真机自查）。3.1.13 首版只在 init 时
// 打了一行 skin=，那一行永远是"(center=default, not yet estimated)" —— 用户
// 拿着日志没法回答「第 2 帧以后估计真的成功了？」。worker 第一次把估计结果
// 写进 g_skinCb/g_skinCr 时置位，渲染线程在 video_render 里取走并打一行。
// panel 只在 init 调 beautyCoreSkinInfo()、没有周期性 re-log，这是补的出口。
std::atomic<bool> g_skinAnnounce{false};

// ===========================================================================
// 3.0.0 —— 多实例隔离：按 slot 的引擎上下文
// ---------------------------------------------------------------------------
// 背景（2026-11-05 实机）：场景里同时挂两个机位的美颜滤镜时，主机位人物
// 不停闪烁、侧机位磨皮/美白/清晰全无效果。根因是上面这一批文件级全局
// （钩子 user、帧号、缩略图指针、肤色 EMA、掩码几何轨迹、诊断计数）与
// beautyEngine() 单例都只有一份，两个滤镜实例交替写同一份状态。
//
// 做法：
//   * 帧级 / 时域状态收进 SlotState，按 slot 各存一份；
//   * 渲染线程与每个引擎自己的异步 worker 用 thread_local 的 t_slot 选中
//     自己那一份 —— 不同 slot 是数组的不同元素，天然无数据竞争；
//   * 引擎实例也按 slot 惰性创建（见 beautyEngine()）；
//   * 配置类全局（g_cfg / g_maskShape / g_skinRu / g_neck / g_featherRamp ...）
//     由环境变量在启动时定、所有实例取值相同，保持进程级共享：272 处
//     g_cfg 引用一行都不用改，也避免把「配置」误当成「帧状态」。
//
// 这条宏必须放在**所有原全局定义之后**：前面的定义保留不动（它们退化成
// 未被使用的兼容声明，零风险），后面的引用一律重定向到当前线程的槽。
// 单实例时 t_slot 恒为 0、g_engines[0] 就是原来那个单例，行为逐字节不变。
//
// 边界：方案 E 分割影子层（g_segWork / g_segTick / g_segEvery）默认关，
// 仍是进程级；启用 BEAUTY_SEG=1 时只支持单实例。
// ===========================================================================
struct SlotState {
    BeautySmoothHook smoothHook = nullptr;
    void*            smoothUser = nullptr;
    long long        smoothHookHit = 0;
    long long        smoothHookCall = 0;
    BeautyChainHook  chainHook = nullptr;
    void*            chainUser = nullptr;
    long long        chainHit = 0;
    long long        chainCall = 0;
    unsigned long long chainSeg[5] = {};

    unsigned long long maskSubmits = 0;
    int  detPeriod = 0;
    int  detDiff = 0;
    int  motHot = 0;
    int  motCalls = 0;
    unsigned long long noFaceFrames = 0;
    unsigned long long alignUsed = 0;
    unsigned long long alignTotal = 0;
    unsigned long long frameTag = 0;

    const unsigned char* thumbPx = nullptr;
    int   thumbW = 0, thumbH = 0;
    int   thumbFullW = 0, thumbFullH = 0;
    float thumbScale = 0.0f;
    int   thumbOx = 0, thumbOy = 0;
    const unsigned char* motPx = nullptr;
    int   motW = 0, motH = 0;
    bool  duePending = false;
    bool  dueValue = false;
    bool  noPixFrame = false;

    float skinCb = -0.065f;
    float skinCr = 0.095f;
    bool  skinAdaptValid = false;
    MaskGeomTrack maskTrack[BEAUTY_WARP_MAX_FACES] = {};
    bool  maskGeomOnly = false;
};

static const int kMaxEngineSlots = 8;
static SlotState g_slots[kMaxEngineSlots];
static thread_local int t_slot = 0;
// 3.0.1：beautyEngine() 对非法 t_slot 做了 clamp，但下面这批宏原来直接
// g_slots[t_slot] 索引。写入点目前都合法，可一旦有新线程忘了 bind（或未来
// 有人写入 -1）就是越界写。统一收敛到一个内联函数，与 beautyEngine() 同口径。
static inline int slotIndex() {
    return (t_slot >= 0 && t_slot < kMaxEngineSlots) ? t_slot : 0;
}
static IBeautyEngine* g_engines[kMaxEngineSlots] = {};
// beautyEngine() 每帧会被调用多次：原子指针做无锁快路径，只有「该槽第一次
// 建引擎」才进互斥。g_engines[] 保留原始指针供 release / 诊断遍历；写都在
// g_engineMutex 内，热路径读一律走原子，避免对共享指针产生数据竞争。
static std::atomic<IBeautyEngine*> g_engineAtomic[kMaxEngineSlots]{};
static std::mutex g_engineMutex;
static bool g_slotOwned[kMaxEngineSlots] = {};
static std::mutex g_slotMutex;

#define g_smoothHook      g_slots[slotIndex()].smoothHook
#define g_smoothUser      g_slots[slotIndex()].smoothUser
#define g_smoothHookHit   g_slots[slotIndex()].smoothHookHit
#define g_smoothHookCall  g_slots[slotIndex()].smoothHookCall
#define g_chainHook       g_slots[slotIndex()].chainHook
#define g_chainUser       g_slots[slotIndex()].chainUser
#define g_chainHit        g_slots[slotIndex()].chainHit
#define g_chainCall       g_slots[slotIndex()].chainCall
#define g_chainSeg        g_slots[slotIndex()].chainSeg
#define g_maskSubmits     g_slots[slotIndex()].maskSubmits
#define g_detPeriod       g_slots[slotIndex()].detPeriod
#define g_detDiff         g_slots[slotIndex()].detDiff
#define g_motHot          g_slots[slotIndex()].motHot
#define g_motCalls        g_slots[slotIndex()].motCalls
#define g_noFaceFrames    g_slots[slotIndex()].noFaceFrames
#define g_alignUsed       g_slots[slotIndex()].alignUsed
#define g_alignTotal      g_slots[slotIndex()].alignTotal
#define g_frameTag        g_slots[slotIndex()].frameTag
#define g_thumbPx         g_slots[slotIndex()].thumbPx
#define g_thumbW          g_slots[slotIndex()].thumbW
#define g_thumbH          g_slots[slotIndex()].thumbH
#define g_thumbFullW      g_slots[slotIndex()].thumbFullW
#define g_thumbFullH      g_slots[slotIndex()].thumbFullH
#define g_thumbScale      g_slots[slotIndex()].thumbScale
#define g_thumbOx         g_slots[slotIndex()].thumbOx
#define g_thumbOy         g_slots[slotIndex()].thumbOy
#define g_motPx           g_slots[slotIndex()].motPx
#define g_motW            g_slots[slotIndex()].motW
#define g_motH            g_slots[slotIndex()].motH
#define g_duePending      g_slots[slotIndex()].duePending
#define g_dueValue        g_slots[slotIndex()].dueValue
#define g_noPixFrame      g_slots[slotIndex()].noPixFrame
#define g_skinCb          g_slots[slotIndex()].skinCb
#define g_skinCr          g_slots[slotIndex()].skinCr
#define g_skinAdaptValid  g_slots[slotIndex()].skinAdaptValid
#define g_maskTrack       g_slots[slotIndex()].maskTrack
#define g_maskGeomOnly    g_slots[slotIndex()].maskGeomOnly

// 为什么 EMA 默认必须是 1.0：任何 <1 的系数都会让中心携带历史，同一张图
// 重复处理（ROI 对拍、多次 BeautyCoreRun）得到不同掩码 —— verify_roi 实测
// maxdiff=1（EMA=0.15），EMA=0 或 1 时 ALL PASS。确定性优先于平滑。
// 真机若观察到磨皮强度逐帧闪，可设 BEAUTY_SKIN_EMA=0.2 压住抖动
// （代价是失去确定性，只在真机场景用）。

// 从画面估计本帧肤色色度中心。
//
// 采样区 = 最大脸框的中心 40%×40%（避开背景、头发、额头镜面高光）；
// 迭代两次「取中位数 → 用新中心剔除离群 → 再取中位数」—— 实测迭代 1 次即收敛，
// 第 2 次只是防极端光照下的抖动。
//
// 返回 false = 采样不足（戴口罩、手挡脸、极暗），调用方保持默认/上一帧中心。
bool estimateSkinCenter(const unsigned char* src, int w, int h,
                        const std::vector<beautycore::FaceBox>& faces,
                        float& outCb, float& outCr) {
    if (faces.empty()) return false;
    // 多脸时取最大的那张 —— 主脸的肤色才是要磨的对象，小脸样本少噪声大
    const beautycore::FaceBox* best = &faces[0];
    for (const auto& f : faces)
        if (f.w * f.h > best->w * best->h) best = &f;

    const float fx0 = best->x + best->w * 0.30f;
    const float fx1 = best->x + best->w * 0.70f;
    const float fy0 = best->y + best->h * 0.40f;
    const float fy1 = best->y + best->h * 0.80f;
    int x0 = (int)fx0, x1 = (int)fx1, y0 = (int)fy0, y1 = (int)fy1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    const int bw = x1 - x0, bh = y1 - y0;
    if (bw < 8 || bh < 8) return false;

    // 采样步长：控制采样点在 8000 以内，保证每 4 帧一次的这个 pass 足够便宜
    int step = 1;
    while ((bw / step) * (bh / step) > 8000) step++;
    if (step > 8) step = 8;

    std::vector<float> cbs, crs;
    cbs.reserve(8192);
    crs.reserve(8192);
    // 先按亮度粗筛一次并缓存色度，后面两轮迭代只做剔除、不再重算 RGB→YCbCr
    std::vector<float> cbAll, crAll;
    cbAll.reserve(8192);
    crAll.reserve(8192);
    std::vector<unsigned char> keep;
    keep.reserve(8192);
    for (int y = y0; y < y1; y += step) {
        const unsigned char* row = src + ((size_t)y * w) * 4;
        for (int x = x0; x < x1; x += step) {
            const int i = x * 4;
            const float r = row[i] * (1.0f / 255.0f);
            const float g = row[i + 1] * (1.0f / 255.0f);
            const float b = row[i + 2] * (1.0f / 255.0f);
            const float yv = 0.299f * r + 0.587f * g + 0.114f * b;
            // 暗部与过曝区不参与统计（鼻孔、发缝、镜面高光）
            if (yv < 0.12f || yv > 0.94f) continue;
            cbAll.push_back(-0.168736f * r - 0.331264f * g + 0.5f * b);
            crAll.push_back(0.5f * r - 0.418688f * g - 0.081312f * b);
        }
    }
    const size_t n = cbAll.size();
    const size_t need = (size_t)(((float)bw / step) * ((float)bh / step) * 0.30f);
    if (n < 64 || n < need) return false;   // 命中太少 → 不信任这个中心

    keep.assign(n, 1);
    float ccB = g_skinCb, ccR = g_skinCr;
    for (int iter = 0; iter < 2; iter++) {
        cbs.clear();
        crs.clear();
        for (size_t i = 0; i < n; i++) {
            if (!keep[i]) continue;
            cbs.push_back(cbAll[i]);
            crs.push_back(crAll[i]);
        }
        if (cbs.size() < 32) return false;
        // 中位数（nth_element 是 O(n)，比完整排序便宜）
        const size_t mid = cbs.size() / 2;
        std::nth_element(cbs.begin(), cbs.begin() + mid, cbs.end());
        std::nth_element(crs.begin(), crs.begin() + mid, crs.end());
        ccB = cbs[mid];
        ccR = crs[mid];
        if (iter == 1) break;
        // 用新中心剔除离群点，下一轮再取一次中位数
        for (size_t i = 0; i < n; i++) {
            const float du = (cbAll[i] - ccB) / g_skinRu;
            const float dv = (crAll[i] - ccR) / g_skinRv;
            keep[i] = (du * du + dv * dv < 2.25f) ? 1 : 0;   // d < 1.5
        }
    }
    outCb = ccB;
    outCr = ccR;
    return true;
}

// 肤色权重，返回 0..255 的**连续值**（不是布尔）。
//
// 判据与参数抄自 dinjufen/lumina-beauty 的 skinMask（核心代码为 MIT）：
//   - 在 CbCr **色度平面上画椭圆**（中心 -0.065/+0.095，半径 0.075/0.070），
//     距离 0.6~1.3 之间用 smoothstep 软过渡；
//   - 再乘亮度门控 smoothstep(0.12, 0.30, Y)：暗部不算皮肤，避免把阴影、
//     鼻孔、发缝当成肤色。
//
// 为什么换掉原来的矩形窗：
//   1) 矩形窗在 (Cb,Cr) 平面的**四个角**会误判 —— 那些组合真实肤色里不存在；
//   2) 硬 0/255 让磨皮强度在掩码边界**瞬间跳变**，观感上就是鼻梁侧面、
//      下颌线出现台阶状硬边。软边把跳变摊开成过渡带。
inline int skinWeight(int R, int G, int B) {
    if (g_skinHard) {
        // 改造前的判据，原样保留以便 A/B 对拍
        const int Y = (66 * R + 129 * G + 25 * B + 128) >> 8;
        const int Cb = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
        const int Cr = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
        const bool ok = (Cr >= 133 && Cr <= 173 && Cb >= 77 && Cb <= 127 &&
                         R > G && G > B && R > 80 && Y > 60 && Y < 235);
        return ok ? 255 : 0;
    }
    const float r = R * (1.0f / 255.0f);
    const float g = G * (1.0f / 255.0f);
    const float b = B * (1.0f / 255.0f);
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float cb = -0.168736f * r - 0.331264f * g + 0.5f * b;
    const float cr = 0.5f * r - 0.418688f * g - 0.081312f * b;

    // 椭圆中心来自 g_skinCb/g_skinCr：默认常量，或由 estimateSkinCenter()
    // 逐帧从画面估计（C20）。半径 g_skinRu/g_skinRv 是方案 B2 的兜底旋钮。
    const float du = (cb - g_skinCb) / g_skinRu;
    const float dv = (cr - g_skinCr) / g_skinRv;
    const float d = sqrtf(du * du + dv * dv);

    float m = 1.0f - smoothStep(0.6f, 1.3f, d);
    // 暗部不算皮肤（避免把阴影、鼻孔、发缝当肤色）；
    // 同理过曝区也不算 —— 鼻梁、额头的镜面高光如果被磨平，会变成一块死白。
    m *= smoothStep(0.12f, 0.30f, y) * (1.0f - smoothStep(0.94f, 0.99f, y));
    if (m <= 0.0f) return 0;
    const int v = (int)(m * 255.0f + 0.5f);
    return (v > 255) ? 255 : v;
}

inline bool isSkin(int R, int G, int B) {
    return skinWeight(R, G, B) > 0;
}

// 毫秒级单调时钟（GetTickCount64 只有 15.6ms 精度，量几毫秒的核不够用）
double nowMsCore() {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// CoreEngine —— 自研引擎（S2 起逐步成型）
//
// 第一版只做磨皮一件事，这样对拍最干净：输入原图 → 自研双边滤波 → 按档位线性
// 混合。掩码、美白、红润、细节回填随后逐个接进来 —— 每接一个都在 golden 上
// 对拍一次，而不是一次性写完再看差多少。
//
// 参数映射：面板磨皮 0..100 → 混合系数 t = 档位/100。与参考实现侧
// bgraToRgbaBlend 的定义完全一致，所以两边同档位可以直接比。
// ---------------------------------------------------------------------------
class CoreEngine : public IBeautyEngine {
public:
    const char* id() const override { return "core"; }
    const char* version() const override { return "1.6.0"; }

    bool ensureReady(std::string& err) override {
        // 人脸检测模型只试一次。失败不阻止引擎就绪 —— 降级为「无掩码，全帧磨皮」，
        // 与参考实现在 region=off 时的行为一致，不会让滤镜整体失效。
        if (!faceTried_) {
            faceTried_ = true;
            const std::string d = selfDir();
            // 必须告诉 ORT 去哪找自己的 DLL：系统里可能另有别的版本
            beautycore::onnxSetRuntimeDir(d.c_str());
            std::string mp;
            const char* envp = getenv("BEAUTY_FACE_MODEL");
            if (envp && *envp) {
                mp = envp;
            } else {
                mp = d + "\\models\\yunet.onnx";
                FILE* probe = nullptr;
                if (fopen_s(&probe, mp.c_str(), "rb") != 0 || !probe) {
                    mp = d + "\\yunet.onnx";
                } else {
                    fclose(probe);
                }
            }
            std::string ferr;
            faceOk_ = face_.load(mp.c_str(), ferr);
            faceErr_ = faceOk_ ? std::string() : (mp + " : " + ferr);
            if (faceOk_) {
                // 推理输入边长：320 比 640 快约 4 倍（像素数差 4 倍）。
                // 代价是小脸检出率下降 —— 默认值由离线对拍定，环境变量留给复验。
                int isz = kFaceInputDefault;
                const char* fenv = getenv("BEAUTY_FACE_INPUT");
                if (fenv && *fenv) isz = atoi(fenv);
                face_.setInputSize(isz);
            }
            // 脖子通道参数（离线扫参用；生产不设环境变量）
            const char* nhw = getenv("BEAUTY_NECK_HALFW");
            if (nhw && *nhw) g_neck.halfW = (float)atof(nhw);
            const char* nln = getenv("BEAUTY_NECK_LEN");
            if (nln && *nln) g_neck.len = (float)atof(nln);
            // 边缘门控灵敏度（0 = 关闭）。生产默认 10，这里只为离线复验留口子。
            const char* ek = getenv("BEAUTY_EDGE_K");
            if (ek && *ek) g_cfg.edgeK = atoi(ek);
            // 肤色判据 A/B：BEAUTY_SKIN_HARD=1 回到旧矩形窗（离线对比用）
            const char* sh = getenv("BEAUTY_SKIN_HARD");
            g_skinHard = (sh && sh[0] == '1');
            // 肤色椭圆中心自适应（C20 方案 A）：BEAUTY_SKIN_ADAPT=0 回到固定中心。
            // 默认开 —— 固定中心在白平衡偏冷的摄像头上会把掩码压到 34%。
            const char* sa = getenv("BEAUTY_SKIN_ADAPT");
            if (sa && *sa) g_skinAdapt = (sa[0] != '0');
            // 半径缩放（C20 方案 B2 兜底）：BEAUTY_SKIN_RADIUS=1.5 放宽 1.5 倍。
            // 自适应失效时的保底手段，代价是暖色背景误判增加。
            const char* sr = getenv("BEAUTY_SKIN_RADIUS");
            if (sr && *sr) {
                const float k = (float)atof(sr);
                if (k > 0.1f) { g_skinRu = 0.075f * k; g_skinRv = 0.070f * k; }
            }
            // 中心帧间平滑系数（0 = 不平滑，1 = 完全跟随本帧）
            const char* se = getenv("BEAUTY_SKIN_EMA");
            if (se && *se) g_skinAdaptEma = (float)atof(se);
            // 五官保护强度 A/B：BEAUTY_PROTECT_K=0 关闭（离线对比用）
            const char* pk = getenv("BEAUTY_PROTECT_K");
            if (pk && *pk) g_cfg.protectK = atoi(pk);
            // 阈值软膝 A/B：BEAUTY_LIMIT_THR=0 关闭
            const char* lt = getenv("BEAUTY_LIMIT_THR");
            if (lt && *lt) g_cfg.limitThr = atoi(lt);
            // 高低频磨皮：BEAUTY_FREQ=0 退回旧的「双边+线性混合」做 A/B
            const char* fq = getenv("BEAUTY_FREQ");
            if (fq && *fq) g_cfg.freqMode = (fq[0] != '0');
            const char* fi = getenv("BEAUTY_FREQ_ITERS");
            if (fi && *fi) g_cfg.freqIters = atoi(fi);
            const char* fg = getenv("BEAUTY_FREQ_GRAIN");
            if (fg && *fg) g_cfg.freqGrain = (float)atof(fg);
            // 低频提取方式：BEAUTY_LOW_MODE=1 用引导滤波
            const char* lm = getenv("BEAUTY_LOW_MODE");
            if (lm && *lm) g_cfg.freqLowMode = atoi(lm);
            const char* gr = getenv("BEAUTY_GUIDE_R");
            if (gr && *gr) g_cfg.guideRadius = atoi(gr);
            const char* ge = getenv("BEAUTY_GUIDE_EPS");
            if (ge && *ge) g_cfg.guideEps = (float)atof(ge);
            // 低频层路线：BEAUTY_LOW_PATH=0 退回旧的 uint8 双边迭代，1 = 只换浮点，
            // 2 = 浮点 + 低频参照（默认）。三个值对应三组对照数据。
            const char* lp = getenv("BEAUTY_LOW_PATH");
            if (lp && *lp) g_cfg.lowPath = atoi(lp);
            const char* lr = getenv("BEAUTY_REF_R");
            if (lr && *lr) g_cfg.refRadius = atoi(lr);
            // C 方案（lowPath=3）的低频尺度：等效 σ² = iters × ((2r+1)²−1)/12
            const char* br = getenv("BEAUTY_BOX_R");
            if (br && *br) g_cfg.boxRadius = atoi(br);
            const char* bi = getenv("BEAUTY_BOX_ITERS");
            if (bi && *bi) g_cfg.boxIters = atoi(bi);
            // 档位饱和：BEAUTY_BLUR_SAT=0 退回线性（对比用）
            const char* bs = getenv("BEAUTY_BLUR_SAT");
            if (bs && *bs) g_cfg.blurSat = (float)atof(bs);
            // ---- surface3 三层磨皮（C22；默认关，只走环境变量）----
            // BEAUTY_SMOOTH_MODEL=surface3|1 打开；不设或 =legacy|0 走旧核。
            const char* smodel = getenv("BEAUTY_SMOOTH_MODEL");
            if (smodel && *smodel)
                g_cfg.smoothModel = (smodel[0] == 's' || smodel[0] == 'S' ||
                                     smodel[0] == '1') ? 1 : 0;
            const char* s3r1 = getenv("BEAUTY_S3_R1");
            if (s3r1 && *s3r1) g_cfg.s3R1 = atoi(s3r1);
            const char* s3i1 = getenv("BEAUTY_S3_IT1");
            if (s3i1 && *s3i1) g_cfg.s3It1 = atoi(s3i1);
            const char* s3r2 = getenv("BEAUTY_S3_R2");
            if (s3r2 && *s3r2) g_cfg.s3R2 = atoi(s3r2);
            const char* s3r0 = getenv("BEAUTY_S3_R0");
            if (s3r0 && *s3r0) g_cfg.s3R0 = atoi(s3r0);
            const char* s3i2 = getenv("BEAUTY_S3_IT2");
            if (s3i2 && *s3i2) g_cfg.s3It2 = atoi(s3i2);
            const char* s3t1 = getenv("BEAUTY_S3_THR1");
            if (s3t1 && *s3t1) g_cfg.s3Thr1 = (float)atof(s3t1);
            const char* s3t2 = getenv("BEAUTY_S3_THR2");
            if (s3t2 && *s3t2) g_cfg.s3Thr2 = (float)atof(s3t2);
            const char* s3k = getenv("BEAUTY_S3_K");
            if (s3k && *s3k) g_cfg.s3K = (float)atof(s3k);
            // 3.1.14：e 模型换成 GLSL 原式，旧旋钮 EHI/EOFF 已废弃（不再解析）。
            // ELO 语义等价于新的 E0（evenfactor 下限），保留为别名，免得老脚本
            // 静默失效 —— 但注意 ELO 现在**只影响下限**，不再影响高段斜率。
            const char* s3e0 = getenv("BEAUTY_S3_E0");
            if (s3e0 && *s3e0) g_cfg.s3E0 = (float)atof(s3e0);
            const char* s3el = getenv("BEAUTY_S3_ELO");
            if (s3el && *s3el) g_cfg.s3E0 = (float)atof(s3el);
            const char* s3es = getenv("BEAUTY_S3_ESAT");
            if (s3es && *s3es) g_cfg.s3ESat = (float)atof(s3es);
            const char* s3et = getenv("BEAUTY_S3_ETHR");
            if (s3et && *s3et) g_cfg.s3EThr = (float)atof(s3et);
            const char* s3ek = getenv("BEAUTY_S3_EYEK");
            if (s3ek && *s3ek) g_cfg.s3EyeK = (float)atof(s3ek);
            // 25 tap（第 2 批 #5/#6/#7）。TAP25 是总开关，其余只在它打开时有用。
            const char* s3t25 = getenv("BEAUTY_S3_TAP25");
            if (s3t25 && *s3t25) g_cfg.s3Tap25 = atoi(s3t25);
            const char* s3sx = getenv("BEAUTY_S3_SX");
            if (s3sx && *s3sx) g_cfg.s3Sx = (float)atof(s3sx);
            const char* s3sy = getenv("BEAUTY_S3_SY");
            if (s3sy && *s3sy) g_cfg.s3Sy = (float)atof(s3sy);
            const char* s3y1 = getenv("BEAUTY_S3_YF1");
            if (s3y1 && *s3y1) g_cfg.s3YF1 = (float)atof(s3y1);
            const char* s3y2 = getenv("BEAUTY_S3_YF2");
            if (s3y2 && *s3y2) g_cfg.s3YF2 = (float)atof(s3y2);
            const char* s3hf = getenv("BEAUTY_S3_HALF");
            if (s3hf && *s3hf) g_cfg.s3Half = atoi(s3hf);
            // 双尺度带通（第 2 批-B）。BAND 是总开关，T 只在它打开时有用。
            const char* s3bd = getenv("BEAUTY_S3_BAND");
            if (s3bd && *s3bd) g_cfg.s3Band = atoi(s3bd);
            const char* s3bt = getenv("BEAUTY_S3_BT");
            if (s3bt && *s3bt) g_cfg.s3BandT = (float)atof(s3bt);
            // 档位 → (I, T) 查表（3.1.18）。LV_TABLE=0 退回旧的饱和式 I（对比用）。
            // LV_I / LV_T 形如 "0,0.005,0.205,0.574,0.975"，对应面板 0/25/50/75/100。
            const char* lvt = getenv("BEAUTY_S3_LV_TABLE");
            if (lvt && *lvt) g_cfg.lvTable = atoi(lvt);
            parse5(getenv("BEAUTY_S3_LV_I"), g_cfg.lvI);
            parse5(getenv("BEAUTY_S3_LV_T"), g_cfg.lvT);
            parse5(getenv("BEAUTY_S3_LV_E"), g_cfg.lvE);
            const char* sef = getenv("BEAUTY_S3_E_FIX");
            if (sef && *sef) g_cfg.s3EFix = (float)atof(sef);
            const char* gwp = getenv("BEAUTY_S3_GWP");
            if (gwp && *gwp) g_cfg.s3Gwp = (float)atof(gwp);
            // clarity：BEAUTY_CLARITY_MAX 改上限（离线标定用），BEAUTY_CLARITY_R 改窗口
            const char* cm = getenv("BEAUTY_CLARITY_MAX");
            if (cm && *cm) g_cfg.clarityMax = (float)atof(cm);
            const char* cr = getenv("BEAUTY_CLARITY_R");
            if (cr && *cr) g_cfg.clarityRadius = atoi(cr);
            // 异步检测：BEAUTY_ASYNC_DETECT=0 关掉（离线对拍要确定性）
            const char* ad = getenv("BEAUTY_ASYNC_DETECT");
            if (ad && *ad) g_cfg.asyncDetect = (ad[0] != '0');
            // 3.1.40：无脸帧是否跳过整段处理。BEAUTY_NOFACE_SKIP=0 回到旧行为
            // （无掩码 → 全帧兜底磨皮，逐字节等于 CPU 版修复前的行为）。
            const char* nfs = getenv("BEAUTY_NOFACE_SKIP");
            if (nfs && *nfs) g_cfg.noFaceSkip = (nfs[0] != '0');
            // 异步提交周期：BEAUTY_MASK_EVERY=4 退回 3.1.7 的节奏（拖影回来，但省 CPU）。
            // 留这个口子是为了让真机能当场对比 0 / 1 / 2 / 4 四档，不用改代码重编译。
            const char* me = getenv("BEAUTY_MASK_EVERY");
            const bool maskEverySet = (me && *me);
            if (maskEverySet) {
                int v = atoi(me);
                if (v < 0) v = 0;
                g_cfg.maskEveryAsync = v;
            }
            // 2.8.0（S7）：运动自适应提交周期。
            // 显式设了 BEAUTY_MASK_EVERY 就默认关掉自适应 —— 那个开关的用途正是
            // 「把周期锁死」来量数据，不能被自适应悄悄改回去；要两个一起上，
            // 显式写 BEAUTY_DET_ADAPT=1。
            const char* da = getenv("BEAUTY_DET_ADAPT");
            if (da && *da) g_cfg.detAdapt = (da[0] != '0');
            else if (maskEverySet) g_cfg.detAdapt = false;
            const char* das = getenv("BEAUTY_DET_ADAPT_STILL");
            if (das && *das) {
                int v = atoi(das);
                if (v >= 1 && v <= 30) g_cfg.detAdaptStill = v;
            }
            const char* dat = getenv("BEAUTY_DET_ADAPT_THR");
            if (dat && *dat) {
                int v = atoi(dat);
                if (v >= 1 && v <= 64) g_cfg.detAdaptThr = v;
            }
            const char* dah = getenv("BEAUTY_DET_ADAPT_HOT");
            if (dah && *dah) {
                int v = atoi(dah);
                if (v >= 0 && v <= 30) g_cfg.detAdaptHot = v;
            }
            // 2.12.0（P1-1c）：16 块的合并口径。只认字面量 "rms"，其余一律按默认
            // 的 "max" 处理 —— 打错字不该静默换掉判据，但也不能因此让插件不起作用，
            // 所以「不识别 → 老行为」而不是报错。
            const char* daa = getenv("BEAUTY_DET_ADAPT_AGG");
            if (daa && *daa) g_cfg.detAdaptAggRms = (strcmp(daa, "rms") == 0);
            // 掩码边界羽化：BEAUTY_MASK_FEATHER=0 关闭（与 3.1.8 逐字节相同，A/B 用），
            // >0 固定像素；不设则按脸宽自适应（0.12 × 脸宽，对齐 wtnl-zhi 的默认值）。
            const char* mf = getenv("BEAUTY_MASK_FEATHER");
            if (mf && *mf) {
                int v = atoi(mf);
                g_maskFeatherPx = (v < 0) ? -1 : v;
            }
            // 掩码几何：BEAUTY_MASK_SHAPE=0 退回矩形（3.1.19 行为），3 = 脸+脖子都用椭圆。
            const char* msh = getenv("BEAUTY_MASK_SHAPE");
            if (msh && *msh) g_maskShape = atoi(msh);
            // 2.15.0：BEAUTY_MASK_ROLL=0 关闭「掩码跟脸转」（回到 2.14.1 的轴对齐几何）。
            const char* mrl = getenv("BEAUTY_MASK_ROLL");
            if (mrl && *mrl) g_maskRoll = (mrl[0] != '0');
            // 2.15.3：掩码几何时间平滑系数（0 = 关，1..255 = α×256）。
            const char* mema = getenv("BEAUTY_MASK_EMA");
            if (mema && *mema) {
                const int v = atoi(mema);
                g_maskEma = (v <= 0) ? 0 : (v > 255 ? 255 : v);
            }
            // 2.13.0：检测几何平移（只诊断）。格式 "dx,dy"，解析失败按 0。
            const char* dof = getenv("BEAUTY_DET_OFFSET");
            if (dof && *dof) {
                int ox = 0, oy = 0;
                if (sscanf(dof, "%d,%d", &ox, &oy) == 2) {
                    g_detOffX = ox;
                    g_detOffY = oy;
                }
            }
            // 2.6.0（S4a）的 BEAUTY_MASK_GEOM_ONLY 不在这里读：开关由宿主的
            // beautyCoreSetMaskGeomOnly() 打开，而它可能晚于本函数（acquireSlot
            // 会触发 init），环境变量判断放在那个 setter 里才不会被覆盖。
            initFeatherRamp();   // 羽化查表：必须早于任何一次 buildMaskInto
            // 3.1.10 掩码↔帧对齐：BEAUTY_MASK_ALIGN=0 关闭（回到 3.1.9 的
            // 「当前帧 + 旧掩码」，会拖影但零延迟，A/B 用）。
            const char* ma = getenv("BEAUTY_MASK_ALIGN");
            if (ma && *ma) alignOn_ = (ma[0] != '0');
            asyncOn_ = g_cfg.asyncDetect;
            // 异步检测会和磨皮抢核：留一个核给 worker，否则磨皮那一帧会被拖慢
            // （实测 1080p 非检测帧从 6.4ms 涨到 13.7ms，等于尖峰换了个形状）。
            if (asyncOn_ && g_cfg.threads <= 0) {
                const unsigned hc = std::thread::hardware_concurrency();
                if (hc > 2) g_cfg.threads = (hc > 9) ? 8 : (int)hc - 1;
            }
            const char* fm = getenv("BEAUTY_FREQ_MAG");
            if (fm && *fm) {
                float lo = 0, hi = 0;
                if (sscanf(fm, "%f,%f", &lo, &hi) == 2) {
                    g_cfg.freqMagLo = lo;
                    g_cfg.freqMagHi = hi;
                }
            }
            // 细节回填（锐化）：BEAUTY_DETAIL_THR 改细频带软阈值（削噪量），
            // BEAUTY_DETAIL_MID 改中频带基准增益。两者只在离线标定时用。
            const char* dt = getenv("BEAUTY_DETAIL_THR");
            if (dt && *dt) g_cfg.detailThrFine = (float)atof(dt);
            const char* dm = getenv("BEAUTY_DETAIL_MID");
            if (dm && *dm) g_cfg.detailMidGain = (float)atof(dm);
            // 3.1.35：磨皮=0 纯锐化模式的细频增益 / 细频阈值（见配置段 faceSharpGain / faceSharpThrFine）。
            const char* fsg = getenv("BEAUTY_FACE_SHARP_GAIN");
            if (fsg && *fsg) g_cfg.faceSharpGain = (float)atof(fsg);
            const char* fst = getenv("BEAUTY_FACE_SHARP_THR");
            if (fst && *fst) g_cfg.faceSharpThrFine = (float)atof(fst);
            // 全局锐化自己的旋钮（3.1.22，从 detailMidGain / detailThrFine 拆出）
            const char* gm = getenv("BEAUTY_DETAIL_MID_GLOBAL");
            if (gm && *gm) g_cfg.globalMidGain = (float)atof(gm);
            const char* gf = getenv("BEAUTY_DETAIL_FINE_GLOBAL");
            if (gf && *gf) g_cfg.globalFineGain = (float)atof(gf);
            const char* gt = getenv("BEAUTY_DETAIL_THR_GLOBAL");
            if (gt && *gt) g_cfg.globalThrFine = (float)atof(gt);
            const char* gt2 = getenv("BEAUTY_DETAIL_THR_MID_GLOBAL");
            if (gt2 && *gt2) g_cfg.globalThrMid = (float)atof(gt2);
            // 磨皮后的基准细频回填比例（0 = 关）。见配置段 blurRefill 的说明。
            const char* brf = getenv("BEAUTY_BLUR_REFILL");
            if (brf && *brf) g_cfg.blurRefill = (float)atof(brf);
            const char* brt = getenv("BEAUTY_BLUR_REFILL_THR");
            if (brt && *brt) g_cfg.blurRefillThr = (float)atof(brt);
            const char* brm = getenv("BEAUTY_BLUR_REFILL_MINI");
            if (brm && *brm) g_cfg.blurRefillMinI = (float)atof(brm);
            const char* brd = getenv("BEAUTY_BLUR_REFILL_DELTA");
            if (brd && *brd) g_cfg.blurRefillDelta = atoi(brd);
            // 细节层时序 EMA：BEAUTY_EMA=0 关掉（做 A/B 用），其余三个是阈值与 beta
            const char* em = getenv("BEAUTY_EMA");
            if (em && *em) g_cfg.emaOn = (em[0] != '0');
            const char* es = getenv("BEAUTY_EMA_STILL");
            if (es && *es) g_cfg.emaStill = (float)atof(es);
            const char* ef = getenv("BEAUTY_EMA_FULL");
            if (ef && *ef) g_cfg.emaFull = (float)atof(ef);
            const char* eb = getenv("BEAUTY_EMA_BETA");
            if (eb && *eb) g_cfg.emaBetaStill = (float)atof(eb);
            // 五官局部五项增益标定：BEAUTY_REG_GAIN="立体,亮眼,去黑眼圈,法令纹"
            // 只在离线对拍时用（要和 参考实现的差分峰值对齐时改这里，别改源码重编译）。
            // 3.1.22：后两项不再是「增益」了 ——
            //   黑眼圈 = 双层椭圆的**整体比例因子**（形状在代码里定档）
            //   法令纹 = 暗度模型的「斜率,封顶」
            const char* rg = getenv("BEAUTY_REG_GAIN");
            if (rg && *rg) {
                float a = 0, b = 0, c = 0, d = 0;
                if (sscanf(rg, "%f,%f,%f,%f", &a, &b, &c, &d) == 4) {
                    g_cfg.regThreedGain = a;
                    g_cfg.regEyeGain = b;
                    g_cfg.regPouchScale = c;
                    g_cfg.regNasoSlope = d;
                }
            }
            const char* rs = getenv("BEAUTY_REG_SPOT");
            if (rs && *rs) g_cfg.regSpotK = (float)atof(rs);
            // 五官局部三项（3.1.22 重建）：
            //   BEAUTY_REG_THREED = 立体增益比例因子（单值）
            //   BEAUTY_REG_POUCH  = 黑眼圈增益比例因子（单值）
            //   BEAUTY_REG_NASO   = "斜率,封顶"
            const char* rt = getenv("BEAUTY_REG_THREED");
            if (rt && *rt) g_cfg.regThreedGain = (float)atof(rt);
            const char* rp = getenv("BEAUTY_REG_POUCH");
            if (rp && *rp) g_cfg.regPouchScale = (float)atof(rp);
            const char* rn = getenv("BEAUTY_REG_NASO");
            if (rn && *rn) {
                float a, b;
                if (sscanf(rn, "%f,%f", &a, &b) == 2) {
                    g_cfg.regNasoSlope = a;
                    g_cfg.regNasoMaxLift = b;
                }
            }
            // 暗线闸门阈值（离线对拍用）："下限,上限"
            const char* rgate = getenv("BEAUTY_REG_NASO_GATE");
            if (rgate && *rgate) {
                float a, b;
                if (sscanf(rgate, "%f,%f", &a, &b) == 2) {
                    g_cfg.regNasoGateLo = a;
                    g_cfg.regNasoGateHi = b;
                }
            }
        }
        // 3.1.21：把「首帧」的一次性成本提前到这里（模块加载 / 引擎首次就绪）。
        warmUp();
        err.clear();
        return true;
    }
    // 预热：加载 YuNet 人脸模型（一次性，约 100~300 ms）。
    //
    // 3.1.4 修：这里以前是空壳（只 clear err 就返回），而 ensureReady() 是**唯一**
    // 做 face_.load() 的地方，obs-beauty.cpp 又从不直接调 ensureReady() —— 结果
    // OBS 里 faceOk_ 永远是 false，updateMask() 第 764 行第一句就 return false：
    //   · 磨皮退回全帧（背景一起磨，CPU 顶到最高）；
    //   · 清晰 / 美白 / 红润 / 五官五项 / 脸内锐化全部落在 if (haveMask) 里，
    //     整片不执行 —— 面板 11 项只剩磨皮和全局锐化能用。
    // 离线脚本（core_probe / golden_capture）是显式调 ensureReady() 的，所以离线
    // 对拍一直是带掩码的：OBS 里跑的画面和验收过的画面根本不是一回事。
    bool preheat(std::string& err, bool& didInit) override {
        didInit = false;
        err.clear();
        const bool wasTried = faceTried_;
        if (!ensureReady(err)) return false;
        // 只有这一次真的做了加载才回 true —— 插件层靠它决定要不要打 init OK
        didInit = !wasTried;
        return true;
    }
    bool ready() const override { return true; }

    int acquireSlot(std::string& err) override {
        // 兜底：preheat 之外还有别的入口（宿主可能很久以后才建滤镜，或 preheat
        // 因故没跑到）。ensureReady 自身幂等，第二次调用只判一次 faceTried_。
        if (!ensureReady(err)) return -1;
        return 0;
    }
    // 3.0.1：旧实现是空的，于是 destroy() 里调 releaseSlot() 什么也没停。
    // worker 会在滤镜销毁后继续跑（占一个线程 + 一份 YuNet 模型），而且它是
    // 唯一会在宿主线程之外读写 g_slots[slot] 的人：beautyCoreReleaseSlot()
    // 紧接着整块清空 SlotState 就构成数据竞争。这里先 join 掉 worker；
    // 引擎对象 / 模型刻意保留，槽复用仍能省掉 ~2 秒预热，重启 worker
    // 由 startAsyncIfNeeded() 负责（asyncStarted_ 已被置回 false）。
    void releaseSlot(int) override { stopAsync(); }
    // 面板「识别模式 / 人物个数」的真正落点（3.1.75）。
    // 之前这里是空实现 —— UI 有下拉、引擎不存值，属于「看着能调、实际没用」。
    // 语义（写死在这里，面板文案照此）：
    //   识别模式 0 标准   = YuNet conf 0.6（与 OpenCV 默认一致）
    //   识别模式 1 高精度 = conf 0.45（更容易抓到小脸/侧脸，代价是误检更多）
    //   人物个数         = 只保留分数最高的 N 个（1..8，NMS 输出本就按分数降序）
    void setDetectOptions(int, int landmarkQuality, int maxFaces) override {
        const int mf = (maxFaces < 1) ? 1 : (maxFaces > 8 ? 8 : maxFaces);
        const int lq = (landmarkQuality > 0) ? 1 : 0;
        if (mf == detMaxFaces_ && lq == detHighQ_) return;
        detMaxFaces_ = mf;
        detHighQ_ = lq;
        face_.setMaxFaces(detMaxFaces_);
    }

    bool process(int, const unsigned char* srcIn, unsigned char* dst,
                 int w, int h, const BeautySettings& s,
                 bool = true) override {
        const unsigned char* src = srcIn;
        if (!src || !dst || w <= 0 || h <= 0) return false;
        // 5b-2 切片 4：本帧的宿主帧号（宿主在 process 之前设好）。
        // 它与「送进检测 worker 的那一帧」绑定；对齐换帧时把被换进来那一帧的帧号
        // 交给宿主，宿主据此在显存帧环里取同一帧纹理 —— 跳过整帧上传而不改语义。
        const unsigned long long curTag = g_frameTag;
        const size_t need = (size_t)w * (size_t)h * 4;
        frameNo_++;   // 时序 EMA 用它判断历史有没有断帧（见 detailEmaGen_ 的注释）
        // Phase 3 诊断：每帧先作废上一帧的磨皮参数快照。任何提前返回（面板全 0、
        // 首帧无掩码、磨皮档位为 0）之后，beautyCoreBandDiag() 都必须报「本帧
        // 没磨皮」，否则 GPU 自检会拿上一帧的 I/e/T 去对拍这一帧的图。
        bandValid_ = false;

        // 2.10.0 方案 B：把本帧输入旋转存进成员，供 updateMask/buildMaskInto 的
        // 检测几何反变换使用（渲染与输出尺寸不变，只动几何）。
        inputRot_ = ((s.input_rot % 360) + 360) % 360;
        // 2.15.0：面板值一变就重新信任它（清掉上次「自动退回 0°」的结论），
        // 否则用户把 90 改成 270 之后引擎还按 0 跑，看起来像开关失灵。
        if (inputRot_ != cfgRot_) {
            cfgRot_ = inputRot_;
            effRot_.store(inputRot_, std::memory_order_relaxed);
            rotFallbackLogged_ = false;
        }

        double blur = s.panel[P_BLUR];
        if (blur < 0) blur = 0;
        if (blur > 100) blur = 100;
        // 档位 → 强度的**饱和映射**：t_eff = t · (1 − sat·t²)
        //
        // 为什么需要它：参考实现的档位曲线在高段是饱和的（脸颊 50→75→100 对应
        // 63.1→48.0→35.5，每档增量递减），而我们换盒式低通后几乎是线性的
        // （64.9→43.7→23.4）—— 磨皮拉满时我们比参考实现狠 12 个百分点，脸会糊。
        // sat=0.16 是按参考实现五个档位反解出来的：
        //     面板   25     50     75     100
        //     等效  0.248  0.480  0.683  0.840   （目标 0.187/0.482/0.679/0.842）
        // 中高段几乎重合，25 档偏强约 3.7pp（磨得稍狠，观感上可接受）。
        //
        // ⚠️ 这条曲线是**单张素材拟合**的（goldens 里只有 face_src 一张脸）。
        // 素材补齐后必须复核，BEAUTY_BLUR_SAT=0 可一键关掉回到线性。
        const float tLin = (float)(blur / 100.0);
        const float t = tLin * (1.0f - g_cfg.blurSat * tLin * tLin);

        // 面板其余各项（0..1）。谁生效取决于它自己，不再被「磨皮是否为 0」绑死 ——
        // 旧实现在 t<=0 时直接逐字节返回，导致磨皮归零时美白/红润/全局锐化一起失效，
        // 与参考实现路线的行为不一致（那边 applyRegionEffects 在磨皮门控之外）。
        const float shFace = clampi01(s.panel[P_SHARPEN] / 100.0);
        const float shGlobal = clampi01(s.panel[P_GLOBAL_SHARP] / 100.0);
        // 脸内锐化原先以「磨皮开启」为前提（legacy_engine.cpp 的
        // wantFaceSharpen = panel[P_BLUR] > 0 && ...）。3.1.35 起取消这个前提：
        // 磨皮为 0 时脸内虽然没有「被磨掉的高频」可回填，但 dst≈src，
        // 回填一份原图细频就等于叠加锐化 —— 这正是用户拉「锐化」时的预期。
        // 旧行为实测：面板 100 档、其余全 0 → 改动量 0.000，滑块完全无效；
        // 同条件下 SDK 的 sharpen 是独立锐化器，改动量 1.012、细节 ×2.00。
        // 磨皮 > 0 时这条路径的行为逐字节不变（仍按 shFace 回填）。
        const bool wantFaceDetail = (shFace > 0.0f);
        const bool wantGlobalDetail = (shGlobal > 0.0f);
        // 3.1.22：磨皮后的基准细频回填。只在「开了磨皮、没开锐化、没开清晰」时
        // 生效 —— 用户自己开了锐化/清晰时，那两项的回填已经起作用了，
        // 再叠一层基准会把毛孔放大成噪声。
        // 基准回填用的实际磨皮强度（band 路径算出来的 I）。
        // I 是在磨皮分支内部定义的，这里先声明，等分支里算出来再回填。
        float blurRefillI = 0.0f;
        // 磨皮强度低于阈值就不回填：轻磨时几乎没削掉高频，
        // 回填带来的只是量化噪声（±1 灰阶），而参照实现那一档本来就几乎不动。
        //
        // 注意这里是**可变**的：真正的判定要等磨皮分支算出实际强度 I 之后
        // （band 分支里才赋值 blurRefillI）。先在这里求值的话，
        // blurRefillI 恒为 0，条件永远不成立 —— 这个坑踩过一次。
        bool wantBlurRefill = false;
        const bool wantTone = (s.panel[P_WHITEN] > 0.0 || s.panel[P_RED] > 0.0);
        // 清晰：原生 参考实现的 clarity 是**纯颜色变换**（S 形对比 + 轻微暖调），
        // 与磨皮毫无关系 —— panel 全 0、只下发 clarity=1.0 时改动量 6.16、
        // 细节 ×1.17（2026-10-30 实测，见 全项对齐_20261030/sdk_native.py）。
        // 3.1.19 之前这里要求 `t > 0`，于是「只开清晰、磨皮为 0」的画面零变化，
        // 离线矩阵直接判「欠 100%」。这条前提已删除。
        const bool wantClarity = (s.panel[P_CLARITY] > 0.0);
        /* 2.16.1：本帧钩子不会跑（磨皮=0）却又要色彩层 → 必须让引擎自己画掩码。
         * 见 buildMaskInto 里 geomOnly 的注释。
         *
         * 3.1.65（2026-11-05）：补 **wantFaceDetail**。
         *
         * 原判据只列了 wantTone / wantClarity，漏掉「磨皮=0 的纯锐化」这一档，
         * 而同一条链对它是致命的：
         *   ① 段区间钩子只在 `t > 0`（磨皮>0）时被调用 —— 纯锐化时 GPU 掩码
         *      链一帧都不跑，没人替 CPU 画掩码；
         *   ② 可 geomOnly 仍然成立（`!forceCpuMask_ && mapped && 单脸 …`），
         *      于是 CPU 也一个像素都不画；
         *   ③ 结果 skin 纹理整张是空的，detail.hlsl 里 `m = 0`，
         *      `fwF = m·dFine` 恒 0 —— **脸内锐化整段失效**，
         *      只剩 `fwG = (1−0)·dGFine` 的全局项。
         *
         * 这与 2.16.1 修「美白完全没效果」是同一个坑，只是那次只修了色彩层。
         * 实测（真实 GPU 链，同一张脸，面板 100）：
         *   · 磨皮=0 + 锐化=100  → 与关滤镜**逐字节相同**（0 px，md5 一致）
         *   · 磨皮=100 + 锐化=100 → 叠加 52 777 px、max 63（有效）
         * 用户报的「锐化拉满没效果」就是第一行。 */
        forceCpuMask_ = ((t <= 0.0f) &&
                         (wantTone || wantClarity || wantFaceDetail));
        // 五官局部五项：全部要求「有掩码 + 有脸」，所以也算掩码需求方。
        // 与参考实现一致地**不**以磨皮为前提（这是妆容层，磨皮关掉也该生效）。
        const bool wantRegion = (s.panel[P_THREED] > 0.0 ||
                                 s.panel[P_EYEBRIGHT] > 0.0 ||
                                 s.panel[P_POUCH] > 0.0 ||
                                 s.panel[P_NASOLABIAL] > 0.0 ||
                                 s.panel[P_DELSPOT] > 0.0);

        // 3.1.27：美形三项（瘦脸 / 大眼 / 下巴）—— 本项目第一次做**几何变形**。
        // 它与上面十项完全独立：不需要磨皮、不需要皮肤掩码，只要「人脸框 +
        // 关键点」。但它需要 faces_ 是新鲜的，而检测链是唯一更新途径，所以
        // 下面把它并进 needMask 的触发条件（而不是另起一条检测路径）。
        // BEAUTY_WARP=0 整体关闭 → 逐字节回到 3.1.26 的行为。
        static const bool warpEnabled = []() {
            const char *e = getenv("BEAUTY_WARP");
            return !(e && *e == '0');
        }();
        const bool wantWarp = warpEnabled &&
                              (s.panel[P_THIN_FACE] > 0.0 ||
                               s.panel[P_BIG_EYE] > 0.0 ||
                               s.panel[P_CHIN] > 0.0);

        // 5c（3.1.62）：美形三项强度。提到这里而不是留在底部那段，理由与色调
        // 3.1.52 提前完全一样 —— 段区间钩子必须在**磨皮段**就把参数递出去，
        // 否则宿主只能在外面再算一遍，两处口径迟早漂移。语义逐字未变。
        beautycore::WarpParams warpParams;
        warpParams.thin = clampi01((float)(s.panel[P_THIN_FACE] / 100.0));
        warpParams.eye = clampi01((float)(s.panel[P_BIG_EYE] / 100.0));
        warpParams.chin = clampi01((float)(s.panel[P_CHIN] / 100.0));

        // 3.1.35：早退条件补 wantFaceDetail。3.1.19 修「只开清晰、磨皮为 0
        // 零变化」时补过 wantClarity，但同一条链上漏了锐化 —— 于是把门控
        // 放开之后仍然被这里拦掉，实测仍是 0.000。两个条件必须成对维护。
        if (t <= 0.0f && !wantGlobalDetail && !wantFaceDetail && !wantTone &&
            !wantClarity && !wantRegion && !wantWarp) {
            // 面板全是 0：逐字节透传，零成本（与旧实现同一条路径）
            if (dst != src) memcpy(dst, src, need);
            lastMs_ = 0.0;
            return true;
        }

        if (smooth_.size() < need) smooth_.resize(need);
        unsigned char* sm = &smooth_[0];

        // ---- 色调三项权重（3.1.52：从 300 行后提到这里）--------------------
        // 提到这里不是为了好看：段区间钩子必须在**磨皮段**就拿到色调参数，
        // 否则宿主只能在主路径外侧再算一遍，两处口径迟早漂移。语义与原位置
        // 逐字相同 —— 只是搬家，没有改任何算式。
        //
        // 旧实现（12px 中频回填当"清晰"）保留在 BEAUTY_CLARITY_LEGACY=1 下，
        // 只作 A/B 对照，不再走默认路径；它走的是 clarityRgb（邻域算子），
        // GPU 色调着色器对不上，所以开着它时段区间钩子不认领色调。
        static const bool legacyClarity =
            (getenv("BEAUTY_CLARITY_LEGACY") != nullptr);
        const float cv = wantClarity ? (float)(s.panel[P_CLARITY] / 100.0) : 0.0f;
        const float wv = (float)(s.panel[P_WHITEN] / 100.0);
        const float rv = (float)(s.panel[P_RED] / 100.0);

        // 段区间钩子认领的段（位掩码）。声明在这里是因为它要跨过磨皮段，
        // 供后面的色调 / 五官 / 细节三段分别判断「要不要跑 CPU」。
        unsigned claimed = 0;

        const double t0 = nowMsCore();
        // ---- 分项计时（3.1.47，纯埋点）------------------------------------
        // 动机：4K 场景引擎 65 ms，但只有掩码覆盖率 2%。没有分项数据就无法判断
        // 该优化哪一段 —— 「磨皮占大头」只是从「2% 掩码却跑全帧双尺度带通」
        // 推出来的猜想，得用实测证伪或证实。
        // 六段顺序与下面的实际执行顺序严格一致，累计起来应该略小于 lastMs_
        // （差值 = 分支判断与循环骨架的开销）。
        double tSeg[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        double tMark = t0;
        auto markSeg = [&tSeg, &tMark](int i) {
            const double n = nowMsCore();
            tSeg[i] = n - tMark;
            tMark = n;
        };
        // 掩码用途变了：以前只有磨皮要它，现在美白/红润/脸内锐化都要。
        const bool needMask = (t > 0.0f) || wantFaceDetail || wantTone ||
                              wantClarity || wantRegion || wantWarp;
        // 先更新掩码再磨皮：ROI 裁剪要用掩码的包围盒。顺序反了的话，
        // 磨皮那一遍全帧算完才知道 ROI，等于白算。
        const bool haveMask = needMask ? updateMask(src, w, h) : false;
        if (haveMask) everHadMask_ = true;
        // 3.1.48 诊断：包围盒面积比。放在这里是因为此时 bounds_ 与 haveMask
        // 都是本帧刚定的值，之后任何分支都不会再改它（磨皮只是照抄一次）。
        // 没有掩码时记 -1 而不是 0：0% 会被当成「包围盒极小、ROI 裁剪收益极大」，
        // 与「这一帧压根没有包围盒」是两件完全不同的事。
        lastBoundsFrac_ = (haveMask && !bounds_.empty() && w > 0 && h > 0)
                              ? (double)bounds_.area() /
                                    ((double)w * (double)h)
                              : -1.0;
        markSeg(0);   // 0 = 掩码更新（异步路径下通常≈0，同步检测时才可见）

        // 3.1.21：**首次启用**这几帧直接透传，不退回全帧磨皮。
        //
        // 打开开关的那一刻异步检测还没跑完（模型 15 ms + worker 排队），掩码是空的。
        // 旧逻辑此时退回「全帧磨皮」，代价是双份的：
        //   · 算力：1080p 全帧双边 ~30 ms，而稳态只磨脸（掩码 16%）~18 ms，
        //     首帧必超 30fps 预算（实测首帧 42 ms vs 稳态 19 ms）；
        //   · 观感：整幅画面先糊一下，等掩码到了再跳回「只磨脸」——每次开开关
        //     都闪一次背景。
        // 一旦得到过掩码就不再走这条分支：后面「人真的走开了」仍按原逻辑全帧兜底，
        // 与旧行为逐字节一致。
        if (needMask && !haveMask && faceOk_ && !everHadMask_) {
            if (dst != src) memcpy(dst, src, need);
            lastMs_ = 0.0;
            for (int i = 0; i < 6; i++)
                lastSeg_[i] = 0.0;   // 别让分项残留上一帧的数，否则日志会自相矛盾
            return true;
        }

        // 3.1.40（从 CPU 版 v3.1.39 移植）：**确认本帧没人 → 直接透传**，
        // 不再退回无掩码 = 全帧磨皮的兜底路径。
        //
        // 旧行为（移植前）在「曾经检测到过脸、之后人走开」时会一直走全帧兜底：
        // 磨皮核把空掩码当「全皮肤」（m = 255）、ROI 也放开成全帧，于是 1080p
        // 磨皮段从「只磨 ROI 约 5~7 ms」涨到全帧量级（约 23 ms），**而这些像素
        // 随后一个都用不到** —— 色调/清晰/五官/细节全部挂在 haveMask 之下，美形
        // 要 faces_ 非空。观感上还把背景、衣物、商品一起磨糊。
        //
        // 判据五条必须同时成立，缺一条都会把「暂时算不出来」误判成「没人」：
        //   ① g_cfg.noFaceSkip  开关打开（BEAUTY_NOFACE_SKIP=0 可回退旧行为）；
        //   ② needMask          本帧确实有美颜需求（否则上面已透传，无需再判）；
        //   ③ !haveMask         本帧无可用掩码；
        //   ④ faceOk_           模型可用（模型没加载时 updateMask 第一句就 return false，
        //                       此时跳过等于「检测坏了 = 完全没美颜」，比慢更难排查）；
        //   ⑤ everHadMask_      历史上真的得到过掩码（否则上面那条分支已处理）；
        //   ⑥ lastFaces_ == 0   最近一次检测**跑完**且一张脸都没有；
        //   ⑦ faceErr_ 里没有 "err=" —— 那是推理/模型错误的标记，不是「没脸」。
        //      onDetectFailed 对这两种情况写的是同一条 lastFaces_ = 0，只能靠
        //      错误串区分；宁可多算（退回全帧），也不要静默不磨。
        //
        // 迟滞不用自己加：mask 的 hold 机制已让「人走出去」先沿用旧掩码若干帧，
        // 之后才进这里；人走回画面时检出当帧即恢复，不会闪。
        if (g_cfg.noFaceSkip && needMask && !haveMask && faceOk_ && everHadMask_ &&
            lastFaces_ == 0 && faceErr_.find("err=") == std::string::npos) {
            if (dst != src) memcpy(dst, src, need);
            lastMs_ = 0.0;
            for (int i = 0; i < 6; i++)
                lastSeg_[i] = 0.0;
            g_noFaceFrames++;
            return true;
        }

        // 3.1.10 掩码↔帧对齐：只在「worker 的结果刚到」这一帧把它描述的原始帧
        // 拿来处理，掩码与画面就逐像素同源了。没赶上（worker 晚一帧）就照旧用
        // 当前帧 + 旧掩码 —— 停顿一次比一帧淡边更显眼。
        // 5b-2 切片 4：本帧 `src` 到底是哪一帧。默认就是他自己的帧号；
        // 对齐换帧时改成 held 帧的帧号（宿主靠它在帧环里取到同一帧的纹理）。
        unsigned long long srcTagThisFrame = curTag;
        if (alignOn_ && maskFresh_ && haveMask) {
            if (heldFrame_.size() == need) {
                src = &heldFrame_[0];
                srcTagThisFrame = heldTag_;
                g_alignUsed++;
            } else if (g_thumbPx || g_noPixFrame) {
                /* 2.7.0（S4b）：缩略图帧不能当 src（1.6 MB ≠ 全帧），但**帧号
                 * 照样要对齐** —— 这份掩码描述的是 heldTag_ 那一帧，链就必须画
                 * 那一帧（宿主按 srcTag 从显存环里取）。不改 srcTag 的话，宿主会
                 * 拿「当前帧纹理 + 上一帧掩码」，3.1.10 修掉的拖影带立刻复现。
                 * src 保持调用方给的那个（缩略图模式下 CPU 段本来就不该跑，
                 * 宿主在认领不全时整帧跳过，不会用到它）。 */
                srcTagThisFrame = heldTag_;
                g_alignUsed++;
            }
        }
        g_alignTotal++;
        // 5b-2：这一帧的掩码是不是「刚换进来的」——只有那时 mask_ 与 src 同帧。
        const bool maskWasFresh = maskFresh_ && haveMask;
        maskFresh_ = false;   // 新鲜度只对「到货那帧」有效，过了这帧就作废

        // ---- 细节段参数缓存（3.1.52）----------------------------------------
        // 段区间钩子要在**磨皮段**就把细节参数递给宿主，所以参数得先算出来。
        // 但不能算两遍：updateMotion() 带运动 EMA 状态，一帧推两次会让 beta
        // 偏离真实速度。所以统一由下面的 lambda 算，算过就记在 cache 里，
        // 后面「细节回填」那一步直接取用。
        beautycore::DetailParams dpCache;
        beautycore::RoiRect droiCache;
        float betaCache = 1.0f;
        bool dpRunnableCache = false;
        bool dpCached = false;

        // refillI = 本帧实际磨皮强度 I（只有 band 路径会给非 0），语义与原来的
        // 闭包变量 blurRefillI 完全一致。返回 false 表示「这一帧的细节段是空转」
        // （ROI 空 / 四档增益全 0），此时调用方不必再调 detailRefill。
        auto buildDetailParams = [&](float refillI, beautycore::DetailParams& dp,
                                     beautycore::RoiRect& droi, float& beta) {
            dp = beautycore::DetailParams();
            // 运动判据每帧算一次（4×4 抽样，720p 约 5.7 万次比较，可忽略）。
            // 只在真的要跑锐化时才算 —— 锐化关着就没有噪点放大问题。
            const double motion = updateMotion(src, w, h);
            beta = emaBeta(motion);

            if (wantFaceDetail && haveMask) {
                dp.fine = shFace;
                dp.mid = shFace * g_cfg.detailMidGain;
                if (dp.mid > 3.0f) dp.mid = 3.0f;
                // 3.1.35：磨皮=0 时是「纯锐化」语义（dst≈src，回填 src 高频 = 叠加锐化）。
                // 这条路不能用中频带（会在全脸皮肤产生与档位无关的固定基底），
                // 也不能用默认细频阈值（会把小幅度高频削光），所以走专属参数：
                //   细频满增益 × faceSharpGain、中频关、阈值放开到 faceSharpThrFine。
                // 磨皮>0 时是「回填被磨掉的高频」，增益沿历史曲线不变。
                if (t <= 0.0f) {
                    dp.fine = shFace * g_cfg.faceSharpGain;
                    dp.mid = 0.0f;
                    dp.thrFine = g_cfg.faceSharpThrFine;
                }
            }
            if (wantBlurRefill && haveMask) {
                // 基准回填：只还细频，不碰中频。
                // 中频是「我方比参照多出来的那部分能量」，还回去只会更糟。
                //
                // 比例随**实际磨皮强度 I** 缩放：磨皮削掉的高频本来就正比于 I，
                // 不乘 I 的话低档会把原图细节放大成超过原图
                // （实测 25 档 dret 从 1.000 涨到 1.083、改动量 0.000→0.104）。
                //
                // delta 模式（加回「原图细频 − dst 细频」）实测更保守：
                // 100 档细节保留只从 0.737 到 0.761（目标 0.835），因为
                // 被磨掉的细频差值本身就在 1 灰阶上下，被软阈值削掉大半。
                // 所以生产用「I 缩放 + 低档不启用」，delta 留作开关备用。
                dp.fine = g_cfg.blurRefill * refillI;
                dp.mid = 0.0f;
                dp.deltaRefill = g_cfg.blurRefillDelta != 0;
            }
            if (wantGlobalDetail) {
                // 全局锐化用**自己**的中频/细频基准（3.1.22 从 detailMidGain 拆出）。
                // 参照路径那边也是两条独立曲线（见 g_cfg.globalMidGain 的注释）。
                dp.globalFine = shGlobal * g_cfg.globalFineGain;
                dp.globalMid = shGlobal * g_cfg.globalMidGain;
                if (dp.globalMid > 3.0f) dp.globalMid = 3.0f;
                dp.thrFineGlobal = g_cfg.globalThrFine;
                dp.thrMidGlobal = g_cfg.globalThrMid;
            }
            // 基准回填用自己的一套细频阈值（默认 0.8，比锐化那套的 3.0 低得多）：
            // 磨皮要还的就是「小幅值细频」，而锐化要防的是「噪点被放大」，
            // 两者的取舍正好相反，所以不能共用一个阈值。
            //
            // 3.1.63（2026-11-05）：这一行以前**无条件**覆盖上面纯锐化分支设的
            // `dp.thrFine = faceSharpThrFine`（默认 0），把「磨皮=0 时的独立锐化」
            // 又按回 3.0 —— |hf|<3 的细频全被 softThrF 削掉，于是面板拉满只剩
            // 0.40% 像素变化（实测 720p 脸区 3724 px / diff 中位 2），滑块观感
            // 等于没有。3.1.35 加纯锐化分支时漏了这一行，属于隐藏的旧 BUG。
            // 三种模式互斥：t<=0 是纯锐化，wantBlurRefill 只在 t 超过阈值时成立，
            // 其余才是普通「回填被磨掉的高频」。
            const bool pureSharpen = (t <= 0.0f && wantFaceDetail && haveMask);
            dp.thrFine = pureSharpen
                             ? g_cfg.faceSharpThrFine
                             : (wantBlurRefill ? g_cfg.blurRefillThr
                                               : g_cfg.detailThrFine);

            // 有全局档就整帧跑（掩码只决定「用哪一档权重」）；否则只跑掩码包围盒。
            if (wantGlobalDetail) {
                droi.x0 = 0; droi.y0 = 0; droi.x1 = w; droi.y1 = h;
            } else {
                droi = bounds_;
            }
            return !droi.empty() && (dp.fine > 0.0f || dp.mid > 0.0f ||
                                     dp.globalFine > 0.0f || dp.globalMid > 0.0f);
        };

        bool didSurface3 = false;
        if (t > 0.0f) {
            if (g_cfg.smoothModel == 1) {
                // ---- surface3：三层表面模糊（C22）------------------------
                // 档位曲线用 参考实现的 I = min(1, k·t)（不是 legacy 的饱和曲线），
                // 所以这里用 tLin 而不是上面的 t。
                beautycore::Surface3Params sp;
                // 半径随档位线性增长（C22 §4），并按**人脸宽度归一化**：
                // 真机 1080p 素材脸宽 532px 上拟合出 r(50)≈8、r(100)≈14；
                // face_src 640×394 的脸只有 111px，等效半径就是它的 1/4.8。
                // 不做归一化的话，小脸会被大半径糊成一片（face_src 100 档
                // 实测细纹理 70.2% vs参考实现 75.7%，归一化后 72.4%）。
                float scale = 1.0f;
                if (!faces_.empty()) {
                    float fw = 0.0f;
                    for (size_t i = 0; i < faces_.size(); i++)
                        if (faces_[i].w > fw) fw = faces_[i].w;
                    if (fw > 8.0f) {
                        scale = fw / 532.0f;          // 532 = 拟合素材的脸宽
                        if (scale < 0.20f) scale = 0.20f;
                        if (scale > 2.50f) scale = 2.50f;
                    }
                }
                int r1 = (int)((g_cfg.s3R0 + (g_cfg.s3R1 - g_cfg.s3R0) * tLin)
                               * scale + 0.5f);
                int r2 = (int)((g_cfg.s3R0 + (g_cfg.s3R2 - g_cfg.s3R0) * tLin)
                               * scale + 0.5f);
                sp.r1 = (r1 < 1) ? 1 : r1;
                sp.r2 = (r2 < 1) ? 1 : r2;
                sp.it1 = g_cfg.s3It1;
                sp.it2 = g_cfg.s3It2;
                sp.thr1 = g_cfg.s3Thr1;
                sp.thr2 = g_cfg.s3Thr2;
                // intensity = mix(t·K, 1.0, step(0.7, t))
                //   step 的自变量是**乘之前的 tLin** —— GLSL 的 mix 三个参数先
                //   求值，所以右式的 intensity 还是旧值。因为 K = 1/0.7，这一行
                //   在数值上等价于 min(1, tLin·K)，但保留原式写法以免后人再"优化"掉。
                float I;
                if (g_cfg.lvTable) {
                    // 3.1.18：档位查表。旧式 min(1, t/0.7) 在 t≥0.7 就恒为 1，
                    // 75 与 100 档输出逐字节相同 —— 那是「拉满没反应」的根因。
                    I = lvLerp5(g_cfg.lvI, tLin);
                } else {
                    I = (tLin >= g_cfg.s3ESat) ? 1.0f : (tLin * g_cfg.s3K);
                }
                if (I > 1.0f) I = 1.0f;
                sp.I = I;
                // evenfactor = mix(E0, intensity − E0, step(0.6, intensity))
                // 三段：[0,0.42] 恒 0.3；(0.42,0.70] 升到 0.7；[0.70,1] 恒 0.7。
                float e;
                if (g_cfg.s3EFix >= 0.0f) {
                    e = g_cfg.s3EFix;
                } else if (g_cfg.lvTable) {
                    e = lvLerp5(g_cfg.lvE, tLin);
                } else {
                    e = (I >= g_cfg.s3EThr) ? (I - g_cfg.s3E0) : g_cfg.s3E0;
                }
                if (e < 0.0f) e = 0.0f;
                sp.e = e;
                sp.protectK = g_cfg.s3EyeK;

                beautycore::RoiRect s3roi;
                if (haveMask && g_cfg.roi && !bounds_.empty()) {
                    s3roi = bounds_;
                } else {
                    s3roi.x0 = 0; s3roi.y0 = 0; s3roi.x1 = w; s3roi.y1 = h;
                }
                const unsigned char* pr3 =
                    (haveMask && protect_.size() == (size_t)w * h)
                        ? &protect_[0] : nullptr;
                if (g_cfg.s3Band) {
                    // 双尺度带通 + 振幅门控（第 2 批-B）。与 25 tap 路径的差别
                    // 只有「两个低频层怎么算」——B1/B2 换成两个不同尺度的模糊，
                    // 外加一道振幅门控；合成段 I/e/skin/eye 仍逐行同口径。
                    // s3R0/R1/R2、s3Sx/s3Sy、s3YF1/YF2 在这条路上**都不参与**。
                    beautycore::BandParams bp;
                    bp.T = g_cfg.lvTable ? lvLerp5(g_cfg.lvT, tLin) : g_cfg.s3BandT;
                    bp.gwp = g_cfg.s3Gwp;
                    bp.halfRes = g_cfg.s3Half;
                    bp.I = I;
                    blurRefillI = I;
                    // I 出来了才能判定要不要做基准回填（见 wantBlurRefill 的注释）
                    wantBlurRefill = (t > 0.0f) && (g_cfg.blurRefill > 0.0f) &&
                                     (s.panel[P_SHARPEN] <= 0.0) &&
                                     (s.panel[P_CLARITY] <= 0.0) &&
                                     (blurRefillI >= g_cfg.blurRefillMinI);
                    bp.e = e;
                    bp.protectK = g_cfg.s3EyeK;
                    // Phase 3 诊断：把本帧真正生效的参数存一份。I/e/T 是查表+夹取
                    // 出来的，OBS 侧无法自行复原（见 BeautyBandDiag 的注释）。
                    lastBand_.f1 = bp.f1;
                    lastBand_.f2 = bp.f2;
                    lastBand_.fa = bp.fa;
                    lastBand_.passes = bp.passes;
                    lastBand_.T = bp.T;
                    lastBand_.gwp = bp.gwp;
                    lastBand_.halfRes = bp.halfRes;
                    lastBand_.I = bp.I;
                    lastBand_.e = bp.e;
                    lastBand_.protectK =
                        (bp.protectK < 0.0f) ? 0.0f
                        : ((bp.protectK > 100.0f) ? 1.0f : bp.protectK / 100.0f);
                    bandValid_ = true;
                    // 3.1.49 / Phase 3.5：优先走宿主注入的 GPU 磨皮。
                    // 钩子拿到的是本帧**实际**要磨的那份 src（可能是掩码对齐
                    // 换来的 heldFrame_）和当帧生效的 mask/protect，所以它与
                    // 后面的色调/五官/细节/美形永远同帧同掩码 —— 这正是把
                    // GPU 磨皮放在主路径外侧做不到的事。
                    // 返回 false 就照旧走 CPU，结果与 3.1.48 逐字节相同。
                    //
                    // 3.1.52 / Phase 5a：这里优先问「段区间钩子」——它除了磨皮
                    // 还能顺手接管色调 / 细节，中间结果一直留在显存里，只在认领
                    // 区间的末尾回读一次。段区间钩子没装（或什么都不认领）时
                    // 退回 3.1.49 的单段钩子，那条路径已验收，保留作 A/B 与兜底。
                    bool gpuDone = false;
                    if (g_chainHook) {
                        BeautyChainJob job;
                        job.src = src;
                        job.dst = dst;
                        job.w = w;
                        job.h = h;
                        job.protect = pr3;
                        // 5b-2：当帧掩码的几何快照。没有掩码时 count 归零 ——
                        // 宿主据此知道「这张 mask 没有对应几何」，不会拿上一帧的去凑。
                        job.maskGeom = maskDiag_;
                        job.maskGeom.fresh = maskWasFresh ? 1 : 0;
                        // 2.6.0（S4a）：只出几何的帧，mask_/protect_ 里是**上一帧的残留**。
                        // 两个指针必须显式置空 —— 否则宿主一旦退回 CPU 上传，就会拿旧内容
                        // 去画，表现是「掩码跳一下」。置空 + maskGeomOnly=1 让宿主二选一：
                        // 要么 GPU 按几何重画，要么整帧跳过处理（绝不按「全皮肤」磨）。
                        const bool geomOnly = job.maskGeom.geomOnly != 0;
                        job.maskGeomOnly = geomOnly;
                        maskGeomOnlyFrame_ = geomOnly;   // 2.9.0：见成员注释
                        job.mask = (haveMask && !geomOnly) ? &mask_[0] : nullptr;
                        if (geomOnly)
                            job.protect = nullptr;
                        job.srcTag = srcTagThisFrame;
                        // 4d：五官几何 —— 在**决定 claim 之前**收集（不施加），
                        // 这样即使本帧 R 被认领，宿主手里也有与 CPU 完全相同的椭圆。
                        applyFaceRegions(nullptr, w, h, s, &job.regionGeom);
                        if (!haveMask)
                            job.maskGeom.count = 0;
                        job.band = lastBand_;
                        job.tone.clarity = cv;
                        job.tone.whiten = wv;
                        job.tone.rosy = rv;

                        // 本帧真正要跑的段。顺序固定为
                        // 磨皮 → 色调 → 五官 → 细节 → 美形，钩子的认领必须是
                        // 这个顺序上的**前缀**（校验见 chainClaimIsPrefix）。
                        unsigned flags = BEAUTY_SEG_SMOOTH;
                        // 色调只在「有掩码 + 三项之一非零 + 没开 legacy 清晰」
                        // 时是 GPU 可复现的：legacy 走 clarityRgb（12px 邻域
                        // 算子），tone.hlsl 是纯 LUT，两者不是同一套数学。
                        if (haveMask && !legacyClarity &&
                            (cv > 0.0f || wv > 0.0f || rv > 0.0f))
                            flags |= BEAUTY_SEG_TONE;
                        if (haveMask && wantRegion)
                            flags |= BEAUTY_SEG_REGION;

                        // 细节段参数：详情见 buildDetailParams 的注释。这里必须
                        // 在调钩子之前算好 —— 宿主拿到的是「算好的」，不是面板值。
                        if (wantFaceDetail || wantGlobalDetail || wantBlurRefill) {
                            if (!dpCached) {
                                dpRunnableCache = buildDetailParams(
                                    blurRefillI, dpCache, droiCache, betaCache);
                                dpCached = true;
                            }
                            // delta 模式 GPU 侧没做（它要读 dst 的细频），
                            // 交给 CPU；其余情况且参数非空转才置位。
                            if (dpRunnableCache && !dpCache.deltaRefill)
                                flags |= BEAUTY_SEG_DETAIL;
                        }

                        job.detail.fine = dpCache.fine;
                        job.detail.mid = dpCache.mid;
                        job.detail.globalFine = dpCache.globalFine;
                        job.detail.globalMid = dpCache.globalMid;
                        job.detail.thrFine = dpCache.thrFine;
                        job.detail.thrMid = dpCache.thrMid;
                        job.detail.thrFineGlobal = dpCache.thrFineGlobal;
                        job.detail.thrMidGlobal = dpCache.thrMidGlobal;
                        job.detail.kneeFine = dpCache.kneeFine;
                        job.detail.kneeMid = dpCache.kneeMid;
                        job.detail.beta = betaCache;
                        job.detail.emaOn = g_cfg.emaOn ? 1 : 0;
                        job.detail.deltaRefill = dpCache.deltaRefill ? 1 : 0;
                        job.detail.roi[0] = droiCache.x0;
                        job.detail.roi[1] = droiCache.y0;
                        job.detail.roi[2] = droiCache.x1;
                        job.detail.roi[3] = droiCache.y1;

                        // ---- 5c：美形段的逐脸几何 ------------------------
                        // 为什么要在这里填：位移场的常量（eyeR / thinYc /
                        // thinSig / ROI ...）来自**人脸框 + 关键点**，宿主手上
                        // 没有这些；它若按公式自己推一遍，就是两份实现，改一处
                        // 忘另一处的那天对拍会报一个假 bug。与色调（I/e/T 查表
                        // 夹取）、细节（fine/mid 自适应重算）同一条理由。
                        //
                        // 逐脸调用 warpFaceGeom 而不是自己判条件：这样「哪些脸
                        // 会被变形」在 CPU 与 GPU 两条路上是**同一个集合** ——
                        // 脸太小、ROI 退化、三项都因关键点不足而失效，都由它
                        // 一个人决定，不可能两边判得不一样。
                        //
                        // faces_ 超过上限（8，与面板人物个数上限一致）时一律
                        // 不认领：GPU 只能覆盖前 8 张脸，剩下几张仍要 CPU 补，
                        // 而补的顺序与 ROI 重叠假设都会变 —— 与其做半套，不如
                        // 整段交回 CPU。这条分支现实中不会触发，但必须写死。
                        memset(&job.warp, 0, sizeof(job.warp));
                        job.warp.thin = warpParams.thin;
                        job.warp.eye = warpParams.eye;
                        job.warp.chin = warpParams.chin;
                        job.warp.count = 0;
                        if (wantWarp && !faces_.empty() &&
                            faces_.size() <= (size_t)BEAUTY_WARP_MAX_FACES) {
                            for (size_t i = 0; i < faces_.size(); i++) {
                                beautycore::WarpGeom wg;
                                if (!beautycore::warpFaceGeom(faces_[i], warpParams,
                                                              w, h, wg))
                                    continue;   // 这张脸不用变 —— 两条路都不变
                                BeautyWarpFace& d =
                                    job.warp.faces[job.warp.count++];
                                d.cx = wg.cx;
                                d.cy = wg.cy;
                                d.halfW = wg.halfW;
                                d.eyeRx = wg.eyeRx;
                                d.eyeRy = wg.eyeRy;
                                d.eyeLx = wg.eyeLx;
                                d.eyeLy = wg.eyeLy;
                                d.eyeR = wg.eyeR;
                                d.eyeAmp = wg.eyeAmp;
                                d.thinYc = wg.thinYc;
                                d.thinSig = wg.thinSig;
                                d.thinK = wg.thinK;
                                d.chinYm = wg.chinYm;
                                d.chinSpan = wg.chinSpan;
                                d.chinK = wg.chinK;
                                d.chinKH = wg.chinKH;
                                d.roi[0] = wg.roi[0];
                                d.roi[1] = wg.roi[1];
                                d.roi[2] = wg.roi[2];
                                d.roi[3] = wg.roi[3];
                                d.flags = wg.flags;
                            }
                            // count>0 才置位：一张脸都没落到几何上时置位等于
                            // 让宿主跑一趟空 pass，白费一次全帧带宽。
                            if (job.warp.count > 0) flags |= BEAUTY_SEG_WARP;
                        }
                        job.flags = flags;

                        g_chainCall++;
                        unsigned c = g_chainHook(g_chainUser, job);
                        c &= flags;   // 只认真实要跑的段
                        if (c && !chainClaimIsPrefix(c, flags))
                            c = 0;    // 非前缀 → 整份丢弃（见函数注释）
                        claimed = c;
                        if (c) {
                            g_chainHit++;
                            for (int i = 0; i < 5; i++)
                                if (c & (1u << i))
                                    g_chainSeg[i]++;
                        }
                        gpuDone = (c & BEAUTY_SEG_SMOOTH) != 0;
                    } else if (g_smoothHook) {
                        g_smoothHookCall++;
                        gpuDone = g_smoothHook(g_smoothUser, src, dst, w, h,
                                               haveMask ? &mask_[0] : nullptr,
                                               pr3, lastBand_);
                        if (gpuDone) {
                            g_smoothHookHit++;
                            claimed = BEAUTY_SEG_SMOOTH;
                        }
                    }
                    if (!gpuDone) {
                        beautycore::surface3RgbBand(src, dst, w, h,
                                                    haveMask ? &mask_[0] : nullptr,
                                                    pr3, bp, s3roi, g_cfg.threads,
                                                    s3ScratchA_);
                    }
                } else if (g_cfg.s3Tap25) {
                    // 25 tap 表面模糊（第 2 批 #5/#6/#7）。与下面盒式路径的
                    // 唯一差别是「两个低频层怎么算」；合成段 I/e/skin/eye 逐行相同。
                    // 半径参数 s3R0/R1/R2 在这条路上**完全不参与** —— 真结构里
                    // 核是固定的，档位只改 I 和 e。fw/532 的归一化也一并作废。
                    beautycore::Surface25Params p25;
                    p25.sx = g_cfg.s3Sx;
                    p25.sy = g_cfg.s3Sy;
                    p25.yFactor1 = g_cfg.s3YF1;
                    p25.yFactor2 = g_cfg.s3YF2;
                    p25.halfRes = g_cfg.s3Half;
                    p25.I = I;
                    p25.e = e;
                    p25.protectK = g_cfg.s3EyeK;
                    beautycore::surface3Rgb25(src, dst, w, h,
                                              haveMask ? &mask_[0] : nullptr, pr3,
                                              p25, s3roi, g_cfg.threads,
                                              s3ScratchA_);
                } else {
                    beautycore::surface3Rgb(src, dst, w, h,
                                            haveMask ? &mask_[0] : nullptr, pr3,
                                            sp, s3roi, g_cfg.threads,
                                            s3ScratchA_, s3ScratchB_);
                }
                didSurface3 = true;
            } else if (g_cfg.down > 1) {
                // 降采样路径已实测失败（档位曲线误差 27+），只为复现当年结论保留
                beautycore::bilateralRgbaDown(src, sm, w, h,
                                              g_cfg.radius, g_cfg.sigmaSpace,
                                              g_cfg.sigmaColor, g_cfg.down);
            } else if (haveMask && g_cfg.roi && !bounds_.empty() && g_cfg.freqMode) {
                // 高低频分离：低频用大半径高斯，只压小幅度高频，低频结构原样保留。
                // strength 传 100（全强度），面板档位由后面的 blendWithMask 按 t 缩放。
                beautycore::FreqParams fp = makeFreqParams();
                beautycore::highLowSmooth(src, sm, w, h, fp, bounds_, g_cfg.threads,
                                          lowBuf_, scratch_, guideScratch_);
            } else if (haveMask && g_cfg.roi && !bounds_.empty()) {
                // 只磨掩码包围盒。逐像素独立，结果与全帧跑逐字节一致，纯省算力。
                beautycore::bilateralRgbaRoi(src, sm, w, h,
                                             g_cfg.radius, g_cfg.sigmaSpace,
                                             g_cfg.sigmaColor,
                                             bounds_, g_cfg.threads);
            } else if (g_cfg.freqMode) {
                // 没有可用掩码（模型没加载 / 这一轮没检出脸）或关掉 ROI 对照时，
                // 同样走高低频，只是范围放到全图 —— 否则「ROI 开/关」对比的是
                // 两种不同算法，等价性测试就失去意义了。
                beautycore::FreqParams fp = makeFreqParams();
                beautycore::RoiRect full;
                full.x0 = 0; full.y0 = 0; full.x1 = w; full.y1 = h;
                beautycore::highLowSmooth(src, sm, w, h, fp, full, g_cfg.threads,
                                          lowBuf_, scratch_, guideScratch_);
            } else {
                // 没有可用掩码（模型没加载 / 这一轮没检出脸）时退回全帧：
                // 宁可多算，也不能出现「有脸却不磨」。
                beautycore::bilateralRgba(src, sm, w, h,
                                          g_cfg.radius, g_cfg.sigmaSpace,
                                          g_cfg.sigmaColor, g_cfg.threads);
            }

            if (didSurface3) {
                // dst 已由 surface3Rgb 写满（含 I / e / skin / eye 四步），
                // 不能再走 blendWithMask —— 否则 skin 与 t 会被应用两遍。
            } else if (haveMask) {
                const unsigned char* pr =
                    (protect_.size() == (size_t)w * h) ? &protect_[0] : nullptr;
                blendWithMask(src, sm, dst, w, h, t, &mask_[0], pr, g_cfg.protectK);
            } else {
                beautycore::mixRgba(src, sm, dst, w, h, t);
            }
        } else {
            // 磨皮为 0：其余各项仍要在原图基础上生效，所以先把原图铺到 dst
            if (dst != src) memcpy(dst, src, need);
        }
        markSeg(1);   // 1 = 磨皮（surface3RgbBand / 双边 / 高低频，含 ROI 判定）

        if (haveMask) {
            // 清晰 / 美白 / 红润：三项合并成一次遍历（toneColor）。
            //
            // 2026-10-30 起三项都改成**照抄原生 参考实现的逐通道查找表**，不再有各自
            // 的拟合公式。理由：把它们拆开量化后发现"箱内 σ"只有零点几灰阶
            // （clarity 0.08~0.27 / color_level 0.36~1.24 / red_level 0.73）——
            // 即它们与空间位置、邻域无关，是纯颜色变换。改表即可对齐到量化误差。
            //
            // 顺序：清晰 → 美白 → 红润。先抬对比再提亮，否则提亮会把对比压平。
            // 输入用 dst（磨皮之后）而不是 src：拿原图算会把刚磨掉的瑕疵加回来。
            //
            // 旧实现（12px 中频回填当"清晰"）保留在 BEAUTY_CLARITY_LEGACY=1 下，
            // 只作 A/B 对照，不再走默认路径。
            // 3.1.52：legacyClarity / cv / wv / rv 已提到磨皮段之前（段区间钩子
            // 需要它们），这里直接沿用。
            if (legacyClarity) {
                if (cv > 0.0f) {
                    beautycore::ClarityParams cp;
                    cp.gain = cv * g_cfg.clarityMax;
                    cp.radius = g_cfg.clarityRadius;
                    cp.kneeLo = g_cfg.clarityKneeLo;
                    cp.kneeHi = g_cfg.clarityKneeHi;
                    beautycore::clarityRgb(dst, w, h, &mask_[0], bounds_, cp,
                                           g_cfg.threads, clarityScratch_);
                }
                if (wv > 0.0f || rv > 0.0f) {
                    beautycore::toneWhitenRosy(dst, w, h, &mask_[0], bounds_,
                                               wv, rv, g_cfg.threads);
                }
            } else if (!(claimed & BEAUTY_SEG_TONE)) {
                // 3.1.52：段区间钩子认领了色调时，dst 里已经是着色器算完的
                // 结果（它回读的就是这一段的输出），这里一个字都不能再动。
                beautycore::toneColor(dst, w, h, &mask_[0], bounds_,
                                      cv, wv, rv, g_cfg.threads);
            }
            markSeg(2);   // 2 = 色调（清晰 + 美白 + 红润，一次遍历）

            // 五官局部五项：妆容层，放在磨皮与美白之后（理由见 applyFaceRegions
            // 的注释）。祛斑痘也在里面 —— 它要「修掉刚磨过之后还剩下的斑点」。
            if (wantRegion && !(claimed & BEAUTY_SEG_REGION))
                applyFaceRegions(dst, w, h, s);
        }
        markSeg(3);   // 3 = 五官局部五项（若 wantRegion 为假则≈0）

        // ---- 细节回填（锐化 / 全局锐化）------------------------------------
        // 放在整条链的最后一个「加法」位置，理由：
        //   · 亮度平面取自 **src（原图）**，所以磨皮抹掉多少都不影响可回填的量；
        //   · 加在 dst 上，不会再被磨皮平均掉，也不会被美白曲线压平；
        //   · 全局锐化在「没有掩码」时也要跑 —— 否则掩码一丢，背景锐度跟着跳档，
        //     这正是参考实现 3.0.13 修过的那个 bug，core 不能重演。
        if (wantFaceDetail || wantGlobalDetail || wantBlurRefill) {
            if (claimed & BEAUTY_SEG_DETAIL) {
                // 段区间钩子认领了细节段：dst 里已经是回填后的结果，CPU 这一帧
                // 完全不碰它。但 CPU 侧的亮度历史没有推进（GPU 用的是显存里的
                // 那份），必须标记作废 —— 否则一旦回退 CPU（着色器失败 / 参数
                // 越界 / 换分辨率），EMA 会把几帧前的旧亮度混进来，表现为
                // 「刚回退那一帧画面滑一下」。这类缺陷不报错，最难查。
                detailEmaReset_ = true;
            } else {
                beautycore::DetailParams dp;
                beautycore::RoiRect droi;
                float beta = 1.0f;
                bool runnable;
                if (dpCached) {
                    // 参数在磨皮段就为段区间钩子算好了，这里复用（见 cache 的注释）
                    dp = dpCache;
                    droi = droiCache;
                    beta = betaCache;
                    runnable = dpRunnableCache;
                } else {
                    runnable = buildDetailParams(blurRefillI, dp, droi, beta);
                    dpCache = dp;
                    droiCache = droi;
                    betaCache = beta;
                    dpRunnableCache = runnable;
                    dpCached = true;
                }
                if (runnable) {
                    // ---- 时序 EMA 的上下文 ----------------------------------
                    // lumT 传 nullptr 时 detailRefill 走原路径，逐字节等于 0.7.1。
                    // 所以 BEAUTY_EMA=0 是「完全关闭」而不是「beta=1 的近似关闭」。
                    beautycore::DetailEma ema;
                    if (g_cfg.emaOn) {
                        const size_t np = (size_t)w * (size_t)h;
                        if (detailLumT_.size() != np) {
                            detailLumT_.assign(np, 0);
                            detailEmaReset_ = true;   // 尺寸变了，历史整片作废
                        }
                        // 上一帧没写历史（断帧）→ 这一帧必须用当前帧重置，
                        // 否则会把陈旧位置的亮度混进来。
                        if (detailEmaGen_ + 1 != frameNo_) detailEmaReset_ = true;
                        ema.lumT = &detailLumT_[0];
                        ema.beta = beta;
                        ema.reset = detailEmaReset_;
                        detailEmaReset_ = false;
                    }
                    beautycore::detailRefill(src, dst, w, h,
                                             haveMask ? &mask_[0] : nullptr,
                                             droi, dp, g_cfg.threads,
                                             detailScratch_, ema);
                    detailEmaGen_ = frameNo_;
                }
            }
        }
        markSeg(4);   // 4 = 细节回填（锐化 / 全局锐化 / 基准回填）

        // ---- 美形三项（3.1.27）--------------------------------------------
        // 为什么放在**最末端**而不是最前面：
        //   · 前面十项都是逐像素颜色运算，先上色再变形与先变形再上色在颜色上
        //     等价（变形只是重采样，不改变颜色本身）；
        //   · 皮肤掩码 / ROI 包围盒 / 五官椭圆全是异步在**原图**上算的。放到
        //     前面就必须把「掩码↔画面同源」那套对齐逻辑重做一遍，风险大得多；
        //   · 放末端只需一次重采样，不碰任何已验收过的路径。
        // 代价：位移场的几何基准来自**原图**的人脸框，如果一帧内脸动得很快，
        // 变形会比磨皮晚一帧生效。实测 30fps 下不可见（与掩码滞后同源）。
        if (wantWarp && !faces_.empty() && !(claimed & BEAUTY_SEG_WARP)) {
            // 3.1.62 / 5c：段区间钩子认领了美形段时，dst 里已经是着色器做完
            // 位移场的结果，这里不能再变一次（会二次变形，画面直接走形）。
            // 认领是**整段**的：钩子要么把本帧所有该变形的脸都做掉，要么一张
            // 都不做（count 的判定见上面 job.warp 的填写处）。
            if (warpParams.thin > 0.0f || warpParams.eye > 0.0f ||
                warpParams.chin > 0.0f) {
                // 逐脸处理。多张脸时 ROI 互不重叠（检测框本身就不会大面积交叠），
                // 所以串行就地修改是安全的。
                for (size_t i = 0; i < faces_.size(); i++)
                    beautycore::warpFace(dst, w, h, faces_[i], warpParams,
                                         warpScratch_);
            }
        }

        markSeg(5);   // 5 = 美形三项（位移场重采样）
        lastMs_ = nowMsCore() - t0;
        for (int i = 0; i < 6; i++)
            lastSeg_[i] = tSeg[i];
        return true;
    }

    int lastFaces() const override { return lastFaces_; }
    // 2.15.0：本帧实际用的输入方向（见 IBeautyEngine::effectiveRot）。
    int effectiveRot() const override { return effRot_.load(std::memory_order_relaxed); }
    bool regionActive() const override { return faceOk_; }
    double lastCoreMs() const override { return lastMs_; }
    void lastSegMs(double* out6) const override
    {
        if (!out6) return;
        for (int i = 0; i < 6; i++) out6[i] = lastSeg_[i];
    }
    const char* lastErr() const override { return faceErr_.c_str(); }
    int itemId() const override { return 0; }
    unsigned long ownerThread() const override { return (unsigned long)GetCurrentThreadId(); }

    // 仅供离线对拍：强制重算一次掩码并拷出，不改磨皮状态、不动时序。
    // 返回 false 表示本帧没有可用掩码，此时 out 全 0。
    bool computeMaskForDump(const unsigned char* src, int w, int h,
                            unsigned char* out) {
        if (!out) return false;
        // 离线对拍必须确定性：停掉 worker，并且以后都走同步路径。
        // 异步下「第 k 帧用哪份掩码」取决于线程调度，哈希会飘。
        stopAsync();
        asyncOn_ = false;
        maskAge_ = 999;                 // 让它走重算分支，不吃缓存
        const bool ok = updateMask(src, w, h);
        if (!ok) {
            memset(out, 0, (size_t)w * h);
            return false;
        }
        memcpy(out, &mask_[0], (size_t)w * h);
        return true;
    }

    // 诊断：只读拷贝当前掩码（GPU 色调 Phase 2 用）。
    //
    // mask_ 只在 collectAsync() 里、持 async_.m 时被 swap 进来，所以这里必须
    // 持同一把锁。1080p 下这是一次 2 MB memcpy（约 0.2 ms），只在
    // BEAUTY_GPU_TONE=1 的自检帧调用 —— 生产路径一帧都不走。
    bool copyMaskTo(unsigned char* out, int w, int h) override {
        if (!out || w <= 0 || h <= 0) return false;
        const size_t px = (size_t)w * h;
        std::lock_guard<std::mutex> lk(async_.m);
        if (!maskValid_ || mask_.size() != px) {
            memset(out, 0, px);
            return false;
        }
        memcpy(out, &mask_[0], px);
        return true;
    }

    // 诊断：当前掩码的置位像素数与同源人脸数（3.1.44，只给日志用）。
    //
    // 两个值本来就在引擎里，同步路径（updateMask）与异步路径（collectAsync）
    // 都会写，所以这里是纯读、O(1)、不扫掩码 —— 放在每帧渲染路径上也不增加
    // 功耗。刻意不加锁：与 lastFaces() 同一取舍 —— 这两个数最坏情况是晚一帧，
    // 对「这窗口的覆盖是多少」这种统计量没有影响，而加锁会让渲染线程去等
    // worker（mask_ 的 swap 在同一次持锁区间里）。
    //
    // 返回 -1 而不是 0 表示「没有有效掩码」，这样日志里能和「有掩码但覆盖为 0」
    // 区分开 —— 前者是没检出脸/没模型，后者才是像素真的全被排除。
    void maskStat(int* pixelsOut, int* facesOut) const override {
        const bool ok = maskValid_;
        if (pixelsOut) *pixelsOut = ok ? maskPixels_ : -1;
        if (facesOut)  *facesOut = ok ? lastFaces_ : 0;
    }

    // 诊断（3.1.48）：ROI 包围盒占整帧的面积比。值在 process() 里算好存着，
    // 这里只读一个 double —— 渲染线程每帧调用，不能在这里做除法以外的任何事。
    double boundsCover() const override { return lastBoundsFrac_; }

    // 诊断：眼睛保护掩码只读拷贝（Phase 3 磨皮对拍用）。
    // 与 copyMaskTo 完全同一套生命周期：protect_ 也只在 collectAsync()/updateMask()
    // 里与 mask_ 一起换进来，所以必须持同一把锁。
    // 注意 protect_ 与 mask_ 是**同尺寸、同源**的；若 maskValid_ 为假，这份
    // protect_ 也是上一轮遗留的，直接清零 —— 拿陈旧保护掩码去对拍，会在眼周
    // 制造一段无法解释的差异。
    bool copyProtectTo(unsigned char* out, int w, int h) override {
        if (!out || w <= 0 || h <= 0) return false;
        const size_t px = (size_t)w * h;
        std::lock_guard<std::mutex> lk(async_.m);
        if (!maskValid_ || protect_.size() != px) {
            memset(out, 0, px);
            return false;
        }
        memcpy(out, &protect_[0], px);
        return true;
    }

    // 诊断：本帧磨皮的生效参数快照（Phase 3）。
    bool bandDiag(BeautyBandDiag* out) const override {
        if (!out || !bandValid_) return false;
        *out = lastBand_;
        return true;
    }

    void release() override {
        stopAsync();          // 先停 worker，否则它可能正在用 face_
        face_.release();
        faceTried_ = false;
        faceOk_ = false;      // 模型已经卸了，别再让 process() 以为掩码路径可用
        // 关闭开关后再开，应当是「干净的一次启用」：掩码重新从零开始
        maskValid_ = false;
        everHadMask_ = false;
        maskAge_ = 999;
        // 3.1.21：持久线程池也要收掉。否则 OBS 卸载滤镜后 worker 线程还在，
        // 它们持有本 DLL 里的代码地址 —— 模块卸载时就是一次野跳转。
        beautycore::poolShutdown();
    }

    // 2.11.0（方案C）：宿主在回读检测输入**之前**问一句「本帧要不要提交」。
    // 这里把节奏（maskAge_）一次性推进完；随后 process() 里的 updateMask 只执行
    // 这个结论、不再自己判 —— 两边都判就会把一帧算成两个周期。
    //
    // 三种情形保守返回 true（宁可多回读一次 1.6 MB，也不能漏提交检测）：
    //   · 同步路径 —— 每帧都要在本帧建掩码；
    //   · 自适应关着 —— 周期是常量，维持由 updateMask 自己判的老分工；
    //   · 宿主没喂运动小图 —— 没有判定依据。
    bool detectDue() override {
        if (!asyncOn_ || !g_cfg.detAdapt || !g_motPx) {
            g_duePending = false;
            return true;
        }
        const int every = detectPeriod(nullptr, 0, 0);
        const bool due = (maskAge_ >= every);
        if (due)
            maskAge_ = 0;
        else
            maskAge_++;
        g_duePending = true;
        g_dueValue = due;
        return due;
    }

private:
    // 首帧预热点（3.1.21）。
    //
    // 「开关美颜时预览窗卡一下」的根因不是稳态算力，而是**第一帧**要额外付三笔
    // 一次性成本，全部压在渲染线程上：
    //   ① smooth_ 这块 1080p 8.3MB 的工作缓冲要 malloc + 逐页缺页；
    //   ② 持久线程池的 7 个 worker 要创建、栈要缺页；
    //   ③ ORT 的第一次推理要分配 arena 并选定 kernel。
    // 实测 1080p 首帧 43.2 ms vs 稳态 21 ms，多出来的 22 ms 就是这三笔（外加
    // 首帧必然要跑的一次检测）。把它们挪到模块加载阶段（那里本来就要花 200 ms
    // 加载模型），用户开开关时就是纯稳态开销。
    //
    // 注意**不跑完整的 process()**：那会把肤色自适应中心（g_skinCb/Cr）按合成
    // 帧算歪，真机前几帧的掩码跟着错。这里只碰纯缓存与纯推理。
    void warmUp() {
        if (getenv("BEAUTY_NO_PREWARM")) return;   // A/B 用：跳过预热看首帧原始成本
        const int w = 1920, h = 1080;
        const size_t need = (size_t)w * h * 4;
        const size_t px = (size_t)w * h;
        if (smooth_.size() < need) {
            smooth_.assign(need, 0);   // 分配 + 逐页触碰（等价于首帧那次缺页）
        }
        // 低频/中间缓冲：默认路径未必用到，但一旦用到就是 8MB 级的首次缺页，
        // 这里先垫上（纯 scratch，写 0 不影响任何结果）。
        if (lowBuf_.size() < need) lowBuf_.assign(need, 0);
        if (scratch_.size() < need) scratch_.assign(need, 0);
        // 掩码与五官保护区同理。maskAge_ 显式压回 999，保证首帧仍然走重算分支，
        // 不会因为「缓冲已经有内容」而误用一份全 0 的掩码。
        if (mask_.size() != px) {
            mask_.assign(px, 0);
            protect_.assign(px, 0);
            maskAge_ = 999;
        }
        // 池提前建好；数量取和 process 里同一口径（≤8，且不超过核数）
        unsigned hc = std::thread::hardware_concurrency();
        int n = (hc == 0) ? 4 : (int)hc;
        if (n > 8) n = 8;
        beautycore::poolReserve(n);
        // ORT 首次推理：只走人脸检测，输入用 640×360 的合成灰帧（模型内部本来
        // 就缩到 640×640，用 1080p 只是白付 8MB 的分配）。
        if (faceOk_) {
            std::vector<unsigned char> fake((size_t)640 * 360 * 4, 128);
            std::vector<beautycore::FaceBox> dummy;
            std::string e;
            face_.detect(&fake[0], 640, 360, dummy, e, 0.9f, 0.3f, 4);
        }
    }

    // S7（2.8.0）：本帧该用多大的提交周期。
    //
    // 只看 16×16 的亮度网格（256 个点，RGBA 步进采样），成本微秒级 —— 与被省掉的
    // 15.4 ms 推理不在一个量级。判据是「4×4 个格子为一块，16 块里最大的块平均差」，
    // 不是全图平均：运动只占少数格子，全图平均会被静止背景稀释到与噪点重叠
    // （实测见函数内注释）。
    //
    // 判据方向刻意做成「非对称」：进入运动是立即的（本帧就回每帧），退出运动要等
    // detAdaptHot 帧。这样最坏滞后是「运动开始的那一帧仍用旧掩码」，即 +1 帧，
    // 与 2.7.1 的 1 帧滞后同量级；反过来若也延迟进入，就会出现可见拖影。
    int detectPeriod(const unsigned char* src, int w, int h) {
        // 2.11.0（方案C）：宿主喂了小图就优先用它。判据、阈值、块划分全部不变，
        // 变的只是「256 个采样点从哪来」——老路径从检测输入（640×640 缩略图或全帧）
        // 按步长点采样，新路径对 64×64 小图取格内最大 |Δ|。两条路径**不逐点等价**，
        // 所以默认关。阈值（`thr=6 / hot=3`）**没有重标**：实测在同一阈值下，
        // 静止素材两臂都是 `dper=7`、运动素材两臂都是 `dper=0`、对齐同值
        // （PROGRESS §9.29.5），说明分离度够；但这只是「这批素材上够」，
        // 不是「证明等价」—— 换素材/换光照仍要重测这一对指标。
        const bool useMot = (g_motPx && g_motW >= 16 && g_motH >= 16);
        if (!g_cfg.detAdapt || (!useMot && (!src || w < 2 || h < 2))) {
            // 自适应关着也要刷新诊断量，否则日志里会留着上一轮的数
            g_detPeriod = g_cfg.maskEveryAsync;
            g_detDiff = -1;                 // -1 = 本窗口没做运动判定
            return g_cfg.maskEveryAsync;
        }
        enum { kGrid = 16 };
        int d16[kGrid * kGrid];
        if (useMot) {
            /* 每格取「格内像素 |Δ| 的最大值」，**不是**「格内先平均再求差」。
             * 这一条是被实测逼出来的：先平均会在运动素材上把信号抹平 ——
             * 方向相反的位移在同一格里互相抵消，dmot 掉到阈值以下，自适应
             * 当成静止（dper 变 7），实测 对齐 从 180/180 掉到 22/180。
             * 取最大值的口径与老路径更接近：老路径对 640×640 每 40 px 取一个点
             * （每点≈6×6 源像素均值），这里每格 4×4 个点（每点≈10×10 均值），
             * 密度同量级。 */
            const int bw = g_motW / kGrid;
            const int bh = g_motH / kGrid;
            if (!motHaveFull_)
                motHave_ = false;   // 首次用小图：按「在动」起步，与老路径首帧一致
            for (int gy = 0; gy < kGrid; gy++) {
                for (int gx = 0; gx < kGrid; gx++) {
                    int best = 0;
                    for (int yy = 0; yy < bh; yy++) {
                        const unsigned char* row =
                            g_motPx +
                            ((size_t)(gy * bh + yy) * (size_t)g_motW +
                             (size_t)(gx * bw)) * 4;
                        const size_t pidx =
                            (size_t)(gy * bh + yy) * (size_t)g_motW +
                            (size_t)(gx * bw);
                        for (int xx = 0; xx < bw; xx++) {
                            const unsigned char* p = row + (size_t)xx * 4;
                            int y = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
                            if (y < 0) y = 0;
                            if (y > 255) y = 255;
                            const size_t k = pidx + (size_t)xx;
                            const int d = motHaveFull_
                                                  ? (y - (int)motPrevFull_[k])
                                                  : 0;
                            const int ad = (d < 0) ? -d : d;
                            motPrevFull_[k] = (unsigned char)y;
                            if (ad > best) best = ad;
                        }
                    }
                    const int i = gy * kGrid + gx;
                    d16[i] = best;
                }
            }
            motHaveFull_ = true;
        } else {
        const int sx = (w > kGrid) ? (w / kGrid) : 1;
        const int sy = (h > kGrid) ? (h / kGrid) : 1;
        for (int gy = 0; gy < kGrid; gy++) {
            const unsigned char* row = src + (size_t)(gy * sy) * (size_t)w * 4;
            for (int gx = 0; gx < kGrid; gx++) {
                const unsigned char* p = row + (size_t)(gx * sx) * 4;
                // BT.601 整数近似：只用来判「动没动」，与掩码的肤色口径无关
                int y = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
                if (y < 0) y = 0;
                if (y > 255) y = 255;
                const int i = gy * kGrid + gx;
                const int d = motHave_ ? (y - (int)motPrev_[i]) : 0;
                d16[i] = (d < 0) ? -d : d;
                motPrev_[i] = (unsigned char)y;
            }
        }
        }
        const bool first = !motHave_;
        motHave_ = true;
        if (first) {                       // 首帧没有可比对象：按「在动」处理
            motHot_ = g_cfg.detAdaptHot;
            motPeriod_ = 0;                // 0 = 每帧（单位见下方注释）
            motDiff_ = 0;
            g_detPeriod = motPeriod_;
            g_detDiff = motDiff_;
            return motPeriod_;
        }
        // 判据用「4×4 个格子为一块，取 16 个块均值的最大者」，不是全图均值。
        // 为什么不用全图均值：实测 motion_720p.mp4（脸在画面里匀速移动）的
        // 16×16 全局平均差中位只有 **3.6**，而静止图 + σ=2 噪点是 2.3 —— 两者
        // 几乎重叠，阈值怎么定都错。原因是运动只占少数格子，被静止背景稀释了。
        // 块均值的分离度则很干净（同一次标定）：
        //     运动视频 4×4 块最大均值 ≥ 9.9（中位 16.9）
        //     静止+噪声            ≤ 3.8
        // 默认阈值 6 对两侧各留 1.6 倍余量。
        //
        // 2.12.0（P1-1c）：块内部照旧先取均值，**块与块之间**多了一种合并口径。
        // 上面的标定把「运动」当成了「整幅画面在动」，而真实场景里最常见的是
        // 「只有人脸那一块在动、位置不动」—— 取最大正好对这种情况最敏感，
        // 于是说话的人每帧都判成动，检测一次次白提交。RMS 把「一块大动」摊到
        // 16 块上，判据因此从「有没有局部动」变成「动的面积够不够大」，
        // 与「检测结果会不会过期」这件事终于对上了（PROGRESS §9.32.7）。
        int best = 0;
        int m[16];
        int nb = 0;
        for (int by = 0; by < kGrid; by += 4) {
            for (int bx = 0; bx < kGrid; bx += 4) {
                int sum = 0;
                for (int yy = 0; yy < 4; yy++) {
                    const int base = (by + yy) * kGrid + bx;
                    for (int xx = 0; xx < 4; xx++)
                        sum += d16[base + xx];
                }
                const int v = sum >> 4;    // /16
                if (nb < 16) m[nb++] = v;
                if (v > best) best = v;
            }
        }
        if (g_cfg.detAdaptAggRms) {
            /* 均方根：best = sqrt(Σm²/16)。
             * 用整数算，别引入 sqrt —— 判定值会进日志、还要和阈值做整数比较，
             * 浮点在这里只会带来「同一份素材换台机器差 1」的麻烦。
             * m ≤ 255，16 项和 ≤ 1,040,400，int 装得下。
             * 开方用牛顿迭代向下取整后再四舍五入，尽量贴近 calib_agg.py 的
             * 浮点结果（每帧只算一次，成本可忽略）。 */
            int sq = 0;
            for (int i = 0; i < nb; i++)
                sq += m[i] * m[i];
            sq /= (nb > 0) ? nb : 1;
            int r = 0;
            if (sq > 0) {
                int x = sq, y = (x + 1) >> 1;
                while (y < x) { x = y; y = (x + sq / x) >> 1; }
                r = x;
                if (sq - r * r > r) r++;    // 余数过半 → 进位
            }
            best = r;
        }
        motDiff_ = best;
        // 2.11.1：只统计「真的用 64×64 小图判过」的帧 —— 老路径不进这两个数，
        // 否则同一个比值会把两条路的命中率混在一起。
        if (useMot) {
            g_motCalls++;
            if (motDiff_ > g_cfg.detAdaptThr)
                g_motHot++;
        }
        // ⚠ 单位是「maskAge_ >= every」的那个 every，**不是**「每帧」的帧数：
        // every=0 → 每帧提交；every=N → 每 N+1 帧提交。这条 off-by-one 踩过一次：
        // 把「在动」写成 1，实际变成每 2 帧提交，运动素材的对齐率从 99% 掉到 50%。
        //
        // 为什么只分「动 / 不动」两档就够（曾加过第三档，被标定数据否掉了）：
        // 阈值 6 对慢动作已经足够敏感 —— 把静止图每帧平移 **1 像素**（肉眼几乎
        // 看不出的行人级慢动），块最大均值的中位就有 8.1，稳定高于 6；而 σ=2 的
        // 传感器噪点只有 ≤3.8。两者的分离区就是 3.8~8，阈值 6 落在正中间。
        // 所以「低于阈值但仍在动」这一档并不存在，多一档只会让噪点摄像头白白
        // 多烧 0.03 核，换不到质量。
        const int still = (g_cfg.detAdaptStill > 1) ? (g_cfg.detAdaptStill - 1) : 0;
        if (motDiff_ > g_cfg.detAdaptThr) {
            motHot_ = g_cfg.detAdaptHot;   // 在动：每帧，并把热窗口续满
            motPeriod_ = 0;
        } else if (motHot_ > 0) {
            motHot_--;                     // 刚停下：再每帧提交几帧，避开边界抖动
            motPeriod_ = 0;
        } else {
            motPeriod_ = still;            // 真的冻住了
        }
        g_detPeriod = motPeriod_;
        g_detDiff = motDiff_;
        return motPeriod_;
    }

    // 生成「肤色 ∩ 人脸框」掩码。返回 false 表示本帧没有可用掩码。
    //
    // 这个函数只做三件轻活：① 收走 worker 算好的结果（O(1) swap）；② worker 空闲
    // 时把本帧提交出去；③ 沿用上一份掩码。**检测本身不在这里跑** —— 1080p 下
    // YuNet 一次约 15ms，压在渲染线程上就是 60fps 每 4 帧掉 1 帧。
    bool updateMask(const unsigned char* src, int w, int h) {
        if (!faceOk_) return false;
        if (mask_.size() != (size_t)w * h) {
            mask_.assign((size_t)w * h, 0);
            protect_.assign((size_t)w * h, 0);
            maskAge_ = 999;
            maskValid_ = false;      // 换分辨率等于重来：别拿旧尺寸的掩码凑合
            everHadMask_ = false;    // 新尺寸下第一帧按「透传」处理，见 process()
        }

        /* 2.7.0（S4b）：本帧有没有 GPU 缩略图。有就把「检测吃什么」从全帧 src
         * 换成它 —— 像素系是缩略图（dw×dh），几何系仍是全帧（w×h）。
         * 这个换法只影响 updateMask：process 的其它段落（磨皮/色调…）在
         * 「全认领」的帧上本来就不读 src，宿主那条路也不给全帧像素。
         * fullW/fullH 必须与本帧 w/h 严格相等：宿主分辨率变了（画布切换）而
         * 缩略图还是上一帧的尺寸时，用旧几何去算新帧的掩码会越界读，宁可不用。 */
        const bool thumb = (g_thumbPx != nullptr) &&
                           g_thumbFullW == w && g_thumbFullH == h;
        const unsigned char* dsrc = thumb ? g_thumbPx : src;
        const int dw = thumb ? g_thumbW : w;
        const int dh = thumb ? g_thumbH : h;
        const int gW = thumb ? w : 0;
        const int gH = thumb ? h : 0;
        const float mScale = thumb ? g_thumbScale : 0.0f;
        const int mOx = thumb ? g_thumbOx : 0;
        const int mOy = thumb ? g_thumbOy : 0;

        if (asyncOn_) {
            startAsyncIfNeeded();
            collectAsync();                 // 每帧都收，不受周期限制
            // 2.11.0（方案C）：宿主已在回读之前调过 beautyCoreDetectDue()，
            // 本帧的节奏与结论都已经定好 —— 这里只执行，不再调用 detectPeriod，
            // 否则 maskAge_ 会被推进两次、运动历史也会被同一帧更新两次。
            if (g_duePending) {
                const bool due = g_dueValue;
                g_duePending = false;
                if (due)
                    submitAsync(dsrc, dw, dh, gW, gH, mScale, mOx, mOy);
                return maskValid_;
            }
            // 到了重算周期：提交一帧给 worker。周期 0 = 每帧提交。
            // worker 忙时 submitAsync 会自己放弃这一轮（见它的注释），
            // 所以 60fps + 1080p 这种「推理 15ms / 帧 16.7ms」的极端情况
            // 会自我节流成「隔帧一次」，滞后从 1 帧变成 1~2 帧，不会堆积。
            //
            // S7（2.8.0）：周期不再固定 —— detectPeriod() 按画面是否在动给出
            // 「1（在动）或 detAdaptStill（静止）」。检测输入（缩略图或全帧）
            // 每帧都够得着，所以运动判定与「本帧是否提交」解耦，静止期也不漏判。
            const int every = detectPeriod(dsrc, dw, dh);
            if (maskAge_ >= every) {
                maskAge_ = 0;
                submitAsync(dsrc, dw, dh, gW, gH, mScale, mOx, mOy);
            } else {
                maskAge_++;
            }
            return maskValid_;              // 结果没回来就继续用旧掩码，不等
        }

        if (maskAge_ < kMaskEverySync) {    // 沿用上一次的掩码
            maskAge_++;
            return maskValid_;
        }
        maskAge_ = 0;

        std::string msg;
        int px = 0, faces = 0;
        BeautyMaskDiag dg;   // 5b-2：随 mask_ 一起换进来的几何
        if (!buildMaskInto(dsrc, dw, dh, mask_, protect_, bounds_, faces_, px, faces,
                           msg, &dg, gW, gH, mScale, mOx, mOy)) {
            return onDetectFailed(msg, face_.inputSize(), face_.lastBestScore());
        }
        maskDiag_ = dg;
        maskPixels_ = px;
        lastFaces_ = faces;
        maskHold_ = 0;
        maskValid_ = true;
        faceErr_ = msg;
        // 方案 E / P0：同步路径也要跑分割。漏了这一句，BEAUTY_ASYNC_DETECT=0
        // （离线台、以及任何降级到同步检测的配置）下 BEAUTY_SEG=1 就是纯摆设。
        runSegIfDue(dsrc, dw, dh, gW, gH, mScale, mOx, mOy, faces_);
        return true;
    }

    // 检测 + 肤色掩码 + 五官保护 + 包围盒，全部算进调用方给的缓冲。
    //
    // 刻意做成**只读 src、不碰任何成员**的函数：异步化时它整段跑在 worker 线程上，
    // 只要不碰成员就不存在数据竞争。这也是异步化能做得这么轻的原因 ——
    // 不需要给磨皮链加锁。
    //
    // ---- 2.7.0（S4b）：双坐标系 ----------------------------------------
    // 默认（geomW/geomH = 0、mapScale = 0）一切照旧：src 就是全帧，w/h 既是像素
    // 尺寸也是几何坐标系。传 geomW/geomH + mapScale/mapOx/mapOy 时语义变成：
    //   · src 是 **GPU 预缩放好的检测缩略图**（w×h，通常 side×side、带 letterbox 黑边）；
    //   · detect 用缩略图，肤色估计也在缩略图上（它只需要主脸内部那块像素）；
    //   · 之后把 faces 的坐标从缩略图坐标系反算到 gw×gh（全帧），后面的全部几何
    //     （框、羽化、脖子、眼睛）都在全帧坐标里算 —— 与老路径逐字相同。
    // 这条路上**没有全帧像素**，所以 maskOut/protectOut 不会被写（强制 geomOnly），
    // 宿主必须用 GPU 掩码链出图。
    // 2.10.0 方案 B：检测在「旋转后全帧」坐标系里的点，反变换回「源坐标系」。
    // 与宿主 beauty_thumb_build 的旋转严格互逆：宿主把源绕 letterbox 矩形中心旋转
    // +θ 画进缩略图，检测看到的就是旋转后的人脸；这里再转回 -θ。
    static void invRotatePoint(float px, float py, int rot, int Wr, int Hr,
                               int Ws, int Hs, float& ox, float& oy) {
        const float crx = (Wr - 1) * 0.5f, cry = (Hr - 1) * 0.5f;
        const float csx = (Ws - 1) * 0.5f, csy = (Hs - 1) * 0.5f;
        const float dx = px - crx, dy = py - cry;
        float rx = dx, ry = dy;
        switch (rot) {
        case 90:  rx =  dy; ry = -dx; break;   // R(-90)
        case 270: rx = -dy; ry =  dx; break;   // R(+90) = R(-270)
        case 180: rx = -dx; ry = -dy; break;   // R(-180)
        default: break;                          // R(0)
        }
        ox = rx + csx;
        oy = ry + csy;
    }
    // 对整个 FaceBox（中心 + 关键点）做反变换；90/270 时框的宽高互换。
    static void invRotateFace(beautycore::FaceBox& f, int rot, int Wr, int Hr,
                              int Ws, int Hs) {
        if (rot == 0) return;
        const float cx0 = f.x + f.w * 0.5f, cy0 = f.y + f.h * 0.5f;
        float ncx = 0.0f, ncy = 0.0f;
        invRotatePoint(cx0, cy0, rot, Wr, Hr, Ws, Hs, ncx, ncy);
        if (rot == 90 || rot == 270) { const float t = f.w; f.w = f.h; f.h = t; }
        f.x = ncx - f.w * 0.5f;
        f.y = ncy - f.h * 0.5f;
        for (int k = 0; k < 10; k += 2) {
            float nx = 0.0f, ny = 0.0f;
            invRotatePoint(f.kps[k], f.kps[k + 1], rot, Wr, Hr, Ws, Hs, nx, ny);
            f.kps[k] = nx;
            f.kps[k + 1] = ny;
        }
    }

    // 2.15.0：改写「本帧有效输入方向」。只在值真的变了时写，避免每帧无谓的
    // 原子写 + 宿主侧缓存失效。
    void setEffRot(int v) {
        if (effRot_.load(std::memory_order_relaxed) != v)
            effRot_.store(v, std::memory_order_relaxed);
    }

    // 2.15.0（掩码跟脸转）：由两眼关键点求「脸的滚转角」，输出 cos/sin。
    //
    // 为什么需要它：掩码的外边界是**几何裁剪**（椭圆/矩形 + 羽化），而几何一直是
    // 「画面轴对齐」的。脸一歪（歪头，或者源本身被转了 90°），那条轴对齐的边界就
    // 横切在皮肤上 —— 用户看到的正是「长方形美白遮罩」。
    //
    // θ 取自 YuNet 的两眼连线（kps[0..1] 右眼、kps[2..3] 左眼）。这个角度**已经包含
    // 「源被旋转」这件事**：invRotateFace 之后关键点就在源坐标系里，躺着的脸算出来
    // 自然是 ±90°，于是 90° 场景下脸框的宽高互换也一并被吸收（见 lw/lh）。
    //
    // 死角 2°：正立人脸的关键点连线有 0.3~1.5° 噪声，不做死角的话四条黄金基准会
    // 因为这点噪声产生全帧差异，回归门禁就废了；2° 以内边界贴不贴脸肉眼也看不出。
    //
    // 返回 false = 按正立处理（c=1, s=0），此时下游每个式子都退化成原式。
    static bool faceRoll(const beautycore::FaceBox& f, float& c, float& s,
                        int inputRot) {
        c = 1.0f;
        s = 0.0f;
        if (!kpsSane(f)) return false;      // 关键点不可信：宁可不动
        const float ex = f.kps[2] - f.kps[0], ey = f.kps[3] - f.kps[1];
        const float e2 = ex * ex + ey * ey;
        if (!(e2 > 4.0f)) return false;                 // 两眼重合：方向无意义
        const float th = atan2f(ey, ex);
        if (fabsf(th) < 0.034906585f) return false;      // < 2°
        /* 2.16.0：**方向的自洽性检查**。
         *
         * 一只眼被手/头发挡住时，YuNet 仍会给出 5 个点，但那只眼的点可能落到手上 ——
         * `kpsSane` 只查「点是否在脸框附近」，查不出这种"位置合理、方向全错"的角度。
         * 判据：真正转过 90° 的脸，它的**检测框必然是横的**（宽 > 高）；脸框还是竖的
         * （高 > 1.25×宽）却算出 30° 以上的滚转角，只可能是关键点坏了 → 按正立处理。
         *
         * 用户 2026-11-04 20:4x 的原话正是这条：「两只眼都在画面里就没事，手遮住
         * 一只眼，遮住的那只手上全是美白遮罩」—— 掩码被一个错角度转到了手上。 */
        if (inputRot == 0 && f.h > f.w * 1.25f && fabsf(sinf(th)) > 0.5f)
            return false;
        c = cosf(th);
        s = sinf(th);
        return true;
    }

    // 2.15.3（掩码几何时间平滑）：检测每跑一次，框中心会抖 1~3 px、滚转角会抖
    // 1~3°，单帧看不出来，但脸一大、或者歪着头，这点抖动摊到掩码边界上就是
    // 用户说的「抽风闪」。这里对**掩码用的**几何做一阶 EMA：
    //
    //   · 只在掩码这一侧平滑（`fboxOut` 早在循环之前就交出去了，五官/美形不动）
    //   · 大跳直接采纳（中心位移 > 8% 脸宽，或宽高变化 > 15%）—— 换人、快动时
    //     不能拖影，宁可跳一下
    //   · 静止画面每帧框完全一样，EMA 是恒等变换 → 黄金基准逐字节不变
    //
    // 为什么放在引擎而不是宿主：掩码几何的唯一来源就是这里，宿主只负责照抄；
    // 放到宿主就是第二份状态，两边一定会分叉。

    static void emaMaskFace(beautycore::FaceBox& sf, float& rc, float& rs, size_t idx) {
        if (idx >= (size_t)BEAUTY_WARP_MAX_FACES) return;
        MaskGeomTrack& t = g_maskTrack[idx];
        const float cx = sf.x + sf.w * 0.5f, cy = sf.y + sf.h * 0.5f;
        const float a = (float)g_maskEma / 256.0f;
        bool snap = !t.valid;
        if (!snap) {
            if (fabsf(cx - t.cx) > 0.08f * sf.w) snap = true;
            if (fabsf(cy - t.cy) > 0.08f * sf.w) snap = true;
            if (fabsf(sf.w - t.w) > 0.15f * sf.w) snap = true;
            if (fabsf(sf.h - t.h) > 0.15f * sf.h) snap = true;
        }
        if (snap) {
            t.cx = cx; t.cy = cy; t.w = sf.w; t.h = sf.h;
            t.rc = rc; t.rs = rs; t.valid = true;
            return;
        }
        t.cx += (cx - t.cx) * a;
        t.cy += (cy - t.cy) * a;
        t.w += (sf.w - t.w) * a;
        t.h += (sf.h - t.h) * a;
        t.rc += (rc - t.rc) * a;
        t.rs += (rs - t.rs) * a;
        const float n = sqrtf(t.rc * t.rc + t.rs * t.rs);
        if (n > 1.0e-4f) {
            rc = t.rc / n;
            rs = t.rs / n;
        } else {
            rc = 1.0f;
            rs = 0.0f;
        }
        t.rc = rc;
        t.rs = rs;
        sf.x = t.cx - t.w * 0.5f;
        sf.y = t.cy - t.h * 0.5f;
        sf.w = t.w;
        sf.h = t.h;
    }
bool buildMaskInto(const unsigned char* src, int w, int h,
                       std::vector<unsigned char>& maskOut,
                       std::vector<unsigned char>& protectOut,
                       beautycore::RoiRect& boundsOut,
                       std::vector<beautycore::FaceBox>& fboxOut,
                       int& pxOut, int& facesOut, std::string& msgOut,
                       BeautyMaskDiag* diagOut = nullptr,
                       int geomW = 0, int geomH = 0,
                       float mapScale = 0.0f, int mapOx = 0, int mapOy = 0) {
        const bool mapped = (mapScale > 0.0f) && geomW > 0 && geomH > 0;
        const int gw = mapped ? geomW : w;
        const int gh = mapped ? geomH : h;
        if (maskOut.size() != (size_t)gw * gh)
            maskOut.assign((size_t)gw * gh, 0);
        if (protectOut.size() != (size_t)gw * gh)
            protectOut.assign((size_t)gw * gh, 0);
        // 5b-2：几何快照。count 归零是「没脸」的语义，宿主据此决定要不要跑 GPU 掩码。
        // ⚠ 肤色椭圆（cb/cr）**不能在这里记**：estimateSkinCenter() 在本函数后面才跑，
        // 落笔用的是更新后的中心。在这里记 = 拿到上一帧的中心，GPU 侧必然对不上
        // （2026-11-01 07:10 的 M1 差异就是这么来的）。它连同 count 一起在后面填。
        if (diagOut) {
            diagOut->count = 0;
            diagOut->geomOnly = 0;   // 2.6.0（S4a）：默认为 0 = 本帧 mask 有内容
            diagOut->shape = g_maskShape;
            diagOut->ru = g_skinRu;
            diagOut->rv = g_skinRv;
        }

        std::vector<beautycore::FaceBox> faces;
        std::string ferr;
        // 直接喂 RGBA（stride=4）。原先要先摊一份全分辨率 RGB，1080p 下那是
        // 600 万字节的写入，而检测只吃 640x640 —— 这份中间缓冲是纯浪费。
        face_.setMaxFaces(detMaxFaces_);
        // 2.13.1（P0-11 阶段 2）：全帧路径也要转 —— 旋转做进 letterbox 的重采样，
        // 不额外分配全帧缓冲。`mapped`（缩略图）路径的图已由宿主 GPU 转过，
        // 这里必须传 0，否则会转两次。
        const float detConf = detHighQ_ ? 0.45f : 0.6f;
        const int effRot = effRot_.load(std::memory_order_relaxed);
        const int detRot = mapped ? 0 : effRot;
        bool detOk = face_.detect(src, w, h, faces, ferr, detConf, 0.3f, 4, detRot) &&
                     !faces.empty();
        /* 2.15.0：输入方向选错时（源正立、面板却填了 90°），YuNet 在躺倒的脸上
         * 一个框都出不来 —— 掩码为空 → 磨皮/美白**整段静默失效**，用户只看到
         * 「美颜没反应」。这里退一步再试一次「不旋转」：能检出就说明这一档方向与源
         * 不匹配，于是把本帧有效方向改成 0，并让宿主下一帧起别再转缩略图
         * （beautyCoreEffectiveRot）。
         * 只在「已经检不到」时多跑一次推理，正常帧一分钱不花。 */
        if (!detOk && !mapped && detRot != 0) {
            std::string e2;
            if (face_.detect(src, w, h, faces, e2, detConf, 0.3f, 4, 0) &&
                !faces.empty()) {
                detOk = true;
                ferr = e2;
                setEffRot(0);
                if (!rotFallbackLogged_) {
                    rotFallbackLogged_ = true;
                    NALOG(BEAUTY_LOG_WARNING,
                          "[美颜] 输入方向 %d° 在本源上检不到脸，已自动改用 0°"
                          "（画面正常；删掉 BEAUTY_INPUT_ROT 环境变量可消掉这条）",
                          detRot);
                }
            }
        }
        if (!detOk) {
            msgOut = ferr;
            facesOut = 0;
            return false;
        }
        // 反变换必须与「实际用的那个方向」一致：重试成功时它已经被改成 0。
        const int invRot = effRot_;
        // 检测在「旋转后整图」上跑，所以几何此刻是旋转后坐标系，必须立刻反变换回
        // 源坐标系。位置有两条硬约束：
        //   · 必须在 estimateSkinCenter 之前 —— 那个函数拿 faces 去采 src 的真实像素，
        //     src 是全帧原图未旋转，坐标差一次旋转就会把肤色椭圆中心采到别处；
        //   · 必须在 fboxOut 赋值之前 —— 交出去的那份是全帧源坐标（宿主/五官段用）。
        // 旋转后整图的宽高在 90/270 时要互换，与 invRotateFace 的 (Wr,Hr) 约定一致。
        if (!mapped && invRot != 0) {
            const bool swap = (invRot == 90 || invRot == 270);
            const int Wr = swap ? h : w;
            const int Hr = swap ? w : h;
            for (size_t i = 0; i < faces.size(); i++)
                invRotateFace(faces[i], invRot, Wr, Hr, w, h);
        }
        facesOut = (int)faces.size();
        // 关键点要留给后续的「五官局部」五项用（亮眼/去黑眼圈/法令纹…都要位置）。
        // 在这里一次性交出去，而不是让那几项自己去调检测 —— 一帧只跑一次推理。
        fboxOut = faces;

        // 2.6.0（S4a）：本帧只出几何、不画 CPU 掩码。
        // 判据与宿主的 gpu_mask_ok **同构**（单脸 + shape 不含 bit2），再加上
        // 宿主「有 GPU 掩码链」的声明。条件任一不满足就走完整路径并在 diag 里
        // 置 0 —— 所以宿主看到 0 时永远可以退回 CPU 上传，不存在「没掩码」的帧。
        /* 2.16.1：**宿主这帧不会有 GPU 段链时，不许只出几何**。
         *
         * 段区间钩子只在 `t > 0`（磨皮>0）时被调用。磨皮=0 而美白/清晰非 0 时，
         * 钩子不跑 → GPU 掩码**没有东西替 CPU 画**；可 geomOnly 仍然为真，于是
         * CPU 掩码是空的 → 色调乘 0 → 「美白完全没效果」。
         * 用户 2026-11-04 20:50 的日志正是这一格：
         *   `仅色彩层：磨皮=0 …美白=79…｜引擎掩码 px=0 有脸=1`
         * `px=0` 不是没脸，而是**CPU 一个像素都没画**。
         * 修法：这种情况下强制走完整路径（CPU 画全帧掩码）。 */
        const bool geomOnly = !forceCpuMask_ && (mapped || g_maskGeomOnly) &&
                              diagOut && faces.size() == 1 &&
                              !(g_maskShape & 4);
        if (mapped && !geomOnly) {
            /* 缩略图模式下 src 只有 w×h 那么大，而几何坐标与掩码缓冲都是全帧 ——
             * 多脸 / shape 带 bit2 时**画不出全帧掩码**（没有像素）。
             * 这里直接报「本帧没有可用掩码」，宿主据此整帧跳过并回退全帧回读。 */
            msgOut = "thumb: full-res mask needed";
            facesOut = 0;
            return false;
        }
        if (diagOut) diagOut->geomOnly = geomOnly ? 1 : 0;

        unsigned char* mask = &maskOut[0];
        unsigned char* protect = &protectOut[0];
        if (!geomOnly) memset(mask, 0, (size_t)w * h);

        // 肤色椭圆中心自适应（C20）：先按本帧画面估计一次，再画掩码。
        //
        // 线程安全：g_skinCb/g_skinCr 只有这一处写。异步模式下本函数整段跑在
        // 唯一的 worker 上，同步模式下跑在渲染线程上，两条路径互斥（asyncOn_），
        // 不存在并发写。skinWeight() 也只被下面的 paintSkin 调用，磨皮链不读它。
        //
        // EMA 平滑：默认 1.0 = 每帧直接采纳（纯函数，与调用次数无关）。
        // <1 的系数只在真机防抖时用，理由见 g_skinAdaptEma 的定义处。
        if (g_skinAdapt) {
            float ecb = 0.0f, ecr = 0.0f;
            if (estimateSkinCenter(src, w, h, faces, ecb, ecr)) {
                if (!g_skinAdaptValid) {
                    g_skinCb = ecb;          // 首帧直接采纳，避免从旧值慢慢爬
                    g_skinCr = ecr;
                    g_skinAdaptValid = true;
                    g_skinAnnounce.store(true, std::memory_order_relaxed);
                } else {
                    g_skinCb += (ecb - g_skinCb) * g_skinAdaptEma;
                    g_skinCr += (ecr - g_skinCr) * g_skinAdaptEma;
                }
            }
            // 估计失败（脸太小 / 被遮挡 / 极暗）时保持上一帧中心，不重置
        }

        // 2.7.0（S4b）：缩略图 → 全帧。肤色估计**必须**在这之前做完 ——
        // estimateSkinCenter 采的是 src 的像素，而 src 此刻还是缩略图。
        // 换算公式与 FaceDetector::detect 里的反算完全对称：
        //   t = (full * scale + offset)  →  full = (t - offset) / scale
        // 关键点一起换，否则眼睛保护椭圆会落在错的地方。
        if (mapped) {
            for (size_t i = 0; i < faces.size(); i++) {
                beautycore::FaceBox& f = faces[i];
                f.x = (f.x - (float)mapOx) / mapScale;
                f.y = (f.y - (float)mapOy) / mapScale;
                f.w /= mapScale;
                f.h /= mapScale;
                for (int k = 0; k < 10; k += 2) {
                    f.kps[k] = (f.kps[k] - (float)mapOx) / mapScale;
                    f.kps[k + 1] = (f.kps[k + 1] - (float)mapOy) / mapScale;
                }
            }
            // 2.10.0 方案 B：上面的 unscale 把缩略图坐标映射到「旋转后全帧」；
            // 再把检测几何反变换回源坐标系（源尺寸 = gw/gh，旋转后尺寸 = 交换后的 gw/gh）。
            if (invRot != 0) {
                const bool swap = (invRot == 90 || invRot == 270);
                const int Wr = swap ? gh : gw;
                const int Hr = swap ? gw : gh;
                for (size_t i = 0; i < faces.size(); i++)
                    invRotateFace(faces[i], invRot, Wr, Hr, gw, gh);
            }
            fboxOut = faces;   // 交给宿主/五官段的那一份也要是全帧坐标
        }

        // 2.13.0 诊断：掩码几何整体平移 (dx,dy)，单位=全帧像素，默认 (0,0)。
        //
        // 位置必须在缩略图 unscale / 旋转反算**之后**：在缩略图坐标里平移再除以
        // mapScale，量级会差一个 640/w 的系数，看起来「没事」其实是没测到。
        // 位置也必须在 `fboxOut` 两次赋值之后：交出去的那份保持原样，
        // 于是「掩码相对脸错位 d 像素」成为实验里唯一的变量（五官/美形不跟着动）。
        if (g_detOffX || g_detOffY) {
            for (size_t i = 0; i < faces.size(); i++) {
                faces[i].x += (float)g_detOffX;
                faces[i].y += (float)g_detOffY;
            }
        }

        // 5b-2：中心定稿之后再记快照 —— 落笔用的就是这两个值（见函数开头的警告）。
        if (diagOut) {
            diagOut->cb = g_skinCb;
            diagOut->cr = g_skinCr;
        }

        // 掩码像素数与包围盒在写入时顺带统计，不再单独扫两遍全帧。
        // 1080p 下那两遍各是 200 万次内存访问，纯属为诊断付的代价。
        // 只有「从 0 变 255」才计数，否则脸框与脖子通道重叠处会被重复算。
        int px = 0;
        int bx0 = w, by0 = h, bx1 = -1, by1 = -1;

        /* 2.15.1：geomOnly 帧的 ROI 来源。
         *
         * geomOnly 帧一个像素都不落笔（掩码由 GPU 按几何重画），所以 bx0..by1 永远
         * 是空的，ROI 只能从**几何**推。这里在逐脸循环里累积「脸 tile ∪ 脖子通道」的
         * 并集 —— 必须含脖子：GPU 掩码沿脖子一路画到胸口，而 ROI 是 CPU 侧色调 /
         * 祛斑 / 美形的裁剪框。只按脸框给 ROI（2.15.0 及以前的做法）会把脖子那段
         * 硬切在下巴下面一条横线上，也就是用户截图里那块「长方形遮罩」。 */
        int ux0 = gw, uy0 = gh, ux1 = -1, uy1 = -1;

        // 单像素落笔：肤色权重 × 边界羽化系数，取最大值合并（脸框与脖子通道重叠）。
        // 返回 true = 这一像素真的落了非零值（用于统计包围盒的 y 范围）。
        auto paintSkin = [&](unsigned char* mrow, const unsigned char* row,
                             int x, int feather) {
            const int wgt = skinWeight(row[x * 4 + 0], row[x * 4 + 1],
                                       row[x * 4 + 2]);
            if (wgt <= 0) return false;
            const int v = (feather >= 256) ? wgt : (wgt * feather + 128) / 256;
            if (v <= 0) return false;
            if (!mrow[x]) {
                px++;
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
            }
            // 脸框与脖子通道可能重叠：取较大值，别让后画的覆盖掉
            if (v > (int)mrow[x]) mrow[x] = (unsigned char)v;
            return true;
        };

        // paintSkin 的「权重已算好」版本。脖子跟随肤色时，同一像素的权重
        // 要先用一次（探边界）、再用一次（落笔），重复调 skinWeight 里那个 sqrt
        // 是白费的。
        auto paintW = [&](unsigned char* mrow, int x, int wgt, int feather) {
            if (wgt <= 0) return false;
            const int v = (feather >= 256) ? wgt : (wgt * feather + 128) / 256;
            if (v <= 0) return false;
            if (!mrow[x]) {
                px++;
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
            }
            if (v > (int)mrow[x]) mrow[x] = (unsigned char)v;
            return true;
        };

        size_t faceIdx = 0;
        for (const auto& f : faces) {
            /* 2.15.3：掩码几何时间平滑（见 emaMaskFace）。
             *
             * 只平滑**掩码**这一侧：`fboxOut` 在本循环之前就已经交出去了，五官/美形
             * 用的仍是本帧原始几何 —— 那边拖影的代价（变形跟着慢半拍）比闪一下大。
             * 静止画面下每帧框完全一样，EMA 是恒等变换，黄金基准逐字节不变。 */
            beautycore::FaceBox sf = f;
            float rc = 1.0f, rs = 0.0f;
            bool rolled = (g_maskRoll != 0) && faceRoll(f, rc, rs, inputRot_);
            if (g_maskEma > 0) {
                emaMaskFace(sf, rc, rs, faceIdx);
                rolled = (rc != 1.0f) || (rs != 0.0f);
            }
            faceIdx++;
            const float cx = sf.x + sf.w * 0.5f, cy = sf.y + sf.h * 0.5f;
            const float ac = fabsf(rc), as = fabsf(rs);
            /* 2026-11-05 修（对齐 2.16.1 基线）：半轴与包围盒必须分开算。
             *
             * 旧写法先把 lw/lh 做成**旋转后的 AABB**（w*|c|+h*|s|），
             * 再拿它当椭圆半轴、又乘一次 |c|/|s| 去算像素迭代的包围盒 —— 等于把
             * 同一个旋转套了两遍。代价随倾角迅速放大：θ=15° 时椭圆半轴比正确
             * AABB 还大 19%（相对脸宽大 1.5 倍），θ=45° 时整整大 2 倍 —— 掩码
             * 沿旋转方向糊出去一大圈，用户看到的是「歪头时磨皮范围忽然变大」。
             * 歪头直播很常见，所以这不是纸面问题。
             *
             * 现在三组量各归各位：
             *   lw/lh 脸**自身**轴上的宽高 → 羽化宽度、脖子宽度/长度
             *   hw/hh 椭圆半轴（= 局部半宽 × kFaceBoxGrow）
             *   hx/hy 旋转后的 AABB 半尺寸 → 只用于整数像素的迭代上下界
             * θ=0 时三者退化成旧值，静态正脸逐字节不变（离线回归 4 条全过）。 */
            const float lw = sf.w, lh = sf.h;
            const float hw = lw * kFaceBoxGrow * 0.5f, hh = lh * kFaceBoxGrow * 0.5f;
            const float hx = hw * ac + hh * as, hy = hw * as + hh * ac;
            int x0 = (int)(cx - hx), x1 = (int)(cx + hx);
            int y0 = (int)(cy - hy), y1 = (int)(cy + hy);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            /* 2.7.1（S4b1）⚠ 钳位边界必须是**几何坐标系**的 gw/gh，不能是 w/h。
             * mapped（缩略图）模式下 w/h 是缩略图尺寸（640），而 faces 已经换成
             * 全帧坐标（x1≈2540），拿 640 去钳会把矩形压成反向的 (1629,666,640,640)
             * —— mask.hlsl 的 faceRect 直接退化，皮肤掩码全零，于是
             * **磨皮整段失效**（美形只吃椭圆，所以照常生效，特别容易漏看）。
             * 2026-11-02 23:04 的 chainDBG 就是这么抓到的。 */
            if (x1 > gw) x1 = gw;
            if (y1 > gh) y1 = gh;

            // 脖子通道的横向范围。脸框底边在它之内时**不做下羽化**，否则下巴
            // 与脖子之间会出现一条水平接缝。
            int nx0 = (int)(cx - sf.w * g_neck.halfW);
            int nx1 = (int)(cx + sf.w * g_neck.halfW);
            if (nx0 < 0) nx0 = 0;
            if (nx1 > gw) nx1 = gw;

            int fe = (g_maskFeatherPx >= 0)
                         ? g_maskFeatherPx
                         : (int)(lw * kMaskFeatherRatio + 0.5f);
            if (fe > kFeatherMax) fe = kFeatherMax;
            if (fe < 0) fe = 0;
            // 脖子通道按**脖子自己的宽度**给羽化宽度 —— 3.1.9 用脸宽 12%，等价于
            // 脖子半宽的 27%，整个脖子外侧四分之一都被压成半强度，这是真机上
            // 「效果弱了较多」的主力来源。
            // 2.15.0：这里的「脸宽」用 lw（局部轴上的脸宽），θ=0 时恒等 f.w。
            int feNeck = (g_maskFeatherPx >= 0)
                             ? g_maskFeatherPx
                             : (int)(2.0f * g_neck.halfW * lw * kMaskFeatherRatio
                                     + 0.5f);
            if (feNeck > kFeatherMax) feNeck = kFeatherMax;
            if (feNeck < 0) feNeck = 0;

            /* 2.15.0：脖子椭圆的中心与外接框。
             *
             * 脖子接在脸 tile 的**局部下沿**（v = +vcN，vcN = y1-cy），θ≠0 时这个点
             * 沿脸的「竖」方向偏出去，不再正好在 (cx, y1)。θ=0 时 ccx≡cx、ccy≡y1、
             * nHx≡A、nHy≡B —— 与旧式逐字节相同（所以下面只对中心做一次 rolled 分支，
             * 循环体不用抄两份）。 */
            const float vcN = (float)y1 - cy;
            const float neckA_ = lw * g_neck.halfW;
            const float neckB_ = lh * g_neck.len;
            const float ccx = rolled ? (cx - rs * vcN) : cx;
            const float ccy = rolled ? (cy + rc * vcN) : (float)y1;
            const float nHx = neckA_ * ac + neckB_ * as;
            const float nHy = neckA_ * as + neckB_ * ac;
            int nxA0 = (int)(ccx - nHx), nxA1 = (int)(ccx + nHx);
            int nyA0 = (int)(ccy - nHy), nyA1 = (int)(ccy + nHy);
            if (nxA0 < 0) nxA0 = 0;
            if (nyA0 < 0) nyA0 = 0;
            if (nxA1 > gw) nxA1 = gw;
            if (nyA1 > gh) nyA1 = gh;
            if (nxA1 < nxA0) nxA1 = nxA0;
            if (nyA1 < nyA0) nyA1 = nyA0;

            // 5b-2：把这张脸的**全部整数几何**原样记下来，供宿主在 GPU 上复刻。
            // 只记录、不参与任何计算 —— 所以这段对画面零影响（纯诊断/接线用）。
            // 记的时机放在羽化宽度算完、落笔之前：此后所有分支都不再改这些量。
            if (diagOut && diagOut->count < BEAUTY_WARP_MAX_FACES) {
                BeautyMaskFace& mg = diagOut->faces[diagOut->count];
                mg.x0 = x0; mg.y0 = y0; mg.x1 = x1; mg.y1 = y1;
                mg.fe = fe; mg.feNeck = feNeck;
                mg.nx0 = nx0; mg.nx1 = nx1;
                // 脖子的循环上下界随模式变（见 BeautyMaskFace 的注释）：
                // 椭圆分支从 cyN∓B 起算，会伸进脸框区域；矩形分支从 y1 往下。
                if (g_maskShape & 2) {
                    // 2.15.0：外接框/中心都已经按 roll 算好（见上面的 neckA_ 段）。
                    mg.nyLo = nyA0; mg.nyHi = nyA1;
                    mg.nx0 = nxA0; mg.nx1 = nxA1;
                    mg.neckA = neckA_; mg.neckB = neckB_;
                    mg.neckCy = ccy; mg.neckCx = ccx;
                } else {
                    int n0 = y1;
                    int n1 = (int)(cy + hh + sf.h * g_neck.len);
                    if (n0 < 0) n0 = 0;
                    if (n1 > gh) n1 = gh;
                    mg.nyLo = n0; mg.nyHi = n1;
                    mg.neckA = 0.0f; mg.neckB = 0.0f; mg.neckCy = 0.0f;
                    mg.neckCx = 0.0f;   // 矩形分支用不到，但别把未初始化值送进常量缓冲
                }
                mg.cx = cx; mg.cy = cy; mg.a = hw; mg.b = hh;
                mg.rc = rc; mg.rs = rs; mg.lw = lw; mg.lh = lh;
                mg.hasEyes = 0;   // 由下面的五官保护段回填（只有第一张脸）
                diagOut->count++;
            }

            // 2.6.0（S4a）：几何已经全部记进 diag，本帧又不需要 CPU 掩码
            // → 直接跳掉后面三段逐像素（脸椭圆/矩形 + 脖子通道）。
            // 这是整段里最贵的部分：4K 8.3M 像素各一次肤色判定。
            // 放在 diag 之后是有意的：diag 填的是整数几何，与逐像素无关。
            if (geomOnly) {
                // 2.15.1：顺手累积 ROI 的并集（脸 tile ∪ 脖子），见 ux0 处的说明。
                int rx0 = x0, ry0 = y0, rx1 = x1, ry1 = y1;
                if (g_maskShape & 2) {
                    if (nxA0 < rx0) rx0 = nxA0;
                    if (nyA0 < ry0) ry0 = nyA0;
                    if (nxA1 > rx1) rx1 = nxA1;
                    if (nyA1 > ry1) ry1 = nyA1;
                } else {
                    if (nx0 < rx0) rx0 = nx0;
                    if (y1 < ry0) ry0 = y1;
                    if (nx1 > rx1) rx1 = nx1;
                    int nEnd = (int)(cy + hh + sf.h * g_neck.len);
                    if (nEnd > gh) nEnd = gh;
                    if (nEnd > ry1) ry1 = nEnd;
                }
                if (rx0 < ux0) ux0 = rx0;
                if (ry0 < uy0) uy0 = ry0;
                if (rx1 > ux1) ux1 = rx1;
                if (ry1 > uy1) uy1 = ry1;
                continue;
            }

            if (g_maskShape & 1) {
                // 椭圆距离场（3.1.20）：边界按 (x/a)²+(y/b)² = 1 收敛，四条直边消失。
                // 到边界的像素距离用 (1−q)·rEff 近似（q 是归一化半径），脸框接近方形
                // 时误差 <10%；羽化斜坡吃的是整数像素，这点误差看不出来。
                const float a = hw, b = hh;
                const float ia2 = 1.0f / (a * a), ib2 = 1.0f / (b * b);
                const float rEff = 0.5f * (a + b);
                for (int y = y0; y < y1; y++) {
                    const unsigned char* row = src + ((size_t)y * w) * 4;
                    unsigned char* mrow = &mask[(size_t)y * w];
                    const float dy0 = (float)y + 0.5f - cy;
                    bool any = false;
                    for (int x = x0; x < x1; x++) {
                        const float dx0 = (float)x + 0.5f - cx;
                        /* 2.15.0：局部坐标。θ=0（rc=1, rs=0）时 u≡dx0、v≡dy0，
                         * 下面的式子逐字节等于 3.1.20 的原式 —— 所以旋转只在
                         * 真歪头时才改变结果。 */
                        const float u = dx0 * rc + dy0 * rs;
                        const float v = -dx0 * rs + dy0 * rc;
                        const float q = sqrtf(u * u * ia2 + v * v * ib2);
                        if (q >= 1.0f) continue;      // 椭圆外不落笔，等价于旧的矩形裁剪
                        const int dpx = (int)((1.0f - q) * rEff + 0.5f);
                        if (paintSkin(mrow, row, x, featherRamp(dpx, fe))) any = true;
                    }
                    // 只在**这一行真的落了像素**时扩 y：否则椭圆上下顶点那几行没肤色，
                    // 也会把包围盒拽到最大，ROI 白白变大、性能数据失真。
                    if (any) {
                        if (y < by0) by0 = y;
                        if (y > by1) by1 = y;
                    }
                }
            } else {
            for (int y = y0; y < y1; y++) {
                const unsigned char* row = src + ((size_t)y * w) * 4;
                unsigned char* mrow = &mask[(size_t)y * w];
                const int fTop = featherRamp(y - y0 + 1, fe);
                const int fBot = featherRamp(y1 - y, fe);
                bool any = false;
                for (int x = x0; x < x1; x++) {
                    const int dl = x - x0 + 1, dr = x1 - x;
                    int feather = fTop;
                    const int fx = featherRamp(dl < dr ? dl : dr, fe);
                    if (fx < feather) feather = fx;
                    // 下巴正下方交给脖子通道续上，底边不羽化（否则出现水平线）
                    if ((x < nx0 || x >= nx1) && fBot < feather) feather = fBot;
                    if (paintSkin(mrow, row, x, feather)) any = true;
                }
                // 只在**这一行真的落了像素**时扩 y：否则脸框下沿那几行没肤色，
                // 也会把包围盒拽到最大，ROI 白白变大、性能数据失真。
                if (any) {
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
            }
            }

            // 脖子通道：从脸框下沿接着往下，横向收窄到 0.45 脸宽，仍然只认肤色。
            // y1 上面已经 clamp 过，所以 ny0 从它起算是安全的。
            int ny0 = y1;
            int ny1 = (int)(cy + hh + sf.h * g_neck.len);
            if (ny0 < 0) ny0 = 0;
            if (ny1 > gh) ny1 = gh;
            if (g_maskShape & 4) {
                // 脖子：**逐行跟随肤色的实际边界**（3.1.20）。
                //
                // 旧版 nx0..nx1 是固定 0.90 脸宽的矩形。脖子比它宽时，那两条竖边就
                // 切在皮肤上：磨与不磨的分界是一条直线，头部一摆就在脖子上来回扫，
                // 用户看到的「遮罩残影」有一半是它（另一半是检测滞后，见 BEAUTY_PROFILE
                // 的对齐统计）。这里从中心向两侧探到肤色真正结束的位置，边界落在
                // 皮肤/头发/衣领的**真实轮廓**上，不再是人为几何。
                static thread_local std::vector<unsigned char> wrow;
                if (wrow.size() < (size_t)w) wrow.assign((size_t)w, 0);
                const int cxI = (int)cx;
                const int maxHalf = (int)(f.w * kNeckSkinMaxHalf);
                const int minHalf = (int)(f.w * kNeckSkinMinHalf);
                int rowL = cxI - maxHalf;
                int rowR = cxI + maxHalf + 1;
                if (rowL < 0) rowL = 0;
                if (rowR > w) rowR = w;
                const int gap = 3;      // 连续 3 个非肤色像素算「到头」
                for (int y = ny0; y < ny1; y++) {
                    const unsigned char* row = src + ((size_t)y * w) * 4;
                    unsigned char* mrow = &mask[(size_t)y * w];
                    for (int x = rowL; x < rowR; x++) {
                        wrow[x] = (unsigned char)skinWeight(
                            row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]);
                    }
                    int lo = cxI, hi = cxI;
                    if (cxI < rowL || cxI >= rowR || wrow[cxI] == 0) {
                        // 中心不是肤色（衣领挡住、下巴阴影）：这一行退回兜底宽度，
                        // 落笔时 wrow 仍然会把非肤色挡掉，不会真的画上去。
                        lo = cxI - minHalf;
                        hi = cxI + minHalf;
                    } else {
                        int miss = 0;
                        for (int x = cxI; x >= rowL; x--) {
                            if (wrow[x]) { lo = x; miss = 0; }
                            else if (++miss >= gap) break;
                        }
                        miss = 0;
                        for (int x = cxI; x < rowR; x++) {
                            if (wrow[x]) { hi = x; miss = 0; }
                            else if (++miss >= gap) break;
                        }
                        // 探到的范围太窄说明肤色被打断（胡须、镜框、阴影），
                        // 按兜底宽度铺开，免得掩码被切成碎片。
                        if (hi - lo < 2 * minHalf) {
                            lo = cxI - minHalf;
                            hi = cxI + minHalf;
                        }
                    }
                    if (lo < rowL) lo = rowL;
                    if (hi > rowR - 1) hi = rowR - 1;
                    if (lo < 0) lo = 0;
                    const int fBot = featherRamp(ny1 - y, feNeck);
                    bool any = false;
                    for (int x = lo; x <= hi; x++) {
                        int feather = fBot;
                        const int dl = x - lo + 1, dr = hi - x + 1;
                        const int fx2 = featherRamp(dl < dr ? dl : dr, feNeck);
                        if (fx2 < feather) feather = fx2;
                        if (paintW(mrow, x, wrow[x], feather)) any = true;
                    }
                    if (any) {
                        if (y < by0) by0 = y;
                        if (y > by1) by1 = y;
                    }
                }
            } else if (g_maskShape & 2) {
                // 脖子也用椭圆距离场（3.1.20）：最宽处落在下颌线 y1，向下按椭圆收窄。
                // 旧版是 nx0..nx1 的**竖直直边**，头部摆动时两条直边在脖子上来回扫，
                // 用户看到的「遮罩残影」正是它们。椭圆没有直边段，收窄也连续。
                // 2.15.0：改成绕**脖子中心**（ccx,ccy，已按 roll 算好）取局部坐标，
                // θ=0 时 ccx≡cx、ccy≡y1，u/v 逐字节等于原来的 dx/dy。
                const float ia2 = 1.0f / (neckA_ * neckA_);
                const float ib2 = 1.0f / (neckB_ * neckB_);
                const float rEff = 0.5f * (neckA_ + neckB_);
                for (int y = nyA0; y < nyA1; y++) {
                    const unsigned char* row = src + ((size_t)y * w) * 4;
                    unsigned char* mrow = &mask[(size_t)y * w];
                    const float dy0 = (float)y + 0.5f - ccy;
                    bool any = false;
                    for (int x = nxA0; x < nxA1; x++) {
                        const float dx0 = (float)x + 0.5f - ccx;
                        const float u = dx0 * rc + dy0 * rs;
                        const float v = -dx0 * rs + dy0 * rc;
                        const float q = sqrtf(u * u * ia2 + v * v * ib2);
                        if (q >= 1.0f) continue;
                        const int dpx = (int)((1.0f - q) * rEff + 0.5f);
                        if (paintSkin(mrow, row, x, featherRamp(dpx, feNeck))) any = true;
                    }
                    if (any) {
                        if (y < by0) by0 = y;
                        if (y > by1) by1 = y;
                    }
                }
            } else {
            for (int y = ny0; y < ny1; y++) {
                const unsigned char* row = src + ((size_t)y * w) * 4;
                unsigned char* mrow = &mask[(size_t)y * w];
                const int fBot = featherRamp(ny1 - y, feNeck);
                bool any = false;
                for (int x = nx0; x < nx1; x++) {
                    const int dl = x - nx0 + 1, dr = nx1 - x;
                    int feather = fBot;
                    const int fx = featherRamp(dl < dr ? dl : dr, feNeck);
                    if (fx < feather) feather = fx;
                    if (paintSkin(mrow, row, x, feather)) any = true;
                }
                if (any) {
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                }
            }
            }
        }

        // 五官保护区：**只画眼睛，不画嘴巴**。
        // 参考实现的掩码只有 .g = eyes 一个保护通道，没有 lip —— 嘴唇在参考实现那里
        // 是被当皮肤磨的。3.1.14 之前我们多画了一个嘴部椭圆（kps[6..9] 那四行），
        // 那是自创的保护，100 档唇区残差 +25.5 的最可能根因，已删除以对齐结构。
        // 尺寸按脸框比例给（YuNet 只给 5 个点，没有轮廓，只能这样估）。
        if (!geomOnly) memset(protect, 0, (size_t)w * h);
        for (size_t fi = 0; fi < faces.size(); fi++) {
            const auto& f = faces[fi];
            if (!kpsSane(f)) continue;    // 关键点飞出去就跳过这张脸的保护
            const float ex = f.w * 0.12f, ey = f.h * 0.075f;
            // 2.6.0（S4a）：裸几何帧不画保护椭圆（GPU 用 eye0/eye1 参数自己画），
            // 但下面的 hasEyes 回填**必须照做** —— 那正是 GPU 侧的判据。
            if (!geomOnly) {
                paintProtectEllipse(protect, w, h, f.kps[0], f.kps[1], ex, ey);
                paintProtectEllipse(protect, w, h, f.kps[2], f.kps[3], ex, ey);
            }
            // 5b-2：记下第一张脸的眼睛几何（GPU 切片 1 只支持单脸）。
            // 注意 hasEyes 的语义 = 「CPU 真的画了保护椭圆」——
            // kpsSane 不通过时 CPU 一个像素都不画，GPU 也必须什么都不画。
            if (diagOut && fi == 0 && diagOut->count > 0) {
                BeautyMaskFace& mg = diagOut->faces[0];
                mg.hasEyes = 1;
                mg.eye0x = f.kps[0]; mg.eye0y = f.kps[1];
                mg.eye1x = f.kps[2]; mg.eye1y = f.kps[3];
                mg.eyeW = ex; mg.eyeH = ey;
            }
        }

        pxOut = px;
        // ROI 只在这里算一次（掩码每 4 帧才重算），中间帧直接复用。
        // pad 必须 >= radius：ROI 边缘像素的邻域会往外读，切掉了结果就变了。
        if (bx1 < 0) {
            /* 2.9.0（P0-8）：这里要区分两种「bx1 < 0」。
             *
             * geomOnly 帧（S4a 显式降频 / S4b 缩略图检测）**一个像素都不落笔**，
             * 所以 bx1 从来没被赋值。它不是「掩码全零」，而是「本帧没画」。
             *
             * 此前一律清空，后果是一条链：bounds_ 空 → GPU 段 geoOut->spotOn=0、
             * CPU 段 applySpotFix 跳过 → 祛斑痘在两条路径上都不执行。而默认配置
             * detect_small=1 几乎恒走缩略图，等于祛斑自 2.6.0 起基本没生效
             * （实测：默认 spot=0，gonly 恒 180/180；关掉 geomOnly 后 spot=1）。
             *
             * 处置分两种：
             *   · 有上一份真实包围盒 → **沿用**。与 mask_ 的「残留」语义一致，
             *     滞后 ≤ 一个检测周期（S7 静止期最多 detAdaptStill 帧）。
             *   · 从没建过（缩略图路径下永远没有落笔帧）→ 用本帧脸框兜底。
             *     否则修了也白修：bounds_ 会一直是空的。
             */
            if (!geomOnly) {
                boundsOut = beautycore::RoiRect();      // 真的全零掩码
            } else if (ux1 >= 0) {
                /* 2.15.1：geomOnly 帧按**本帧几何**重算 ROI（之前只在 bounds_ 为空时
                 * 用脸框兜底一次，之后就一直沿用那个不含脖子的框）。ROI 边缘与掩码
                 * 支撑之间只靠 pad 隔开，而 pad（radius+1）远小于羽化宽度，所以把
                 * 脖子切在框外的那条边是**硬**的 —— 正是用户看到的横线。 */
                const int pad = g_cfg.radius + 1;
                beautycore::RoiRect r;
                r.x0 = ux0 - pad;
                r.y0 = uy0 - pad;
                r.x1 = ux1 + pad + 1;
                r.y1 = uy1 + pad + 1;
                if (r.x0 < 0) r.x0 = 0;
                if (r.y0 < 0) r.y0 = 0;
                if (r.x1 > gw) r.x1 = gw;
                if (r.y1 > gh) r.y1 = gh;
                if (r.x1 > r.x0 && r.y1 > r.y0) boundsOut = r;
            }
        } else {
            const int pad = g_cfg.radius + 1;
            beautycore::RoiRect r;
            r.x0 = (bx0 - pad < 0) ? 0 : bx0 - pad;
            r.y0 = (by0 - pad < 0) ? 0 : by0 - pad;
            r.x1 = (bx1 + pad + 1 > w) ? w : bx1 + pad + 1;
            r.y1 = (by1 + pad + 1 > h) ? h : by1 + pad + 1;
            boundsOut = r;
        }

        // 诊断：五官保护覆盖率按椭圆面积估，不扫全帧
        double prArea = 0.0;
        for (const auto& f : faces) {
            if (!f.hasKps) continue;
            const double ex = f.w * 0.12, ey = f.h * 0.075;
            prArea += 2.0 * 3.14159265 * ex * ey;
            const double mrx = fabsf(f.kps[8] - f.kps[6]) * 0.5 + f.w * 0.05;
            prArea += 3.14159265 * mrx * (f.h * 0.09);
        }
        char b[192];
        snprintf(b, sizeof(b),
                 "mask=%d px (%.2f%%) faces=%d roi=%.2f%% protect=%.2f%% kps=%d%s",
                 px, 100.0 * px / (double)((size_t)w * h), facesOut,
                 100.0 * (double)boundsOut.area() / (double)((size_t)w * h),
                 100.0 * prArea / (double)((size_t)w * h),
                 (faces.empty() || !faces[0].hasKps) ? 0 : 1,
                 geomOnly ? " geomonly=1" : "");
        msgOut = b;
        return true;
    }

    // 检不出脸时的统一处理（同步/异步两条路共用）：
    // 丢掉一次检测结果 ≠ 脸消失了，先沿用旧掩码最多 kMaskHold 次。
    bool onDetectFailed(const std::string& ferr, int inputSize, float bestScore) {
        lastFaces_ = 0;
        if (maskValid_ && maskHold_ < kMaskHold) {
            maskHold_++;
            char b[128];
            snprintf(b, sizeof(b), "detect lost, holding mask %d/%d",
                     maskHold_, kMaskHold);
            faceErr_ = b;
            return true;
        }
        maskValid_ = false;
        bounds_ = beautycore::RoiRect();     // 空矩形 = 无 ROI，调用方退回全帧
        // 诊断：把「推理输入边长」和「最高置信度」留下来。检不出脸时这两个
        // 数决定了该调尺寸还是该调阈值，缺了就只能瞎猜。
        char b[160];
        snprintf(b, sizeof(b), "no face x%d (input=%d best=%.3f%s%s)",
                 kMaskHold + 1, inputSize, bestScore,
                 ferr.empty() ? "" : " err=", ferr.c_str());
        faceErr_ = b;
        return false;
    }

    // ---- 异步检测 ------------------------------------------------
    //
    // 模型：一个 worker 线程 + 一个「输入槽」+ 一个「输出槽」。
    //   · 渲染线程：把结果 swap 走（O(1)），把本帧 memcpy 进输入槽（空闲时）。
    //   · worker：等输入槽有货 → 跑 buildMaskInto → 结果放进输出槽。
    // 渲染线程**永远不 wait**，所以检测再慢也只是「掩码晚一帧到位」。
    //
    // 为什么不做多缓冲/队列：检测只要 15ms，而提交周期是 4 帧（133ms），
    // 一个槽位绰绰有余。排队的意义是「一帧都不能丢」，但掩码本来就只需要最新
    // 那份 —— 中间结果没有价值（wtnl-zhi 的做法也是「只保留最新帧」）。
    struct AsyncState {
        std::mutex m;
        std::condition_variable cv;
        std::thread th;
        bool quit = false;
        bool hasJob = false;      // 输入槽有货
        bool working = false;     // worker 正在算
        bool ready = false;       // 输出槽有结果待取
        std::vector<unsigned char> frame;
        int w = 0, h = 0;
        // 2.7.0（S4b）：这一份帧的像素系与实际几何系要不要分开。
        // geomW/geomH 为 0 = 老路径（像素系 == 几何系，mapScale 不参与）。
        // 缩略图帧下 w/h 是 640×640、geomW/geomH 是全帧，mapScale/ox/oy 是
        // 宿主做 letterbox 时的三个参数（buildMaskInto 用它们反算）。
        int geomW = 0, geomH = 0;
        float mapScale = 0.0f;
        int mapOx = 0, mapOy = 0;
        std::vector<unsigned char> mask;
        std::vector<unsigned char> protect;
        beautycore::RoiRect bounds;
        std::vector<beautycore::FaceBox> fbox;   // 本帧人脸（含关键点）
        BeautyMaskDiag maskDiag;                 // 5b-2：生成这份掩码用的几何
        unsigned long long tag = 0;              // 5b-2：这份帧/掩码的宿主帧号
        int pixels = 0;
        int faces = 0;
        bool valid = false;
        int inputSize = 0;
        float bestScore = 0.0f;
        std::string msg;
        double ms = 0.0;
    };
    AsyncState async_;
    bool asyncOn_ = true;         // 由 g_cfg.asyncDetect 初始化
    bool asyncStarted_ = false;
    double asyncMs_ = 0.0;        // 最近一次 worker 的耗时（诊断用）

    // ---- 3.1.10 掩码 ↔ 帧对齐（修「羽化压不住的拖影」）----------------------
    //
    // 3.1.9 之前，`mask_` 与它描述的帧是**脱钩**的：worker 在帧 k 上算掩码，
    // 渲染线程在帧 k+1~k+2 上用它。皮肤在动，掩码就永远落后 1~2 帧，边界每一帧
    // 都在脸后扫一条带 —— 羽化只能把这条带从"硬边"软成"淡边"，消不掉。
    //
    // 修法：把「掩码 + 它描述的那一帧」作为**一个整体**扣下来，磨皮直接处理那一帧。
    // 数据本来就在：worker 的输入槽 `async_.frame` 里存着它算掩码用的原始帧，而且
    // 结果收走时（collectAsync）worker 一定已经空闲 —— 交换出去零拷贝、无竞态。
    // 代价是输出整体落后 1 帧（30fps 下 33ms，摄像头链路里本来就不可见），
    // 收益是**掩码与画面逐像素同源**：脸后的背景不再被扫出一条带，脸的前缘也不再
    // 有一条"没磨"的粗边。
    //
    // 只在「结果刚到」的那一帧用（maskFresh_）：worker 偶尔慢了一帧，就退回旧的
    // 「当前帧 + 旧掩码」，**不重复输出已显示过的帧** —— 持续停顿比一次性对齐
    // 更显眼。30fps 下 worker 15ms < 33ms 帧预算，结果每帧都到，对齐是常态。
    std::vector<unsigned char> heldFrame_;   // 与 mask_ 同源的原始帧（RGBA）
    unsigned long long heldTag_ = 0;         // 5b-2：heldFrame_ 对应的宿主帧号
    bool maskFresh_ = false;                 // 本帧刚收到一份新的有效结果
    bool alignOn_ = true;                    // BEAUTY_MASK_ALIGN=0 关闭（A/B 用）

    void startAsyncIfNeeded() {
        if (!asyncOn_ || asyncStarted_) return;
        asyncStarted_ = true;
        // 3.0.0：worker 是新建线程，thread_local 的 t_slot 默认是 0。必须把
        // 创建它的那个槽带过去，否则第二个实例的异步检测会去读写 slot 0 的
        // 帧号 / 肤色 EMA / 掩码轨迹 —— 正是「主机位闪烁」的来源之一。
        const int mySlot = t_slot;
        async_.th = std::thread([this, mySlot] { t_slot = mySlot; asyncWorker(); });
        // 把 worker 降到「低于正常」：它晚一两帧出结果完全没关系（掩码本来就
        // 4 帧才更新一次），但渲染线程晚一帧就掉帧。让 OS 优先保渲染线程。
        SetThreadPriority(async_.th.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    }

    void stopAsync() {
        if (!asyncStarted_) return;
        {
            std::lock_guard<std::mutex> lk(async_.m);
            async_.quit = true;
        }
        async_.cv.notify_all();
        if (async_.th.joinable()) async_.th.join();
        asyncStarted_ = false;
    }

    // 把本帧交给 worker。worker 忙就直接放弃这一轮（下一个周期再说）——
    // 排队没有意义，掩码只需要最新的那份。
    void submitAsync(const unsigned char* src, int w, int h,
                     int geomW = 0, int geomH = 0, float mapScale = 0.0f,
                     int mapOx = 0, int mapOy = 0) {
        {
            std::lock_guard<std::mutex> lk(async_.m);
            if (async_.hasJob || async_.working) return;
            if (async_.frame.size() != (size_t)w * h * 4)
                async_.frame.resize((size_t)w * h * 4);
            async_.w = w;
            async_.h = h;
            async_.geomW = geomW;
            async_.geomH = geomH;
            async_.mapScale = mapScale;
            async_.mapOx = mapOx;
            async_.mapOy = mapOy;
        }
        // 拷贝放在锁外：1080p 下这是一次 8MB memcpy（约 0.7ms），
        // 持锁做会把 worker 的收尾也堵住。
        memcpy(&async_.frame[0], src, (size_t)w * h * 4);
        {
            std::lock_guard<std::mutex> lk(async_.m);
            async_.hasJob = true;
            // 5b-2：这一份帧属于哪个宿主帧号。submitAsync 只从 updateMask 调用，
            // 而 updateMask 发生在「对齐换帧」之前 —— 所以此刻的 g_frameTag 就是
            // 这份帧自己的帧号（不是某个旧帧的）。
            async_.tag = g_frameTag;
            g_maskSubmits++;      // 诊断计数：只在**真的提交了**时才加
        }
        async_.cv.notify_one();
    }

    // 收走 worker 的结果。返回 true = 这一轮确实消费了一个结果。
    bool collectAsync() {
        beautycore::RoiRect b;
        int px = 0, faces = 0, inputSize = 0;
        float best = 0.0f;
        bool valid = false, got = false;
        std::string msg;
        double ms = 0.0;
        {
            std::lock_guard<std::mutex> lk(async_.m);
            if (async_.ready) {
                // 只有「有脸」时才换缓冲：没脸时 worker 的 mask 内容无意义，
                // 换进来会把当前正在用的掩码冲掉（丢帧保持就失效了）。
                if (async_.valid) {
                    mask_.swap(async_.mask);
                    protect_.swap(async_.protect);
                    faces_.swap(async_.fbox);
                    // 5b-2：几何和掩码必须同进同退（见 maskDiag_ 的声明处）。
                    maskDiag_ = async_.maskDiag;
                    // 3.1.10：把**这份掩码描述的原始帧**一起扣下来。此时 worker
                    // 必然空闲（ready=working 结束），帧槽没人读写，swap 无竞态；
                    // swap 之后帧槽里是被换出的旧 heldFrame_，下一次 submitAsync
                    // 的 memcpy（同样要求 worker 空闲）会先把它覆盖掉再交活。
                    if (async_.frame.size() ==
                        (size_t)async_.w * async_.h * 4) {
                        heldFrame_.swap(async_.frame);
                        heldTag_ = async_.tag;   // 5b-2：这份帧的宿主帧号
                        maskFresh_ = true;   // 渲染线程自己在 process 里消费
                    }
                }
                b = async_.bounds;
                px = async_.pixels;
                faces = async_.faces;
                valid = async_.valid;
                inputSize = async_.inputSize;
                best = async_.bestScore;
                msg = async_.msg;
                ms = async_.ms;
                async_.ready = false;
                got = true;
            }
        }
        if (!got) return false;
        asyncMs_ = ms;
        if (valid) {
            bounds_ = b;
            maskPixels_ = px;
            lastFaces_ = faces;
            maskHold_ = 0;
            maskValid_ = true;
            faceErr_ = msg;
        } else {
            onDetectFailed(msg, inputSize, best);
        }
        return true;
    }

    void asyncWorker() {
        for (;;) {
            std::vector<unsigned char> mask, protect;
            int w = 0, h = 0;
            int geomW = 0, geomH = 0, mapOx = 0, mapOy = 0;
            float mapScale = 0.0f;
            {
                std::unique_lock<std::mutex> lk(async_.m);
                async_.cv.wait(lk, [this] { return async_.quit || async_.hasJob; });
                if (async_.quit) return;
                mask.swap(async_.mask);
                protect.swap(async_.protect);
                w = async_.w;
                h = async_.h;
                geomW = async_.geomW;
                geomH = async_.geomH;
                mapScale = async_.mapScale;
                mapOx = async_.mapOx;
                mapOy = async_.mapOy;
                async_.hasJob = false;
                async_.working = true;
            }

            beautycore::RoiRect b;
            std::vector<beautycore::FaceBox> fbox;
            BeautyMaskDiag dgcol;   // 5b-2：本轮的掩码几何
            int px = 0, faces = 0, inputSize = 0;
            float best = 0.0f;
            std::string msg;
            const double t0 = nowMsCore();
            const bool ok = buildMaskInto(&async_.frame[0], w, h, mask, protect,
                                          b, fbox, px, faces, msg, &dgcol,
                                          geomW, geomH, mapScale, mapOx, mapOy);
            inputSize = face_.inputSize();
            best = face_.lastBestScore();
            const double ms = nowMsCore() - t0;

            // ---- 方案 E / P0：人脸裁切分割（影子层，默认完全不进这条分支）----
            // 与 buildMaskInto 同一份输入帧、同一个 worker，天然帧对齐；
            // 人脸框也直接复用本轮结果，不额外检测。
            // 结果由 beauty_skinseg 内部发布，渲染线程只读 beautyCoreCopySeg。
            runSegIfDue(&async_.frame[0], w, h, geomW, geomH,
                        mapScale, mapOx, mapOy, fbox);

            {
                std::lock_guard<std::mutex> lk(async_.m);
                async_.mask.swap(mask);
                async_.protect.swap(protect);
                async_.fbox.swap(fbox);
                async_.maskDiag = dgcol;
                async_.bounds = b;
                async_.pixels = px;
                async_.faces = faces;
                async_.valid = ok;
                async_.inputSize = inputSize;
                async_.bestScore = best;
                async_.msg = msg;
                async_.ms = ms;
                async_.working = false;
                async_.ready = true;
            }
        }
    }

    // 亮度 Sobel 梯度 → 0..256 的「边缘强度」（256 = 强边缘，完全不磨）。
    //
    // 为什么需要它：双边滤波靠「颜色差大 → 权重小」隐式保护边缘，对高频纹理
    // 有效，但对**同色系的硬边**（黑眉毛压在皮肤上、眼镜框边缘）保护不足，
    // 会把边缘磨成渐变。显式梯度门控把「真边缘」和「皮肤瑕疵」分开：
    // 前者梯度大、后者梯度小。
    //
    // 用 |gx|+|gy| 代替 sqrt(gx²+gy²)：边缘检测本来就容忍近似，L1 省掉一次
    // 开方，在 1080p 十几万像素上是实打实的差别。
    //
    // 阈值为什么是「死区 + 斜坡」而不是 wuxiwuhen 的「线性 ×10」：
    // 实测这张图的梯度分布（归一化到 0..1）是
    //     脸颊平坦处  中位 0.020 / 90 分位 0.082
    //     眉毛横带    中位 0.017 / 90 分位 0.111   ← 中位数比脸颊还低！
    // 眉毛横带的**中位数**和脸颊一样，因为那个区域里大部分像素是眉间的皮肤，
    // 只有眉毛本体是高梯度。所以 Sobel 标的是「边缘线」而不是「区域」，
    // 线性放大 ×10 会让皮肤纹理（0.02×10=0.2）也吃到 20% 门控 —— 实测确实
    // 如此：低梯度区保留率被从 84% 抬到 94%，等于整体少磨了一大截。
    // 正确做法是给死区：低于 0.10 完全不门控，高于 0.30 完全门控。
    // 阈值按 G 通道尺度标定（G 0..255，Sobel 系数和 4 → gx 上界 ±1020）。
    // 原 luma*1000 口径下的 0.10/0.30 归一化，等价换算到这里是 102/306。
    static const int kEdgeLo = 102;
    static const int kEdgeHi = 306;

    static int sobelEdge(const unsigned char* src, int w, int h,
                         int x, int y, int k) {
        const int xm = (x > 0) ? (x - 1) : 0;
        const int xp = (x + 1 < w) ? (x + 1) : (w - 1);
        const int ym = (y > 0) ? (y - 1) : 0;
        const int yp = (y + 1 < h) ? (y + 1) : (h - 1);
        const unsigned char* r0 = src + (size_t)ym * w * 4;
        const unsigned char* r1 = src + (size_t)y * w * 4;
        const unsigned char* r2 = src + (size_t)yp * w * 4;

        // 用 G 通道当亮度（人眼对绿最敏感，梯度性质与 luma 基本一致）。
        // 完整 luma 要 3 次乘加 × 8 邻居 = 24 次/像素，实测这占了门控开销的
        // 大头；换成直接读 G 通道后少掉 2/3 的算术，阈值按同一口径重新标定过。
        const int l00 = r0[xm * 4 + 1];
        const int l01 = r0[x * 4 + 1];
        const int l02 = r0[xp * 4 + 1];
        const int l10 = r1[xm * 4 + 1];
        const int l12 = r1[xp * 4 + 1];
        const int l20 = r2[xm * 4 + 1];
        const int l21 = r2[x * 4 + 1];
        const int l22 = r2[xp * 4 + 1];

        const int gx = (l02 + 2 * l12 + l22) - (l00 + 2 * l10 + l20);
        const int gy = (l20 + 2 * l21 + l22) - (l00 + 2 * l01 + l02);
        const int mag = ((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) / 2;

        // k 是灵敏度倍数（越大越容易判为边缘）：k=10 用标定好的阈值，
        // k=5 把上阈值翻倍（更难门控），k=20 减半（更激进）。
        int hi = (k > 0) ? (kEdgeHi * 10 / k) : kEdgeHi;
        if (hi <= kEdgeLo + 1) hi = kEdgeLo + 1;
        if (mag <= kEdgeLo) return 0;
        if (mag >= hi) return 256;
        return (int)((long long)(mag - kEdgeLo) * 256 / (hi - kEdgeLo));
    }

    // 线程数决策。行数太少时线程创建开销大于收益，直接单线程。
    static int blendThreads(int threads, int h) {
        int n = threads;
        if (n <= 0) {
            const unsigned hc = std::thread::hardware_concurrency();
            n = hc ? (int)hc : 4;
            if (n > 8) n = 8;
        }
        if (h < 32) return 1;
        const int byRows = h / 16;
        if (n > byRows) n = byRows;
        return (n < 1) ? 1 : n;
    }

    // 只在掩码内混合：掩码外逐字节保持原图。
    // 按行多线程：掩码外虽然是简单拷贝，1080p 也是 200 万像素，单线程要 2~3ms。
    static void blendWithMask(const unsigned char* src, const unsigned char* sm,
                              unsigned char* dst, int w, int h, float t,
                              const unsigned char* mask,
                              const unsigned char* protect, int protectK) {
        const int ti = (int)(t * 256.0f + 0.5f);
        const int inv = 256 - ti;
        const int kEdge = g_cfg.edgeK;
        const int kLimitThr = g_cfg.limitThr;

        auto runRows = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * w;
            for (int x = 0; x < w; x++) {
                const size_t o = (rowOff + x) * 4;
                if (mask[rowOff + x] == 0) {    // 掩码外：逐字节还原
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                    dst[o + 3] = src[o + 3];
                    continue;
                }
                int k = (ti * mask[rowOff + x]) >> 8;   // 掩码值也参与权重
                // 五官保护：眼睛、嘴巴上的磨皮按 protectK 减弱
                if (protect && protectK > 0) {
                    const int p = (int)protect[rowOff + x] * protectK / 100;
                    k = (k * (256 - p)) >> 8;
                }
                if (kEdge > 0) {
                    const int e = sobelEdge(src, w, h, x, y, kEdge);
                    // e=0 时与上面的公式逐位相同，保证「不门控」与旧行为一致
                    k = (k * (256 - e)) >> 8;
                }
                // 阈值软膝（立体感）：只抹「小幅度起伏」（坑洼、斑点、毛孔），
                // 鼻梁两侧、下颌线这类**大幅度明暗过渡**按比例保住，
                // 否则过渡带被抹平后脸会显得塌、出现台阶。
                // 公式同 lumina：limit = 1 - smoothstep(thr, thr*2.5, |src-sm|)
                if (kLimitThr > 0 && k > 0) {
                    int dmax = 0;
                    for (int c = 0; c < 3; c++) {
                        int dv = (int)src[o + c] - (int)sm[o + c];
                        if (dv < 0) dv = -dv;
                        if (dv > dmax) dmax = dv;
                    }
                    const int hi = kLimitThr * 5 / 2;
                    int limit;
                    if (dmax <= kLimitThr) {
                        limit = 256;
                    } else if (dmax >= hi) {
                        limit = 0;
                    } else {
                        limit = 256 * (hi - dmax) / (hi - kLimitThr);
                    }
                    k = (k * limit) >> 8;
                }
                const int ki = 256 - k;
                dst[o + 0] = (unsigned char)(((int)src[o + 0] * ki + (int)sm[o + 0] * k + 128) >> 8);
                dst[o + 1] = (unsigned char)(((int)src[o + 1] * ki + (int)sm[o + 1] * k + 128) >> 8);
                dst[o + 2] = (unsigned char)(((int)src[o + 2] * ki + (int)sm[o + 2] * k + 128) >> 8);
                dst[o + 3] = src[o + 3];
            }
        }
        };

        const int nt = blendThreads(g_cfg.threads, h);
        if (nt <= 1) {
            runRows(0, h);
            return;
        }
        // 持久线程池（3.1.21）。这里原来每帧 new 一组 std::thread 再 join，
        // 实测光建线程就要 0.29 ms × nt，一帧 21 趟下来 6 ms 上下。
        beautycore::parallelRows(0, h, nt, runRows);
        (void)inv;
    }

    // **同步**路径的重算周期（帧）。异步路径用 g_cfg.maskEveryAsync（默认 0 = 每帧）。
    // 两条路必须分开：同步时检测压在渲染线程上，每帧跑就是每帧 +14ms（掉帧）；
    // 异步时它跑在 worker 上，每帧跑只多烧 42% 的一个核。
    // 这个 4 也决定了离线台架的口径 —— cpu_bench / verify_roi / detail_test 全走同步，
    // 所以它们报的每帧毫秒里，检测成本是按 1/5 摊的。别拿它直接对生产值。
    static const int kMaskEverySync = 4;
    // 连续丢检多少次之后才真正放弃掩码。按 4 帧一次算，6 次 ≈ 0.8 秒 @30fps：
    // 够撑过挥手、低头、眨眼这类短暂遮挡，又不会让「人真的走开了」一直磨着空气。
    static const int kMaskHold = 6;

    beautycore::FaceDetector face_;
    bool faceTried_ = false;
    bool faceOk_ = false;
    std::string faceErr_;
    std::vector<unsigned char> mask_;
    std::vector<unsigned char> protect_;   // 五官保护区（眼睛/嘴巴），0..255
    // 5b-2：mask_ 对应的整数几何（哪张脸、矩形/羽化/脖子上下界、肤色椭圆）。
    // 与 mask_ 同生命周期：只在 mask_ 被换进来时一起换，读它的人必须同时确认
    // maskValid_（否则拿到的是上一轮的几何）。
    BeautyMaskDiag maskDiag_;
    // 本帧检出的人脸（含 5 个关键点）。五官局部五项全靠它定位；
    // 与掩码同一生命周期（异步时由 worker 一起交回），所以不会出现
    // 「掩码是这一帧的、关键点是上一帧的」这种错配。
    std::vector<beautycore::FaceBox> faces_;
    beautycore::RoiRect bounds_;   // 掩码包围盒（外扩 radius），随掩码一起更新
    // 2.9.0（P0-8）：本帧 mask_ 是不是「没有 GPU 掩码链时的那份真掩码」。
    // geomOnly 帧（S4a 降频 / S4b 缩略图检测）**不落笔**，mask_ 里是上一份残留
    // 或全零，任何按像素读它的算子都会整体跳过。祛斑就栽在这上面（另一处是
    // bounds_ 被清空）。凡是「只在有掩码时才有意义」的 CPU 算子都要先看这个标志。
    bool maskGeomOnlyFrame_ = false;
    bool maskValid_ = false;
    int  detMaxFaces_ = 8;   // 面板「人物个数」（1..8）
    int  detHighQ_ = 0;      // 面板「识别模式」：0 标准 / 1 高精度（conf 0.6 / 0.45）
    int  inputRot_ = 0;      // 2.10.0 方案 B：输入旋转 0/90/180/270（检测几何反变换用）
    // 2.15.0：面板声明的方向（cfgRot_）与**验证过**的方向（effRot_，原子量 ——
    // 宿主渲染线程要读它决定「要不要转缩略图」，检测可能跑在 worker 上）。
    // 检不到脸时 effRot_ 会退回 0，见 buildMaskInto 的重试段。
    int  cfgRot_ = 0;
    std::atomic<int> effRot_{0};
    bool rotFallbackLogged_ = false;
    // 2.16.1：本帧强制走 CPU 掩码（钩子不会跑，见 process() 里的设置点）。
    bool forceCpuMask_ = false;
    // 3.1.21：本次「启用以来」是否成功产出过掩码。只看第一次 —— 用它区分
    // 「刚开开关、检测还没回来」（这几帧透传）和「人不在画面里」（全帧兜底）。
    bool everHadMask_ = false;
    int maskAge_ = 999;
    // S7（2.8.0）：运动检测状态。motPrev_ 是上一帧的 16×16 亮度网格。
    unsigned char motPrev_[256] = {};
    bool motHave_ = false;   // 是否有可比对象（首帧恒按「在动」）
    // 2.11.0（方案C）：走 64×64 小图时，逐像素保存上一帧亮度（4096 B），
    // 因为判据是「格内 |Δ| 取最大」，必须先有逐像素的上一帧值。
    unsigned char motPrevFull_[64 * 64] = {};
    bool motHaveFull_ = false;
    int  motHot_ = 0;        // 剩余「热」帧数（运动刚结束还要每帧跑几帧）
    int  motPeriod_ = 1;     // 上一次算出的周期
    int  motDiff_ = 0;       // 上一次的网格平均亮度差（诊断）
    int maskHold_ = 0;   // 连续丢检次数（用于沿用旧掩码）
    int lastFaces_ = 0;
    int maskPixels_ = 0;
    std::vector<unsigned char> smooth_;
    std::vector<unsigned char> warpScratch_;   /* 3.1.27：美形的 ROI 采样源 */
    std::vector<unsigned char> lowBuf_;   // 高低频的低频层
    std::vector<unsigned char> scratch_;  // 可分离高斯的中间缓冲
    std::vector<float> guideScratch_;     // 引导滤波的 float 缓冲
    std::vector<float> clarityScratch_;   // clarity 的灰度 box 缓冲
    std::vector<float> detailScratch_;    // 锐化的 4 个亮度平面
    std::vector<float> spotScratch_;      // 祛斑痘的两个浮点平面
    std::vector<float> s3ScratchA_;       // surface3 的 b1 平面（6 个 ROI 平面）
    std::vector<float> s3ScratchB_;       // surface3 的 b2 平面（同上，不能共用）
    double lastMs_ = 0.0;
    // 3.1.47 分项计时：掩码 / 磨皮 / 色调 / 五官 / 细节 / 美形，顺序与 process()
    // 里 markSeg(i) 的编号严格一致。只在主路径末尾写一次，早退路径清零。
    double lastSeg_[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    // 3.1.48 诊断：本帧磨皮包围盒面积比（0..1），-1 = 无包围盒。
    // 存在的唯一目的是给「要不要给 surface3RgbBand 补 ROI 裁剪」提供依据：
    // 那个函数目前收了 roi 却不看，全帧跑 18 趟盒式，理论可省上限就是 1 − 本值。
    double lastBoundsFrac_ = -1.0;

    // ---- 磨皮参数快照（Phase 3 诊断）---------------------------------------
    // 写点在 band 分支里，读点是 beautyCoreBandDiag()。刻意不加锁：
    // 消费方（GPU 自检）是在**同一次 process() 调用返回之后**立刻读的，
    // 中间不可能插进另一帧，所以这就是本帧生效值。
    BeautyBandDiag lastBand_{};
    bool bandValid_ = false;

    // ---- 细节层时序 EMA（0.7.2）-------------------------------------------
    // motionPrev_ 是 1/4 分辨率的亮度快照，只用来算「帧间差均值」这个标量；
    // detailLumT_ 是全帧亮度历史，喂给 detailRefill 做 EMA。
    // 两个都按帧尺寸增长后复用，不每帧分配。
    std::vector<unsigned char> motionPrev_;
    bool motionHave_ = false;
    std::vector<unsigned char> detailLumT_;
    bool detailEmaReset_ = true;
    // 帧序号 + 「历史最后一次被写入的帧序号」。
    // 为什么需要这个：EMA 的历史必须**逐帧连续**推进，中间断一帧（面板全 0 提前
    // 返回、脸丢了导致 detailRefill 早退、只有脸内锐化但这一帧没检出脸）之后，
    // 历史就是陈旧的 —— 再拿它混会把几帧前那个位置的亮度渗进当前帧，观感是
    // 「重新开磨皮的第一帧闪一下」。所以只要 detailEmaGen_ + 1 != frameNo_
    // 就当作重置。用序号比较而不是布尔标志，是因为提前返回的路径太多，
    // 逐个去置标志一定会漏。
    uint64_t frameNo_ = 0;
    uint64_t detailEmaGen_ = 0;   // 0 = 从未写过

    // 运动判据：全帧亮度帧间差的均值（4×4 抽样，单位=灰阶，0..255）。
    // 顺手把快照推进一帧。首帧返回一个「大值」，让调用方按运动处理（beta=1），
    // 避免从空历史起步时把画面拉向黑。
    double updateMotion(const unsigned char* src, int w, int h) {
        const int sw = w / 4, sh = h / 4;
        if (!src || sw <= 0 || sh <= 0) return 255.0;
        const size_t n = (size_t)sw * (size_t)sh;
        if (motionPrev_.size() != n) {
            motionPrev_.assign(n, 0);
            motionHave_ = false;
        }
        uint64_t acc = 0;
        for (int y = 0; y < sh; y++) {
            const unsigned char* row = src + (size_t)(y * 4) * w * 4;
            unsigned char* pr = &motionPrev_[(size_t)y * (size_t)sw];
            for (int x = 0; x < sw; x++) {
                const unsigned char* q = row + (size_t)(x * 4) * 4;
                // 77/151/28 是整数版 0.299/0.587/0.114，和 detailRefill 的浮点
                // 权重略有出入，但这里只用来判「动没动」，不需要与它逐位一致。
                const int l = (q[0] * 77 + q[1] * 151 + q[2] * 28) >> 8;
                const int d = l - (int)pr[x];
                acc += (uint64_t)(d < 0 ? -d : d);
                pr[x] = (unsigned char)l;
            }
        }
        if (!motionHave_) {
            motionHave_ = true;
            return 255.0;
        }
        return (double)acc / (double)n;
    }

    // 帧间差 → 新帧权重 beta。<= still 视为静止（强平滑），>= full 完全不平滑。
    // 中间线性过渡，避免在阈值附近抖动导致「时有时无」的观感。
    float emaBeta(double motion) const {
        if (!g_cfg.emaOn) return 1.0f;
        if (motion <= (double)g_cfg.emaStill) return g_cfg.emaBetaStill;
        if (motion >= (double)g_cfg.emaFull) return 1.0f;
        const double t = (motion - (double)g_cfg.emaStill) /
                         ((double)g_cfg.emaFull - (double)g_cfg.emaStill);
        return (float)(g_cfg.emaBetaStill + (1.0 - g_cfg.emaBetaStill) * t);
    }

    // ---- 人脸局部五项 ------------------------------------------------------
    // 五官立体 / 亮眼 / 去黑眼圈 / 去法令纹 / 祛斑痘。
    //
    // 几何全部相对**脸框 + 5 个关键点**，没有一个绝对像素值 —— 理由见
    // beauty_region.h：绝对像素在 720p 会移到瞳孔上、在 4K 又跑到下眼睑。
    //
    // 为什么必须放在磨皮与美白之后：
    //   · 放磨皮之前，刚提亮的眼白会被磨皮平均掉，变成一片雾；
    //   · 放美白之前，两次抬亮叠加容易把眼白/牙齿推过 255，出现死白斑。
    //
    // 五项共用一次 applyEllipses（增益相加），因为它们的贡献互不干扰：
    // 面板为 0 的那项贡献恒为 0，所以「只开一项」和「五项全开」都能各自对拍。
    void applyFaceRegions(unsigned char* dst, int w, int h, const BeautySettings& s,
                          BeautyRegionGeom* geoOut = nullptr) {
        const float threed = clampi01(s.panel[P_THREED] / 100.0);
        const float eye = clampi01(s.panel[P_EYEBRIGHT] / 100.0);
        const float pouch = clampi01(s.panel[P_POUCH] / 100.0);
        const float naso = clampi01(s.panel[P_NASOLABIAL] / 100.0);
        const float spot = clampi01(s.panel[P_DELSPOT] / 100.0);
        // 4d：快照先清零 —— 「这一帧没有五官」与「没收集过」必须可区分。
        if (geoOut) {
            geoOut->faceCount = 0;
            geoOut->ellipseCount = 0;
            geoOut->spotOn = 0;
        }
        if (threed <= 0.0f && eye <= 0.0f && pouch <= 0.0f && naso <= 0.0f &&
            spot <= 0.0f)
            return;
        if (getenv("BEAUTY_REGION_DEBUG")) {
            for (const auto& f : faces_) {
                fprintf(stderr, "[dbg] face x=%.1f y=%.1f w=%.1f h=%.1f kps=%d "
                        "kps=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n",
                        f.x, f.y, f.w, f.h, (int)f.hasKps,
                        f.kps[0], f.kps[1], f.kps[2], f.kps[3], f.kps[4], f.kps[5],
                        f.kps[6], f.kps[7], f.kps[8], f.kps[9]);
            }
        }
        if (faces_.empty() || mask_.size() != (size_t)w * h) return;

        const unsigned char* mk = &mask_[0];
        std::vector<beautycore::Ellipse> els;
        els.reserve(48);
        // 法令纹走的是「暗度填平」模型，区域椭圆与其它三项分开收集，
        // 因为它的作用方式不是「加常量」而是「min(封顶, 斜率×暗差)」。
        std::vector<beautycore::Ellipse> nasoEls;
        nasoEls.reserve(4);
        // 法令纹**下瓣**（常量增益椭圆）：在 applyFoldRemoval 之后叠加，
        // 免得影响上瓣的邻域最大 LR 计算。
        std::vector<beautycore::Ellipse> nasoTailEls;
        nasoTailEls.reserve(2);
        float nasoSlope = 0.0f;


        for (const auto& f : faces_) {
            if (f.w < 8.0f || f.h < 8.0f) continue;
            const float fx = f.x, fy = f.y, fw = f.w, fh = f.h;
            // 关键点不可信时只保留「不需要位置」的祛斑痘，其余四项跳过：
            // 位置错了会把亮度糊到脸颊/背景上，比不做更糟。
            const bool kp = kpsSane(f);

            if (kp) {
                // 归一化坐标（相对脸框）
                auto nx = [&](float px) { return (px - fx) / fw; };
                auto ny = [&](float py) { return (py - fy) / fh; };
                const float e0x = nx(f.kps[0]), e0y = ny(f.kps[1]);
                const float e1x = nx(f.kps[2]), e1y = ny(f.kps[3]);
                const float nsx = nx(f.kps[4]), nsy = ny(f.kps[5]);
                const float m0x = nx(f.kps[6]), m0y = ny(f.kps[7]);
                const float m1x = nx(f.kps[8]), m1y = ny(f.kps[9]);

                if (threed > 0.0f) {
                    // 3.1.22：五官立体 = 8 个锚在关键点上的宽椭圆。
                    //
                    // 三条实测结论（数据见 06_离线验证/全项对齐_20261030/
                    // _grid_seg.py 的分段输出 与 fit_threed.json）：
                    //  1. 参照是**解剖/网格驱动**的：额头恒亮、脖子恒暗，与画面
                    //     自身明暗无关。之前的「低频明暗场」路线实测 corr≈0.05，
                    //     已废弃。
                    //  2. 参照的**面部中线 = 双眼中点**，不是 YuNet 的鼻尖 ——
                    //     侧脸时鼻尖会偏 0.12 脸宽，参照中线不偏。鼻梁/鼻底/
                    //     额头/下颌全部锚在 midX/midY 上。
                    //  3. 提亮区是**大片平顶**、压暗区是**窄带**，所以 feather
                    //     分两组（亮 0.705 / 暗 0.378）。
                    //
                    // 拟合集（m00/m03/m06）评分 79.67，留出集（m01/m02/m04/
                    // m05/m07）78.17 —— 没有过拟合。
                    //
                    // 3.1.23：重新拟合时把**引擎自己的皮肤软掩码**也模拟进去了
                    // （applyEllipses 会 acc *= mw）。不模拟这一层会高估增益。
                    const float g = g_cfg.regThreedGain * threed;
                    const float midX = (e0x + e1x) * 0.5f;
                    const float midY = (e0y + e1y) * 0.5f;
                    const float mouthMidX = (m0x + m1x) * 0.5f;
                    const float mouthMidY = (m0y + m1y) * 0.5f;
                    const float sh = 0.15f;
                    /* 3.1.24（2026-11-05）：形状从「8 个独立平顶光斑」改成
                     * 「互相重叠的连续光场 + 钟形衰减」。
                     *
                     * 为什么改：拉满时肉眼能数出**四个白圈**（额头 / 左右颧 /
                     * 下巴）。两个原因叠在一起：
                     *   1. 旧羽化 feather=0.705 是「平台半径」——内圈 70% 恒为满
                     *      增益、外圈 30% 才落下去。平台→斜坡的拐点二阶不连续，
                     *      人眼把它看成一道圆边（马赫带）。这不是强度问题，
                     *      调小滑块只会把圈调淡，不会调没。
                     *   2. 8 个椭圆彼此不重叠，各是一个孤立斑点；斑点越小越像
                     *      「打光斑」而不是「脸有立体感」。
                     *
                     * 改法：dome=1（fall = 1 − smoothStep(0,1,d)，中心 1、边缘 0、
                     * 两端导数为 0），同时把亮部椭圆放大到**互相重叠**：额带下沿
                     * 压住眉弓、鼻梁竖条穿过额带、左右颧带向外下铺开并与鼻梁相连，
                     * 重叠区的平台被邻居填平，边界自然消失。
                     *
                     * 量化（frame_1280x720，脸内 dY）：
                     *   旧：均值 +0.672  p95 7.60  峰 +12.2/−6.7  max|∇²| 1.89
                     *   新：均值 +0.677  p95 5.59  峰 +8.6/−6.2   max|∇²| 0.13
                     * 总量不变，二阶突变降 14 倍 —— 这一项才是「看得见圈」的来源。
                     * 实拍 720p 大脸（face_front_720p）同向：∇² 0.90 → 0.42。
                     */
                    // 额头：宽软的一条带，往下压住眉弓、两侧盖到太阳穴
                    els.push_back({midX - 0.005f, midY - 0.200f, 0.470f,
                                   0.200f, 0.0f, g * 9.300f, sh, 0.0f, 1.0f});
                    // 眉弓 / 眼窝：宽软凹陷（旧版 −2.15 又窄又硬，会显出一道暗箍）
                    els.push_back({midX - 0.020f, midY - 0.045f, 0.340f,
                                   0.115f, 0.0f, g * -4.650f, sh, 0.0f, 1.0f});
                    // 鼻梁：竖条，上端穿进额带、下端到鼻头，把上下连成一片
                    els.push_back({midX - 0.005f, midY + 0.020f, 0.090f,
                                   0.245f, 0.0f, g * 6.900f, sh, 0.0f, 1.0f});
                    // 左 / 右颧骨：斜向宽带（沿颧弓外下方向），与鼻梁重叠。
                    // 旋转符号按解剖：屏幕左侧那条朝左下、右侧朝右下。
                    els.push_back({e0x - 0.030f, e0y + 0.125f, 0.235f,
                                   0.150f, -0.14f, g * 7.350f, sh, 0.0f, 1.0f});
                    els.push_back({e1x + 0.005f, e1y + 0.135f, 0.235f,
                                   0.150f, 0.14f, g * 7.350f, sh, 0.0f, 1.0f});
                    // 鼻底 / 上唇：横向软阴影（旧的 −7.25 一压就是一条硬带）
                    els.push_back({midX - 0.005f, midY + 0.190f, 0.265f,
                                   0.080f, 0.0f, g * -7.800f, sh, 0.0f, 1.0f});
                    // 下巴
                    els.push_back({mouthMidX - 0.010f, mouthMidY + 0.140f,
                                   0.160f, 0.100f, 0.0f, g * 9.600f,
                                   sh, 0.0f, 1.0f});
                    // 下颌 / 脖子：大面积压暗
                    els.push_back({midX + 0.050f, midY + 0.500f, 0.505f,
                                   0.185f, 0.0f, g * -6.300f, sh, 0.0f, 1.0f});
                }
                if (eye > 0.0f) {
                    const float g = g_cfg.regEyeGain * eye;
                    // 眼白提亮。shade 给 0.35：眼白本来就接近饱和，按亮度加权
                    // 之后高光不会继续被推（推过去就是一块死白，观感比不提更差）。
                    // rx 从 0.075 放到 0.10：参考实现的亮眼 bbox 宽 0.65 脸宽
                    // （从内眼角一直盖到外眼角），0.075 只盖住瞳孔那一小块，
                    // 眼白提亮不完整，观感是「两个亮点」而不是「眼睛变亮」。
                    els.push_back({e0x, e0y, 0.100f, 0.055f, 0.0f, g, 0.35f, 0.35f});
                    els.push_back({e1x, e1y, 0.100f, 0.055f, 0.0f, g, 0.35f, 0.35f});
                }
                if (pouch > 0.0f) {
                    // 3.1.22：去黑眼圈 = 双层椭圆（亮核 + 淡晕）。
                    //
                    // 实测：参照的两个眼袋与**暗度无关**（corr(原亮度,Δ) ≈ +0.1），
                    // 是纯几何区域 —— 所以用固定形状的椭圆是对的。
                    // 3.1.23：改成**斜向窄脊**（_pouch_map.py 实测）。
                    //
                    // 参照的响应不是水平椭圆带，而是一条斜向窄脊：
                    // m00 眼R 从 (x=0.66,y=0.43) 斜向右下到 (x=0.74,y=0.50)，
                    // 宽约 12 px、长约 62 px、方向约 41°。
                    // 椭圆能表达这条脊：rx = 脊长（脸宽单位）、ry = 脊宽（脸高单位）、
                    // rot = 脊方向。注意 rx 按 faceW 缩放、ry 按 faceH 缩放。
                    //
                    //  · 核   ：rx 0.0950 / ry 0.0300，增益 10.25，平台窄
                    //  · 晕   ：rx 0.2050 / ry 0.0600，增益 3.00，铺开
                    //  · 位置 ：眼下方 0.064 脸高
                    //  · 转头时近侧眼袋在画面上被拉宽 → rx *= 1 − yawK·yaw·side
                    //    （yaw = 鼻尖相对眼中点的横向偏移 / 眼距）
                    const float g = g_cfg.regPouchScale * pouch;
                    const float eyeDist = e1x - e0x;
                    const float yaw = (eyeDist > 0.01f)
                                          ? (nsx - (e0x + e1x) * 0.5f) / eyeDist
                                          : 0.0f;
                    struct Side { float ex, ey, dx, dy, rot; int sgn; };
                    const Side sides[2] = {
                        {e0x, e0y, -0.0532f, 0.0638f, 0.0633f, -1},
                        {e1x, e1y, 0.0094f, 0.0666f, 0.5817f, +1},
                    };
                    for (int i = 0; i < 2; i++) {
                        const Side& s = sides[i];
                        float w = 1.0f - g_cfg.regPouchYawK * yaw * (float)s.sgn;
                        if (w < 0.25f) w = 0.25f;
                        const float r = s.rot * (float)s.sgn;
                        // 核（窄而强）
                        els.push_back({s.ex + s.dx, s.ey + s.dy,
                                       0.0750f * w, 0.0220f, r, g * 10.2500f,
                                       0.4000f, 0.3187f});
                        // 晕（宽而弱）
                        els.push_back({s.ex + s.dx, s.ey + s.dy,
                                       0.2050f * w, 0.0600f, r, g * 3.0000f,
                                       0.2000f, 0.4600f});
                    }
                }
                if (naso > 0.0f) {
                    // 3.1.22：去法令纹 = **暗度驱动**（不是固定形状 + 固定亮度）。
                    //
                    // 实测（_region_model.py / _naso_fill.py）：
                    //  · 参照的峰值像素 Δ **恒等于 +52**（封顶）
                    //  · corr(原亮度, Δ) = −0.37 ~ −0.44 → 越暗提得越多
                    //  · 输出值可以超过邻域最大值 → 不是「填到邻域均值」
                    // 所以模型是：Δ = min(maxLift, slope·max(0, LR − Y))，
                    // LR = 邻域最大亮度（半径 6）。区域用两个椭圆限定（哪条线
                    // 是法令纹由「暗线在哪」自己决定，脸一转也跟得上）。
                    //
                    // 拟合集 60.18，留出集 54.65。
                    //
                    // 3.1.23：模型升级（8 帧统一拟合）
                    //  1. 区域改成**窄椭圆**，锚在「眼正下方 0.13 脸高」——
                    //     实测参照的法令纹响应就在这个位置（沿眼下方竖线扫描，
                    //     峰值 Δ 恒为 51~52，且只有 5~8 px 宽）。
                    //  2. 限制因子从「皮肤掩码」改成 max(皮肤掩码, 区域权重)：
                    //     皮肤掩码是肤色椭圆，褶皱越深掩码越低，引擎恰好在
                    //     最该提亮的线上把效果掐掉（corr(掩码,参照Δ) = −0.66）。
                    //  3. 加**暗线闸门** gate(d)，把响应宽度收到褶皱线上。
                    //
                    // 3.1.23 第二轮：改成**双瓣**模型（naso6，8 帧统一拟合）
                    //
                    // 依据（_naso_line.py，沿眼下方竖线扫描的峰值剖面，m00 眼R）：
                    //     dy(眼下方脸高)  参照 Δ   旧模型(naso5) Δ
                    //          0.13          11.7        23.9
                    //          0.15          18.3        60.0（封顶，过冲）
                    //          0.17          48.0（峰）  60.0（封顶）
                    //          0.19           5.2        14.0
                    //          0.23           6.7         3.8
                    //          0.25           8.0         0    ← 下瓣从这开始
                    //          0.31          12.0         0    ← 下瓣自己的峰
                    //          0.45           0.7         0
                    // 参照的响应有**两个瓣**：
                    //   · 上瓣 = 真正的褶皱线（窄、强，峰在眼下方 0.17 脸高）
                    //   · 下瓣 = 褶皱朝嘴角延伸的那一段（宽、弱，dy 0.25~0.45）
                    // 旧模型只有上瓣，而且上瓣还过冲到封顶 60（参照峰才 48）。
                    //
                    // 下瓣的幅度只有 3~12 灰阶，且和亮度无关（那儿并不暗），
                    // 所以**不能**用暗度驱动表达 —— 改用**常量增益椭圆**
                    // （和五官立体同一套原语）。下瓣在 applyFoldRemoval **之后**
                    // 叠加，避免影响上瓣的邻域最大 LR 计算。
                    const float g = naso;
                    const float slope = g_cfg.regNasoSlope * g;
                    struct Side { float ex, ey, dx, dy, rot; };
                    const Side sides[2] = {
                        //   dx      dy      rot      rx     ry
                        {e0x, e0y, -0.0085f, 0.1496f, 0.2766f},
                        {e1x, e1y, -0.0295f, 0.1148f, 0.3266f},
                    };
                    for (int i = 0; i < 2; i++) {
                        const Side& s = sides[i];
                        nasoEls.push_back({s.ex + s.dx, s.ey + s.dy, 0.0800f,
                                           0.1500f, s.rot, 0.0f, 0.0f, 0.4000f});
                    }
                    // 下瓣：常量增益椭圆（fit_naso6.json，8 帧统一拟合）
                    struct Tail { float ex, ey, dx, rot; int sgn; };
                    const Tail tails[2] = {
                        {e0x, e0y, -0.0631f, 0.2294f, -1},
                        {e1x, e1y, -0.0332f, -0.2007f, +1},
                    };
                    const float eyeDist = e1x - e0x;
                    const float yaw = (eyeDist > 0.01f)
                                          ? (nsx - (e0x + e1x) * 0.5f) / eyeDist
                                          : 0.0f;
                    for (int i = 0; i < 2; i++) {
                        const Tail& t = tails[i];
                        float w = 1.0f - 1.9508f * yaw * (float)t.sgn;
                        if (w < 0.25f) w = 0.25f;
                        nasoTailEls.push_back({t.ex + t.dx, t.ey + 0.2843f,
                                               0.1000f * w, 0.1200f,
                                               t.rot * (float)t.sgn,
                                               g * 4.0000f, 0.15f, 0.5500f});
                    }
                    nasoSlope = slope;
                    if (geoOut) {
                        geoOut->nasoMaxLift = g_cfg.regNasoMaxLift;
                        geoOut->nasoLrRadius = g_cfg.regNasoLrRadius;
                        geoOut->nasoGateLo = g_cfg.regNasoGateLo;
                        geoOut->nasoGateHi = g_cfg.regNasoGateHi;
                    }
                }
            }

            // 4d：把这张脸的三组椭圆记进快照 —— 顺序与下面施加的顺序**完全一致**
            // （主椭圆 → 法令纹上瓣 → 下瓣），因为 GPU 侧要照这个顺序串三趟。
            if (geoOut && geoOut->faceCount < 8) {
                BeautyRegionGeom::Face& gf = geoOut->faces[geoOut->faceCount];
                gf.fx = fx; gf.fy = fy; gf.fw = fw; gf.fh = fh;
                gf.nasoSlope = nasoSlope;

                gf.elsBeg = geoOut->ellipseCount;
                gf.elsCount = 0;
                for (size_t i = 0; i < els.size() &&
                                   geoOut->ellipseCount < 128; i++) {
                    const beautycore::Ellipse& e = els[i];
                    BeautyRegionEllipse& o =
                        geoOut->els[geoOut->ellipseCount++];
                    o.cx = e.cx; o.cy = e.cy; o.rx = e.rx; o.ry = e.ry;
                    o.rot = e.rot; o.gain = e.gain;
                    o.shade = e.shade; o.feather = e.feather;
                    o.dome = e.dome;
                    gf.elsCount++;
                }
                gf.nasoBeg = geoOut->ellipseCount;
                gf.nasoCount = 0;
                for (size_t i = 0; i < nasoEls.size() &&
                                   geoOut->ellipseCount < 128; i++) {
                    const beautycore::Ellipse& e = nasoEls[i];
                    BeautyRegionEllipse& o =
                        geoOut->els[geoOut->ellipseCount++];
                    o.cx = e.cx; o.cy = e.cy; o.rx = e.rx; o.ry = e.ry;
                    o.rot = e.rot; o.gain = e.gain;
                    o.shade = e.shade; o.feather = e.feather;
                    o.dome = e.dome;
                    gf.nasoCount++;
                }
                gf.tailBeg = geoOut->ellipseCount;
                gf.tailCount = 0;
                for (size_t i = 0; i < nasoTailEls.size() &&
                                   geoOut->ellipseCount < 128; i++) {
                    const beautycore::Ellipse& e = nasoTailEls[i];
                    BeautyRegionEllipse& o =
                        geoOut->els[geoOut->ellipseCount++];
                    o.cx = e.cx; o.cy = e.cy; o.rx = e.rx; o.ry = e.ry;
                    o.rot = e.rot; o.gain = e.gain;
                    o.shade = e.shade; o.feather = e.feather;
                    o.dome = e.dome;
                    gf.tailCount++;
                }
                geoOut->faceCount++;
            }

            // 只收集模式（dst == nullptr）：这一帧一个像素都不改，直接下一张脸。
            if (!dst) {
                els.clear();
                nasoEls.clear();
                nasoTailEls.clear();
                continue;
            }

            if (!els.empty()) {
                /* 调参口：把这一帧真正交给 applyEllipses 的椭圆表按行导出，
                 * 用来在 Python 里逐式重放（形状/羽化怎么改，离线几秒钟一轮，
                 * 不用每次重编 + 跑整条链）。只在设了 BEAUTY_REGION_DUMP 时写。 */
                if (const char *dp = getenv("BEAUTY_REGION_DUMP")) {
                    if (*dp) {
                        static FILE *dfp = nullptr;
                        static int dumped = 0;
                        if (!dfp) dfp = fopen(dp, "wb");
                        if (dfp && dumped < 8) {
                            fprintf(dfp, "FACE %.4f %.4f %.4f %.4f N %d\n",
                                    fx, fy, fw, fh, (int)els.size());
                            for (size_t i = 0; i < els.size(); i++) {
                                const beautycore::Ellipse &e = els[i];
                                fprintf(dfp,
                                        "E %.6f %.6f %.6f %.6f %.4f %.4f %.4f %.4f\n",
                                        e.cx, e.cy, e.rx, e.ry, e.rot,
                                        e.gain, e.shade, e.feather);
                            }
                            fflush(dfp);
                            dumped++;
                        }
                    }
                }
                // 每个椭圆坐标都是**相对这张脸的归一化值**，所以这里传脸框，
                // 一次调用把这五项贡献一起加掉（增益相加，互不干扰）。
                beautycore::applyEllipses(dst, w, h, mk, fx, fy, fw, fh,
                                          &els[0], (int)els.size(), g_cfg.threads);
                els.clear();
            }
            if (!nasoEls.empty()) {
                // 法令纹：Δ = min(maxLift, slope·max(0, LR − Y)) × 区域权重
                beautycore::applyFoldRemoval(dst, w, h, mk, fx, fy, fw, fh,
                                             &nasoEls[0], (int)nasoEls.size(),
                                             nasoSlope, g_cfg.regNasoMaxLift,
                                             g_cfg.regNasoLrRadius,
                                             g_cfg.regNasoGateLo,
                                             g_cfg.regNasoGateHi,
                                             g_cfg.threads);
                nasoEls.clear();
            }
            if (!nasoTailEls.empty()) {
                // 法令纹**下瓣**：常量增益椭圆（参照的褶皱朝嘴角延伸的那一段，
                // 幅度 3~12 灰阶，与亮度无关）。必须在 applyFoldRemoval 之后，
                // 否则上瓣的邻域最大 LR 会被下瓣抬起来。
                beautycore::applyEllipses(dst, w, h, mk, fx, fy, fw, fh,
                                          &nasoTailEls[0],
                                          (int)nasoTailEls.size(),
                                          g_cfg.threads);
                nasoTailEls.clear();
            }
        }

        // 4d：祛斑参数也进快照（它与单张脸无关，作用于 bounds_）。
        if (geoOut && spot > 0.0f && !bounds_.empty()) {
            geoOut->spotOn = 1;
            geoOut->spotK = g_cfg.regSpotK * spot;
            geoOut->spotRadius = g_cfg.regSpotRadius;
            geoOut->spotThrLo = g_cfg.regSpotThrLo;
            geoOut->spotThrHi = g_cfg.regSpotThrHi;
            geoOut->roiX0 = bounds_.x0;
            geoOut->roiY0 = bounds_.y0;
            geoOut->roiX1 = bounds_.x1;
            geoOut->roiY1 = bounds_.y1;
        }
        if (dst && spot > 0.0f && !bounds_.empty()) {
            /* 2.9.0（P0-8）：geomOnly 帧传 nullptr = 不做掩码门控。
             * 原因：applySpotFix 的主循环是 `mw <= 0 → continue`，而 geomOnly
             * 帧的 mask_ 是残留/全零，于是「spot=1 已进链、算子逐像素全跳过」，
             * 画面一点变化都没有（实测：人造 26 灰阶斑点，maxdiff 仍为 0）。
             * ROI 本身来自脸框，dev 阈值 [thrLo,thrHi] 已经把细纹理与真边缘
             * 排除在外，所以 ROI 内全算是安全的。 */
            const unsigned char* smask = maskGeomOnlyFrame_ ? nullptr : mk;
            beautycore::applySpotFix(dst, w, h, smask, bounds_,
                                     g_cfg.regSpotK * spot, g_cfg.regSpotRadius,
                                     g_cfg.regSpotThrLo, g_cfg.regSpotThrHi,
                                     g_cfg.threads, spotScratch_);
        }
    }

    // 高低频参数在两个分支里各拼一次，抽出来避免两处漂移（以前就是改一处忘一处）。
    beautycore::FreqParams makeFreqParams() const {
        beautycore::FreqParams fp;
        fp.radius = g_cfg.radius;          // 沿用标定好的 r=2
        fp.iters = g_cfg.freqIters;        // 迭代次数决定低频尺度
        fp.sigmaSpace = g_cfg.sigmaSpace;
        fp.sigmaColor = g_cfg.sigmaColor;
        fp.magLo = g_cfg.freqMagLo;
        fp.magHi = g_cfg.freqMagHi;
        fp.grain = g_cfg.freqGrain;
        fp.strength = 100;                 // 面板档位由 blendWithMask 按 t 缩放
        fp.lowMode = g_cfg.freqLowMode;
        fp.guideRadius = g_cfg.guideRadius;
        fp.guideEps = g_cfg.guideEps;
        fp.lowPath = g_cfg.lowPath;
        fp.refRadius = g_cfg.refRadius;
        fp.boxRadius = g_cfg.boxRadius;
        fp.boxIters = g_cfg.boxIters;
        return fp;
    }
};

// 选择实现。只解析一次环境变量。
//
// 3.1.7 起**默认是 core**（自研引擎）。离线基准 DLL 里可用 BEAUTY_ENGINE_IMPL=legacy 切回旧链路。
// 为什么现在敢切：
//   · 面板 11/11 项已覆盖（3.1.2 起）；
//   · 真机 3.1.5 日志确认 engine=core/region=on/faces=1（3.1.4 修好的人脸模型加载）；
//   · 最后一个已知画质缺口（锐化放大的噪点闪烁）在 3.1.6 用时序 EMA 补掉。
//
// 未知值（比如写错成 "cor"）当作「没设置」处理，走默认。为什么不静默退回旧链路：
// 那样写错一个字母会悄悄跑回旧引擎，用户以为自己在测新引擎。
// 写错是可见的 —— init OK 那行会打出 engine=core/0.7.2，一眼能看出来。
// ---------------------------------------------------------------------------
// GpuEngine（方案 A：GPU 主路径的引擎壳）
//
// Phase 1：本类是一个**直通壳** —— 内部持有一个 CoreEngine，所有方法转发给它，
// 因此 `BEAUTY_ENGINE_IMPL=gpu` 在当前阶段与 `core` 逐字节等价（零回归），
// 只用于验证「引擎选择开关」真的接上了、且 `supportsGpu()` 标记正确。
//
// Phase 2 起：process() 改为在 D3D11 着色器上直接对 OBS 的 in_tex/out_tex 计算，
// 不再需要 obs-beauty.cpp 做「下载 → process → 上传」往返。届时本类接管
// gs_get_device() 拿到的 D3D11 设备，并管理着色器 / 常量缓冲 / 中间纹理；
// 内部 CoreEngine 退化为「CPU 兜底 + 逐像素 golden 对拍基准」（被本类按需调用，
// 不再由 obs-beauty.cpp 直接驱动）。
// ---------------------------------------------------------------------------
class GpuEngine : public IBeautyEngine {
public:
    GpuEngine() : core_(new CoreEngine()) {}
    const char* id() const override { return "gpu"; }
    const char* version() const override { return "2.0.0"; }   // 大版本 +1（GPU 主路径）

    bool ensureReady(std::string& err) override { return core_->ensureReady(err); }
    bool preheat(std::string& err, bool& didInit) override {
        return core_->preheat(err, didInit);
    }
    bool ready() const override { return core_->ready(); }

    int  acquireSlot(std::string& err) override { return core_->acquireSlot(err); }
    void releaseSlot(int slot) override { core_->releaseSlot(slot); }
    void setDetectOptions(int rotate, int lq, int mf) override {
        core_->setDetectOptions(rotate, lq, mf);
    }

    bool process(int slot, const unsigned char* src, unsigned char* dst,
                 int w, int h, const BeautySettings& s,
                 bool advanceFrame = true) override {
        // Phase 2+：此处改为 D3D11 着色器直算（in_tex -> out_tex），obs-beauty.cpp
        // 不再做下载/上传往返。当前 Phase 1 为直通：转发内部 CoreEngine（纯 CPU），
        // 行为与 `core` 完全一致，用于验证选择开关与 supportsGpu() 标记。
        return core_->process(slot, src, dst, w, h, s, advanceFrame);
    }

    int           lastFaces() const override { return core_->lastFaces(); }
    // 2.15.0：不转发的话，BEAUTY_GPU=1 下宿主会永远读到 0 —— 而 0 恰好是
    // 「自动退回不旋转」的语义，宿主会把缩略图路径永久停掉（画面正常、白慢一截）。
    int           effectiveRot() const override { return core_->effectiveRot(); }
    bool          regionActive() const override { return core_->regionActive(); }
    double        lastCoreMs() const override { return core_->lastCoreMs(); }
    void          lastSegMs(double* out6) const override { core_->lastSegMs(out6); }
    const char*   lastErr() const override { return core_->lastErr(); }
    int           itemId() const override { return core_->itemId(); }
    unsigned long ownerThread() const override { return core_->ownerThread(); }

    bool supportsGpu() const override { return true; }   // 标记：走 GPU 渲染路径

    // 2.11.0（方案C）：提交预判必须落到内部 CoreEngine 上 —— 节奏（maskAge_）
    // 与运动历史都在那里，包装层自己判会两套状态各走一遍。
    bool detectDue() override { return core_->detectDue(); }

    // 诊断透传：GPU 色调自检要拿「当前帧掩码」，而 CoreEngine 才是掩码的持有者。
    // 少了这一层，beautyCoreCopyMask() 在 BEAUTY_GPU=1 下会永远返回 false
    // （beautyCoreComputeMask 现有的 dynamic_cast 就是这样在 gpu 引擎下失效的）。
    bool copyMaskTo(unsigned char* out, int w, int h) override {
        return core_->copyMaskTo(out, w, h);
    }

    // 诊断透传：掩码覆盖率/人脸数的持有者是内部 CoreEngine（Phase 5 全链路上
    // GPU 后，掩码仍由 CoreEngine 的人脸检测产生）。不转发的话，BEAUTY_GPU=1 时
    // 性能日志会永远报「无掩码」，而画面明明是好的 —— 与 copyMaskTo 同一坑。
    void maskStat(int* pixelsOut, int* facesOut) const override {
        core_->maskStat(pixelsOut, facesOut);
    }

    // 3.1.48：同一个坑的第四次。包围盒统计也在 CoreEngine 里（检测仍由它做），
    // 不转发的话 BEAUTY_GPU=1 下日志会恒报「无包围盒」，等于白量。
    double boundsCover() const override { return core_->boundsCover(); }

    // Phase 3 磨皮自检用的两项透传。同一个坑的第三次复发：掩码与磨皮参数都存在
    // 内部 CoreEngine 上，不转发的话 BEAUTY_GPU=1 下自检会一直报「无掩码/无参数」，
    // 看起来像「着色器没输出」，实际是接口取不到东西。
    bool copyProtectTo(unsigned char* out, int w, int h) override {
        return core_->copyProtectTo(out, w, h);
    }
    bool bandDiag(BeautyBandDiag* out) const override {
        return core_->bandDiag(out);
    }

    void release() override { core_->release(); }

private:
    CoreEngine* core_;   // 拥有；与单例「永不析构」约定一致，不释放
};

IBeautyEngine* createEngine() {
    const char* v = getenv("BEAUTY_ENGINE_IMPL");
    if (v && v[0]) {
#ifdef BEAUTY_WITH_LEGACY_ENGINE
        if (strcmp(v, "legacy") == 0) return new LegacyAdapter();
#endif
        if (strcmp(v, "passthrough") == 0) return new PassthroughEngine();
        if (strcmp(v, "core") == 0) return new CoreEngine();
        if (strcmp(v, "gpu") == 0) return new GpuEngine();   // 方案 A：GPU 主路径（Phase 1 直通壳）
    }
    return new CoreEngine();
}

}  // namespace

// 5b-2 切片 4：宿主帧号入口。同样必须定义在匿名命名空间**外面**，否则
// obs-beauty.cpp 链接不到（与 beautyCoreSetSmoothParams / 皮肤快照同一规则）。
void beautyCoreSetFrameTag(unsigned long long tag) { g_frameTag = tag; }

// 2.6.0（S4a）：宿主声明「有 GPU 掩码链」。打开后引擎在单脸且 shape 不含 bit2 时
// 只算几何、跳过全帧 CPU 掩码的逐像素绘制；条件不满足仍照常画（安全兜底）。
//
// 环境变量在这里判、而不是放在 init 里的原因：宿主可能在 acquireSlot()（会触发
// init）之后才拿到 supportsGpu()，那时 init 早已跑完，init 里的判断会被这次调用
// 覆盖掉。放在这里 = 无论谁先谁后，`BEAUTY_MASK_GEOM_ONLY=0` 永远能关掉。
void beautyCoreSetMaskGeomOnly(bool on) {
    const char *mgo = getenv("BEAUTY_MASK_GEOM_ONLY");
    g_maskGeomOnly = on && !(mgo && mgo[0] == '0');
}

// 2.7.0（S4b）：本帧的检测输入换成一 GPU 预缩放的缩略图。
// 传 nullptr（或 tw/th/fullW/fullH 任一非正）即关闭，回到「用全帧 src」。
// 每帧都要设：宿主在 render 里算完缩略图就设，帧末清掉 —— 绝不能跨帧残留，
// 否则某帧回退全帧路径时引擎仍会去读上一帧的缩略图，几何直接错一帧。
void beautyCoreSetThumbSource(const unsigned char *p, int tw, int th,
                              int fullW, int fullH, float scale,
                              int ox, int oy) {
    if (!p || tw <= 0 || th <= 0 || fullW <= 0 || fullH <= 0 ||
        !(scale > 0.0f)) {
        beautyCoreClearThumbSource();
        return;
    }
    g_thumbPx = p;
    g_thumbW = tw;
    g_thumbH = th;
    g_thumbFullW = fullW;
    g_thumbFullH = fullH;
    g_thumbScale = scale;
    g_thumbOx = ox;
    g_thumbOy = oy;
}

void beautyCoreClearThumbSource() {
    g_thumbPx = nullptr;
    g_thumbW = g_thumbH = 0;
    g_thumbFullW = g_thumbFullH = 0;
    g_thumbScale = 0.0f;
    g_thumbOx = g_thumbOy = 0;
}

// 2.11.0（方案C）：运动判定的小图（RGBA8，stride = w*4）。
// 与缩略图源同一套生命周期约定：每帧设、用完清。传 nullptr 即回到点采样。
void beautyCoreSetMotionImage(const unsigned char *rgba64, int w, int h)
{
    // 上限 64×64：引擎里逐像素的上一帧缓存就那么大（motPrevFull_）。
    // 超过就退回点采样，不动态扩容 —— 这条路只是一次节流实验，不值得为它
    // 引入一块可变的缓冲。
    if (!rgba64 || w < 16 || h < 16 || w > 64 || h > 64) {
        beautyCoreClearMotionImage();
        return;
    }
    g_motPx = rgba64;
    g_motW = w;
    g_motH = h;
}

void beautyCoreClearMotionImage()
{
    g_motPx = nullptr;
    g_motW = g_motH = 0;
    /* ⚠ 这里**不能**清 g_duePending：宿主是「先问预判、再清小图、最后 process()」
     * 的顺序，清了就等于把刚定好的结论扔掉，process() 会再自行判一次 ——
     * maskAge_ 被推进两次、运动历史被同一帧更新两次。
     * 预判决只有两个出口：process() 内部的 updateMask 消费掉，或下一次
     * beautyCoreDetectDue() 覆盖。 */
}

bool beautyCoreDetectDue()
{
    return beautyEngine().detectDue();
}

void beautyCoreSetNoPixelFrame(bool on)
{
    g_noPixFrame = on;
}

// 皮肤判据快照（C20，只用于日志）。定义放在匿名命名空间外面 —— 否则外部
// （obs-beauty.cpp）链接不到，与上面 beautyCoreSetSmoothParams 同一规则。
// 内部的 g_skin* 仍在匿名空间里，同一 TU 内可见。
const char* beautyCoreSkinInfo() {
    static char buf[192];
    snprintf(buf, sizeof(buf),
             "skin=%s cb=%+.4f cr=%+.4f r=%.4f/%.4f ema=%.2f%s",
             g_skinAdapt ? "adapt" : "fixed",
             g_skinCb, g_skinCr, g_skinRu, g_skinRv, g_skinAdaptEma,
             g_skinAdaptValid ? "" : " (center=default, not yet estimated)");
    return buf;
}

// 估计首次收效后返回 true（并清标志），渲染线程按它决定要不要补打一行
// skin= —— 与 beautyCoreSkinInfo() 连用，两步之间没有别的线程会再改 g_skin*。
// 注意：g_skinAdaptValid 一旦置位整个会话不会再翻回 false（估计连续失败也
// 只沿用上一帧中心），所以这个标志整个生命周期至多被置位一次，取走即完。
bool beautyCoreSkinAnnounce() {
    return g_skinAnnounce.exchange(false, std::memory_order_relaxed);
}

// 自研核参数（离线调参入口）。定义必须放在匿名命名空间**外面**，否则外部
// （core_probe.cpp）链接不到；g_cfg 本身仍在匿名空间内，只给 CoreEngine 用。
void beautyCoreSetSmoothParams(int radius, float sigmaSpace,
                               float sigmaColor, int down) {
    g_cfg.radius = radius;
    g_cfg.sigmaSpace = sigmaSpace;
    g_cfg.sigmaColor = sigmaColor;
    g_cfg.down = (down < 1) ? 1 : down;
}

// 性能相关开关（离线调参 / 对照实验入口）。定义同样要放在匿名命名空间外面。
//   threads <= 0 自动；1 强制单线程
//   useRoi  0 = 关掉 ROI 裁剪（只在测「全帧成本」时用，生产必须开）
void beautyCoreSetPerf(int threads, int useRoi) {
    g_cfg.threads = (threads < 1) ? 0 : threads;
    g_cfg.roi = (useRoi != 0);
}

// Sobel 边缘门控灵敏度（离线调参入口）。0 = 关闭。
void beautyCoreSetEdgeK(int k) {
    g_cfg.edgeK = (k < 0) ? 0 : k;
}

void beautyCoreSetClarity(float maxGain, int radius, float kneeLo, float kneeHi) {
    if (maxGain >= 0.0f) g_cfg.clarityMax = maxGain;
    if (radius > 0) g_cfg.clarityRadius = radius;
    if (kneeHi > kneeLo && kneeLo >= 0.0f) {
        g_cfg.clarityKneeLo = kneeLo;
        g_cfg.clarityKneeHi = kneeHi;
    }
}

void beautyCoreSetLowPath(int path, int refRadius) {
    g_cfg.lowPath = (path < 0) ? 0 : (path > 3 ? 3 : path);
    if (refRadius >= 0) g_cfg.refRadius = refRadius;
}

void beautyCoreSetGuide(int mode, int radius, float eps) {
    g_cfg.freqLowMode = (mode != 0) ? 1 : 0;
    if (radius > 0) g_cfg.guideRadius = radius;
    if (eps > 0.0f) g_cfg.guideEps = eps;
}

void beautyCoreSetFreqGrain(float g) {
    g_cfg.freqGrain = (g < 0.0f) ? 0.0f : ((g > 1.0f) ? 1.0f : g);
}

void beautyCoreSetFreq(int mode, float sigma, float magLo, float magHi) {
    g_cfg.freqMode = (mode != 0) ? 1 : 0;
    // sigma 参数在这里复用为「迭代次数」（1..6），保持导出签名不变
    if (sigma >= 1.0f) {
        g_cfg.freqIters = (int)(sigma + 0.5f);
        if (g_cfg.freqIters < 1) g_cfg.freqIters = 1;
        if (g_cfg.freqIters > 8) g_cfg.freqIters = 8;
    }
    if (magHi > magLo) {
        g_cfg.freqMagLo = magLo;
        g_cfg.freqMagHi = magHi;
    }
}

void beautyCoreSetLimitThr(int thr) {
    g_cfg.limitThr = (thr < 0) ? 0 : thr;
}

void beautyCoreSetProtectK(int k) {
    if (k < 0) k = 0;
    if (k > 100) k = 100;
    g_cfg.protectK = k;
}

// 掩码导出（离线对拍用）。同样必须放在匿名命名空间外面。
bool beautyCoreComputeMask(const unsigned char* src, int w, int h,
                           unsigned char* outMask) {
    if (!src || !outMask || w <= 0 || h <= 0) return false;
    CoreEngine* ce = dynamic_cast<CoreEngine*>(&beautyEngine());
    if (!ce) return false;
    return ce->computeMaskForDump(src, w, h, outMask);
}

// 掩码只读拷贝（GPU 色调 Phase 2 诊断用）。走接口而不是 dynamic_cast ——
// BEAUTY_GPU=1 时 beautyEngine() 是 GpuEngine，dynamic_cast<CoreEngine*> 会失败，
// 那正是 beautyCoreComputeMask 在 gpu 引擎下静默返回 false 的原因。
bool beautyCoreCopyMask(unsigned char* outMask, int w, int h) {
    if (!outMask || w <= 0 || h <= 0) return false;
    return beautyEngine().copyMaskTo(outMask, w, h);
}

// CPU 端色调基准（GPU 着色器对拍用）。与 CoreEngine::process 里调用的是同一个
// toneColor()，参数也同尺度（面板 / 100）。刻意只跑色调：这样对拍的两边
// 「算什么」完全一致，差异只可能来自「怎么算」（CPU 循环 vs HLSL）。
bool beautyCoreToneCpu(const unsigned char* src, int w, int h,
                       const unsigned char* mask, float clarity, float whiten,
                       float rosy, unsigned char* rgbaOut) {
    if (!src || !mask || !rgbaOut || w <= 0 || h <= 0) return false;
    memcpy(rgbaOut, src, (size_t)w * h * 4);
    beautycore::RoiRect roi;
    roi.x1 = w;
    roi.y1 = h;
    beautycore::toneColor(rgbaOut, w, h, mask, roi, clarity, whiten, rosy, 1);
    return true;
}

// 2.15.0：本帧实际使用的输入方向（0/90/180/270）。见 beauty_core.h 的说明 ——
// 宿主要在构建检测缩略图**之前**读它，发现与面板值不一致就别再按错方向转。
int beautyCoreEffectiveRot() {
    return beautyEngine().effectiveRot();
}

// 2.15.1：掩码几何的**当前生效**配置，一行字符串，宿主启动时打一次。
// 存在的理由：shape/roll 都是环境变量可覆盖的，光看面板看不出「到底用的哪套掩码」；
// 用户报「能看到长方形遮罩」时，第一件事就是确认 shape 是多少。
const char* beautyCoreMaskConfig() {
    static char s[160];
    snprintf(s, sizeof(s),
             "shape=%d roll=%d feather=%d neck=%.2fx%.2f",
             g_maskShape, g_maskRoll ? 1 : 0, g_maskFeatherPx,
             (double)g_neck.halfW, (double)g_neck.len);
    return s;
}

IBeautyEngine& beautyEngine() {
    // 3.0.0：按当前线程绑定的 slot 惰性建引擎。slot 0 就是原来那个单例
    // （模块加载阶段的 preheat 已替它建好），单实例走的是完全相同的路径。
    // 对象刻意不析构，避免宿主 FreeLibrary 时静态析构与渲染线程赛跑。
    const int s = (t_slot >= 0 && t_slot < kMaxEngineSlots) ? t_slot : 0;
    IBeautyEngine* p = g_engineAtomic[s].load(std::memory_order_acquire);
    if (p) return *p;
    std::lock_guard<std::mutex> lk(g_engineMutex);
    p = g_engines[s];
    if (!p) {
        p = createEngine();
        g_engines[s] = p;
        g_engineAtomic[s].store(p, std::memory_order_release);
    }
    return *p;
}

// ---------------------------------------------------------------------------
// 3.0.0 多实例槽位 API —— 宿主 obs-beauty.cpp 的唯一入口
// ---------------------------------------------------------------------------

// 每次进入宿主回调都要重新绑：OBS 的 create / update / render / destroy 可能
// 不在同一线程，t_slot 是 thread_local，不绑就退回 0（= 别人的引擎）。
void beautyCoreBindSlot(int slot) {
    t_slot = (slot >= 0 && slot < kMaxEngineSlots) ? slot : 0;
}

void beautyCoreReleaseSlot(int slot) {
    if (slot < 0 || slot >= kMaxEngineSlots) return;
    // 3.0.1：owned 判定 / 停 worker / 清空 SlotState 收敛到同一把锁下。
    // 旧实现在锁外整块赋值，和该槽 worker 正在读写的帧号 / 肤色 EMA /
    // 掩码轨迹构成数据竞争；「删一路马上再建一路」时还可能抹掉新实例刚
    // 写进去的钩子状态。releaseSlot() 会 join worker，之后本函数就是唯一
    // 读写这份 SlotState 的人。
    std::lock_guard<std::mutex> lk(g_slotMutex);
    if (!g_slotOwned[slot]) return;
    g_slotOwned[slot] = false;
    IBeautyEngine* p = g_engineAtomic[slot].load(std::memory_order_acquire);
    if (p) p->releaseSlot(slot);   // CoreEngine：停 worker，不卸模型、不碰 t_slot
    g_slots[slot] = SlotState{};
    // 引擎对象**不析构**：槽复用时直接续用已加载的模型，省掉一次
    // ~2 秒预热；进程退出时由 OS 回收（与既有「永不析构」约定一致）。
}

// 占一个槽 → 绑定当前线程 → 惰性建该槽引擎 → ensureReady（加载 YuNet）。
// 返回值就是宿主的 f->slot；失败返回 -1 并把原因写进 err。
int beautyCoreAcquireSlot(std::string& err) {
    err.clear();
    int s = -1;
    {
        std::lock_guard<std::mutex> lk(g_slotMutex);
        for (int i = 0; i < kMaxEngineSlots; ++i) {
            if (!g_slotOwned[i]) {
                g_slotOwned[i] = true;
                // 3.0.1：与 release 的清空同锁，杜绝「释放中的旧实例把
                // 刚接管本槽的新实例状态抹掉」的复用竞态。
                g_slots[i] = SlotState{};   // 清掉上个实例的钩子 / EMA / 帧号
                s = i;
                break;
            }
        }
    }
    if (s < 0) {
        err = "engine slots exhausted (max 8 beauty filters per process)";
        return -1;   // 注意：不 bind。宿主必须按 f->slot<0 走直通，不得再碰引擎。
    }
    beautyCoreBindSlot(s);
    IBeautyEngine& e = beautyEngine();
    if (e.acquireSlot(err) < 0) {    // CoreEngine 在这一步做 ensureReady
        beautyCoreReleaseSlot(s);
        return -1;
    }
    return s;
}

int beautyCoreActiveSlots() {
    std::lock_guard<std::mutex> lk(g_slotMutex);
    int n = 0;
    for (int i = 0; i < kMaxEngineSlots; ++i)
        if (g_slotOwned[i]) ++n;
    return n;
}

// 模块卸载：逐个 release 所有曾经建过的引擎。旧实现只 release 当前 t_slot
// 指向的那一个，多实例下其余引擎的异步 worker 会一直活到进程退出。
void beautyCoreReleaseAllEngines() {
    for (int i = 0; i < kMaxEngineSlots; ++i) {
        IBeautyEngine* p = g_engineAtomic[i].load(std::memory_order_acquire);
        if (!p) continue;
        t_slot = i;                 // release() 内部也会碰按槽状态
        p->release();
    }
    t_slot = 0;
}

const char* beautyEngineId() {
    return beautyEngine().id();
}

int beautyCoreMaskCadence() {
    return g_cfg.maskEveryAsync;
}

// 2.8.0（S7）：当前生效的提交周期（1 = 每帧）。
// 与 beautyCoreMaskCadence 的区别：那个是**配置值**（固定周期），这个是**本帧
// 实际用的值**。宿主按窗口取最大值，静止段应看到 detAdaptStill、运动段回到 1。
int beautyCoreMaskPeriod() {
    return g_detPeriod;
}

// 最近一帧的网格平均亮度差；-1 表示自适应关着、本窗口没做判定。
int beautyCoreMaskMotion() {
    return g_detDiff;
}

// 2.11.1：自上次 Reset 以来「判成在动」的次数 / 判定次数。
// 取值即清零 —— 与宿主的窗口统计同频，避免窗口边界对齐问题。
void beautyCoreMotionStat(int* hotOut, int* callsOut) {
    if (hotOut)
        *hotOut = g_motHot;
    if (callsOut)
        *callsOut = g_motCalls;
    g_motHot = 0;
    g_motCalls = 0;
}

uint64_t beautyCoreMaskSubmits() {
    return g_maskSubmits;
}

// 2.12.0（P1-1c）：块聚合口径，1 = RMS / 0 = 取最大。
int beautyCoreMotionAggRms() {
    return g_cfg.detAdaptAggRms ? 1 : 0;
}

// 3.1.40：无脸透传帧数（诊断用，移植自 CPU 版 v3.1.39）。
uint64_t beautyCoreNoFaceFrames() {
    return g_noFaceFrames;
}

// 3.1.40：无脸透传开关状态（1 = 开 / 0 = 旧的全帧兜底行为）。
int beautyCoreNoFaceSkip() {
    return g_cfg.noFaceSkip ? 1 : 0;
}

uint64_t beautyCoreAlignUsed() {
    return g_alignUsed;
}

// 3.1.10 探针：一次调用带回 (对齐帧数, 处理总帧数)，供日志算比率。
int beautyCoreAlignStat(uint64_t* used, uint64_t* total) {
    if (used) *used = g_alignUsed;
    if (total) *total = g_alignTotal;
    return 0;
}

// 掩码诊断（3.1.44）：见头文件里的说明。走引擎虚函数，所以 gpu 实现也能拿到
// —— 掩码始终由 CoreEngine 的人脸检测产生，GpuEngine 只是转发。
void beautyCoreMaskStat(int* pixelsOut, int* facesOut) {
    beautyEngine().maskStat(pixelsOut, facesOut);
}

// ---- 3.1.48 ROI 包围盒占比 -------------------------------------------------
double beautyCoreBoundsCover() {
    return beautyEngine().boundsCover();
}

// ---- 3.1.47 分项计时 ------------------------------------------------------
void beautyCoreSegMs(double* out6) {
    beautyEngine().lastSegMs(out6);
}

const char* beautyCoreSegLabel(int i) {
    static const char* kNames[6] = {"掩码", "磨皮", "色调", "五官", "细节", "美形"};
    return (i >= 0 && i < 6) ? kNames[i] : "?";
}

// ---- Phase 3 磨皮诊断 ------------------------------------------------------

bool beautyCoreBandDiag(BeautyBandDiag* out) {
    if (!out) return false;
    return beautyEngine().bandDiag(out);
}

bool beautyCoreCopyProtect(unsigned char* outMask, int w, int h) {
    if (!outMask || w <= 0 || h <= 0) return false;
    return beautyEngine().copyProtectTo(outMask, w, h);
}

// CPU 端磨皮基准（GPU 着色器对拍用）。
//
// 三点刻意的选择：
//   1) threads 传 1 —— 多线程只是分块，每个像素的输出与单线程逐位相同，
//      但固定成 1 让「复现同一次对拍」不依赖机器核数。
//   2) ROI 传全帧 —— surface3RgbBand 目前只在入口检查 roi.empty()，之后根本
//      不读它；传全帧是「说明它确实不参与」，而不是暗示将来会参与。
//   3) scratch 每次现分配 —— 1080p 半分辨率约 33 MB，只在自检那一帧发生一次。
//      复用成员缓冲会让 CoreEngine 多背一份 33 MB 常驻内存，而生产路径根本
//      不调这个函数。
bool beautyCoreSmoothCpu(const unsigned char* src, unsigned char* dst, int w, int h,
                         const unsigned char* mask, const unsigned char* protect,
                         const BeautyBandDiag& p) {
    if (!src || !dst || w <= 0 || h <= 0) return false;

    beautycore::BandParams bp;
    bp.f1 = p.f1;
    bp.f2 = p.f2;
    bp.fa = p.fa;
    bp.passes = p.passes;
    bp.T = p.T;
    bp.gwp = p.gwp;
    bp.halfRes = p.halfRes;
    bp.I = p.I;
    bp.e = p.e;
    bp.protectK = p.protectK * 100.0f;   // 内部按 0..100 再 /100，这里还原回去

    beautycore::RoiRect roi;
    roi.x1 = w;
    roi.y1 = h;

    std::vector<float> scratch;
    beautycore::surface3RgbBand(src, dst, w, h, mask, protect, bp, roi, 1, scratch);
    return true;
}

// ---- T1：CPU 端细节基准（GPU 着色器对拍用）---------------------------------
//
// 与 beautyCoreSmoothCpu 同一套口径：threads=1、全帧 ROI、scratch 现分配
// （1080p 四个平面约 33 MB，只在自检那一帧发生一次）。
// EMA 关（DetailEma 默认 lumT=nullptr）—— 与 GPU 侧对拍帧的 emaMode=0 对齐，
// 两边都无状态，比的是细节算法本体（3/7 盒式 + 软阈值 + 回填 + 掩码语义）。
bool beautyCoreDetailCpu(const unsigned char* src, unsigned char* dst, int w, int h,
                         const unsigned char* mask, const BeautyDetailDiag& d) {
    if (!src || !dst || w <= 0 || h <= 0) return false;

    // dst 从 src 起：GPU 侧 image 也是原图，两边同起点（只比算子本身）。
    memcpy(dst, src, (size_t)w * h * 4);

    beautycore::DetailParams p;
    p.fine = d.fine;
    p.mid = d.mid;
    p.globalFine = d.globalFine;
    p.globalMid = d.globalMid;
    p.thrFine = d.thrFine;
    p.thrMid = d.thrMid;
    p.thrFineGlobal = d.thrFineGlobal;
    p.thrMidGlobal = d.thrMidGlobal;
    p.kneeFine = d.kneeFine;
    p.kneeMid = d.kneeMid;
    p.deltaRefill = d.deltaRefill != 0;

    beautycore::RoiRect roi;
    roi.x1 = w;
    roi.y1 = h;

    std::vector<float> scratch;
    beautycore::DetailEma ema;   // lumT=nullptr → 不做 EMA，与 GPU emaMode=0 一致
    beautycore::detailRefill(src, dst, w, h, mask, roi, p, 1, scratch, ema);
    return true;
}

// ---- 5c：CPU 端美形基准（GPU 着色器对拍用）---------------------------------
//
// 与 beautyCoreDetailCpu 同一套口径：dst 从 src 起，两边同起点，只比算子本身。
//
// 关键设计：几何**原样取自** wd（BeautyWarpDiag），不重算。
// 对拍要问的是「warp.hlsl 有没有正确实现这份几何」，不是「几何算得对不对」。
// 后者已经由离线黄金基准的 mixed 用例（美形三项各 10 档）钉住了 —— 那次
// 验证的是 warpFaceGeom 的算式，与这里正好互补：
//   · 离线台  → 引擎算出来的几何对不对（CPU 全链路）
//   · 这里    → 着色器照着同一份几何做得对不对（CPU vs GPU 同几何）
// 两件事混在一条判据里的话，出问题时分不清是「几何错了」还是「着色器错了」。
//
// 多脸串行、就地修改，与 warpFaceWithGeom 在生产路径上的调用方式完全一致
// （CPU 侧就是这么串的，见 process() 末端）。
bool beautyCoreWarpCpu(const unsigned char* src, unsigned char* dst, int w, int h,
                       const BeautyWarpDiag& wd) {
    if (!src || !dst || w <= 0 || h <= 0) return false;

    memcpy(dst, src, (size_t)w * h * 4);
    if (wd.count <= 0) return true;

    int n = wd.count;
    if (n > BEAUTY_WARP_MAX_FACES) n = BEAUTY_WARP_MAX_FACES;

    std::vector<unsigned char> scratch;
    for (int i = 0; i < n; i++) {
        const BeautyWarpFace& s = wd.faces[i];
        beautycore::WarpGeom g;
        g.cx = s.cx;
        g.cy = s.cy;
        g.halfW = s.halfW;
        g.eyeRx = s.eyeRx;
        g.eyeRy = s.eyeRy;
        g.eyeLx = s.eyeLx;
        g.eyeLy = s.eyeLy;
        g.eyeR = s.eyeR;
        g.eyeAmp = s.eyeAmp;
        g.thinYc = s.thinYc;
        g.thinSig = s.thinSig;
        g.thinK = s.thinK;
        g.chinYm = s.chinYm;
        g.chinSpan = s.chinSpan;
        g.chinK = s.chinK;
        g.chinKH = s.chinKH;
        g.roi[0] = s.roi[0];
        g.roi[1] = s.roi[1];
        g.roi[2] = s.roi[2];
        g.roi[3] = s.roi[3];
        g.flags = s.flags;
        beautycore::warpFaceWithGeom(dst, w, h, g, scratch);
    }
    return true;
}

// ---- 磨皮执行钩子 ---------------------------------------------------------
void beautyCoreSetSmoothHook(BeautySmoothHook fn, void* user) {
    g_smoothHook = fn;
    g_smoothUser = user;
}

// 命中率探针：hits/calls 是启动以来的累计值，插件层取差值算窗口比率。
// calls 只统计「引擎真的走到了 band 磨皮段」的帧，面板关掉磨皮时两者都不涨。
void beautyCoreSmoothHookStat(long long* hits, long long* calls) {
    if (hits)
        *hits = g_smoothHookHit;
    if (calls)
        *calls = g_smoothHookCall;
}

// ---- 段区间钩子（3.1.52 / Phase 5a）---------------------------------------
void beautyCoreSetChainHook(BeautyChainHook fn, void* user) {
    g_chainHook = fn;
    g_chainUser = user;
}

void beautyCoreChainStat(long long* hits, long long* calls,
                         unsigned long long* segCounts5) {
    if (hits)
        *hits = g_chainHit;
    if (calls)
        *calls = g_chainCall;
    if (segCounts5)
        for (int i = 0; i < 5; i++)
            segCounts5[i] = g_chainSeg[i];
}
