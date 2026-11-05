// ===========================================================================
// warp.hlsl —— 美形三项（瘦脸 / 大眼 / 下巴）位移场 GPU 着色器
//
// 移植自 beauty_warp.cpp 的 warpFaceWithGeom()（core 1.5.0）。
// 一个 technique，一张脸一趟：每趟只处理**这张脸 ROI** 内的像素，ROI 外
// 原样透传（CPU 那边 ROI 外的字节一个都不动，透传才是等价语义）。
//
// == 为什么是「一张脸一趟」而不是「一趟画完全部脸」=========================
// CPU 是**串行**的：face[0] 就地改完，face[1] 读到的是 face[0] 改过的数据。
// 单趟要复现这个语义，只能假设各脸 ROI 完全不重叠 —— 而 CPU 侧的原话是
// 「检测框本身就不会**大面积**交叠」，不是「不交叠」。假设一旦破了，画面会
// 出现一小块错位，且只在多脸场景偶发，极难定位。串行 + ping-pong 纹理
// 把这个假设彻底去掉，代价只是多几趟（现实中 1~2 张脸）。
//
// == 逐位对齐的三个要害（错一个就是 ±1~±255）===============================
//  1) **不用双线性采样器**。硬件 bilinear 的权重由 uv 反推，uv 又是浮点，
//     4K 下末位误差会让权重偏移 → 每像素 ±1。这里改成 4 次 Point 采样 +
//     手工加权，权重 fx/fy 与 CPU 完全同源。
//  2) **在 0..255 域里算**。CPU 全程是 `float p00 * (1-fx) + ...`（p 是
//     0..255 的整数），最后 `(unsigned char)(v + 0.5)` 截断。GPU 若在 0..1
//     域算再乘回去，等于多两次除乘，边界像素会翻 1。所以采样先 ×255，
//     算完 floor(v+0.5) 再 ÷255 写回 RGBA8（硬件 round 回来正好是整数）。
//  3) **alpha 也要插值**。CPU 那个 `for (c = 0; c < 4; c++)` 是**四通道**
//     一起算的，不是 RGB。顺手写个 `col.a` 会让透明区边缘差出几十灰阶。
//
// == 像素整数坐标怎么来的 ==================================================
// uv 在像素中心是 (x+0.5)/w，所以 `floor(uv * texSize)` 就是 x。它只要求
// 插值误差 < 0.5 像素（实际是 1e-4 量级），比依赖 SV_POSITION 语义更稳 ——
// 后者要改结构体、踩 OBS effect 方言的语义映射，不值得。
//
// == OBS effect 方言（五条红线，见 tone.hlsl 文件头）=========================
//   1) 顶层不能有静态数组（本文件不用表）
//   2) 不能有数组形参
//   3) sampler 必须 sampler_state <name> { ... };
//   4) technique 入口必须带参
//   5) 矩阵参数名必须是 ViewProj，行向量 × 矩阵
// ===========================================================================

uniform float4x4 ViewProj;

uniform texture2d image;

