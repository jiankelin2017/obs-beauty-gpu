#pragma once
#include <vector>
// ---------------------------------------------------------------------------
// beauty_smooth —— 自研保边平滑核（不依赖任何参考实现库）
//
// 为什么自己写而不用 OpenCV：本项目的边界定在「开源可商用」就够，OpenCV
// (Apache-2.0) 本身合规。手写的真实理由是体积 —— 即使只裁 core+imgproc，
// 也要把十几 MB 的 DLL 重新塞回安装包，而我们要的只有双边滤波这一个函数。
//
// 参数是怎么定的：见 05_文档/S2_磨皮核对拍结论.md。离线对拍结论是
// cv2.bilateralFilter(d=9, sigmaColor=40) 能复现 参考实现的档位曲线（RMSE 1.16 个
// 百分点），所以默认参数按它的口径换算：
//     d = 9             -> radius = 4（(9-1)/2）
//     sigmaSpace = 13.5 （OpenCV 内部取 d*1.5）
//     sigmaColor = 40   -> 三通道 L1 颜色距离的高斯 sigma
//
// 性能策略（按收益排序，见 05_文档/C1_CPU性能优化.md）：
//   1) ROI 裁剪 —— 磨皮只作用于掩码区（约 7% 画面），全帧跑等于浪费 90% 算力。
//      逐像素独立计算，裁剪不改变结果，画质零损失。
//   2) 多线程按行切分 —— 行与行之间没有数据依赖，天然可并行。
//   3) SIMD / 降采样 —— 前者收益中等且代码复杂，后者已实测失败（曲线误差 27+）。
// ---------------------------------------------------------------------------

namespace beautycore {

// 半开区间 [x0,x1) × [y0,y1)。x1<=x0 或 y1<=y0 表示空矩形。
struct RoiRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    long long area() const {
        return empty() ? 0LL : (long long)(x1 - x0) * (y1 - y0);
    }
};

// 双边滤波。src / dst 都是 RGBA8（w*h*4 字节），允许 src == dst。
//   radius     空间半径（像素）。越大越慢（O(radius^2)）。
//   sigmaSpace 空间高斯 sigma
//   sigmaColor 颜色高斯 sigma（作用于三通道 L1 距离，0..765 尺度）
// alpha 通道原样保留（不参与滤波）。
//
// threads <= 0 表示自动（按硬件并发数，上限 8）。传 1 强制单线程（调参复现用）。
void bilateralRgba(const unsigned char* src, unsigned char* dst,
                   int w, int h, int radius,
                   float sigmaSpace, float sigmaColor, int threads = 0);

// ROI 版：只写 [roi] 内的像素，其余字节不动（调用方负责保证 ROI 外无意义）。
//
// 关键性质：ROI 内每个像素的结果与「整帧跑」逐字节相同 —— 邻域照常从原图
// src 读取，只在图像四边做 clamp。所以裁剪是纯粹的省算力，不引入画质损失。
// 前提：调用方必须保证 roi 覆盖所有会被用到的像素（mask 非零处 + radius 边距）。
void bilateralRgbaRoi(const unsigned char* src, unsigned char* dst,
                      int w, int h, int radius,
                      float sigmaSpace, float sigmaColor,
                      const RoiRect& roi, int threads = 0);

// 求掩码的包围盒，四周外扩 pad 像素后 clamp 到画面内。全零掩码返回空矩形。
// pad 至少要给 radius，否则 ROI 边缘像素的邻域会被切掉。
RoiRect maskBounds(const unsigned char* mask, int w, int h, int pad);

// 降采样版：先在 1/down 分辨率上做双边，再双线性放大回原尺寸。
//
// 已实测失败（档位曲线误差 27+）：降采样本身就是一层低通，叠加在双边之上会
// 把细节再抹掉一大截（100 档细节保留率 64%→21%），按比例缩 radius/sigma 补不
// 回来。保留这个函数只为复现当年的失败结论，生产路径不要用。
void bilateralRgbaDown(const unsigned char* src, unsigned char* dst,
                       int w, int h, int radius,
                       float sigmaSpace, float sigmaColor, int down);

