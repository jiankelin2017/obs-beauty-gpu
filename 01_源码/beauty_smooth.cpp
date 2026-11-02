// beauty_smooth.cpp —— 自研保边平滑核实现（纯 C++，无参考实现依赖）
//
// 实现要点（都是为性能服务的）：
//   1) 两张查找表：空间权重 (2r+1)² 项，颜色权重 768 项（三通道 L1 距离 0..765）。
//      内层循环只剩查表与乘加。
//   2) 权重用 16 位定点（0..1024），累加用 32 位整数 —— 避免每像素 exp() 调用。
//   3) 颜色权重为 0 时直接跳过该邻居。
//   4) ROI 裁剪：只算掩码覆盖的区域。逐像素独立计算，裁剪不改变任何结果。
//   5) 多线程按行切分：行与行之间没有数据依赖，天然可并行。
//   6) 边界用 clamp（复制边缘像素）。与 OpenCV 的 BORDER_REFLECT 略有差异，
//      但对拍比较的是画面中心区域，边界差异不影响结论。

#include "beauty_smooth.h"
#include "beauty_pool.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace beautycore {

namespace {

// 定点精度：权重放大到 0..1024
const int kWeightScale = 1024;

void buildSpatialLut(std::vector<int>& lut, int radius, float sigmaS) {
    const int n = radius * 2 + 1;
    lut.assign((size_t)n * n, 0);
    const float inv = (sigmaS > 0.f) ? 1.0f / (2.0f * sigmaS * sigmaS) : 0.f;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            const float d2 = (float)(dx * dx + dy * dy);
            const float w = std::exp(-d2 * inv);
            lut[(size_t)(dy + radius) * n + (dx + radius)] =
                (int)(w * kWeightScale + 0.5f);
        }
    }
}

void buildColorLut(std::vector<int>& lut, float sigmaR) {
    lut.assign(768, 0);
    const float inv = (sigmaR > 0.f) ? 1.0f / (2.0f * sigmaR * sigmaR) : 0.f;
    for (int d = 0; d < 768; d++) {
        const float w = std::exp(-(float)(d * d) * inv);
        lut[d] = (int)(w * kWeightScale + 0.5f);
    }
}

// 线程数决策：太少没收益，太多会被线程创建开销和内存带宽吃掉。
// 行数少于 32 时直接单线程（创建/汇合一个线程约 20~50us，不值）。
int effectiveThreads(int threads, int rows) {
    if (threads == 1) return 1;
    int n = threads;
    if (n <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        n = hc ? (int)hc : 4;
        if (n > 8) n = 8;          // 超线程对内存密集的滤波帮助很小
    }
    if (n < 1) n = 1;
    if (rows < 32) return 1;
    const int byRows = rows / 16;   // 每线程至少 16 行
    if (n > byRows) n = byRows;
    return (n < 1) ? 1 : n;
}

}  // namespace

void bilateralRgbaRoi(const unsigned char* src, unsigned char* dst,
                      int w, int h, int radius,
                      float sigmaSpace, float sigmaColor,
                      const RoiRect& roi, int threads) {
    if (!src || !dst || w <= 0 || h <= 0) return;

    int rx0 = roi.x0 < 0 ? 0 : roi.x0;
    int ry0 = roi.y0 < 0 ? 0 : roi.y0;
    int rx1 = roi.x1 > w ? w : roi.x1;
    int ry1 = roi.y1 > h ? h : roi.y1;
    if (rx1 <= rx0 || ry1 <= ry0) return;

    if (radius < 1) {
        const size_t rowBytes = (size_t)(rx1 - rx0) * 4;
        for (int y = ry0; y < ry1; y++) {
            const size_t off = (size_t)y * w * 4 + (size_t)rx0 * 4;
            memcpy(dst + off, src + off, rowBytes);
        }
        return;
    }

    std::vector<int> sLut, cLut;
    buildSpatialLut(sLut, radius, sigmaSpace);
    buildColorLut(cLut, sigmaColor);
    const int n = radius * 2 + 1;
    const int* const sBase = sLut.empty() ? nullptr : &sLut[0];
    const int* const cBase = &cLut[0];

    auto runRows = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const int ny0 = (y - radius < 0) ? 0 : y - radius;
            const int ny1 = (y + radius >= h) ? h - 1 : y + radius;
            unsigned char* orow = dst + (size_t)y * w * 4;
            for (int x = rx0; x < rx1; x++) {
                const unsigned char* c = src + ((size_t)y * w + x) * 4;
                const int cr = c[0], cg = c[1], cb = c[2];
                const int nx0 = (x - radius < 0) ? 0 : x - radius;
                const int nx1 = (x + radius >= w) ? w - 1 : x + radius;

                int acc0 = 0, acc1 = 0, acc2 = 0;
                int wsum = 0;
                for (int yy = ny0; yy <= ny1; yy++) {
                    const unsigned char* row = src + (size_t)yy * w * 4;
                    const int* srow = sBase + (size_t)(yy - y + radius) * n;
                    const unsigned char* p = row + (size_t)nx0 * 4;
                    const int* sp = srow + (nx0 - x + radius);
                    for (int xx = nx0; xx <= nx1; xx++, p += 4, sp++) {
                        const int d = std::abs((int)p[0] - cr) +
                                      std::abs((int)p[1] - cg) +
                                      std::abs((int)p[2] - cb);
                        const int cw = cBase[d];
                        if (cw == 0) continue;
                        const int wt = (*sp * cw) >> 10;
                        if (wt == 0) continue;
                        acc0 += (int)p[0] * wt;
                        acc1 += (int)p[1] * wt;
                        acc2 += (int)p[2] * wt;
                        wsum += wt;
                    }
                }
                // wsum 恒 > 0：中心像素颜色差为 0，权重固定为满值。
                if (wsum > 0) {
                    orow[x * 4 + 0] = (unsigned char)((acc0 + wsum / 2) / wsum);
                    orow[x * 4 + 1] = (unsigned char)((acc1 + wsum / 2) / wsum);
                    orow[x * 4 + 2] = (unsigned char)((acc2 + wsum / 2) / wsum);
                }
                orow[x * 4 + 3] = c[3];   // alpha 不参与滤波
            }
        }
    };

    const int rows = ry1 - ry0;
    const int nt = effectiveThreads(threads, rows);
    if (nt <= 1) {
        runRows(ry0, ry1);
        return;
    }

    beautycore::parallelRows(ry0, ry1, nt, runRows);
}

void bilateralRgba(const unsigned char* src, unsigned char* dst,
                   int w, int h, int radius,
                   float sigmaSpace, float sigmaColor, int threads) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    RoiRect all;
    all.x0 = 0; all.y0 = 0; all.x1 = w; all.y1 = h;
    if (dst == src) {
        // 滤波要读邻域原值，原地不能边读边写
        std::vector<unsigned char> tmp((size_t)w * h * 4);
        memcpy(&tmp[0], src, (size_t)w * h * 4);
        bilateralRgbaRoi(&tmp[0], dst, w, h, radius, sigmaSpace, sigmaColor,
                         all, threads);
        return;
    }
    bilateralRgbaRoi(src, dst, w, h, radius, sigmaSpace, sigmaColor, all, threads);
}

RoiRect maskBounds(const unsigned char* mask, int w, int h, int pad) {
    RoiRect r;
    if (!mask || w <= 0 || h <= 0) return r;

    int x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char* row = mask + (size_t)y * w;
        int first = -1, last = -1;
        for (int x = 0; x < w; x++) {
            if (!row[x]) continue;
            if (first < 0) first = x;
            last = x;
        }
        if (first < 0) continue;      // 整行无掩码，跳过
        if (y < y0) y0 = y;
        y1 = y;
        if (first < x0) x0 = first;
        if (last > x1) x1 = last;
    }
    if (x1 < 0) return r;             // 全零掩码

    x0 -= pad; y0 -= pad;
    x1 += pad + 1; y1 += pad + 1;     // 转回半开区间
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;

    r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
    return r;
}

namespace {

void boxDown2(const unsigned char* src, unsigned char* dst, int w, int h,
              int& ow, int& oh) {
    ow = (w + 1) / 2;
    oh = (h + 1) / 2;
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            const int x0 = x * 2, y0 = y * 2;
            const int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
            const int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
            const unsigned char* a = src + ((size_t)y0 * w + x0) * 4;
            const unsigned char* b = src + ((size_t)y0 * w + x1) * 4;
            const unsigned char* c = src + ((size_t)y1 * w + x0) * 4;
            const unsigned char* d = src + ((size_t)y1 * w + x1) * 4;
            unsigned char* o = dst + ((size_t)y * ow + x) * 4;
            for (int k = 0; k < 4; k++)
                o[k] = (unsigned char)(((int)a[k] + b[k] + c[k] + d[k] + 2) >> 2);
        }
    }
}

