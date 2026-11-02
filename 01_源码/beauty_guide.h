#pragma once
// ---------------------------------------------------------------------------
// beauty_guide —— 引导滤波（Guided Filter, He et al. 2010）
//
// 为什么需要它：
//   现在的高低频磨皮用「双边迭代」提取低频。双边有两个毛病：
//     1) 平坦区被压成平台，平台之间出现新台阶（阶梯效应/塑料感）；
//     2) 想磨更大尺度的瑕疵必须加大半径，而双边的成本是 O(r²) —— r=6 就是 9 倍。
//   引导滤波是 O(1)/像素（只靠盒式滤波，与半径无关），而且输出是**局部线性**的：
//   q = a·I + b，a 在边缘处趋近 0（不平滑），在平坦区趋近 1（平滑），
//   不会产生平台之间的台阶。
//
// 用灰度引导：保边效果来自亮度，彩色引导需要 3×3 矩阵求逆，收益不值那个成本。
//
// 全部计算在 ROI 的局部缓冲里做，内存与 ROI 面积成正比（1080p 的 ROI 约 25 万
// 像素，8 个 float 缓冲 ≈ 8 MB）。
// ---------------------------------------------------------------------------

#include <vector>

#include "beauty_smooth.h"   // RoiRect

namespace beautycore {

struct GuideParams {
    int radius = 12;      // 盒式窗口半径（决定低频尺度）
    float eps = 400.0f;   // 正则化：越大越平滑、越不保边。约等于「多少灰阶的差异算边缘」
};

// 引导滤波（源图自己当引导图，即"自引导"）。
// 只写 roi 内的像素；读可以超出 roi（边界 clamp）。
// scratch 由调用方持有，内部按需扩容。
void guidedSmoothRoi(const unsigned char* src, unsigned char* dst,
                     int w, int h, const GuideParams& p,
                     const RoiRect& roi, int threads,
                     std::vector<float>& scratch);

}  // namespace beautycore