// 档位混合：dst = src + (smooth - src) * t，t ∈ [0,1]。
// 参考实现的磨皮档位就是这条线性混合（见 legacy_engine.cpp 的 bgraToRgbaBlend）。
void mixRgba(const unsigned char* src, const unsigned char* smooth,
             unsigned char* dst, int w, int h, float t);

// 默认参数：离线对拍扫出来的最优组合（05_文档/S2_磨皮核对拍结论.md）
//   radius=2 / σSpace=7 / σColor=40 → 档位曲线误差 1.09 个百分点
// 注意半径小反而更贴合 参考实现：说明它的平滑核半径本来就不大。
enum { kSmoothRadius = 2 };
const float kSmoothSigmaSpace = 7.0f;
const float kSmoothSigmaColor = 40.0f;

// ---------------------------------------------------------------------------
// 改进双边（低频参照 + 浮点）—— 「只看局部起伏」的保边权重
//
// 与上面的 bilateralRgbaRoi 只有一个差别，就是权重判据：
//
//     普通双边：  diff = tap - center
//     改进双边：  diff = (tap - center) - (refTap - refCenter)
//
// ref 是一张大半径盒式模糊（O(1)/像素）。减掉它之后，权重只看**局部起伏**：
//   · 平缓的大幅明暗过渡（鼻梁侧、下颌线）：tap-center 与 refTap-refCenter 相等，
//     diff≈0 → 照常参与平均 → 不会在过渡带两侧各磨一遍、中间挤出一条硬边；
//   · 真边缘（发际线、眼镜框）：ref 在一两个像素内跟不上阶跃，diff 仍然很大
//     → 照旧不平均。
//
// 做法来自 dinjufen/lumina-beauty 的 kSkinBlurShader（核心代码为 MIT）注释：
// 「颜色差先减去大半径模糊的差，只看局部起伏…直接用颜色差时，过渡带两侧会被
//  各自抹平，中间挤出一条锯齿状的硬边」。
//
// 另一半改动是**中间结果用 float 存**（lumina 用 GL_RGBA16F）：原来每轮迭代
// 都把结果压回 uint8，4 轮下来必然积累平台/色带。
// ---------------------------------------------------------------------------
struct BilateralRefParams {
    int radius = kSmoothRadius;
    float sigmaSpace = kSmoothSigmaSpace;
    float sigmaColor = kSmoothSigmaColor;
    int iters = 2;
    // 参照的大半径（盒式）。<=0 表示不做参照 → 退回绝对差（离线对照用）。
    int refRadius = 8;
};

// 低频层的输出视图：R/G/B 三个平面，**局部坐标** (x - x0, y - y0)。
// 指向调用方传入的 scratch，生命周期由调用方保证。
struct LowFreqBuf {
    const float* r = nullptr;
    const float* g = nullptr;
    const float* b = nullptr;
    int x0 = 0, y0 = 0, bw = 0, bh = 0;
    bool valid() const { return r && g && b && bw > 0 && bh > 0; }
};

// 计算低频层（不改 dst，只返回视图）。
//
// 输出有效范围比 roi 大一圈（外扩 radius×iters + refRadius），因为参照与迭代
// 都要读到 ROI 外面的像素；调用方只在 roi 内取用，所以多算的那圈不可见。
LowFreqBuf lowFreqRefRoi(const unsigned char* src, int w, int h,
                         const BilateralRefParams& p, const RoiRect& roi,
                         int threads, std::vector<float>& scratch);