void upscaleBilinear(const unsigned char* src, int sw, int sh,
                     unsigned char* dst, int w, int h) {
    for (int y = 0; y < h; y++) {
        const int sy = (y * sh) / h;
        const int sy1 = (sy + 1 < sh) ? sy + 1 : sy;
        const int fy = ((y * sh * 256) / h) - sy * 256;
        for (int x = 0; x < w; x++) {
            const int sx = (x * sw) / w;
            const int sx1 = (sx + 1 < sw) ? sx + 1 : sx;
            const int fx = ((x * sw * 256) / w) - sx * 256;
            const unsigned char* p00 = src + ((size_t)sy * sw + sx) * 4;
            const unsigned char* p01 = src + ((size_t)sy * sw + sx1) * 4;
            const unsigned char* p10 = src + ((size_t)sy1 * sw + sx) * 4;
            const unsigned char* p11 = src + ((size_t)sy1 * sw + sx1) * 4;
            unsigned char* o = dst + ((size_t)y * w + x) * 4;
            for (int k = 0; k < 4; k++) {
                const int top = (int)p00[k] * (256 - fx) + (int)p01[k] * fx;
                const int bot = (int)p10[k] * (256 - fx) + (int)p11[k] * fx;
                o[k] = (unsigned char)(((top * (256 - fy) + bot * fy) >> 16));
            }
        }
    }
}

}  // namespace

void bilateralRgbaDown(const unsigned char* src, unsigned char* dst,
                       int w, int h, int radius,
                       float sigmaSpace, float sigmaColor, int down) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    if (down <= 1) {
        bilateralRgba(src, dst, w, h, radius, sigmaSpace, sigmaColor);
        return;
    }
    int sw = 0, sh = 0;
    std::vector<unsigned char> small((size_t)((w + 1) / 2) * ((h + 1) / 2) * 4);
    boxDown2(src, &small[0], w, h, sw, sh);
    if (down > 2) {
        std::vector<unsigned char> s2((size_t)((sw + 1) / 2) * ((sh + 1) / 2) * 4);
        int sw2 = 0, sh2 = 0;
        boxDown2(&small[0], &s2[0], sw, sh, sw2, sh2);
        small.swap(s2);
        sw = sw2;
        sh = sh2;
    }
    std::vector<unsigned char> smallOut((size_t)sw * sh * 4);
    const int rLow = (radius / down) < 1 ? 1 : (radius / down);
    const float sLow = sigmaSpace / (float)down;
    bilateralRgba(&small[0], &smallOut[0], sw, sh, rLow, sLow, sigmaColor);
    upscaleBilinear(&smallOut[0], sw, sh, dst, w, h);
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4) dst[i] = src[i];
}

void mixRgba(const unsigned char* src, const unsigned char* smooth,
             unsigned char* dst, int w, int h, float t) {
    if (!src || !smooth || !dst || w <= 0 || h <= 0) return;
    if (t <= 0.f) {
        if (dst != src) memcpy(dst, src, (size_t)w * h * 4);
        return;
    }
    if (t >= 1.f) {
        if (dst != smooth) memcpy(dst, smooth, (size_t)w * h * 4);
        return;
    }
    const int ti = (int)(t * 256.0f + 0.5f);
    const int inv = 256 - ti;
    const size_t n = (size_t)w * h * 4;
    for (size_t i = 0; i < n; i += 4) {
        // 两项系数和为 256，结果天然落在 [0,255]，不需要 clamp
        dst[i + 0] = (unsigned char)(((int)src[i + 0] * inv + (int)smooth[i + 0] * ti + 128) >> 8);
        dst[i + 1] = (unsigned char)(((int)src[i + 1] * inv + (int)smooth[i + 1] * ti + 128) >> 8);
        dst[i + 2] = (unsigned char)(((int)src[i + 2] * inv + (int)smooth[i + 2] * ti + 128) >> 8);
        dst[i + 3] = src[i + 3];
    }
}

// ===========================================================================
// 改进双边（低频参照 + 浮点）
// ===========================================================================

namespace {

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 向下对齐到 32 —— boxPlaneH/V 的重算周期 kFlush。
// 用途：ROI 的起点对齐到它之后，ROI 版与整帧版的重算点才会落在同一批全局
// 坐标上，两边浮点累加的相位一致，结果才能逐位相同（否则会差出灰阶）。
inline int alignDown32(int v) { return (v > 0) ? ((v / 32) * 32) : 0; }

// 按索引区间并行（既用于「按行」，也用于「按列」）
template <typename Fn>
void runRange(int a, int b, int threads, Fn fn) {
    const int n = b - a;
    if (n <= 0) return;
    const int nt = effectiveThreads(threads, n);
    if (nt <= 1) {
        fn(a, b);
        return;
    }
    // 持久线程池（3.1.21）。旧的写法是每趟 new 一组 std::thread 再 join，
    // 而一帧磨皮要跑 21 趟 —— 实测光建线程就 6.08 ms/帧（整帧才 18.28 ms）。
    beautycore::parallelRows(a, b, nt, [&fn](int s, int e) { fn(s, e); });
}

// 重算点按局部坐标 x % kFlush == 0。这要求**调用方把 ROI 起点对齐到 kFlush 的倍数**
// （见 boxRgbRoi 里的 alignDown32）：ROI 版与整帧版的重算点必须落在同一批全局
// 坐标上，否则两边浮点累加的相位不同，四舍五入到 uint8 时会差出灰阶。
// 单平面水平盒式滤波。滑动窗，O(1)/像素，边界 clamp。
// 用滑动窗而不是「每个像素扫 2r+1 次」：参照半径 8 时后者要 17 次读，
// 1080p 的 ROI 上是 450 万次加法，白给。
void boxPlaneH(const float* src, float* dst, int bw, int bh, int r, int threads) {
    const float inv = 1.0f / (float)(2 * r + 1);
    // 每 kFlush 个像素**重算一次**窗口和，而不是一路滑到底。
    //
    // 为什么必须重算：滑动窗是 s += add - rem 的反复累加，float 的舍入误差会
    // 一路累积。ROI 版与全帧版的 bw 不同 → 累加步数不同 → 舍入误差不同，
    // 实测在 ROI 边界上差出 1 个灰阶（verify_roi 报 maxdiff=1）。差 1 看着小，
    // 但它意味着「ROI 内结果与整帧逐字节相同」这条性质不成立 —— 真机上就是
    // 掩码边缘一圈接缝。重算把误差压到 32 步以内（≈5e-4 灰阶），代价是
    // 每 32 像素多 (2r+1) 次加法，摊销约 0.28 次/像素，测不出来。
    const int kFlush = 32;
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const float* row = src + (size_t)y * bw;
            float* orow = dst + (size_t)y * bw;
            float s = 0.0f;
            for (int x = 0; x < bw; x++) {
                if ((x % kFlush) == 0) {
                    s = 0.0f;
                    for (int k = -r; k <= r; k++)
                        s += row[clampi(x + k, 0, bw - 1)];
                }
                orow[x] = s * inv;
                const int add = clampi(x + r + 1, 0, bw - 1);
                const int rem = clampi(x - r, 0, bw - 1);
                s += row[add] - row[rem];
            }
        }
    });
}

// 单平面垂直盒式滤波。**按列分块**：逐列扫的话每读 4 字节要跳一整行
// （1080p 的 ROI 上一行 1.8 KB），缓存全废 —— 实测这一步单独就吃掉 1.5ms 以上。
// 改成一次处理 32 列（= 128 字节 = 2 条 cache line），行内是连续的，
// 每行的有效载荷从 4 字节涨到 128 字节，涨 32 倍。
void boxPlaneV(const float* src, float* dst, int bw, int bh, int r, int threads) {
    const float inv = 1.0f / (float)(2 * r + 1);
    const int kBlock = 32;
    const int kFlush = 32;   // 同 boxPlaneH：定期重算列和，防 float 误差累积
    const int nblk = (bw + kBlock - 1) / kBlock;
    runRange(0, nblk, threads, [&](int bBegin, int bEnd) {
        std::vector<float> sum((size_t)kBlock);
        for (int b = bBegin; b < bEnd; b++) {
            const int x0 = b * kBlock;
            const int n = ((x0 + kBlock <= bw) ? kBlock : (bw - x0));
            for (int y = 0; y < bh; y++) {
                if ((y % kFlush) == 0) {
                    // 各列重算窗口和（y 处的窗口 [y-r, y+r]，两端 clamp）
                    for (int i = 0; i < n; i++) {
                        const float* col = src + x0 + i;
                        float s = 0.0f;
                        for (int k = -r; k <= r; k++)
                            s += col[(size_t)clampi(y + k, 0, bh - 1) * bw];
                        sum[(size_t)i] = s;
                    }
                } else {
                    const int add = clampi(y + r, 0, bh - 1);
                    const int rem = clampi(y - 1 - r, 0, bh - 1);
                    const float* pa = src + (size_t)add * bw + x0;
                    const float* pr = src + (size_t)rem * bw + x0;
                    for (int i = 0; i < n; i++) sum[(size_t)i] += pa[i] - pr[i];
                }
                float* pd = dst + (size_t)y * bw + x0;
                for (int i = 0; i < n; i++) pd[i] = sum[(size_t)i] * inv;
            }
        }
    });
}

