// beauty_region.cpp —— 人脸局部效果的几何层实现
//
// 设计要点（改动前先读）：
//   · 椭圆用**解析判据**直接算距离，不先画一张全帧权重图。理由：这五项的作用域
//     合起来不到脸框面积的一半，画权重图要先分配 w*h 缓冲再全帧扫，成本是解析法
//     的几十倍；解析法还天然满足「椭圆外逐字节不变」。
//   · 增益是**加法**（逐通道同量），不是乘法。加法不动色相、在暗部与亮部
//     效果一致，和实测到的参考实现行为（dY 近似常数 ±2~4）对得上。
//   · shade 项按亮度加权：眼白、牙齿、高光不该被继续提亮（会糊成一片白）。
//     这不是抄来的，是「提亮」类效果不做高光保护的必然结果。

#include "beauty_region.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace beautycore {
namespace {

template <class F>
void runRange(int begin, int end, int threads, F fn) {
    const int n = end - begin;
    if (n <= 0) return;
    int t = threads;
    if (t <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        t = hc ? (int)hc : 4;
        if (t > 8) t = 8;
    }
    if (t > n / 8) t = n / 8;            // 行数太少，线程开销大于收益
    if (t <= 1) {
        fn(begin, end);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve((size_t)t);
    const int step = (n + t - 1) / t;
    for (int i = 0; i < t; i++) {
        const int a = begin + i * step;
        int b = a + step;
        if (b > end) b = end;
        if (a >= b) break;
        pool.emplace_back([&fn, a, b] { fn(a, b); });
    }
    for (auto& th : pool) th.join();
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float smoothStep(float a, float b, float x) {
    if (b <= a) return (x < a) ? 0.0f : 1.0f;
    float t = (x - a) / (b - a);
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

void applyEllipses(unsigned char* dst, int w, int h,
                   const unsigned char* mask,
                   float faceX, float faceY, float faceW, float faceH,
                   const Ellipse* els, int n, int threads) {
    if (!dst || !els || n <= 0 || w <= 0 || h <= 0) return;
    if (!(faceW > 1.0f) || !(faceH > 1.0f)) return;

    // ---- 预计算 + 并集包围盒 -------------------------------------------
    // 旋转后椭圆的轴对齐包围盒取外接圆半径 max(rx,ry)，宁可多扫几行也不错杀。
    struct E {
        float cx, cy, rx, ry, cosT, sinT, gain, shade, feather;
    };
    std::vector<E> v((size_t)n);
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    int used = 0;
    for (int i = 0; i < n; i++) {
        const Ellipse& e = els[i];
        const float rx = e.rx * faceW, ry = e.ry * faceH;
        if (rx < 0.5f || ry < 0.5f) continue;
        if (e.gain == 0.0f) continue;
        E o;
        o.cx = faceX + e.cx * faceW;
        o.cy = faceY + e.cy * faceH;
        o.rx = rx;
        o.ry = ry;
        o.cosT = cosf(e.rot);
        o.sinT = sinf(e.rot);
        o.gain = e.gain;
        o.shade = clampf(e.shade, 0.0f, 1.0f);
        o.feather = clampf(e.feather, 0.0f, 0.95f);
        v[(size_t)used++] = o;
        const float r = (rx > ry) ? rx : ry;
        if (o.cx - r < bx0) bx0 = o.cx - r;
        if (o.cy - r < by0) by0 = o.cy - r;
        if (o.cx + r > bx1) bx1 = o.cx + r;
        if (o.cy + r > by1) by1 = o.cy + r;
    }
    if (used <= 0) return;
    v.resize((size_t)used);

    int x0 = (int)floorf(bx0), y0 = (int)floorf(by0);
    int x1 = (int)ceilf(bx1) + 1, y1 = (int)ceilf(by1) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return;

    runRange(y0, y1, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const size_t rowOff = (size_t)y * w;
            // p 必须从 x0 起算：循环变量 x 从 x0 开始，但缓冲区指针是从行首
            // 算的。漏掉这个偏移会让整片椭圆左移 x0 个像素（x0 是并集包围盒
            // 左边界，脸不在画面左边时就是几百像素）—— 效果落到画面左上角，
            // 而且不会报错，只能靠对拍的 bbox 看出来。
            unsigned char* p = dst + (rowOff + (size_t)x0) * 4;
            const unsigned char* mrow = mask ? (mask + rowOff) : nullptr;
            for (int x = x0; x < x1; x++, p += 4) {
                const float mw = mrow ? (float)mrow[x] * (1.0f / 255.0f) : 1.0f;
                if (mw <= 0.0f) continue;
                const float Y = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
                float acc = 0.0f;
                // 像素**中心**坐标（x+0.5 / y+0.5）。椭圆中心是连续坐标
                // （faceX + cx·faceW），用整数像素索引会整体偏半个像素 ——
                // 对小而锐的特征（眼袋）这一下就够让效果明显偏一半。
                // applyFoldRemoval 一直用的是 x+0.5，这里统一。
                const float px = (float)x + 0.5f;
                const float py = (float)y + 0.5f;
                for (int k = 0; k < used; k++) {
                    const E& e = v[(size_t)k];
                    const float dx = px - e.cx;
                    const float dy = py - e.cy;
                    const float u = (dx * e.cosT + dy * e.sinT) / e.rx;
                    const float t2 = (-dx * e.sinT + dy * e.cosT) / e.ry;
                    const float d2 = u * u + t2 * t2;
                    if (d2 >= 1.0f) continue;
                    const float d = sqrtf(d2);
                    // 软边：内圈满强度，外圈 smoothstep 归零（避免硬边台阶）
                    const float fall = 1.0f - smoothStep(e.feather, 1.0f, d);
                    float g = e.gain * fall;
                    if (e.shade > 0.0f) g *= (1.0f - e.shade * (Y * (1.0f / 255.0f)));
                    acc += g;
                }
                if (acc == 0.0f) continue;
                acc *= mw;
                // 上下限只为防御：参数写错时不要让整张脸爆掉
                acc = clampf(acc, -48.0f, 48.0f);
                for (int c = 0; c < 3; c++) {
                    // ★ 3.1.23 修：**负增益整体偏移一档的 bug**。
                    //   旧写法 (int)(p[c] + acc + (acc>=0 ? 0.5 : -0.5)) 里，
                    //   负分支算的是 floor(p[c]+acc−0.5) —— 而 (int) 是**截断**
                    //   不是 floor，p[c]+acc−0.5 是正数，所以截断=向下取整，
                    //   结果比正确的 round(p[c]+acc) **少 1**。
                    //   例：p[c]=100, acc=−0.4 → (int)(99.1)=99，实际减了 1
                    //   而不是 0.4；acc=−0.6 → 98，减了 2。
                    //   后果：所有压暗类效果（眼窝/上唇/下颌）系统性地多暗
                    //   1 档 —— 立体对拍时引擎带符号均值 +0.215 vs 模拟器
                    //   +0.647，差 0.43 = 负增益像素占比，就是这个 bug。
                    //   修法：用 lroundf（四舍五入，半数远离零），正负都对。
                    const int nv = (int)lroundf((float)p[c] + acc);
                    p[c] = (unsigned char)(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
                }
                // alpha 不动：OBS 的合成依赖它
            }
        }
    });
}

// 法令纹：把暗线填平到周围皮肤的亮度（3.1.22）
//
//   Δ = min(maxLift, slope·max(0, LR − Y)) × 区域权重
//   LR = 邻域最大亮度（半径 lrRadius 的可分离 max 滤波）
//
// 为什么用 max 而不是均值/中位数：暗线自己在窗口里，均值/中位数会被它拉低，
// 「暗差」就估不准；max 取的是旁边亮皮肤的亮度，正好是「这条线本该有多亮」。
//
// 代价：邻域 max 对高光敏感（旁边有个亮点会把 LR 顶高）。但区域限定在法令纹
// 那一小片，且封顶 +52，实测观感没问题（拟合集 60.18 / 留出集 54.65）。
void applyFoldRemoval(unsigned char* dst, int w, int h,
                       const unsigned char* mask,
                       float faceX, float faceY, float faceW, float faceH,
                       const Ellipse* els, int n,
                       float slope, float maxLift, int lrRadius,
                       float gateLo, float gateHi,
                       int threads) {
    if (!dst || !mask || !els || n <= 0 || w <= 0 || h <= 0) return;
    if (slope <= 0.0f) return;
    if (lrRadius < 1) lrRadius = 1;
    if (lrRadius > 24) lrRadius = 24;
    if (!(faceW > 1.0f) || !(faceH > 1.0f)) return;

    struct E {
        float cx, cy, rx, ry, cosT, sinT, feather;
    };
    std::vector<E> v;
    v.reserve((size_t)n);
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < n; i++) {
        const Ellipse& e = els[i];
        const float rx = e.rx * faceW, ry = e.ry * faceH;
        if (rx < 0.5f || ry < 0.5f) continue;
        E o;
        o.cx = faceX + e.cx * faceW;
        o.cy = faceY + e.cy * faceH;
        o.rx = rx;
        o.ry = ry;
        o.cosT = cosf(e.rot);
        o.sinT = sinf(e.rot);
        o.feather = clampf(e.feather, 0.0f, 0.95f);
        v.push_back(o);
        const float r = (rx > ry) ? rx : ry;
        if (o.cx - r < bx0) bx0 = o.cx - r;
        if (o.cy - r < by0) by0 = o.cy - r;
        if (o.cx + r > bx1) bx1 = o.cx + r;
        if (o.cy + r > by1) by1 = o.cy + r;
    }
    if (v.empty()) return;

    // 输出区 = 椭圆并集包围盒；LR 计算区再外扩 lrRadius+2，
    // 否则边缘像素的「邻域亮皮肤」会被截断，暗差估不准。
    const int pad = lrRadius + 2;
    int x0 = (int)floorf(bx0), y0 = (int)floorf(by0);
    int x1 = (int)ceilf(bx1) + 1, y1 = (int)ceilf(by1) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return;
    int ex0 = x0 - pad, ey0 = y0 - pad, ex1 = x1 + pad, ey1 = y1 + pad;
    if (ex0 < 0) ex0 = 0;
    if (ey0 < 0) ey0 = 0;
    if (ex1 > w) ex1 = w;
    if (ey1 > h) ey1 = h;
    const int ew = ex1 - ex0, eh = ey1 - ey0;
    if (ew <= 0 || eh <= 0) return;

    // ---- 亮度平面（扩展区）--------------------------------------------
    std::vector<float> lum((size_t)ew * (size_t)eh);
    for (int y = 0; y < eh; y++) {
        const unsigned char* row = dst + (size_t)(ey0 + y) * w * 4;
        float* o = &lum[(size_t)y * ew];
        const unsigned char* q = row + (size_t)ex0 * 4;
        for (int x = 0; x < ew; x++, q += 4)
            o[x] = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
    }

    // ---- 可分离邻域最大（先横向后纵向）--------------------------------
    std::vector<float> tmp((size_t)ew * (size_t)eh);
    for (int y = 0; y < eh; y++) {
        const float* s = &lum[(size_t)y * ew];
        float* t = &tmp[(size_t)y * ew];
        for (int x = 0; x < ew; x++) {
            float mx = s[x];
            const int lo = x - lrRadius, hi = x + lrRadius;
            for (int k = lo; k <= hi; k++) {
                if (k < 0 || k >= ew) continue;
                const float val = s[k];
                if (val > mx) mx = val;
            }
            t[x] = mx;
        }
    }
    std::vector<float> lr((size_t)ew * (size_t)eh);
    for (int x = 0; x < ew; x++) {
        for (int y = 0; y < eh; y++) {
            float mx = tmp[(size_t)y * ew + x];
            const int lo = y - lrRadius, hi = y + lrRadius;
            for (int k = lo; k <= hi; k++) {
                if (k < 0 || k >= eh) continue;
                const float val = tmp[(size_t)k * ew + x];
                if (val > mx) mx = val;
            }
            lr[(size_t)y * ew + x] = mx;
        }
    }

    // ---- 施加：Δ = min(maxLift, slope·max(0, LR − Y)) × 区域权重 ---------
    for (int y = y0; y < y1; y++) {
        unsigned char* row = dst + (size_t)y * w * 4;
        const unsigned char* mrow = mask + (size_t)y * w;
        const int ly = y - ey0;
        for (int x = x0; x < x1; x++) {
            const float mw = (float)mrow[x] * (1.0f / 255.0f);
            // 注意：这里**不能**因为 mw == 0 就跳过 —— 限制因子已改成
            // max(mw, wgt)，区域核心的 wgt > 0，即使肤色判定为 0 也该作用。
            // 区域权重（各椭圆软边之和，截到 1）
            float wgt = 0.0f;
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            for (size_t k = 0; k < v.size(); k++) {
                const E& e = v[k];
                const float dx = px - e.cx;
                const float dy = py - e.cy;
                const float u = (dx * e.cosT + dy * e.sinT) / e.rx;
                const float t2 = (-dx * e.sinT + dy * e.cosT) / e.ry;
                const float d2 = u * u + t2 * t2;
                if (d2 >= 1.0f) continue;
                const float d = sqrtf(d2);
                wgt += 1.0f - smoothStep(e.feather, 1.0f, d);
            }
            if (wgt <= 0.0f) continue;
            if (wgt > 1.0f) wgt = 1.0f;
            const int lx = x - ex0;
            const float Y = lum[(size_t)ly * ew + lx];
            const float LR = lr[(size_t)ly * ew + lx];
            const float dRaw = LR - Y;
            if (dRaw <= 0.0f) continue;
            // 暗线闸门：暗差小的像素不起作用 —— 响应宽度收敛到真正的褶皱线上
            const float gate = smoothStep(gateLo, gateHi, dRaw);
            float d = slope * dRaw;
            if (d > maxLift) d = maxLift;
            // 限制因子 = max(皮肤掩码, 区域权重)
            // 皮肤掩码是肤色椭圆，褶皱越深掩码越低 —— 直接乘会在最该提亮的
            // 线上把效果掐掉（实测 corr(掩码, 参照Δ) = −0.66）。改用 max：
            // 只有区域核心旁路肤色判断，区域外仍由肤色把关。
            const float lim = (mw > wgt) ? mw : wgt;
            d *= gate * wgt * lim;
            if (d <= 0.0f) continue;
            unsigned char* p = row + (size_t)x * 4;
            for (int c = 0; c < 3; c++) {
                const int nv = (int)(p[c] + d + 0.5f);
                p[c] = (unsigned char)(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
            }
        }
    }
}

void applySpotFix(unsigned char* dst, int w, int h,
                  const unsigned char* mask, const RoiRect& roi,
                  float k, int radius, float thrLo, float thrHi,
                  int threads, std::vector<float>& scratch) {
    if (!dst || !mask || roi.empty() || w <= 0 || h <= 0) return;
    if (k <= 0.0f) return;
    if (radius < 1) radius = 1;
    if (radius > 6) radius = 6;

    const int bw = roi.width(), bh = roi.height();
    const size_t n = (size_t)bw * (size_t)bh;
    if (scratch.size() < n * 2) scratch.resize(n * 2);
    float* lum = &scratch[0];
    float* mean = &scratch[n];

    // 亮度平面取自**当前 dst**（磨皮/美白已经做完）。用原图算会把已经磨掉的
    // 斑再"修"一遍，等于白做。
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            const unsigned char* row = dst + (size_t)(roi.y0 + y) * w * 4;
            float* o = lum + (size_t)y * bw;
            const unsigned char* q = row + (size_t)roi.x0 * 4;
            for (int x = 0; x < bw; x++, q += 4)
                o[x] = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
        }
    });