// ---------------------------------------------------------------------------
// 细节回填（锐化 / 全局锐化）—— 频率分离式回填
//
// 为什么不能「对磨完的图做 unsharp」：双边已经把高频抹掉，再对磨完的图取高通，
// 没有输入可放大。实测磨皮 75 时锐化 0→100 只能把细节能量拉回 +0.46，观感是糊。
// 所以高频层必须从**原图**取：
//     hFine = Y(src) − box3(Y(src))          1~2px：毛孔、睫毛、发丝
//     hMid  = box3(Y(src)) − box7(Y(src))    3~7px：轮廓过渡、皮肤结构
//     out  += w · (fine·hFine + mid·hMid)
// 磨皮决定「大尺度抹掉多少」，锐化决定「最细两段放回多少」，两个滑块解耦。
//
// 三条与参考实现版（legacy_engine.cpp 的 applySharpen）的差别，都是刻意的：
//   1) 只在**亮度**上取高通，增量等量加到 RGB 三通道 —— 不会因三通道独立增益
//      产生彩边；成本是参考实现版的 1/3。
//   2) 细频带带一个软阈值 thrFine：相机噪点与毛孔在幅值上重叠（±2~6 vs 2~8），
//      幅值分不开，只能靠软阈值削掉最细的一小段，避免 3.0.0 那版「脸部屏闪」。
//   3) 没有做时序 EMA（core 引擎目前无跨帧状态），所以噪点抑制不如参考实现版彻底。
//      这是已知取舍，写在 05_文档/C11_锐化与全局锐化.md。
//
// 权重按像素取：m = mask/255，
//     fine_eff = m·fine + (1−m)·globalFine
//     mid_eff  = m·mid  + (1−m)·globalMid
// 所以「全局锐化 = 0」时掩码外 fine_eff/mid_eff 恒为 0 → 背景逐字节不动。
// ---------------------------------------------------------------------------
struct DetailParams {
    float fine = 0.0f;        // 掩码内细频带回填比例，0..1（面板「锐化」/100）
    float mid = 0.0f;         // 掩码内中频带增益，0..3（锐化/100 × 3.30）
    float globalFine = 0.0f;  // 掩码外细频带（面板「全局锐化」/100）
    float globalMid = 0.0f;
    float thrFine = 3.0f;     // 细频带软阈值（削噪，0 = 不回填不减）
    float thrMid = 0.0f;      // 中频带软阈值（结构为主，默认不削）
    // 全局（掩码外）那一路的阈值，3.1.22 起与脸内分开标定。
    // 为什么分开：参照路径里「锐化」与「全局锐化」本来就是两条独立曲线，
    // 共用一个阈值就没法各自调噪声抑制强度。
    float thrFineGlobal = 3.0f;
    float thrMidGlobal = 0.0f;
    float kneeFine = 12.0f;   // 软膝：|增量| 超过它按比例压缩，防硬边过冲
    float kneeMid = 10.0f;
    // delta 模式（3.1.22）：加回的是「原图细频 − dst 细频」，也就是
    // **被磨掉的那一部分**，而不是把原图细频整份加回去。
    // 差值随磨皮强度自动伸缩：磨皮轻 → 差值≈0 → 不动；磨皮重 → 才补。
    bool deltaRefill = false;
};

// 细节层的时序 EMA（运动自适应）。对应参考实现侧的 srcT + detailBetaFromVel
// （见 legacy_engine.cpp），解决的问题是：锐化把**相机白噪**一起放大，而噪点
// 帧间独立 → 静止的脸呈「沸腾」式闪动。
//
// 做法：把「算高通用的亮度平面」在时间轴上按 beta 平滑一次，再用平滑后的
// 亮度去减盒式模糊。白噪被平均掉；静态纹理帧间相关，EMA 稳态就等于自身，
// 所以纹理不损失。beta = 新帧权重：1.0 = 不平滑（等价于没有这个特性），
// 0.20 = 静止时噪声压到约 0.38 倍。运动时调用方把 beta 抬到 1，避免拖影。
struct DetailEma {
    unsigned char* lumT = nullptr;  // 全帧亮度历史，长度 w*h；nullptr = 不做 EMA
    float beta = 1.0f;              // 新帧权重（0,1]
    bool reset = false;             // true = 用当前帧重置历史（首帧 / 尺寸变化）
};