// 三平面双边一轮：dst = bilateral(src)，权重判据带可选参照。
// ref 为空指针 = 绝对差（用于「只换成浮点、不做参照」的对照）。
void bilateralPlanesF(const float* sR, const float* sG, const float* sB,
                      float* dR, float* dG, float* dB,
                      const float* refR, const float* refG, const float* refB,
                      int bw, int bh, int radius,
                      const int* sLut, const int* cLut, int threads) {
    const int n = radius * 2 + 1;
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const int ny0 = (y - radius < 0) ? 0 : y - radius;
            const int ny1 = (y + radius >= bh) ? bh - 1 : y + radius;
            const size_t rowBase = (size_t)y * bw;
            for (int x = 0; x < bw; x++) {
                const size_t i = rowBase + x;
                const float cr = sR[i], cg = sG[i], cb = sB[i];
                const float rr = refR ? refR[i] : 0.0f;
                const float rg = refR ? refG[i] : 0.0f;
                const float rb = refR ? refB[i] : 0.0f;
                const int nx0 = (x - radius < 0) ? 0 : x - radius;
                const int nx1 = (x + radius >= bw) ? bw - 1 : x + radius;

                float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f, wsum = 0.0f;
                for (int yy = ny0; yy <= ny1; yy++) {
                    const int* srow = sLut + (size_t)(yy - y + radius) * n;
                    const size_t base = (size_t)yy * bw;
                    for (int xx = nx0; xx <= nx1; xx++) {
                        const size_t j = base + xx;
                        float d0 = sR[j] - cr;
                        float d1 = sG[j] - cg;
                        float d2 = sB[j] - cb;
                        if (refR) {
                            // 减掉大半径参照的差：只剩「局部起伏」
                            d0 -= (refR[j] - rr);
                            d1 -= (refG[j] - rg);
                            d2 -= (refB[j] - rb);
                        }
                        const float l1 = std::fabs(d0) + std::fabs(d1) +
                                         std::fabs(d2);
                        int idx = (int)(l1 + 0.5f);
                        if (idx > 767) idx = 767;
                        const int cw = cLut[idx];
                        if (cw == 0) continue;      // 颜色差太大：不参与
                        const float wt = (float)((srow[xx - x + radius] * cw) >> 10);
                        if (wt <= 0.0f) continue;
                        sumR += sR[j] * wt;
                        sumG += sG[j] * wt;
                        sumB += sB[j] * wt;
                        wsum += wt;
                    }
                }
                if (wsum > 0.0f) {
                    const float invw = 1.0f / wsum;
                    dR[i] = sumR * invw;
                    dG[i] = sumG * invw;
                    dB[i] = sumB * invw;
                } else {
                    dR[i] = cr; dG[i] = cg; dB[i] = cb;
                }
            }
        }
    });
}

}  // namespace

LowFreqBuf lowFreqRefRoi(const unsigned char* src, int w, int h,
                         const BilateralRefParams& p, const RoiRect& roi,
                         int threads, std::vector<float>& scratch) {
    LowFreqBuf out;
    if (!src || w <= 0 || h <= 0 || roi.empty()) return out;

    const int r = (p.radius < 1) ? 1 : p.radius;
    const int iters = (p.iters < 1) ? 1 : p.iters;
    const int rr = (p.refRadius > 0) ? p.refRadius : 0;

    // 外扩：迭代 iters 轮各向外扩散 r 像素；参照还要在最外圈被读到，再扩 rr。
    // 只在 roi 内取用，所以外扩那圈的误差不可见。
    const int grow = r * iters + rr;
    const int bx0 = clampi(roi.x0 - grow, 0, w);
    const int bx1 = clampi(roi.x1 + grow, 0, w);
    const int by0 = clampi(roi.y0 - grow, 0, h);
    const int by1 = clampi(roi.y1 + grow, 0, h);
    const int bw = bx1 - bx0;
    const int bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return out;

    const size_t np = (size_t)bw * (size_t)bh;
    // 9 个平面：cur(3) / alt(3) / ref(3)
    if (scratch.size() < np * 9) scratch.resize(np * 9);
    float* cur[3] = {&scratch[0], &scratch[np], &scratch[np * 2]};
    float* alt[3] = {&scratch[np * 3], &scratch[np * 4], &scratch[np * 5]};
    float* ref[3] = {&scratch[np * 6], &scratch[np * 7], &scratch[np * 8]};

    // 1) 取出 RGB 到浮点平面（uint8 → float 一次转换，之后全 float 迭代）
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* row = src + (size_t)(by0 + y) * w * 4;
            for (int k = 0; k < 3; k++) {
                float* dstp = cur[k] + (size_t)y * bw;
                const unsigned char* p = row + (size_t)bx0 * 4 + k;
                for (int x = 0; x < bw; x++, p += 4) dstp[x] = (float)*p;
            }
        }
    });

    // 2) 参照：大半径盒式（两趟可分离）
    if (rr > 0) {
        for (int k = 0; k < 3; k++) {
            boxPlaneH(cur[k], alt[k], bw, bh, rr, threads);
            boxPlaneV(alt[k], ref[k], bw, bh, rr, threads);
        }
    }

    // 3) 迭代
    std::vector<int> sLut, cLut;
    buildSpatialLut(sLut, r, p.sigmaSpace);
    buildColorLut(cLut, p.sigmaColor);
    const int* sBase = &sLut[0];
    const int* cBase = &cLut[0];

    for (int it = 0; it < iters; it++) {
        bilateralPlanesF(cur[0], cur[1], cur[2], alt[0], alt[1], alt[2],
                         rr > 0 ? ref[0] : nullptr,
                         rr > 0 ? ref[1] : nullptr,
                         rr > 0 ? ref[2] : nullptr,
                         bw, bh, r, sBase, cBase, threads);
        for (int k = 0; k < 3; k++) {
            float* t = cur[k];
            cur[k] = alt[k];
            alt[k] = t;
        }
    }

    out.r = cur[0]; out.g = cur[1]; out.b = cur[2];
    out.x0 = bx0; out.y0 = by0; out.bw = bw; out.bh = bh;
    return out;
}

LowFreqBuf boxRgbRoi(const unsigned char* src, int w, int h,
                     const BoxLowParams& p, const RoiRect& roi,
                     int threads, std::vector<float>& scratch) {
    LowFreqBuf out;
    if (!src || w <= 0 || h <= 0 || roi.empty()) return out;

    const int r = (p.radius < 1) ? 1 : p.radius;
    const int iters = (p.iters < 1) ? 1 : p.iters;

    // 每轮迭代向外扩散 r 像素；只在 roi 内取用，外扩那圈不可见。
    //
    // +2 是安全余量，不是随手加的：外扩区**边界上的 clamp**（复制边缘像素）
    // 不是真值，这个误差经过 iters 轮 box 会向外传播 r×iters 像素 —— 恰好触及
    // roi 边界。实测只外扩 r×iters 时 verify_roi 在小图上报 maxdiff=1（1080p
    // 侥幸通过是因为 ROI 离画面边缘更远），补 2 像素后三档全过。
    // 起点**下对齐到 32**（kFlush）：让 ROI 版与整帧版的重算点落在同一批全局坐标上。
    // 不对齐的话两边浮点累加相位不同，四舍五入时会差出灰阶 —— 实测 640×394 上
    // maxdiff=1，真机就是掩码边缘一圈接缝。多算的 ≤31 行列成本可忽略。
    const int grow = r * iters + 2;
    const int bx0 = alignDown32(clampi(roi.x0 - grow, 0, w));
    const int bx1 = clampi(roi.x1 + grow, 0, w);
    const int by0 = alignDown32(clampi(roi.y0 - grow, 0, h));
    const int by1 = clampi(roi.y1 + grow, 0, h);
    const int bw = bx1 - bx0;
    const int bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return out;

    const size_t np = (size_t)bw * (size_t)bh;
    // 6 个平面：cur(3) / tmp(3)
    if (scratch.size() < np * 6) scratch.resize(np * 6);
    float* cur[3] = {&scratch[0], &scratch[np], &scratch[np * 2]};
    float* tmp[3] = {&scratch[np * 3], &scratch[np * 4], &scratch[np * 5]};

    // 1) 取出 RGB 到浮点平面
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* row = src + (size_t)(by0 + y) * w * 4;
            for (int k = 0; k < 3; k++) {
                float* dstp = cur[k] + (size_t)y * bw;
                const unsigned char* q = row + (size_t)bx0 * 4 + k;
                for (int x = 0; x < bw; x++, q += 4) dstp[x] = (float)*q;
            }
        }
    });

    for (int it = 0; it < iters; it++) {
        for (int k = 0; k < 3; k++) {
            boxPlaneH(cur[k], tmp[k], bw, bh, r, threads);
            boxPlaneV(tmp[k], cur[k], bw, bh, r, threads);
        }
    }

    out.r = cur[0]; out.g = cur[1]; out.b = cur[2];
    out.x0 = bx0; out.y0 = by0; out.bw = bw; out.bh = bh;
    return out;
}