// 全流程 1:1 像素中心采样，必须 Point；Clamp 对应 CPU 的 clampi(x, 0, n-1)。
sampler_state pointTex {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform float2 texSize;    // (w, h)，用来把 uv 换成整数像素坐标

// ---- 逐脸几何（每趟由宿主重设一次，与 BeautyWarpFace 逐项对应）----------
uniform float4 fA;         // cx, cy, halfW, eyeR
uniform float4 fB;         // eyeRx, eyeRy, eyeLx, eyeLy
uniform float4 fC;         // eyeAmp, thinYc, thinSig, thinK
uniform float4 fD;         // chinYm, chinSpan, chinK(中线拉长), chinKH(两侧内收)
uniform float4 roi;        // x0, y0, x1, y1 —— x1/y1 是**开区间**（与 CPU 一致）

// BEAUTY_WARP_F_*：1 大眼 / 2 瘦脸 / 4 下巴 / 8 眼睛来自关键点
uniform int wFlags;

struct vertex_data {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

vertex_data VSDefault(vertex_data vert_in) {
    vertex_data vsout;
    vsout.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    vsout.uv  = vert_in.uv;
    return vsout;
}

// ---------------------------------------------------------------------------
// 位移场 + 重采样
// ---------------------------------------------------------------------------
// 三段位移的顺序**不能换**，而且每一段读的都是上一段的输出：
//   大眼读 (sx,sy) 原值 → 瘦脸的侧向权重用大眼之后的 sx → 下巴判断用 sy
// CPU 那边也是这么串的（warpFaceWithGeom 的三个 if 依次改写 sx/sy）。
// 换顺序 = 换算法，不是等价变形。
float4 PS_Warp(vertex_data vert_in) : TARGET {
    float4 col = image.Sample(pointTex, vert_in.uv);

    // ---- 像素整数坐标 ----
    float2 pixc = vert_in.uv * texSize;      // (x+0.5, y+0.5)
    float ox = floor(pixc.x);
    float oy = floor(pixc.y);

    // ROI 外原样透传：CPU 只写 ROI 内的像素，这里必须一个字节都不动。
    if (ox < roi.x || ox >= roi.z || oy < roi.y || oy >= roi.w)
        return col;

    float sx = ox;
    float sy = oy;

    // ---- ① 大眼：以眼心为中心径向放大 ----
    // 采样点向眼心收拢（除以 m>1），内容就被「撑大」。
    // 两只眼串行处理：第二只眼读到的是第一只眼位移后的坐标（CPU 同）。
    if ((wFlags & 1) != 0) {
        for (int e = 0; e < 2; e++) {
            float ex = (e == 0) ? fB.x : fB.z;
            float ey = (e == 0) ? fB.y : fB.w;
            float dx = sx - ex;
            float dy = sy - ey;
            float r2 = (dx * dx + dy * dy) / (fA.w * fA.w);
            if (r2 >= 1.0)
                continue;
            float q = 1.0 - r2;
            float fall = q * q;              // (1−r²)²：中心平、边缘陡
            float m = 1.0 + fC.x * fall;
            sx = ex + dx / m;
            sy = ey + dy / m;
        }
    }

    // ---- ② 瘦脸：向面中线水平压缩 ----
    // 垂直权重是高斯（只动脸颊那一段），侧向权重在 |u|≈0.1 起升、到 1.55
    // 归零 —— 后者就是 ROI 外扩 30% 的原因。
    // 注意垂直权重用的是 **oy**（原始行），侧向用的是 **sx**（大眼之后）：
    // 这不是笔误，CPU 逐字如此。
    if ((wFlags & 2) != 0) {
        float dyv = (oy - fC.y) / fC.z;
        float wv = exp(-0.5 * dyv * dyv);
        if (wv > 1e-4) {
            float u = (sx - fA.x) / fA.z;
            float au = abs(u);
            float lat = smoothstep(0.10, 0.80, au) *
                        (1.0 - smoothstep(0.95, 1.55, au));
            float k = fC.w * wv * lat;
            if (k > 1e-4)
                sx = fA.x + (sx - fA.x) * (1.0 + k);
        }
    }

    // ---- ③ 下巴：中线纵向拉长 + 两侧横向内收（3.1.64，与 CPU 逐字对应）----
    // 纵向让下巴尖往下延（脸变长），横向让下颌轮廓向中线收（脸变窄）。
    // 旧版只有纵向压缩，出来是「脸变短变圆」，所以这一段的权重区间也跟着改了：
    // 衰减从 t=0.92 挪到 t>1.0 —— 拉长时下巴尖本身必须动，否则等于没拉。
    // 2026-11-05：横向权重带收窄（0.25~0.60 上升、0.85~1.00 归零）。
    // 原来的 0.10~0.55 满档且不衰减，会把脸框外的背景按同比例外移，
    // 画面上就是「下巴两边空间被硬扯」。
    if ((wFlags & 4) != 0 && sy > fD.x) {
        float t = (sy - fD.x) / fD.y;
        float wv = smoothstep(0.0, 0.40, t) *
                   (1.0 - smoothstep(1.00, 1.30, t));
        if (wv > 1e-4) {
            float u = abs((sx - fA.x) / fA.z);
            float latMid  = 1.0 - 0.55 * smoothstep(0.15, 0.95, u);
            float latSide = smoothstep(0.25, 0.60, u) *
                            (1.0 - smoothstep(0.85, 1.00, u));
            float kV = fD.z * wv * latMid;
            if (kV > 1e-4)
                sy = fD.x + (sy - fD.x) * (1.0 - kV);
            float kH = fD.w * wv * latSide;
            if (kH > 1e-4)
                sx = fA.x + (sx - fA.x) * (1.0 + kH);
        }
    }

    // ---- 位移可忽略：直拷 ----
    // 脸心一带三项位移都接近 0，CPU 在这里 memcpy 4 字节。等价于原样返回。
    float dxo = sx - ox;
    float dyo = sy - oy;
    if (dxo > -0.02 && dxo < 0.02 && dyo > -0.02 && dyo < 0.02)
        return col;

    // ---- 重采样：钳到 ROI 内，再 4 点手工双线性 ----
    // CPU：lx = clamp(sx - x0, 0, rw-1)，然后 ix=floor(lx)、fx=frac(lx)、
    //      ix1=min(ix+1, rw-1)。不钳的话瘦脸大幅时会把采样点推出图像边界。
    float rw1 = roi.z - roi.x - 1.0;
    float rh1 = roi.w - roi.y - 1.0;
    float lx = clamp(sx - roi.x, 0.0, rw1);
    float ly = clamp(sy - roi.y, 0.0, rh1);
    float ix = floor(lx);
    float iy = floor(ly);
    float ix1 = min(ix + 1.0, rw1);
    float iy1 = min(iy + 1.0, rh1);
    float fx = lx - ix;
    float fy = ly - iy;

    float2 invSize = 1.0 / texSize;
    // 回到图像空间的像素中心 uv
    float2 b0 = roi.xy;
    float2 u00 = (b0 + float2(ix,  iy ) + 0.5) * invSize;
    float2 u01 = (b0 + float2(ix1, iy ) + 0.5) * invSize;
    float2 u10 = (b0 + float2(ix,  iy1) + 0.5) * invSize;
    float2 u11 = (b0 + float2(ix1, iy1) + 0.5) * invSize;

    // ×255 进灰阶域：与 CPU 的整数域算式同尺度（见文件头第 2 条）
    float4 p00 = image.Sample(pointTex, u00) * 255.0;
    float4 p01 = image.Sample(pointTex, u01) * 255.0;
    float4 p10 = image.Sample(pointTex, u10) * 255.0;
    float4 p11 = image.Sample(pointTex, u11) * 255.0;

    float4 a = p00 * (1.0 - fx) + p01 * fx;
    float4 b = p10 * (1.0 - fx) + p11 * fx;
    float4 v = a * (1.0 - fy) + b * fy;

    // CPU 是 `(unsigned char)(v + 0.5)` —— 加 0.5 再**截断**。
    // 写 RGBA8 时硬件会自己四舍五入，所以这里先取整再 ÷255，否则每像素 +1。
    v = floor(v + 0.5) * (1.0 / 255.0);
    return v;
}

technique WarpTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Warp(vert_in);
    }
}