// src 是**原图**（高频层从这里取），dst 是磨皮后的结果（增量加在它上面）。
// 两者可以是同一块内存，但那样就退回「对磨完的图做 unsharp」——没有输入可放大，
// 正是参考实现 2.1.0 那版失效的原因，不要这么用。
// 只写 roi 内、且权重非零的像素；alpha 不动。
// scratch 由调用方持有（4 个平面，按 ROI 面积分配）。
// ema 非空且 beta<1 时，亮度平面先过一遍运动自适应时序 EMA 再算高通。
void detailRefill(const unsigned char* src, unsigned char* dst, int w, int h,
                  const unsigned char* mask, const RoiRect& roi,
                  const DetailParams& p, int threads,
                  std::vector<float>& scratch, const DetailEma& ema = DetailEma());

// ---------------------------------------------------------------------------
// 纯尺度低通（盒式迭代近似高斯）—— C 方案：把「保边」交给幅度门控
//
// 为什么需要它：双边迭代的低频层**本身也保边**，于是高频层 high = src − low 在
// 真边缘处接近 0，幅度门控再保一次边 → 两道闸门叠在一起，「瑕疵」和「结构」
// 一起被放过。实测脸颊细纹理只削掉 14%，参考实现削掉 37%（见 C14）。
//
// 盒式迭代不做任何保边：它就是各向同性的低通。等效方差
//     σ² = iters × ((2r+1)² − 1) / 12
// 迭代 3 次后波形已接近高斯（中心极限定理），但成本是 O(1)/像素、与半径无关
// （前缀和滑动窗），比 19-tap 可分离高斯便宜一个数量级 —— 当年记的
// 「高斯 1080p 34 ms 超预算」指的是可分离高斯，不是这条路线。
//
// 保边完全交给 freqCompositePlanes 的幅度门控：边缘处 |src − low| 大 → 门控
// 拦住不动；痘印 / 鼻翼阴影 / 唇纹幅度中等 → 被削掉。这正是高低频分离原本
// 的设计意图，只是此前低通抢了门控的活。
// ---------------------------------------------------------------------------
struct BoxLowParams {
    int radius = 4;    // 单趟盒式半径
    int iters = 3;     // 迭代次数（3 次 ≈ 高斯）
};

// 计算低频层（不改 dst，只返回视图）。语义与 lowFreqRefRoi 完全一致，
// 只是里面换成盒式迭代。输出范围比 roi 外扩 radius×iters。
LowFreqBuf boxRgbRoi(const unsigned char* src, int w, int h,
                     const BoxLowParams& p, const RoiRect& roi,
                     int threads, std::vector<float>& scratch);

// 灰度（luma）盒式模糊，ROI 局部，O(1)/像素。用于 clarity 的大半径参照。
//
// 与 lowFreqRefRoi 的区别：那里要的是「保边低频」（双边），这里要的是
// 「纯尺度参照」（盒式）—— clarity 加的是明暗层次，不需要保边，
// 需要的是**便宜且尺度可控**。盒式正好：O(1)/像素，半径随便调。
//
// 返回的 LowFreqBuf 三个指针指向**同一个灰度平面**（调用方只用 .r 即可）。
LowFreqBuf boxLumaRoi(const unsigned char* rgba, int w, int h, int radius,
                      const RoiRect& roi, int threads,
                      std::vector<float>& scratch);