LowFreqBuf boxLumaRoi(const unsigned char* rgba, int w, int h, int radius,
                      const RoiRect& roi, int threads,
                      std::vector<float>& scratch) {
    LowFreqBuf out;
    if (!rgba || w <= 0 || h <= 0 || roi.empty()) return out;
    const int r = (radius < 1) ? 1 : radius;

    // 两趟可分离，每趟各向外扩 r → 合计 2r。只在 roi 内取用，外圈误差不可见。
    const int grow = r * 2;
    const int bx0 = clampi(roi.x0 - grow, 0, w);
    const int bx1 = clampi(roi.x1 + grow, 0, w);
    const int by0 = clampi(roi.y0 - grow, 0, h);
    const int by1 = clampi(roi.y1 + grow, 0, h);
    const int bw = bx1 - bx0;
    const int bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return out;

    const size_t np = (size_t)bw * (size_t)bh;
    if (scratch.size() < np * 2) scratch.resize(np * 2);
    float* lum = &scratch[0];
    float* tmp = &scratch[np];

    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* row = rgba + (size_t)(by0 + y) * w * 4;
            float* o = lum + (size_t)y * bw;
            const unsigned char* p = row + (size_t)bx0 * 4;
            for (int x = 0; x < bw; x++, p += 4) {
                // 整数权重近似 luma（0..255 尺度），够 clarity 用，省两次浮点乘
                o[x] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]);
            }
        }
    });

    boxPlaneH(lum, tmp, bw, bh, r, threads);
    boxPlaneV(tmp, lum, bw, bh, r, threads);

    out.r = lum; out.g = lum; out.b = lum;   // 灰度：三指针同平面
    out.x0 = bx0; out.y0 = by0; out.bw = bw; out.bh = bh;
    return out;
}

// ---------------------------------------------------------------------------
// surface3 —— 三层表面模糊（C22）
// ---------------------------------------------------------------------------

// 表面模糊的「保留原值」软膝：|C−b| ≤ thr 全取低频，≥ 2·thr 全取原值。
// thr<=0 时退化成普通盒式（= 无阈值，用于对照）。
inline float surfaceKeep(float c, float b, float thr) {
    if (thr <= 0.0f) return b;
    const float a = fabsf(c - b);
    if (a <= thr) return b;
    if (a >= 2.0f * thr) return c;
    const float w = (a - thr) / thr;
    return b + (c - b) * w;
}

void surface3Rgb(const unsigned char* src, unsigned char* dst, int w, int h,
                 const unsigned char* mask, const unsigned char* protect,
                 const Surface3Params& p, const RoiRect& roi, int threads,
                 std::vector<float>& scratch1, std::vector<float>& scratch2) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty()) return;

    BoxLowParams bp1;
    bp1.radius = (p.r1 < 1) ? 1 : p.r1;
    bp1.iters = (p.it1 < 1) ? 1 : p.it1;
    BoxLowParams bp2;
    bp2.radius = (p.r2 < 1) ? 1 : p.r2;
    bp2.iters = (p.it2 < 1) ? 1 : p.it2;

    // 边界误差余量：boxRgbRoi 内部只外扩 r×iters+2，对 r=2 够用（C15 实测过），
    // 但 r=8/iters=2 时边界 clamp 的误差量级大得多，+2 不够 —— verify_roi 在
    // 640×394 上实测 maxdiff=1（单像素，x 落在 32 块末尾）。这里再外扩
    // r×iters 像素，保证 ROI 内（掩码内）的像素完全落在误差传播带之外。
    // 全帧路径下 clamp 到画面边界，行为不变，所以 ROI 等价性成立。
    const int reach1 = bp1.radius * bp1.iters;
    const int reach2 = bp2.radius * bp2.iters;
    const int extra = (reach1 > reach2) ? reach1 : reach2;
    RoiRect ex = roi;
    ex.x0 = clampi(roi.x0 - extra, 0, w);
    ex.y0 = clampi(roi.y0 - extra, 0, h);
    ex.x1 = clampi(roi.x1 + extra, 0, w);
    ex.y1 = clampi(roi.y1 + extra, 0, h);

    const LowFreqBuf b1 = boxRgbRoi(src, w, h, bp1, ex, threads, scratch1);
    const LowFreqBuf b2 = boxRgbRoi(src, w, h, bp2, ex, threads, scratch2);
    if (!b1.valid() || !b2.valid()) return;

    const float I = (p.I < 0.0f) ? 0.0f : ((p.I > 1.0f) ? 1.0f : p.I);
    const float e = (p.e < 0.0f) ? 0.0f : ((p.e > 1.0f) ? 1.0f : p.e);
    const float thr1 = p.thr1;
    const float thr2 = p.thr2;
    const float eyeK = (p.protectK < 0.0f) ? 0.0f
                     : ((p.protectK > 100.0f) ? 1.0f : p.protectK / 100.0f);
    const float inv255 = 1.0f / 255.0f;

    runRange(0, h, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * w;
            const int py1 = y - b1.y0;
            const int py2 = y - b2.y0;
            const bool rowOk1 = (py1 >= 0 && py1 < b1.bh);
            const bool rowOk2 = (py2 >= 0 && py2 < b2.bh);
            for (int x = 0; x < w; x++) {
                const size_t o = (rowOff + (size_t)x) * 4;
                const int m = mask ? (int)mask[rowOff + (size_t)x] : 255;
                const int px1 = x - b1.x0;
                const int px2 = x - b2.x0;
                if (m == 0 || !rowOk1 || !rowOk2 ||
                    px1 < 0 || px1 >= b1.bw || px2 < 0 || px2 >= b2.bw) {
                    // 掩码外 / 低频层覆盖不到：逐字节还原（与 blendWithMask 同口径）
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                    dst[o + 3] = src[o + 3];
                    continue;
                }
                const size_t i1 = (size_t)py1 * b1.bw + (size_t)px1;
                const size_t i2 = (size_t)py2 * b2.bw + (size_t)px2;
                const float skin = (float)m * inv255;
                const float eye = protect
                    ? ((float)protect[rowOff + (size_t)x] * inv255 * eyeK)
                    : 0.0f;
                const float keepEye = 1.0f - eye;
                const float se = skin * e;

                const float* lb1[3] = {b1.r, b1.g, b1.b};
                const float* lb2[3] = {b2.r, b2.g, b2.b};
                for (int k = 0; k < 3; k++) {
                    const float c = (float)src[o + (size_t)k];
                    const float v1f = lb1[k][i1];
                    const float v2f = lb2[k][i2];
                    // 表面模糊近似：颜色差超过阈值 → 认为跨过细节/边缘，保留原值。
                    // 用**软膝**而不是硬分支：硬 `>thr ? c : b` 在阈值附近一跳一停，
                    // 会在 |C−b|≈thr 的纹理上造出新的台阶（实测 50 档细纹理
                    // 反而涨到 102.6%）。软膝 = 阈值内全取低频、2×阈值外全取原值、
                    // 中间线性过渡，与 blendWithMask 的 limitThr 同一套写法。
                    const float v1 = surfaceKeep(c, v1f, thr1);
                    const float v2 = surfaceKeep(c, v2f, thr2);

                    float even = c - v1 + v2;
                    // ① 先按皮肤掩码把 even 拉回原色 —— GLSL 第二级的第一行：
                    //      even = mix(C.rgb, even, skin_mask);
                    // 3.1.14 之前缺这一行，只在 0<skin<1 的羽化带/掩码边缘起作用，
                    // 表现为 b2 的贡献被高估（even 没有先被 skin 拉回 c）。
                    // 顺带落实 参考实现的 nonskin_blur_scale=0：skin→0 时 even→c、
                    // res→c，非肤区还原原色（m==0 的硬直通在上面已经拦掉了）。
                    even = c + (even - c) * skin;
                    even += (v2 - even) * se;          // ② mix(even, b2, skin·e)
                    const float res = c + (even - c) * I;
                    float outv = c + (res - c) * keepEye;
                    if (outv < 0.0f) outv = 0.0f;
                    if (outv > 255.0f) outv = 255.0f;
                    dst[o + (size_t)k] = (unsigned char)(outv + 0.5f);
                }
                dst[o + 3] = src[o + 3];
            }
        }
    });
}

