// beauty_warp.cpp —— 美形三项（瘦脸 / 大眼 / 下巴）的位移场实现
//
// 实现要点（每一条都是踩过或算过才写的，改之前先读）：
//
// 1) **反向映射**：对每个输出像素求它的采样源坐标，再双线性取样。
//    正向映射（源→目标）会在目标上留空洞，补洞逻辑比反向映射贵得多。
//
// 2) **ROI 外扩 + 权重归零**：位移场只在脸框附近非零。如果 ROI 边界上位移
//    还有值，ROI 内外的像素就会错位，画面上表现为一条竖直/水平的硬缝。
//    所以每个权重函数都必须在 ROI 边界之前降到 0，ROI 还要比脸框外扩一圈
//    （瘦脸的侧向权重在 |u|≈1 处才最强，不扩就正好切在脸边）。
//
// 3) **坐标钳在 ROI 内**：外扩区已经包含背景，钳住不会读到 ROI 之外的
//    旧像素；不钳的话瘦脸在大幅度下会把采样点推出图像边界。
//
// 4) **位移接近 0 时直拷**：脸心一带三项位移都接近 0，直接 memcpy 一个像素
//    比 4 次双线性快得多，实测能省掉约一半耗时。
//
// 5) 没有关键点时退化：用「脸框比例」估眼位（0.38h / ±0.22w）和嘴线
//    （0.72h）。估出来的位置比关键点差，但至少不会让功能失效 ——
//    检不出关键点的帧在真机上并不罕见（侧脸、低头）。

#include "beauty_warp.h"

#include <cmath>
#include <cstring>

namespace beautycore {

namespace {

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float smoothstep01(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 一张脸做位移场需要的全部几何量，先算一次，内层循环只读。
struct Geom {
    float cx = 0, cy = 0;      // 脸框中心
    float fw = 0, fh = 0;      // 脸框宽高
    float eyeRx = 0, eyeRy = 0;  // 右眼（YuNet kps[0]）
    float eyeLx = 0, eyeLy = 0;  // 左眼（YuNet kps[1]）
    float eyeY = 0;            // 双眼平均高度
    float eyeDist = 0;         // 双眼间距，用来定「大眼」的作用半径
    float mouthY = 0;          // 嘴线高度，下巴变形的分界
    float chinY = 0;           // 下巴底（脸框下沿）
    bool hasEyes = false;      // 眼睛来自关键点（否则是比例估算）
};

Geom makeGeom(const FaceBox& f) {
    Geom g;
    g.cx = f.x + f.w * 0.5f;
    g.cy = f.y + f.h * 0.5f;
    g.fw = f.w;
    g.fh = f.h;
    g.chinY = f.y + f.h;

    // 比例兜底：三庭五眼的粗估。只在没有关键点时用。
    g.eyeY = f.y + 0.38f * f.h;
    g.eyeRx = g.cx - 0.22f * f.w;
    g.eyeRy = g.eyeY;
    g.eyeLx = g.cx + 0.22f * f.w;
    g.eyeLy = g.eyeY;
    g.eyeDist = 0.44f * f.w;
    g.mouthY = f.y + 0.72f * f.h;

    if (f.hasKps) {
        // YuNet 关键点顺序：0 右眼 / 1 左眼 / 2 鼻尖 / 3 右嘴角 / 4 左嘴角
        const float r0x = f.kps[0], r0y = f.kps[1];
        const float l1x = f.kps[2], l1y = f.kps[3];
        const float d = sqrtf((l1x - r0x) * (l1x - r0x) + (l1y - r0y) * (l1y - r0y));
        // 合理性检查：间距太小说明两个点重合（解码失败），太大说明跨了半张图。
        if (d > 4.0f && d < 3.0f * f.w) {
            g.eyeRx = r0x;
            g.eyeRy = r0y;
            g.eyeLx = l1x;
            g.eyeLy = l1y;
            g.eyeDist = d;
            g.eyeY = (r0y + l1y) * 0.5f;
            g.hasEyes = true;
        }
        const float my = (f.kps[7] + f.kps[9]) * 0.5f;
        // 嘴线必须落在眼睛和下巴之间，否则说明嘴角点不可信，用比例值。
        if (my > g.eyeY + 0.05f * f.h && my < g.chinY) g.mouthY = my;
    }
    return g;
}

}  // namespace

bool warpFaceGeom(const FaceBox& f, const WarpParams& p, int w, int h,
                  WarpGeom& out) {
    out = WarpGeom();
    if (w <= 0 || h <= 0) return false;
    if (p.thin <= 0.0f && p.eye <= 0.0f && p.chin <= 0.0f) return false;
    if (!(f.w > 8.0f) || !(f.h > 8.0f)) return false;

    const Geom g = makeGeom(f);

    // ---- ROI：脸框外扩 30% 宽 / 18% 高，并钳到画面内 ----
    const float mx = 0.30f * g.fw;
    const float my = 0.18f * g.fh;
    int x0 = (int)floorf(f.x - mx);
    int x1 = (int)ceilf(f.x + f.w + mx);
    int y0 = (int)floorf(f.y - my);
    int y1 = (int)ceilf(f.y + f.h + my);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 2 || rh <= 2) return false;

