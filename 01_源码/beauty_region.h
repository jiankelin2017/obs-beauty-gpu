#pragma once
#include <vector>
#include "beauty_smooth.h"   // RoiRect
// ---------------------------------------------------------------------------
// beauty_region —— 人脸局部效果的几何层（五官立体 / 亮眼 / 去黑眼圈 /
// 去法令纹 / 祛斑痘 共用）
//
// 为什么单独一层：这五项的共同点是「**只在脸上某几个小区域**动一点亮度」，
// 差别只在椭圆放在哪、给多少增益。把它们拆成五个独立函数，几何计算会复制
// 五遍，参数也会各自漂移。这一层只做两件事：算几何、按几何改像素。
//
// 坐标系为什么归一化到脸框：脸框（YuNet 检出）随分辨率与镜头距离变化，
// 而解剖位置相对脸框是稳定的。同一套系数在 640×394 和 1080p 上落在同一个
// 解剖点上，不用按分辨率改参数。这也是不直接写死像素半径的原因 —— 写死
// 像素半径的版本在 720p 上会把「亮眼」压到瞳孔上、在 4K 上又跑到下眼睑。
//
// 这些系数是怎么来的：不是估的。先用还能跑的参考实现把每个参数单独拉满，
// 和「该项为 0」的输出相减，得到真实作用域的连通域（中心 + σ），再换算成
// 相对脸框的比例。数据见 06_离线验证/goldens/_probe/ 与
// 05_文档/C12_人脸局部五项.md。
// ---------------------------------------------------------------------------

namespace beautycore {

// 一个软椭圆区域。中心/半径都是**相对脸框**的比例（0..1），
// 所以 (0.5, 0.5) 永远是脸框正中，0.1 永远是「十分之一个脸宽」。
struct Ellipse {
    float cx, cy;    // 中心
    float rx, ry;    // 半径
    float rot;       // 旋转（弧度，顺时针为正）。0 = 轴对齐，法令纹要斜
    float gain;      // 中心处最大亮度增量（灰阶，可负 = 压暗）
    float shade;     // 0..1：按 (1 - shade*Y/255) 加权，越大越只提亮暗部
    float feather;   // 0..1：d < feather 处满强度，d = 1 处归零
};

// 把 n 个椭圆叠加作用到 dst 上（加法，逐通道同量 → 不动色相）。
//   mask  ：0..255 软边肤色掩码，会乘进增益；传 nullptr 表示不限制。
//   face  ：脸框（像素坐标）。椭圆按它换算成像素位置与半径。
// 椭圆并集之外**逐字节不变** —— 这是产品承诺的硬指标（背景不许被改）。
//
// threads <= 0 自动。椭圆面积通常只有脸框的几成，实际开销远小于磨皮。
void applyEllipses(unsigned char* dst, int w, int h,
                   const unsigned char* mask,
                   float faceX, float faceY, float faceW, float faceH,
                   const Ellipse* els, int n, int threads = 0);

// 祛斑痘：把「斑点」往局部均值拉回去。
//
// 判据用「偏离局部均值的幅度落在 [thrLo, thrHi]」而不是「所有偏离」：
//   · |dev| < thrLo  → 皮肤本身的细纹理，动了就是糊，不管
//   · |dev| > thrHi  → 真结构边缘（睫毛、鼻孔、唇线），动了会出光晕，不管
//   · 中间这一段才是斑/痘印的尺度
// 拉回量按 k（0..1）缩放，面板档位直接乘在 k 上。
//
// scratch 由调用方持有（两个浮点平面，按 ROI 面积分配）。
void applySpotFix(unsigned char* dst, int w, int h,
                  const unsigned char* mask, const RoiRect& roi,
                  float k, int radius, float thrLo, float thrHi,
                  int threads, std::vector<float>& scratch);

// 法令纹：**把暗线填平到周围皮肤的亮度**（3.1.22）
//
// 为什么不是「固定形状 + 固定亮度」
// -------------------------------
// 实测参照（原生 SDK 的 remove_nasolabial_folds_strength）的行为：
//   · 峰值像素的 Δ **恒等于 +52**（封顶，不是随亮度变化的连续函数）
//   · corr(原亮度, Δ) = −0.37 ~ −0.44 → 越暗的像素提得越多
//   · 输出值可以**超过邻域最大值** → 不是「填到邻域均值/最大值」
// 也就是说参照是在「法令纹所在的那条暗线」上按暗度提亮，上限 +52。
//
// 模型
// ----
//   Δ = min(maxLift, slope · max(0, LR − Y)) × gate(d) × 区域权重 × 限制因子
//   LR = 邻域最大亮度（半径 lrRadius，可分离 max 滤波）
//   d  = LR − Y（暗差）
//   gate(d) = smoothstep(gateLo, gateHi, d)   —— 暗线闸门
//   区域权重 = 椭圆软边（与 applyEllipses 同一套判据）
//   限制因子 = max(皮肤掩码, 区域权重)
//
// 区域只起「大致框住法令纹」的作用，具体哪条线是法令纹由「暗线在哪」
// 自己决定 —— 所以脸一转、姿态一变也跟得上（这是固定形状方案做不到的）。
//
// 限制因子为什么要 max(皮肤掩码, 区域权重)（3.1.23）
// ------------------------------------------------------
// 皮肤掩码是 **YCbCr 肤色椭圆**，褶皱越深越暗就越「不像肤色」→ 掩码越低。
// 实测（06_离线验证/全项对齐_20261030/_mw_align.py）：
//   corr(皮肤掩码, 参照Δ) = **−0.66**
// 也就是引擎**恰好在最该提亮的那条褶皱线上把效果掐掉** —— 这是把
// 「按肤色选区」用在「填平暗纹」上的固有矛盾。
// 改成 max(皮肤掩码, 区域权重)：只有在**区域核心**才旁路肤色判断
// （区域权重 ≈ 1 处），区域边缘与区域外仍由肤色掩码把关，不会溢到
// 头发/背景。核心区域本来就是我们自己用关键点摆出来的脸颊位置，
// 由几何负责即可。
//
// 暗线闸门（gate）的作用：把响应收窄到真正的褶皱线上。
// 实测参照的响应是 **5~8 px 宽的尖峰**（沿眼下方竖线扫描：
// 11 33 50 51 51 39 10，两侧立刻归零），不是一整片区域。
// 闸门让「暗差」小的像素不起作用，宽度自然收敛到线上。
//
// scratch 由调用方持有（1 个 float 平面，按 ROI 面积分配）。
void applyFoldRemoval(unsigned char* dst, int w, int h,
                       const unsigned char* mask,
                       float faceX, float faceY, float faceW, float faceH,
                       const Ellipse* els, int n,
                       float slope, float maxLift, int lrRadius,
                       float gateLo, float gateHi,
                       int threads);

}  // namespace beautycore