// ---------------------------------------------------------------------------
// 【重建·待证】25 tap 表面模糊（C23 §3，第 2 批 #5/#6/#7）
// ---------------------------------------------------------------------------
//
// 证据等级见 beauty_smooth.h 的 Surface25Params 注释。这里只重复一句最要紧的：
// **25 tap 结构 / 环半径 {1,2,5} / yFactor 40-20 是【重建·待证】，不是【源码】。**
// 0a″ 合成图实验只验到「核尺度随画幅线性（σx∝W^0.98，四画幅 1.95/3.85/7.72/11.20）」，
// 环半径本身仍无独立实测证据。不许宣称"照抄完成"。

// 双线性采样单平面。x/y 为连续坐标，纹素中心在 (i+0.5)。
static inline float planeBilinear(const float* p, int w, int h, float x, float y) {
    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = clampi(x0, 0, w - 1);
    x1 = clampi(x1, 0, w - 1);
    y0 = clampi(y0, 0, h - 1);
    y1 = clampi(y1, 0, h - 1);
    const float a = p[(size_t)y0 * (size_t)w + (size_t)x0];
    const float b = p[(size_t)y0 * (size_t)w + (size_t)x1];
    const float c = p[(size_t)y1 * (size_t)w + (size_t)x0];
    const float d = p[(size_t)y1 * (size_t)w + (size_t)x1];
    const float ab = a + (b - a) * tx;
    const float cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

// 一趟 25 tap 表面模糊。四个必须照抄的细节（C23 §3.1）：
//   ① 空间权重恒为 1（盒式，不是高斯）—— 换高斯会让高频衰减曲线变形
//   ② 值域权重是二次抛物线截断 1−min(Δg²·yFactor, 1)
//   ③ 只测**绿**通道 —— 换亮度会让人脸肤色差异参与门控
//   ④ 中心点权重恒为 1，不参与门控 —— 加门控会让核在强边缘处退化
static void surfaceBlur25Pass(const float* sr, const float* sg, const float* sb,
                              float* dr, float* dg, float* db,
                              int w, int h, float sxPx, float syPx,
                              float yFactor, int threads) {
    // 8 个方向（45° 间隔）与 3 个环半径，都是常量，提到循环外
    static const float kDirX[8] = { 1.0f,  0.70710678f,  0.0f, -0.70710678f,
                                  -1.0f, -0.70710678f,  0.0f,  0.70710678f};
    static const float kDirY[8] = { 0.0f,  0.70710678f,  1.0f,  0.70710678f,
                                   0.0f, -0.70710678f, -1.0f, -0.70710678f};
    static const float kRing[3] = {1.0f, 2.0f, 5.0f};
    const float inv255 = 1.0f / 255.0f;

    runRange(0, h, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            for (int x = 0; x < w; x++) {
                const size_t i = (size_t)y * (size_t)w + (size_t)x;
                const float c0 = sg[i];
                float ar = sr[i], ag = c0, ab = sb[i], ws = 1.0f;
                for (int d = 0; d < 8; d++) {
                    const float dx = kDirX[d];
                    const float dy = kDirY[d];
                    for (int j = 0; j < 3; j++) {
                        const float rr = kRing[j];
                        const float px = (float)x + 0.5f + dx * sxPx * rr;
                        const float py = (float)y + 0.5f + dy * syPx * rr;
                        const float kg = planeBilinear(sg, w, h, px, py);
                        const float dif = (kg - c0) * inv255;   // 归一化色差
                        float wt = 1.0f - dif * dif * yFactor;
                        if (wt < 0.0f) wt = 0.0f;
                        ar += wt * planeBilinear(sr, w, h, px, py);
                        ag += wt * kg;
                        ab += wt * planeBilinear(sb, w, h, px, py);
                        ws += wt;
                    }
                }
                const float inv = 1.0f / ws;
                dr[i] = ar * inv;
                dg[i] = ag * inv;
                db[i] = ab * inv;
            }
        }
    });
}

void surface3Rgb25(const unsigned char* src, unsigned char* dst, int w, int h,
                   const unsigned char* mask, const unsigned char* protect,
                   const Surface25Params& p, const RoiRect& roi, int threads,
                   std::vector<float>& scratch) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty()) return;

    // 处理分辨率（#6 半分辨率）。注意 UV 与分辨率无关，所以半分辨率**不改变
    // 核在画面上的物理尺寸**（环1 仍是 sx·W 个显示像素），只改变采样网格：
    // 网格更粗、双线性插值会额外柔化一点。别把"半分辨率"误解成"核减半"。
    int ww = w, wh = h;
    if (p.halfRes) {
        ww = (w + 1) / 2;
        wh = (h + 1) / 2;
        if (ww < 4 || wh < 4) { ww = w; wh = h; }
    }
    const float sxPx = p.sx * (float)ww;
    const float syPx = p.sy * (float)wh;

    const size_t np = (size_t)ww * (size_t)wh;
    // 6 个平面：src(3) + blur1(3)。pass1 读完 src 后，src 的槽位改当 blur2 用。
    if (scratch.size() < np * 6) scratch.resize(np * 6);
    float* sr = &scratch[0];
    float* sg = sr + np;
    float* sb = sg + np;
    float* b1r = sb + np;
    float* b1g = b1r + np;
    float* b1b = b1g + np;
    float* b2r = sr;          // 复用 src 槽位
    float* b2g = sg;
    float* b2b = sb;

    const float inv255 = 1.0f / 255.0f;

    // ---- 下采样到处理分辨率 ------------------------------------------------
    if (ww == w && wh == h) {
        runRange(0, h, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                const unsigned char* row = src + (size_t)y * (size_t)w * 4;
                const size_t o = (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++) {
                    const size_t q = (size_t)x * 4;
                    sr[o + x] = (float)row[q + 0];
                    sg[o + x] = (float)row[q + 1];
                    sb[o + x] = (float)row[q + 2];
                }
            }
        });
    } else {
        // 2×2 盒式【推断】：C23 §7 记「半分辨率的下采样滤波未定」。
        // 选盒式而不是双线性取样：双线性在 0.5 缩放下只取到 2×2 的加权组合，
        // 对高频更容易出摩尔纹；盒式是正经的 4 点平均，标【推断】不标【源码】。
        runRange(0, wh, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                const int y0 = y * 2;
                const int y1 = (y0 + 1 < h) ? (y0 + 1) : y0;
                const unsigned char* r0 = src + (size_t)y0 * (size_t)w * 4;
                const unsigned char* r1 = src + (size_t)y1 * (size_t)w * 4;
                const size_t o = (size_t)y * (size_t)ww;
                for (int x = 0; x < ww; x++) {
                    const int x0 = x * 2;
                    const int x1 = (x0 + 1 < w) ? (x0 + 1) : x0;
                    const size_t i0 = (size_t)x0 * 4;
                    const size_t i1 = (size_t)x1 * 4;
                    sr[o + x] = (float)(r0[i0] + r0[i1] + r1[i0] + r1[i1]) * 0.25f;
                    sg[o + x] = (float)(r0[i0 + 1] + r0[i1 + 1]
                                      + r1[i0 + 1] + r1[i1 + 1]) * 0.25f;
                    sb[o + x] = (float)(r0[i0 + 2] + r0[i1 + 2]
                                      + r1[i0 + 2] + r1[i1 + 2]) * 0.25f;
                }
            }
        });
    }

    // ---- 串行两趟（#7）：pass1 yFactor 容限窄，pass2 容限宽，读 pass1 的输出
    surfaceBlur25Pass(sr, sg, sb, b1r, b1g, b1b, ww, wh, sxPx, syPx,
                      p.yFactor1, threads);
    surfaceBlur25Pass(b1r, b1g, b1b, b2r, b2g, b2b, ww, wh, sxPx, syPx,
                      p.yFactor2, threads);

    // ---- 合成（与 surface3Rgb 逐行同口径，只是 v1/v2 来自 25 tap 层）-------
    const float I = (p.I < 0.0f) ? 0.0f : ((p.I > 1.0f) ? 1.0f : p.I);
    const float e = (p.e < 0.0f) ? 0.0f : ((p.e > 1.0f) ? 1.0f : p.e);
    const float eyeK = (p.protectK < 0.0f) ? 0.0f
                     : ((p.protectK > 100.0f) ? 1.0f : p.protectK / 100.0f);
    const float sxw = (float)ww / (float)w;
    const float syw = (float)wh / (float)h;

    runRange(0, h, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * (size_t)w;
            const float vy = ((float)y + 0.5f) * syw;
            for (int x = 0; x < w; x++) {
                const size_t o = (rowOff + (size_t)x) * 4;
                const int m = mask ? (int)mask[rowOff + (size_t)x] : 255;
                if (m == 0) {
                    // 掩码外逐字节还原（与 surface3Rgb / blendWithMask 同口径）
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                    dst[o + 3] = src[o + 3];
                    continue;
                }
                const float vx = ((float)x + 0.5f) * sxw;
                const float skin = (float)m * inv255;
                const float eye = protect
                    ? ((float)protect[rowOff + (size_t)x] * inv255 * eyeK)
                    : 0.0f;
                const float keepEye = 1.0f - eye;
                const float se = skin * e;

                const float v1[3] = {planeBilinear(b1r, ww, wh, vx, vy),
                                     planeBilinear(b1g, ww, wh, vx, vy),
                                     planeBilinear(b1b, ww, wh, vx, vy)};
                const float v2[3] = {planeBilinear(b2r, ww, wh, vx, vy),
                                     planeBilinear(b2g, ww, wh, vx, vy),
                                     planeBilinear(b2b, ww, wh, vx, vy)};
                for (int k = 0; k < 3; k++) {
                    const float c = (float)src[o + (size_t)k];
                    float even = c - v1[k] + v2[k];
                    even = c + (even - c) * skin;      // ① mix(C, even, skin)
                    even += (v2[k] - even) * se;       // ② mix(even, b2, skin·e)
                    const float res = c + (even - c) * I;
                    float outv = c + (res - c) * keepEye;
                    if (outv < 0.0f) outv = 0.0f;
                    if (outv > 255.0f) outv = 255.0f;
                    dst[o + (size_t)k] = (unsigned char)(outv + 0.5f);
                }
                dst[o + 3] = src[o + 3];
            }
        }
    });
}