    // ---- 三个场的常量，循环外算好 ----
    // 大眼：作用半径取双眼间距的一半多一点（覆盖眼睛 + 眼皮 + 一点眼袋）。
    float eyeR = 0.52f * g.eyeDist;
    const float eyeMin = 0.10f * g.fw;
    const float eyeMax = 0.35f * g.fw;
    if (eyeR < eyeMin) eyeR = eyeMin;
    if (eyeR > eyeMax) eyeR = eyeMax;
    const float eyeAmp = p.eye * 0.55f;   // 满档中心放大 1.55 倍
    const bool doEye = (p.eye > 0.0f) && (eyeR > 1.0f);

    // 瘦脸：垂直高斯中心放在「眼线 → 嘴线」之间偏下，正好是脸颊。
    const float thinYc = g.eyeY + 0.62f * (g.mouthY - g.eyeY);
    const float thinSig = 0.24f * g.fh;
    const float thinK = p.thin * 0.24f;   // 满档外侧采样点外移 24%
    const bool doThin = (p.thin > 0.0f) && (thinSig > 0.5f);

    // 下巴（3.1.64，C 方案）：缩短 + 变尖的混合场。
    // 旧实现只有纵向压缩（满档下巴缩短 18%），观感是「下巴没了、脸变短变圆」，
    // 与美颜诉求相反。新实现拆成两件事：
    //   · 中线纵向拉长（chinK）  —— 采样点上移，内容下移，下巴尖往下延
    //   · 两侧横向内收（chinKH） —— 采样点外移，下颌轮廓向中线收，脸变窄
    // 两者叠加得到的才是「尖下巴」，而不是单一的纵向缩放。
    // 幅度取 18% / 9%：首版 10% / 14% 实测下巴轮廓位移只有约 8 px（720p），
    // 6 倍放大图里勉强可辨 —— 原因是 YuNet 的脸框下沿本来就压在下巴中部，
    // chinSpan（嘴线→框底）通常只有 0.25~0.30 个脸高，缩放系数乘上去很小。
    // 18% 对应约 15 px 纵向、20 px 横向（脸宽 220 px 时），是「明显但不假」的一档。
    // 2026-11-05 用户反馈「下巴两边的空间拉扯非常明显」：横向 20% 且作用到
    // u=1.0（脸框边缘）会把框外的背景一起吸进来 —— 观感是两侧空间被硬扯。
    // 横向降到 9%，并把权重带收到下颌线（见下面的 latSide）。
    const float chinYm = g.mouthY + 0.02f * g.fh;
    const float chinSpan = g.chinY - chinYm;
    const float chinK = p.chin * 0.18f;    // 满档中线纵向拉长 18%
    const float chinKH = p.chin * 0.09f;   // 满档两侧横向内收 9%（原 20%，拉扯过大）
    const bool doChin = (p.chin > 0.0f) && (chinSpan > 1.0f);

    // 大眼项实际生效还要求「眼睛来自关键点」—— 比例估算的眼位只用来定
    // 瘦脸的垂直中心，不参与径向放大（原实现就是这个语义）。
    const bool eyeActive = doEye && g.hasEyes;
    if (!eyeActive && !doThin && !doChin) return false;