// ---------------------------------------------------------------------------
// surface3 —— 三层表面模糊（对齐参考实现 2.4.2 的磨皮结构，见 C22）
//
// 参考实现磨皮的 GLSL 反编译结论（beauty_source_readable.md）：
//     even = C − b1 + b2
//     even = mix(even, b2, skin·e)
//     res  = C + (even − C)·I
//     out  = C + (res − C)·(1 − eye)
// 其中 b1/b2 是**表面模糊**（带颜色阈值），不是普通盒式；阈值负责保住
// 高对比细纹理 —— 这正是两层模型在 100 档过磨的根因（C21 §2.3）。
//
// 本函数是 P0 的**廉价近似**：b = box(C, r)，再按 |C − b| > thr 决定保留 C。
// 语义等价于「只平均颜色接近的邻域」，比逐像素 17×17 表面模糊便宜一个量级。
// 参数拟合出来之前不要当定稿（C22 §5）。
//
// 与 blendWithMask 的分工：这里**自己**完成 I / e / skin / eye 四步并直接写
// dst（含掩码外逐字节还原），调用方不要再叠一次 t —— 否则 skin 与 t 会
// 被应用两遍，过渡带会明显偏弱。
//
// scratch1 / scratch2 由调用方持有（每个需要 6 个 ROI 平面），不能共用。
// ---------------------------------------------------------------------------
// 注意：本结构体在 beauty_core.cpp 里**每次都被完整填满**，下面的默认值只是
// 兜底，不代表运行时行为。真实默认值在 beauty_core.cpp:234-247（g_cfg.s3*）。
// 3.1.14：兜底值改成与「t=0 档位」一致，并删掉原来 8/12 的阈值 —— 那组值从未
// 被下发过（core 一直填 0/0），但注释让人以为阈值是开着的，误导过两轮排查。
struct Surface3Params {
    int r1 = 2, it1 = 1;      // 小半径层 b1（core 按档位从 2 线性推到 14）
    int r2 = 2, it2 = 1;      // 大半径层 b2
    float thr1 = 0.0f;        // b1 颜色阈值（灰阶 0..255）；0 = 纯盒式，无阈值
    float thr2 = 0.0f;        // b2 颜色阈值
    float I = 1.0f;           // 强度 0..1（= mix(t·K, 1, step(0.7, t))）
    float e = 0.3f;           // evenfactor：even → b2 的比例（再乘 skin）
    float protectK = 100.0f;  // 五官保护倍率（%，100 = 完全按 protect 排除）
};

void surface3Rgb(const unsigned char* src, unsigned char* dst, int w, int h,
                 const unsigned char* mask, const unsigned char* protect,
                 const Surface3Params& p, const RoiRect& roi, int threads,
                 std::vector<float>& scratch1, std::vector<float>& scratch2);

// ---------------------------------------------------------------------------
// surface3Rgb25 —— 25 tap 表面模糊版（C23 §3，第 2 批 #5/#6/#7）
// ---------------------------------------------------------------------------
// 与 surface3Rgb 的差别只有「两个低频层怎么算」：
//     surface3Rgb   : boxRgbRoi，半径按档位线性 + 按脸宽 fw/532 归一化（拟合痕迹）
//     surface3Rgb25 : 固定 UV 半轴的 25 tap 表面模糊，串行两趟（yFactor 40→20）
// 合成段（I / e / skin / eye 四步）两者**逐行相同**，所以可以拿来做 A/B。
//
// 证据等级（不许含糊）：
//   25 tap 结构、环半径 {1,2,5}、yFactor 40/20、只测绿通道、中心权重恒 1
//        —— 【重建·待证】master_analysis §3.5 只拿到文件名，GLSL 体未干净捕获。
//        0a″ 合成图实验只验到了「核尺度随画幅线性（σ∝W^0.98）」，
//        环半径本身**仍无独立实测证据**。
//   UV 半轴 sx/sy 固定、不随画幅自适应 —— 【实测】2026-10-29 光栅实验，
//        四画幅 σx = 1.95/3.85/7.72/11.20 px，拟合 σx∝W^0.98。
//   半分辨率处理 —— 【实测】01 §3.3；注意 UV 与分辨率无关，所以它只改变
//        采样网格（更粗、双线性插值带来一点额外柔化），**不改变核的物理尺寸**。
struct Surface25Params {
    float sx = 0.002640845f;   // UV 半轴 x【实测】
    float sy = 0.0046875f;     // UV 半轴 y【实测】
    float yFactor1 = 40.0f;    // pass1 值域权重系数【重建·待证】
    float yFactor2 = 20.0f;    // pass2（容限更宽）【重建·待证】
    int   halfRes = 1;         // 1 = 在半分辨率上跑两趟【实测】
    float I = 1.0f;            // 强度 0..1
    float e = 0.3f;            // evenfactor
    float protectK = 100.0f;   // 五官保护倍率（%）
};

void surface3Rgb25(const unsigned char* src, unsigned char* dst, int w, int h,
                   const unsigned char* mask, const unsigned char* protect,
                   const Surface25Params& p, const RoiRect& roi, int threads,
                   std::vector<float>& scratch);