// ---------------------------------------------------------------------------
// 双尺度带通 + 振幅门控（C23 §11/§12，第 2 批-B）
// ---------------------------------------------------------------------------
//
// 为什么换掉 25 tap —— 三条实测证据，不是"觉得它不好"：
//   §10.4 频响：25 tap 是**双凹陷**（8–13 与 32–60，中间反弹），参考实现是单凹陷
//          （13–20）。稀疏环形核（8 向 × 3 环）的径向频响是若干贝塞尔函数叠加，
//          旁瓣是结构性的，调 yFactor 消不掉。
//   §11.4 振幅：参考实现在 P=8 处 A=2→A=16 增益 0.289→0.579（翻倍，强纹理自动
//          放过），25 tap 是 0.396→0.415（1.05 倍，强弱一视同仁）→ 边缘被磨。
//   §11.5 根因：`even = C − B1 + B2` 是带通，去掉的频段由 B1/B2 的**空间尺度差**
//          决定。25 tap 的 B1/B2 用同一个核、只有 yFactor 不同 → 尺度差为 0，
//          带通中心只能靠值域门控挪，落在 12 px 而不是 参考实现的 24 px。
//
// 所以 B1、B2 必须是两个不同尺度的模糊。这一版用 3 次盒式近似高斯（O(1)/像素，
// 积分窗滑动），比 25 tap 的 24 次双线性采样便宜一个数量级。
//
// 全部常数都是【拟合·传递函数】—— 目标是对上 参考实现的**光栅表**，不是金标 RMSE。
// 详见 C23 §12，那里有扫描表和对不上的那一格（判据 2）的说明。

// 盒宽：按处理宽度的比例换算成奇数。UV 固定（C23 §3.2），所以随画幅缩放。
static inline int bandBoxW(int ww, float frac, int minW) {
    int k = (int)(frac * (float)ww + 0.5f);
    if (k < minW) k = minW;
    if ((k & 1) == 0) k++;
    return k;
}

// 多平面同时做水平盒式 —— 一次 runRange 处理 n 个平面。
//
// 为什么需要：每帧要跑 3 组平面 × 3 趟 × 2 方向 = 18 次盒式。如果每次都单独
// 起线程（默认 8 条），光 std::thread 的创建/回收就吃掉 3~4 ms —— 1080p 上
// 实测占总耗时 20%。把 3 个通道塞进同一个并行区域，线程数不变、创建次数降到 1/3。
static void bandBoxH(const float* const* src, float* const* dst, int n,
                     int bw, int bh, int r, int threads) {
    const float inv = 1.0f / (float)(2 * r + 1);
    const int kFlush = 32;    // 同 boxPlaneH：定期重算窗口和，防 float 误差累积
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            for (int p = 0; p < n; p++) {
                const float* row = src[p] + (size_t)y * bw;
                float* orow = dst[p] + (size_t)y * bw;
                float s = 0.0f;
                for (int x = 0; x < bw; x++) {
                    if ((x % kFlush) == 0) {
                        s = 0.0f;
                        for (int k = -r; k <= r; k++)
                            s += row[clampi(x + k, 0, bw - 1)];
                    }
                    orow[x] = s * inv;
                    s += row[clampi(x + r + 1, 0, bw - 1)]
                       - row[clampi(x - r, 0, bw - 1)];
                }
            }
        }
    });
}

// 多平面同时做垂直盒式。分块策略与 boxPlaneV 相同（32 列一块，行内连续）。
static void bandBoxV(const float* const* src, float* const* dst, int n,
                     int bw, int bh, int r, int threads) {
    const float inv = 1.0f / (float)(2 * r + 1);
    const int kBlock = 32;
    const int kFlush = 32;
    const int nblk = (bw + kBlock - 1) / kBlock;
    runRange(0, nblk, threads, [&](int bBegin, int bEnd) {
        std::vector<float> sum((size_t)kBlock * (size_t)n);
        for (int b = bBegin; b < bEnd; b++) {
            const int x0 = b * kBlock;
            const int nn = ((x0 + kBlock <= bw) ? kBlock : (bw - x0));
            for (int y = 0; y < bh; y++) {
                if ((y % kFlush) == 0) {
                    for (int p = 0; p < n; p++) {
                        float* ps = &sum[(size_t)p * kBlock];
                        for (int i = 0; i < nn; i++) {
                            const float* col = src[p] + x0 + i;
                            float s = 0.0f;
                            for (int k = -r; k <= r; k++)
                                s += col[(size_t)clampi(y + k, 0, bh - 1) * bw];
                            ps[i] = s;
                        }
                    }
                } else {
                    const int add = clampi(y + r, 0, bh - 1);
                    const int rem = clampi(y - 1 - r, 0, bh - 1);
                    for (int p = 0; p < n; p++) {
                        const float* pa = src[p] + (size_t)add * bw + x0;
                        const float* pr = src[p] + (size_t)rem * bw + x0;
                        float* ps = &sum[(size_t)p * kBlock];
                        for (int i = 0; i < nn; i++) ps[i] += pa[i] - pr[i];
                    }
                }
                for (int p = 0; p < n; p++) {
                    const float* ps = &sum[(size_t)p * kBlock];
                    float* pd = dst[p] + (size_t)y * bw + x0;
                    for (int i = 0; i < nn; i++) pd[i] = ps[i] * inv;
                }
            }
        }
    });
}

// n 个平面同时迭代 N 次盒式。acc 初值必须已经是源数据。
static void bandBoxN(float* const* acc, float* const* tmp, int n,
                     int bw, int bh, int r, int passes, int threads) {
    for (int i = 0; i < passes; i++) {
        bandBoxH((const float* const*)acc, tmp, n, bw, bh, r, threads);
        bandBoxV((const float* const*)tmp, acc, n, bw, bh, r, threads);
    }
}