    out.cx = g.cx;
    out.cy = g.cy;
    out.halfW = 0.5f * g.fw;
    out.eyeRx = g.eyeRx;
    out.eyeRy = g.eyeRy;
    out.eyeLx = g.eyeLx;
    out.eyeLy = g.eyeLy;
    out.eyeR = eyeR;
    out.eyeAmp = eyeAmp;
    out.thinYc = thinYc;
    out.thinSig = thinSig;
    out.thinK = thinK;
    out.chinYm = chinYm;
    out.chinSpan = chinSpan;
    out.chinK = chinK;
    out.chinKH = chinKH;
    out.roi[0] = x0;
    out.roi[1] = y0;
    out.roi[2] = x1;
    out.roi[3] = y1;
    out.flags = (eyeActive ? WARP_F_EYE : 0u) |
                (doThin ? WARP_F_THIN : 0u) |
                (doChin ? WARP_F_CHIN : 0u) |
                (g.hasEyes ? WARP_F_KPS : 0u);
    return true;
}

void warpFaceWithGeom(unsigned char* rgba, int w, int h, const WarpGeom& g,
                      std::vector<unsigned char>& scratch) {
    if (!rgba || w <= 0 || h <= 0) return;
    if (!(g.flags & (WARP_F_EYE | WARP_F_THIN | WARP_F_CHIN))) return;

    const int x0 = g.roi[0], y0 = g.roi[1], x1 = g.roi[2], y1 = g.roi[3];
    if (x0 < 0 || y0 < 0 || x1 > w || y1 > h) return;
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw <= 2 || rh <= 2) return;

