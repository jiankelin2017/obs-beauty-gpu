// beauty_guide.cpp —— 引导滤波实现（O(1)/像素）

#include "beauty_guide.h"
#include "beauty_pool.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace beautycore {

namespace {

inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

int effThreads(int threads, int rows) {
    if (threads == 1) return 1;
    int n = threads;
    if (n <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        n = hc ? (int)hc : 4;
        if (n > 8) n = 8;
    }
    if (n < 1) n = 1;
    if (rows < 32) return 1;
    const int byRows = rows / 16;
    if (n > byRows) n = byRows;
    return (n < 1) ? 1 : n;
}

template <typename Fn>
void runRows(int y0, int y1, int threads, Fn fn) {
    const int rows = y1 - y0;
    const int nt = effThreads(threads, rows);
    if (nt <= 1) {
        fn(y0, y1);
        return;
    }
    beautycore::parallelRows(y0, y1, nt, [&fn](int b, int e) { fn(b, e); });
}

// 水平盒式滤波：src → dst（滑动和，O(1)/像素）
void boxH(const float* src, float* dst, int bw, int bh, int r, int threads) {
    const float inv = 1.0f / (float)(2 * r + 1);
    runRows(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const float* s = src + (size_t)y * bw;
            float* d = dst + (size_t)y * bw;
            float sum = 0.0f;
            for (int k = -r; k <= r; k++) sum += s[clampi(k, 0, bw - 1)];
            for (int x = 0; x < bw; x++) {
                d[x] = sum * inv;
                sum += s[clampi(x + r + 1, 0, bw - 1)];
                sum -= s[clampi(x - r, 0, bw - 1)];
            }
        }
    });
}

// 垂直盒式滤波。按列做前缀和（bh 只有几百行，比滑动窗好写且同样 O(1)/像素）。
//
// 注意 colSum 必须**每个线程各一份** —— 它是按列复用的累加缓冲，共享会串数据。
void boxV(const float* src, float* dst, int bw, int bh, int r, int threads) {
    const float inv = 1.0f / (float)(2 * r + 1);
    runRows(0, bw, threads, [&](int xBegin, int xEnd) {
        std::vector<float> colSum((size_t)bh);
        for (int x = xBegin; x < xEnd; x++) {
            float acc = 0.0f;
            for (int y = 0; y < bh; y++) {
                acc += src[(size_t)y * bw + x];
                colSum[(size_t)y] = acc;
            }
            for (int y = 0; y < bh; y++) {
                const int y0 = clampi(y - r, 0, bh - 1);
                const int y1 = clampi(y + r, 0, bh - 1);
                const float sum = colSum[(size_t)y1] -
                                  ((y0 > 0) ? colSum[(size_t)(y0 - 1)] : 0.0f);
                dst[(size_t)y * bw + x] = sum * inv;
            }
        }
    });
}

void box2d(const float* src, float* dst, float* tmp,
           int bw, int bh, int r, int threads) {
    boxH(src, tmp, bw, bh, r, threads);
    boxV(tmp, dst, bw, bh, r, threads);
}

}  // namespace