void surface3RgbBand(const unsigned char* src, unsigned char* dst, int w, int h,
                     const unsigned char* mask, const unsigned char* protect,
                     const BandParams& p, const RoiRect& roi, int threads,
                     std::vector<float>& scratch) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty()) return;

    int ww = w, wh = h;
    if (p.halfRes) {
        ww = (w + 1) / 2;
        wh = (h + 1) / 2;
        if (ww < 8 || wh < 8) { ww = w; wh = h; }
    }

    const int w1 = bandBoxW(ww, p.f1, 3);
    int       w2 = bandBoxW(ww, p.f2, 5);
    if (w2 <= w1 + 1) w2 = w1 + 2;          // 保证 B2 确实比 B1 宽
    const int wa = bandBoxW(ww, p.fa, 3);
    const int r1 = (w1 - 1) / 2;
    const int r2 = (w2 - 1) / 2;
    const int ra = (wa - 1) / 2;
    const int nPass = (p.passes < 1) ? 1 : ((p.passes > 6) ? 6 : p.passes);

    const size_t np = (size_t)ww * (size_t)wh;
    // 平面：src(3) + b1(3) + b2(3) + amp(1) + tmp(3) = 13，留 16
    if (scratch.size() < np * 16) scratch.resize(np * 16);
    float* sr  = &scratch[0];
    float* sg  = sr + np;
    float* sb  = sg + np;
    float* b1r = sb + np;
    float* b1g = b1r + np;
    float* b1b = b1g + np;
    float* b2r = b1b + np;
    float* b2g = b2r + np;
    float* b2b = b2g + np;
    float* amp = b2b + np;
    float* tm0 = amp + np;
    float* tm1 = tm0 + np;
    float* tm2 = tm1 + np;

    // 小画幅（640×394 半分辨率只有 63k 像素）时线程创建比计算还贵，退回单线程。
    const int bt = (np >= 150000) ? threads : 1;
    float* srcp[3] = {sr, sg, sb};
    float* b1p[3]  = {b1r, b1g, b1b};
    float* b2p[3]  = {b2r, b2g, b2b};
    float* tmpp[3] = {tm0, tm1, tm2};

    const float inv255 = 1.0f / 255.0f;

    // ---- 下采样到处理分辨率（2×2 盒式，与 25 tap 路径同口径【推断】）------
    if (ww == w && wh == h) {
        runRange(0, h, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                const unsigned char* row = src + (size_t)y * (size_t)w * 4;
                const size_t o = (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++) {
                    const size_t q = (size_t)x * 4;
                    sr[o + x] = (float)row[q + 0];
                    sg[o + x] = (float)row[q + 1];
                    sb[o + x] = (float)row[q + 2];
                }
            }
        });
    } else {
        runRange(0, wh, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                const int y0 = y * 2;
                const int y1 = (y0 + 1 < h) ? (y0 + 1) : y0;
                const unsigned char* r0 = src + (size_t)y0 * (size_t)w * 4;
                const unsigned char* r1 = src + (size_t)y1 * (size_t)w * 4;
                const size_t o = (size_t)y * (size_t)ww;
                for (int x = 0; x < ww; x++) {
                    const int x0 = x * 2;
                    const int x1 = (x0 + 1 < w) ? (x0 + 1) : x0;
                    const size_t i0 = (size_t)x0 * 4;
                    const size_t i1 = (size_t)x1 * 4;
                    sr[o + x] = (float)(r0[i0] + r0[i1] + r1[i0] + r1[i1]) * 0.25f;
                    sg[o + x] = (float)(r0[i0 + 1] + r0[i1 + 1]
                                      + r1[i0 + 1] + r1[i1 + 1]) * 0.25f;
                    sb[o + x] = (float)(r0[i0 + 2] + r0[i1 + 2]
                                      + r1[i0 + 2] + r1[i1 + 2]) * 0.25f;
                }
            }
        });
    }

    // ---- B1 = 窄模糊，B2 = 宽模糊（两个不同尺度，这是 §11.5 的核心）------
    // 先把 src 拷进累加器再迭代（bandBoxN 的 acc 初值必须是 src）。
    for (int k = 0; k < 3; k++) memcpy(b1p[k], srcp[k], np * sizeof(float));
    bandBoxN(b1p, tmpp, 3, ww, wh, r1, nPass, bt);

    for (int k = 0; k < 3; k++) memcpy(b2p[k], srcp[k], np * sizeof(float));
    bandBoxN(b2p, tmpp, 3, ww, wh, r2, nPass, bt);

    // ---- 振幅代理 a = blur(|C.g − B2.g|)，只测绿通道（同 §3.1 ③）---------
    // 为什么用 |C−B2| 而不是 |B2−B1|：后者在高频处两者都≈0，相减也≈0，
    // 测不到高频振幅 —— §12.3 的对照表里 proxy=d 在 P=8 的振幅依赖是 1.0x，
    // 等于门控失效。|C−B2| 在整个频段都随振幅线性增长。
    runRange(0, wh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t o = (size_t)y * (size_t)ww;
            for (int x = 0; x < ww; x++) {
                float d = sg[o + x] - b2g[o + x];
                amp[o + x] = (d < 0.0f) ? -d : d;
            }
        }
    });
    float* ampp[1] = {amp};
    bandBoxN(ampp, tmpp, 1, ww, wh, ra, nPass, bt);

    // ---- 合成（与 surface3Rgb25 逐行同口径，多一步 ③ 振幅门控）----------
    const float I = (p.I < 0.0f) ? 0.0f : ((p.I > 1.0f) ? 1.0f : p.I);
    const float e = (p.e < 0.0f) ? 0.0f : ((p.e > 1.0f) ? 1.0f : p.e);
    const float eyeK = (p.protectK < 0.0f) ? 0.0f
                     : ((p.protectK > 100.0f) ? 1.0f : p.protectK / 100.0f);
    const float invT = (p.T > 1e-3f) ? (1.0f / p.T) : 0.0f;
    // 门控指数：p=2 走乘方（无 pow 开销），否则用 powf。逐像素只算一次。
    const float gwp = (p.gwp < 1.0f) ? 1.0f : ((p.gwp > 8.0f) ? 8.0f : p.gwp);
    const bool  gwpSq = (gwp > 1.99f && gwp < 2.01f);
    const float sxw = (float)ww / (float)w;
    const float syw = (float)wh / (float)h;

    runRange(0, h, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * (size_t)w;
            const float vy = ((float)y + 0.5f) * syw;
            for (int x = 0; x < w; x++) {
                const size_t o = (rowOff + (size_t)x) * 4;
                const int m = mask ? (int)mask[rowOff + (size_t)x] : 255;
                if (m == 0) {
                    dst[o + 0] = src[o + 0];
                    dst[o + 1] = src[o + 1];
                    dst[o + 2] = src[o + 2];
                    dst[o + 3] = src[o + 3];
                    continue;
                }
                const float vx = ((float)x + 0.5f) * sxw;
                const float skin = (float)m * inv255;
                const float eye = protect
                    ? ((float)protect[rowOff + (size_t)x] * inv255 * eyeK)
                    : 0.0f;
                const float keepEye = 1.0f - eye;
                const float se = skin * e;

                // ③ 振幅门控：弱纹理 w→1（照磨），强纹理 w→0（放过）
                //    gw = 1 / (1 + (a/T)^gwp)，gwp>2 时陡度更大 → 选择性的来源。
                float a = planeBilinear(amp, ww, wh, vx, vy) * invT;
                const float ap = gwpSq ? (a * a)
                                       : ((a > 1e-6f) ? powf(a, gwp) : 0.0f);
                float gw = 1.0f / (1.0f + ap);

                const float v1[3] = {planeBilinear(b1r, ww, wh, vx, vy),
                                     planeBilinear(b1g, ww, wh, vx, vy),
                                     planeBilinear(b1b, ww, wh, vx, vy)};
                const float v2[3] = {planeBilinear(b2r, ww, wh, vx, vy),
                                     planeBilinear(b2g, ww, wh, vx, vy),
                                     planeBilinear(b2b, ww, wh, vx, vy)};
                for (int k = 0; k < 3; k++) {
                    const float c = (float)src[o + (size_t)k];
                    float even = c - v1[k] + v2[k];
                    even = c + (even - c) * skin;      // ① mix(C, even, skin)
                    even += (v2[k] - even) * se;       // ② mix(even, b2, skin·e)
                    even = c + (even - c) * gw;        // ③ 振幅门控（本版新增）
                    const float res = c + (even - c) * I;
                    float outv = c + (res - c) * keepEye;
                    if (outv < 0.0f) outv = 0.0f;
                    if (outv > 255.0f) outv = 255.0f;
                    dst[o + (size_t)k] = (unsigned char)(outv + 0.5f);
                }
                dst[o + 3] = src[o + 3];
            }
        }
    });
}


// ---------------------------------------------------------------------------
// 细节回填（锐化 / 全局锐化）
// ---------------------------------------------------------------------------

// 软阈值：|v| ≤ thr 归零，超过的部分原样保留（不做缩放，避免把纹理整体压低）。
inline float softThrF(float v, float thr) {
    if (thr <= 0.0f) return v;
    const float a = v < 0.0f ? -v : v;
    if (a <= thr) return 0.0f;
    return v < 0.0f ? -(a - thr) : (a - thr);
}

// 软膝：|v| ≤ k 原样，超过后增量按 k/|v| 衰减（极值收敛到 2k），防硬边过冲。
inline float softKneeF(float v, float k) {
    if (k <= 0.0f) return v;
    const float a = v < 0.0f ? -v : v;
    if (a <= k) return v;
    const float c = k + (a - k) * k / a;
    return v < 0.0f ? -c : c;
}

