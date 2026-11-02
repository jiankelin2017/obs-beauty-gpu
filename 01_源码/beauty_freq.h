#pragma once
// ---------------------------------------------------------------------------
// beauty_freq —— 高低频分离磨皮（频率分离）
//
// 为什么不用「滤波结果与原图线性混合」：
//     dst = src·(1-t) + blur·t  ==  src - (src-blur)·t
// 这个式子会把**所有频段**一起削弱 t 倍 —— 包括脸部的低频结构（明暗层次、
// 骨骼起伏）和中频轮廓。t 一大，脸就"糊"了，不是"磨"了。
//
// 高低频分离的思路：
//     low  = 大半径模糊(src)          低频：脸型、光影、明暗过渡
//     high = src - low                高频：毛孔、痘印、细纹 + 五官边缘
//     只削弱 high 里**幅度小**的部分，幅度大的（眉毛、鼻梁边、嘴唇轮廓）原样保留
//     out  = low + high·keep  ==  src - high·atten
//
// 幅度门控是这套方法的关键：它让"瑕疵"和"边缘"分开，而线性混合做不到这一点。
// 这也是"磨皮之后脸不塌"的原因 —— 低频从头到尾没动过。
// ---------------------------------------------------------------------------

#include "beauty_smooth.h"   // RoiRect / LowFreqBuf

#include <vector>

#include "beauty_smooth.h"   // RoiRect

namespace beautycore {

// 可分离高斯模糊（RGBA8，alpha 原样拷贝）。
//   radius/sigma 决定低频尺度：sigma≈3 对应毛孔~细纹，sigma≈6 对应痘印~色斑。
//   scratch 由调用方持有（避免每帧分配）。
// 只在 roi 内写 dst，但读取可以超出 roi（边界 clamp）。
void gaussBlurRgba(const unsigned char* src, unsigned char* dst,
                   int w, int h, int radius, float sigma,
                   const RoiRect& roi, int threads,
                   std::vector<unsigned char>& scratch);

// 高低频合成：out = src - (src-low) · atten
//   atten = strength · (1 - smoothstep(magLo, magHi, |src-low|))
// 即：幅度小于 magLo 的高频被磨掉 strength 那么多，大于 magHi 的完全不动。
// magLo/magHi 是 0..255 尺度的三通道最大差值。
void freqComposite(const unsigned char* src, const unsigned char* low,
                   unsigned char* dst, int w, int h,
                   float strength, float magLo, float magHi, float grain,
                   const RoiRect& roi, int threads);

// 同上，但低频层来自浮点平面（LowFreqBuf）。合成公式一个字都没改，
// 只是 `src - low` 里的 low 保持浮点精度，不再先压回 uint8。
void freqCompositePlanes(const unsigned char* src, unsigned char* dst,
                         int w, int h, const LowFreqBuf& low,
                         float strength, float magLo, float magHi, float grain,
                         const RoiRect& roi, int threads);

// ---------------------------------------------------------------------------
// clarity（清晰 / 大半径局部对比）
//
// 为什么需要它：磨皮的幅度门控只削 6~30 的中频，而实测我们**已经保住了 96.8%
// 的中频**（05_文档/D1 §4）—— 立体感的差距不是「被磨掉了」，而是「从来没增强过」。
// 所以只能主动加：把 25px 尺度上的明暗差按比例加回去，脸就不塌。
//
// 为什么半径必须是 17~25px：磨皮的核动的是 ≤8px（毛孔、痘印），立体感在 10~30px
// （鼻翼两侧、眼窝、颧骨下方）。窗口取小了等于把痘印加回去。
//
// 为什么不会在「脸/背景交界」处出现光晕：软膝。|mid| 在硬边处很大 → gate≈0 → 不加。
// 这和磨皮的幅度门控是同一套思路，参数也沿用同样的量级。
//
// 只在 mask>0 处生效，掩码外逐字节不动。
// ---------------------------------------------------------------------------
struct ClarityParams {
    float gain = 0.25f;    // 0..1，面板「清晰」100 档映射到这里
    int radius = 12;       // 盒式半径 → 25px 窗口
    float kneeLo = 20.0f;  // |mid| 低于它：全增益
    float kneeHi = 45.0f;  // 高于它：完全不加（防光晕）
};

void clarityRgb(unsigned char* dst, int w, int h, const unsigned char* mask,
                const RoiRect& roi, const ClarityParams& p, int threads,
                std::vector<float>& scratch);

// 默认参数（离线标定，见 05_文档/C4_高低频磨皮.md）
struct FreqParams {
    // 低频层：双边迭代。radiu/ters 一起决定低频尺度（等效半径 ≈ radius × iters）
    int radius = 2;
    int iters = 3;
    float sigmaSpace = 7.0f;
    float sigmaColor = 40.0f;
    float magLo = 6.0f;    // 幅度低于它：完全按 strength 磨
    float magHi = 30.0f;   // 幅度高于它：完全不动
    // 颗粒回填 0..1：把 4 邻域尺度的极细颗粒扣回一部分，抑制双边迭代的阶梯感。
    // 0 = 不回填（最干净但有塑料感），0.5 = 默认
    float grain = 0.5f;
    int strength = 50;     // 0..100，面板磨皮档位

