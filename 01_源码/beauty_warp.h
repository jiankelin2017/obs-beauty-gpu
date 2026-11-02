#pragma once
// ---------------------------------------------------------------------------
// beauty_warp —— 美形三项的几何变形（瘦脸 / 大眼 / 下巴）
//
// 与前面十个效果的根本区别：那十项都是**逐像素颜色变换**（查表、混合、
// 局部椭圆增益），输出像素位置与输入一一对应；这三项改的是**像素位置**，
// 必须做「反向映射 + 重采样」，因此单独成模块、单独在管线末端跑一趟。
//
// 为什么用反向映射：正向映射（把源像素推到目标位置）会在目标画布上留下
// 空洞，得再补洞；反向映射对每个输出像素问「你该从哪来」，天然无空洞、
// 且天然可并行。代价是每个像素要做一次双线性采样（4 次读 + 4 次乘加）。
//
// 精度边界（必须写清楚，别让人误以为是网格变形）：
//   我们只有 YuNet 的 **5 个关键点**（左右眼、鼻尖、左右嘴角）+ 人脸框，
//   没有 106/468 点稠密网格。所以这三项是**解析位移场近似**：
//     · 大眼 = 以眼心为中心的径向放大，衰减用 (1−r²)²
//     · 瘦脸 = 向面中线的水平压缩，权重 = 垂直高斯 × 侧向 smoothstep
//     · 下巴 = 下半脸相对嘴线的纵向压缩，中线强、两侧弱
//   形状方向是对的，但做不到商业 SDK 那种「沿真实轮廓贴合」的精度 ——
//   网格变形能保住下颌骨的转折，解析场只能给出平滑的鼓包。
// ---------------------------------------------------------------------------

#include <vector>

#include "beauty_face.h"

namespace beautycore {

// 三项强度，都是 0..1（面板 0..100 除以 100）。
struct WarpParams {
    float thin = 0.0f;   // 瘦脸
    float eye = 0.0f;    // 大眼
    float chin = 0.0f;   // 下巴
};

// ---------------------------------------------------------------------------
// 5c：一张脸做位移场需要的全部**派生常量**
//
// 为什么要把这些抽出来公开（3.1.62）：GPU 侧必须与 CPU 用**同一份**常量。
// 宿主若按公式自己再算一遍，两处口径迟早漂移 —— 这正是色调（I/e/T 查表+夹取）
// 与细节（fine/mid 按质感与运动自适应重算）当时定下「引擎把算好的参数递出去」
// 的同一条理由。所以 CPU 路径也改成「先 warpFaceGeom、再 warpFaceWithGeom」，
// 让两条路共用同一个来源，而不是各算各的。
//
// 字段顺序与 beauty_core.h 的 BeautyWarpFace 一一对应（beauty_core.cpp 负责搬运）。
// ---------------------------------------------------------------------------
enum {
    WARP_F_EYE  = 1u << 0,   // 大眼项生效（p.eye>0 且 eyeR>1）
    WARP_F_THIN = 1u << 1,   // 瘦脸项生效（p.thin>0 且 thinSig>0.5）
    WARP_F_CHIN = 1u << 2,   // 下巴项生效（p.chin>0 且 chinSpan>1）
    WARP_F_KPS  = 1u << 3,   // 眼睛来自关键点（否则是比例估算）
};

struct WarpGeom {
    float cx = 0, cy = 0;      // 脸框中心
    float halfW = 0;           // fw/2
    float eyeRx = 0, eyeRy = 0;   // 右眼
    float eyeLx = 0, eyeLy = 0;   // 左眼
    float eyeR = 0;            // 大眼作用半径（已夹到 [0.10fw, 0.35fw]）
    float eyeAmp = 0;          // p.eye × 0.55（满档中心放大 1.55 倍）
    float thinYc = 0;          // 瘦脸垂直高斯中心
    float thinSig = 0;         // 瘦脸垂直高斯 sigma
    float thinK = 0;           // 瘦脸满档外移量 p.thin × 0.24
    float chinYm = 0;          // 下巴压缩起点（嘴线略下）
    float chinSpan = 0;        // 下巴压缩跨度
    float chinK = 0;           // 下巴满档压缩 p.chin × 0.18
    int   roi[4] = {0, 0, 0, 0};   // x0,y0,x1,y1（已钳到画面内）
    unsigned flags = 0;        // WARP_F_*
};

// 返回 false = 这张脸**不需要**变形（三项全 0 / 脸框太小 / ROI 退化 /
// 三项都因参数或关键点不足而失效）。返回 true 时 out 里是一份可直接用于
// warpFaceWithGeom 的常量。
bool warpFaceGeom(const FaceBox& f, const WarpParams& p, int w, int h,
                  WarpGeom& out);

// 用已经算好的常量就地变形。语义与旧 warpFace 逐字相同。
void warpFaceWithGeom(unsigned char* rgba, int w, int h, const WarpGeom& g,
                      std::vector<unsigned char>& scratch);

// 就地修改 rgba（RGBA8，紧凑 stride = w*4）里属于这张脸的那一块。
//
// 只处理「人脸框外扩 30%×宽 / 18%×高」的 ROI：三项的位移权重都在 ROI
// 边界归零，所以 ROI 内外的像素不会出现错位接缝。
// scratch 是调用方持有的复用缓冲（避免每帧 malloc），函数内部按需扩容。
//
// 三项全 0 或脸框太小（< 8px）时直接返回，逐字节不动。
// 3.1.62 起它只是 geom + withGeom 的包装，保留给离线台与旧调用点。
void warpFace(unsigned char* rgba, int w, int h, const FaceBox& f,
              const WarpParams& p, std::vector<unsigned char>& scratch);

}  // namespace beautycore