    // 均值：滑动窗口 + 边界 clamp。半径 <= 6，直接累加比前缀和更简单，
    // 而且在 14k~200k 像素的 ROI 上两者差别可以忽略。
    // 边界处窗口被 clamp 成"重复边缘行/列"，所以分母必须按**实际参与的元素数**
    // 算，不能一律用 win×win —— 否则 ROI 四边两行的均值会偏小，dev 被撑大，
    // 斑点判据在边缘整片命中（这 bug 的实际表现是 dev 中位数 587）。
    const int win = 2 * radius + 1;
    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        std::vector<float> acc((size_t)bw);
        for (int y = yBegin; y < yEnd; y++) {
            int yA = y - radius, yB = y + radius;
            if (yA < 0) yA = 0;
            if (yB >= bh) yB = bh - 1;
            // 竖直方向：先累加，再按参与行数归一化 → acc 变成"列均值"
            for (int x = 0; x < bw; x++) acc[(size_t)x] = 0.0f;
            for (int yy = yA; yy <= yB; yy++) {
                const float* s = lum + (size_t)yy * bw;
                for (int x = 0; x < bw; x++) acc[(size_t)x] += s[x];
            }
            const float invV = 1.0f / (float)(yB - yA + 1);
            for (int x = 0; x < bw; x++) acc[(size_t)x] *= invV;

            float* o = mean + (size_t)y * bw;
            float sum = 0.0f;
            int cnt = 0;
            for (int x = 0; x <= radius && x < bw; x++) { sum += acc[(size_t)x]; cnt++; }
            for (int x = 0; x < bw; x++) {
                o[x] = sum / (float)cnt;
                const int add = x + radius + 1;
                const int sub = x - radius;
                if (add < bw) { sum += acc[(size_t)add]; cnt++; }
                if (sub >= 0) { sum -= acc[(size_t)sub]; cnt--; }
            }
        }
    });

    if (getenv("BEAUTY_SPOT_DEBUG")) {
        long hit = 0;
        float dmax = 0.0f;
        for (size_t i = 0; i < n; i++) {
            const size_t gy = (size_t)roi.y0 + i / (size_t)bw;
            const size_t gx = (size_t)roi.x0 + i % (size_t)bw;
            if (mask[gy * (size_t)w + gx] == 0) continue;
            const float ad = fabsf(lum[i] - mean[i]);
            if (ad > dmax) dmax = ad;
            if (ad >= thrLo && ad <= thrHi) hit++;
        }
        fprintf(stderr, "[dbg] spot k=%.2f r=%d thr=[%.1f,%.1f] roi=%dx%d "
                "hit=%ld devMax=%.2f\n", k, radius, thrLo, thrHi, bw, bh, hit, dmax);
    }

    runRange(0, bh, threads, [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; y++) {
            unsigned char* row = dst + (size_t)(roi.y0 + y) * w * 4;
            const unsigned char* mrow = mask + (size_t)(roi.y0 + y) * w;
            const float* l = lum + (size_t)y * bw;
            const float* m = mean + (size_t)y * bw;
            unsigned char* p = row + (size_t)roi.x0 * 4;
            for (int x = 0; x < bw; x++, p += 4) {
                const float mw = (float)mrow[roi.x0 + x] * (1.0f / 255.0f);
                if (mw <= 0.0f) continue;
                const float dev = l[x] - m[x];
                const float ad = fabsf(dev);
                if (ad < thrLo || ad > thrHi) continue;   // 细纹理 / 真边缘都不动
                float pull = -k * mw * dev;
                pull = clampf(pull, -24.0f, 24.0f);
                for (int c = 0; c < 3; c++) {
                    // 同 applyEllipses：lroundf 才能对负增益正确四舍五入
                    // （旧的 (int)(… - 0.5f) 写法会让压暗多一档）。
                    const int nv = (int)lroundf((float)p[c] + pull);
                    p[c] = (unsigned char)(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
                }
            }
        }
    });
}

}  // namespace beautycore