    // 低频提取方式：
    //   0 = 双边迭代（iters × radius），快、保边，但有阶梯效应
    //   1 = 引导滤波（半径与成本无关），无阶梯效应，适合更大尺度的瑕疵
    int lowMode = 0;
    int guideRadius = 12;
    float guideEps = 400.0f;

    // 低频层的第二条实现路线（与 lowMode 互斥，lowPath>0 时优先）：
    //   0 = 旧的 uint8 双边迭代（绝对颜色差、中间结果压回 uint8）
    //   1 = 浮点双边迭代（绝对颜色差，只换精度）
    //   2 = 浮点双边迭代 + 低频参照（权重只看局部起伏）
    //   3 = 盒式迭代低通（**不保边**，保边全交给幅度门控）—— C 方案，见 C14
    // 2 是对照实验的终点，1 用于把「浮点」和「参照」两个改动的贡献拆开。
    int lowPath = 0;
    int refRadius = 8;   // 参照半径（盒式）；<=0 表示这条路线内也不做参照

    // lowPath == 3 时的低频尺度。等效 σ² = boxIters × ((2·boxRadius+1)² − 1) / 12
    int boxRadius = 4;
    int boxIters = 3;
};

// 清晰 + 美白 + 红润（肤色区域内）。三个都是**逐通道颜色变换**，
// 与空间位置无关 —— 这是 2026-10-30 从原生参考实现反解出来的事实，不是假设。
//
// 依据（见 06_离线验证\全项对齐_20261030\tone_curve.py）：
//   把 panel 全置 0、用 BEAUTY_EXTRA_PARAMS_FILE 直接下发单个参考实现参数，
//   再按输入灰阶分箱统计输出 → 「箱内 σ」就是它有多依赖空间/邻域。
//     clarity   σ 0.08~0.27  → 纯曲线（S 形对比 + 轻微暖调）
//     color_level σ 0.36~1.24 → 纯曲线（提亮）
//     red_level σ 0.73        → 纯曲线（只压绿通道）
//   σ 只有零点几灰阶，说明"照抄曲线"就能对齐到量化误差级。
//
// 所以这里不再用任何拟合公式（原来那套 log 提亮 / 钟形粉调都来自别的实现），
// 直接用实测的 5 档 × 17 锚点查找表：档位与输入值都做线性插值。
//
//   mask    0..255 的肤色权重（就是磨皮用的那张）
//   clarity 0..1 清晰强度（面板值 /100）
//   whiten  0..1 美白强度
//   rosy    0..1 红润强度
// 只在 roi 内、且 mask>0 的像素上动手（参考实现是整帧生效，我们保持"只动脸"）。
void toneColor(unsigned char* dst, int w, int h,
               const unsigned char* mask, const RoiRect& roi,
               float clarity, float whiten, float rosy, int threads);

// 兼容旧调用点（清晰=0）。
void toneWhitenRosy(unsigned char* dst, int w, int h,
                    const unsigned char* mask, const RoiRect& roi,
                    float whiten, float rosy, int threads);

// 一站式：高斯拉低 + 幅度门控合成。scratch/low 由内部复用调用方缓冲。
void highLowSmooth(const unsigned char* src, unsigned char* dst,
                   int w, int h, const FreqParams& p,
                   const RoiRect& roi, int threads,
                   std::vector<unsigned char>& lowBuf,
                   std::vector<unsigned char>& scratch,
                   std::vector<float>& guideScratch);

}  // namespace beautycore