// ---------------------------------------------------------------------------
// surface3RgbBand —— 双尺度带通 + 振幅门控（C23 §11/§12，第 2 批-B）
// ---------------------------------------------------------------------------
// 为什么不再用 25 tap：§10.4 的频响实测证明 25 tap 的径向频响是**双凹陷**
// （8–13 与 32–60），中间还反弹；参考实现是干净的单凹陷（13–20）。那是稀疏环形核
// 的旁瓣，不是参数没调好。§11 的振幅-频率实测又证明 参考实现的振幅依赖是
// 「弱纹理全磨、强纹理基本放过」，而 25 tap 的 yFactor 只能做到 1.05 倍。
//
// 结构（每个常数的来历都标在后面）：
//     B1 = 窄模糊    3 次盒式，宽 ≈ 5/960·ww   ← 【拟合·传递函数】C23 §12.1
//     B2 = 宽模糊    3 次盒式，宽 ≈ 9/960·ww   ← 同上
//     D  = B2 − B1                            带通
//     even = C + (1−e)·D + e·(B2 − C)         ≡ §3.3 的 (1−e)C − (1−e)B1 + B2
//     a  = blur(|C.g − B2.g|)                 振幅代理（只测绿通道，同 §3.1）
//     w  = 1 / (1 + (a/T)²)                   Wiener 式收缩 ← 【拟合】§12.2
//     out = C + w·(even − C)
//
// 三条结构判据（C23 §11.6）的实测结果见 §12.4：
//     主凹陷 24 px ✅ / A=2·P=24 增益 0.376（参考实现0.201）❌ /
//     A=16·P=8 增益 0.654（参考实现0.579）✅
// 判据 2 没过是已知偏差：我们对**噪声级弱纹理**没有参考实现那么狠。强行对上会
// 让像素 RMSE 变差（§12.3 的幂次扫描：p=2→8 时 RMSE 1.34→2.03），所以保留。
//
// 反拟合纪律（C23 §8）：w1/w2 是照着**传递函数**（光栅表）定的，不是照着
// 金标 RMSE 扫出来的；T 是照「改动量 = 2.74」定的，也不是 RMSE。
struct BandParams {
    // 盒宽占**处理宽度**的比例。基准 1920 宽 + 半分辨率（ww=960）时
    // w1=5 / w2=9 / wa=5。UV 固定（C23 §3.2），所以按比例缩放而不是按像素。
    float f1 = 5.0f / 960.0f;    // B1 盒宽/ww【拟合·传递函数】
    float f2 = 9.0f / 960.0f;    // B2 盒宽/ww【拟合·传递函数】
    float fa = 5.0f / 960.0f;    // 振幅平滑盒宽/ww【拟合·传递函数】
    int   passes = 3;            // 盒式次数（3 次盒式 ≈ 高斯；2 次改动量掉到 2.32，不划算）
    float T = 6.5f;              // 振幅门控阈值，灰阶【3.1.19 按平坦/纹理比重定 8.3→6.5】
    // 门控曲线的陡度指数：gw = 1 / (1 + (a/T)^p)。
    //   p=2 是双曲形式（等价于原来的 1/(1+(a/T)²)）；
    //   p>2 时低振幅更贴近 1（平坦区照磨）、高振幅掉得更快（纹理区更保守），
    //   也就是参考实现「越平越磨、越有结构越保守」的非对称——这是选择性的来源。
    float gwp = 2.0f;            // 【3.1.19】平坦/纹理选择性
    int   halfRes = 1;           // 1 = 半分辨率（实测 RMSE 1.336→1.362，可忽略）
    float I = 1.0f;              // 强度 0..1
    float e = 0.3f;              // evenfactor
    float protectK = 100.0f;     // 五官保护倍率（%）
};

void surface3RgbBand(const unsigned char* src, unsigned char* dst, int w, int h,
                     const unsigned char* mask, const unsigned char* protect,
                     const BandParams& p, const RoiRect& roi, int threads,
                     std::vector<float>& scratch);

}  // namespace beautycore