void detailRefill(const unsigned char* src, unsigned char* dst, int w, int h,
                  const unsigned char* mask, const RoiRect& roi,
                  const DetailParams& p, int threads,
                  std::vector<float>& scratch, const DetailEma& ema) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty()) return;
    const bool anyGlobal = (p.globalFine != 0.0f || p.globalMid != 0.0f);
    // 掩码内两档为 0 且没有全局档 → 整帧不动（面板两项都归零时的零开销路径）
    if (!anyGlobal && p.fine == 0.0f && p.mid == 0.0f) return;

    // 平面外扩 3px：box7 要读 (x±3)，而 box3 的滑动窗在 box7 那趟里也要 (x±3)，
    // 所以 3 就是最小安全外扩量（边界由 boxPlaneH/V 自己 clamp）。
    const int bx0 = clampi(roi.x0 - 3, 0, w);
    const int bx1 = clampi(roi.x1 + 3, 0, w);
    const int by0 = clampi(roi.y0 - 3, 0, h);
    const int by1 = clampi(roi.y1 + 3, 0, h);
    const int bw = bx1 - bx0;
    const int bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return;

    const size_t np = (size_t)bw * (size_t)bh;
    // delta 模式多要两个平面：dst 的亮度与它的 3×3 盒式（用来算「现在还剩多少细频」）
    const size_t need = p.deltaRefill ? np * 6 : np * 4;
    if (scratch.size() < need) scratch.resize(need);
    float* lum = &scratch[0];          // 亮度原图
    float* tmp = &scratch[np];         // 横向结果暂存
    float* b3  = &scratch[np * 2];     // 3×3 盒式
    float* b7  = &scratch[np * 3];     // 7×7 盒式
    float* dlum = p.deltaRefill ? &scratch[np * 4] : nullptr;  // dst 亮度
    float* db3  = p.deltaRefill ? &scratch[np * 5] : nullptr;  // dst 的 3×3 盒式

    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            // 亮度平面从 **src（原图）** 取，不是从 dst。这是整段的命门：
            // 磨皮已经把 dst 的高频抹平，从 dst 取高通等于没有输入可放大，
            // 正是参考实现 2.1.0 那版「锐化推满只能拉回 +0.46」的原因。
            const unsigned char* row = src + (size_t)(by0 + y) * w * 4;
            float* o = lum + (size_t)y * bw;
            const unsigned char* q = row + (size_t)bx0 * 4;
            for (int x = 0; x < bw; x++, q += 4)
                o[x] = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
        }
    });

    // ---- 时序 EMA（运动自适应）------------------------------------------
    // 为什么放在「算盒子之前」而不是「加增量之后」：噪点是通过**高通**被放大的，
    // 把输入亮度先在时间轴上平滑一次，高通里就没有独立噪声了；静态纹理帧间
    // 相关，EMA 稳态收敛到自身，纹理 100% 保留。若改成对输出做 EMA，则磨皮
    // 的结果也被一起平滑，头一停就出现「由糊转清」的拖影。
    //
    // 与参考实现 3.0.1/3.0.2 同一套做法（legacy_engine.cpp 的 srcT / bgraToRgbaBlend），
    // 连 8bit 存历史这一点也一样 —— 实测量化误差对高通的影响可以忽略。
    if (ema.lumT) {
        // beta >= 1 时仍然要走一遍，只是「只推进历史、不改亮度平面」。为什么
        // 不干脆跳过：运动那几帧如果不动历史，等头停下来 beta 一降，就会把
        // 几帧前的旧亮度混进来 —— 观感上是「头停了画面才滑一下」。所以历史
        // 必须每帧都推进。
        //
        // 而且这一步**不写回 lum**：lum 保持浮点原值，盒子与高通照旧算。这样
        // beta=1 的帧输出与「完全不开 EMA」逐字节相同（可用来证明运动时零拖影）。
        // 若这里把 lum 量化成整数再写回，输出会差 1 个灰阶 —— 虽然看不出，
        // 但就没法做逐字节断言了。
        const bool copyOnly = (ema.reset || ema.beta >= 0.999f);
        if (copyOnly) {
            runRange(0, bh, threads, [&](int yBegin, int yEnd) {
                for (int y = yBegin; y < yEnd; y++) {
                    const float* o = lum + (size_t)y * bw;
                    unsigned char* hrow = ema.lumT + (size_t)(by0 + y) * w + bx0;
                    for (int x = 0; x < bw; x++)
                        hrow[x] = (unsigned char)(o[x] + 0.5f);
                }
            });
        } else {
            const int b256 = (int)(ema.beta * 256.0f + 0.5f);
            const int ib = 256 - b256;
            runRange(0, bh, threads, [&](int yBegin, int yEnd) {
                for (int y = yBegin; y < yEnd; y++) {
                    float* o = lum + (size_t)y * bw;
                    unsigned char* hrow = ema.lumT + (size_t)(by0 + y) * w + bx0;
                    for (int x = 0; x < bw; x++) {
                        const int cur = (int)(o[x] + 0.5f);
                        const int prev = (int)hrow[x];
                        const int v = (cur * b256 + prev * ib + 128) >> 8;
                        hrow[x] = (unsigned char)v;
                        o[x] = (float)v;   // 盒子从平滑后的平面算
                    }
                }
            });
        }
    }

    boxPlaneH(lum, tmp, bw, bh, 1, threads);
    boxPlaneV(tmp, b3, bw, bh, 1, threads);
    boxPlaneH(lum, tmp, bw, bh, 3, threads);
    boxPlaneV(tmp, b7, bw, bh, 3, threads);

    // ---- delta 模式：算「dst 现在还剩多少细频」--------------------------------
    // 为什么需要：普通模式加回的是**原图的**细频，磨皮轻的时候会把原图细节
    // 放大成超过原图（实测 25 档 dret 从 1.000 涨到 1.083，改动量 0.000→0.104）。
    // delta 模式加回的是「原图细频 − dst 细频」= **被磨掉的那一部分**：
    // 磨皮轻 → 差值≈0 → 自然什么都不做；磨皮重 → 差值大 → 才补回来。
    // 这样回填量自动正比于「实际被削掉了多少」，不需要再乘档位系数。
    if (p.deltaRefill) {
        runRange(0, bh, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                const unsigned char* row = dst + (size_t)(by0 + y) * w * 4;
                float* o = dlum + (size_t)y * bw;
                const unsigned char* q = row + (size_t)bx0 * 4;
                for (int x = 0; x < bw; x++, q += 4)
                    o[x] = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
            }
        });
        boxPlaneH(dlum, tmp, bw, bh, 1, threads);
        boxPlaneV(tmp, db3, bw, bh, 1, threads);
    }

    // 只在 roi 内落笔（外扩那 3px 只用于算盒子，不写）
    const int lx0 = roi.x0 - bx0;
    const int lx1 = roi.x1 - bx0;
    const int ly0 = roi.y0 - by0;
    const int ly1 = roi.y1 - by0;

    // 3.1.22：脸内与全局两路各有自己的软阈值（thrFine/thrMid vs
    // thrFineGlobal/thrMidGlobal）。阈值相同时走「算一次」的快路径 ——
    // 这是逐像素热循环，多一次 softThrF 就是白白多一遍分支。
    const bool sameFineThr = (p.thrFineGlobal == p.thrFine);
    const bool sameMidThr = (p.thrMidGlobal == p.thrMid);

    runRange(ly0, ly1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* mrow = mask ? mask + (size_t)(by0 + y) * w : nullptr;
            unsigned char* drow = dst + (size_t)(by0 + y) * w * 4;
            const float* lr = lum + (size_t)y * bw;
            const float* a3 = b3 + (size_t)y * bw;
            const float* a7 = b7 + (size_t)y * bw;
            for (int x = lx0; x < lx1; x++) {
                const float m = mrow ? (float)mrow[bx0 + x] * (1.0f / 255.0f) : 0.0f;
                // 脸内权重（掩码内）与全局权重（掩码外）分开算。
                // 以前是先把两条权重加成一个 fw/mw 再乘同一个带，
                // 那等于「两条路共用一个软阈值」，拆开之后就各自可调了。
                const float fwF = m * p.fine;
                const float fwG = (1.0f - m) * p.globalFine;
                const float mwF = m * p.mid;
                const float mwG = (1.0f - m) * p.globalMid;
                if (fwF == 0.0f && fwG == 0.0f && mwF == 0.0f && mwG == 0.0f)
                    continue;

                const float y0 = lr[x];
                float hf = y0 - a3[x];              // 细频带（≤3px）
                if (p.deltaRefill) {
                    // 差值模式：hf 变成「被磨掉的那部分细频」
                    const float hd = dlum[(size_t)y * bw + x] - db3[(size_t)y * bw + x];
                    hf = hf - hd;
                }
                const float hm = a3[x] - a7[x];     // 中频带（3~7px）
                float d = 0.0f;
                if (fwF != 0.0f || fwG != 0.0f) {
                    const float kF = softKneeF(softThrF(hf, p.thrFine), p.kneeFine);
                    if (fwF != 0.0f) d += fwF * kF;
                    if (fwG != 0.0f) {
                        if (sameFineThr) {
                            d += fwG * kF;
                        } else {
                            d += fwG * softKneeF(softThrF(hf, p.thrFineGlobal),
                                                p.kneeFine);
                        }
                    }
                }
                if (mwF != 0.0f || mwG != 0.0f) {
                    const float kM = softKneeF(softThrF(hm, p.thrMid), p.kneeMid);
                    if (mwF != 0.0f) d += mwF * kM;
                    if (mwG != 0.0f) {
                        if (sameMidThr) {
                            d += mwG * kM;
                        } else {
                            d += mwG * softKneeF(softThrF(hm, p.thrMidGlobal),
                                                p.kneeMid);
                        }
                    }
                }
                if (d == 0.0f) continue;

                unsigned char* q = drow + (size_t)(bx0 + x) * 4;
                for (int c = 0; c < 3; c++) {
                    const float v = (float)q[c] + d;
                    q[c] = (unsigned char)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
                }
            }
        }
    });
}

}  // namespace beautycore