void guidedSmoothRoi(const unsigned char* src, unsigned char* dst,
                     int w, int h, const GuideParams& p,
                     const RoiRect& roi, int threads,
                     std::vector<float>& scratch) {
    if (!src || !dst || w <= 0 || h <= 0 || roi.empty()) return;
    const int r = (p.radius < 1) ? 1 : p.radius;

    // 外扩 radius：盒式窗口在 ROI 边界需要读到外面的像素，否则边界一圈的
    // 均值是错的。外扩后误差只落在 roi 之外，不影响最终合成区。
    const int bx0 = clampi(roi.x0 - r, 0, w);
    const int bx1 = clampi(roi.x1 + r, 0, w);
    const int by0 = clampi(roi.y0 - r, 0, h);
    const int by1 = clampi(roi.y1 + r, 0, h);
    const int bw = bx1 - bx0;
    const int bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return;

    const size_t n = (size_t)bw * (size_t)bh;
    // 9 个 float 缓冲：I, I2, meanI, varI, p, meanP, corrIp, a, b
    // 再加 2 个中转：tmp1, tmp2
    if (scratch.size() < n * 11) scratch.resize(n * 11);
    float* I = &scratch[0];
    float* I2 = I + n;
    float* meanI = I2 + n;
    float* varI = meanI + n;
    float* tmp1 = varI + n;
    float* tmp2 = tmp1 + n;
    float* pch = tmp2 + n;
    float* meanP = pch + n;
    float* corrIp = meanP + n;
    float* a = corrIp + n;
    float* b = a + n;

    // 1) 灰度图 + 灰度平方
    runRows(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* row = src + (size_t)(by0 + y) * w * 4;
            float* gi = I + (size_t)y * bw;
            float* g2 = I2 + (size_t)y * bw;
            for (int x = 0; x < bw; x++) {
                const unsigned char* q = row + (size_t)(bx0 + x) * 4;
                const float v = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
                gi[x] = v;
                g2[x] = v * v;
            }
        }
    });

    // 2) meanI 与 varI
    box2d(I, meanI, tmp1, bw, bh, r, threads);
    box2d(I2, tmp2, tmp1, bw, bh, r, threads);
    runRows(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int i = yBegin * bw; i < yEnd * bw; i++) {
            const float mi = meanI[i];
            float v = tmp2[i] - mi * mi;
            if (v < 0.0f) v = 0.0f;
            varI[i] = v;
        }
    });

    // 3) 逐通道
    const float eps = (p.eps > 0.0f) ? p.eps : 1.0f;
    for (int c = 0; c < 3; c++) {
        runRows(0, bh, threads, [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; y++) {
                const unsigned char* row = src + (size_t)(by0 + y) * w * 4;
                float* pc = pch + (size_t)y * bw;
                float* pc2 = meanP + (size_t)y * bw;   // 暂存 I*p
                for (int x = 0; x < bw; x++) {
                    const float v = (float)row[(size_t)(bx0 + x) * 4 + c];
                    pc[x] = v;
                    pc2[x] = v * I[(size_t)y * bw + x];
                }
            }
        });
        // meanP = box(p) 先写进 corrIp 中转，避免再开缓冲
        box2d(pch, corrIp, tmp1, bw, bh, r, threads);   // corrIp = mean_p
        // mean(I*p) 写进 tmp2
        box2d(meanP, tmp2, tmp1, bw, bh, r, threads);   // tmp2 = mean(I·p)

        runRows(0, bh, threads, [&](int yBegin, int yEnd) {
            for (int i = yBegin * bw; i < yEnd * bw; i++) {
                const float mi = meanI[i];
                const float mp = corrIp[i];          // mean_p
                const float cov = tmp2[i] - mi * mp; // cov(I,p)
                const float av = cov / (varI[i] + eps);
                a[i] = av;
                b[i] = mp - av * mi;
            }
        });
        // mean_a / mean_b
        box2d(a, tmp2, tmp1, bw, bh, r, threads);        // tmp2 = mean_a
        box2d(b, corrIp, tmp1, bw, bh, r, threads);      // corrIp = mean_b

        runRows(roi.y0 - by0, roi.y1 - by0, threads, [&](int yBegin, int yEnd) {
            for (int ly = yBegin; ly < yEnd; ly++) {
                const size_t rowOff = (size_t)ly * bw;
                unsigned char* out = dst + (size_t)(by0 + ly) * w * 4;
                for (int lx = roi.x0 - bx0; lx < roi.x1 - bx0; lx++) {
                    const float q = tmp2[rowOff + lx] * I[rowOff + lx] + corrIp[rowOff + lx];
                    const int v = (int)(q + 0.5f);
                    out[(size_t)(bx0 + lx) * 4 + c] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
            }
        });
    }

    // 4) alpha 原样
    runRows(roi.y0, roi.y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            for (int x = roi.x0; x < roi.x1; x++)
                dst[((size_t)y * w + x) * 4 + 3] = src[((size_t)y * w + x) * 4 + 3];
        }
    });
}

}  // namespace beautycore