    // ---- 先把 ROI 拷进 scratch 当采样源（就地写会污染后面的取样）----
    const size_t rowBytes = (size_t)rw * 4;
    const size_t needBytes = rowBytes * (size_t)rh;
    if (scratch.size() < needBytes) scratch.resize(needBytes);
    unsigned char* src = &scratch[0];
    for (int y = 0; y < rh; y++)
        memcpy(src + (size_t)y * rowBytes,
               rgba + ((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 4, rowBytes);

    const bool doEye = (g.flags & WARP_F_EYE) != 0;
    const bool doThin = (g.flags & WARP_F_THIN) != 0;
    const bool doChin = (g.flags & WARP_F_CHIN) != 0;
    const float halfW = g.halfW;

    for (int y = 0; y < rh; y++) {
        const float oy = (float)(y0 + y);
        unsigned char* out = rgba + ((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 4;
        const unsigned char* srow = src + (size_t)y * rowBytes;

        for (int x = 0; x < rw; x++) {
            const float ox = (float)(x0 + x);
            float sx = ox;
            float sy = oy;

            // ---- ① 大眼：以眼心为中心径向放大 ----
            // 采样点向眼心收拢（除以 m>1），内容就被「撑大」。
            if (doEye) {
                for (int e = 0; e < 2; e++) {
                    const float ex = e ? g.eyeLx : g.eyeRx;
                    const float ey = e ? g.eyeLy : g.eyeRy;
                    const float dx = sx - ex;
                    const float dy = sy - ey;
                    const float r2 = (dx * dx + dy * dy) / (g.eyeR * g.eyeR);
                    if (r2 >= 1.0f) continue;
                    const float q = 1.0f - r2;
                    const float fall = q * q;          // (1−r²)²：中心平、边缘陡
                    const float m = 1.0f + g.eyeAmp * fall;
                    sx = ex + dx / m;
                    sy = ey + dy / m;
                }
            }

            // ---- ② 瘦脸：向面中线水平压缩 ----
            // 垂直权重是高斯（只动脸颊那一段），侧向权重在 |u|≈0.1 起升、
            // 到 1.55 归零 —— 后者就是 ROI 外扩 30% 的原因。
            if (doThin) {
                const float dyv = (oy - g.thinYc) / g.thinSig;
                const float wv = expf(-0.5f * dyv * dyv);
                if (wv > 1e-4f) {
                    const float u = (sx - g.cx) / halfW;
                    const float au = fabsf(u);
                    const float lat = smoothstep01(0.10f, 0.80f, au) *
                                      (1.0f - smoothstep01(0.95f, 1.55f, au));
                    const float k = g.thinK * wv * lat;
                    if (k > 1e-4f) sx = g.cx + (sx - g.cx) * (1.0f + k);
                }
            }

            // ---- ③ 下巴：中线纵向拉长 + 两侧横向内收（3.1.64）----
            // 两条位移方向不同：纵向让下巴尖往下延（脸变长），横向让下颌轮廓
            // 向中线收（脸变窄）。旧版只有纵向压缩，出来的是「脸变短变圆」。
            if (doChin && sy > g.chinYm) {
                const float t = (sy - g.chinYm) / g.chinSpan;
                // 垂直权重：嘴线处 0 → t≥0.40 全效 → 下巴底(t=1)仍全效 →
                // 越过下巴底进脖子的那段(t>1.0)渐降。旧版在 t=0.92~1.10 就
                // 衰减到 0，那是为「压缩」保护下巴尖；现在要拉长，衰减必须
                // 挪到 t>1，否则下巴尖一动不动。
                const float wv = smoothstep01(0.0f, 0.40f, t) *
                                 (1.0f - smoothstep01(1.00f, 1.30f, t));
                if (wv > 1e-4f) {
                    const float u = fabsf((sx - g.cx) / halfW);
                    // 中线强、两侧弱 → 只拉长面部中线（下巴尖）
                    const float latMid = 1.0f - 0.55f * smoothstep01(0.15f, 0.95f, u);
                    // 两侧强、中线弱 → 只收下颌轮廓，不挤压下巴尖本身。
                    // 2026-11-05：原来 u∈[0.10,0.55] 就满档、之后一直不衰减，
                    // 等于把「脸框半宽 55% 以外直到框外」的区域全都按 20% 外移，
                    // 背景跟着被吸进来 → 用户看到的「两边空间拉扯」。
                    // 现在权重带收在下颌线（0.60~0.85）并在 u→1 归零，
                    // 框外（u>1）由 smoothstep 的钳位自动为 0。
                    const float latSide = smoothstep01(0.25f, 0.60f, u) *
                                          (1.0f - smoothstep01(0.85f, 1.00f, u));
                    const float kV = g.chinK * wv * latMid;
                    if (kV > 1e-4f)
                        sy = g.chinYm + (sy - g.chinYm) * (1.0f - kV);
                    const float kH = g.chinKH * wv * latSide;
                    if (kH > 1e-4f)
                        sx = g.cx + (sx - g.cx) * (1.0f + kH);
                }
            }

            // ---- 采样 ----
            const float dxo = sx - ox;
            const float dyo = sy - oy;
            unsigned char* dstp = out + (size_t)x * 4;

            if (dxo > -0.02f && dxo < 0.02f && dyo > -0.02f && dyo < 0.02f) {
                memcpy(dstp, srow + (size_t)x * 4, 4);   // 位移可忽略：直拷
                continue;
            }

            float lx = clampf(sx - (float)x0, 0.0f, (float)(rw - 1));
            float ly = clampf(sy - (float)y0, 0.0f, (float)(rh - 1));
            int ix = (int)lx;
            int iy = (int)ly;
            int ix1 = ix + 1;
            int iy1 = iy + 1;
            if (ix1 > rw - 1) ix1 = rw - 1;
            if (iy1 > rh - 1) iy1 = rh - 1;
            const float fx = lx - (float)ix;
            const float fy = ly - (float)iy;

            const unsigned char* p00 = src + (size_t)iy * rowBytes + (size_t)ix * 4;
            const unsigned char* p01 = src + (size_t)iy * rowBytes + (size_t)ix1 * 4;
            const unsigned char* p10 = src + (size_t)iy1 * rowBytes + (size_t)ix * 4;
            const unsigned char* p11 = src + (size_t)iy1 * rowBytes + (size_t)ix1 * 4;

            for (int c = 0; c < 4; c++) {
                const float a = (float)p00[c] * (1.0f - fx) + (float)p01[c] * fx;
                const float b = (float)p10[c] * (1.0f - fx) + (float)p11[c] * fx;
                dstp[c] = (unsigned char)(a * (1.0f - fy) + b * fy + 0.5f);
            }
        }
    }
}

void warpFace(unsigned char* rgba, int w, int h, const FaceBox& f,
              const WarpParams& p, std::vector<unsigned char>& scratch) {
    WarpGeom g;
    if (!warpFaceGeom(f, p, w, h, g)) return;
    warpFaceWithGeom(rgba, w, h, g, scratch);
}

}  // namespace beautycore
