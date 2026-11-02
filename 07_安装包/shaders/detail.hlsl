// ===========================================================================
// detail.hlsl —— 细节回填（锐化 / 全局锐化 / 基准回填）GPU 着色器
//
// 移植自 beauty_smooth.cpp 的 detailRefill()（core 1.4.0, L1283-1472）。
// 那个函数做三件事，这里拆成四趟：
//   1) 从 **src（原图）** 取亮度平面，可选地过一遍时序 EMA（运动自适应去噪）
//   2) 3×3 盒式 = 细频带基准，7×7 盒式 = 中频带基准
//   3) hf = lum − box3（细频）  hm = box3 − box7（中频）
//   4) 按「脸内 / 全局」四档权重 × 软阈值 × 软膝，把增量加回颜色
//
// == 为什么拆成 4 趟而不是一趟 7×7 ==========================================
// 3×3 和 7×7 都可分离。合成 2D 是 9+49 = 58 tap/像素，拆开后是
// 水平(3+7=10) + 垂直(3+7=10) = 20 tap，且 4K 下差得更远。
// 更重要：拆开后**浮点求和顺序**与 CPU 的 boxPlaneH/boxPlaneV 逐趟对应，
// 逐像素对拍的误差才压得住（与 smooth.hlsl 里「不合成 2D 核」同一个理由）。
//
// == 值域约定 ==============================================================
// 中间平面（det_a / det_b）装 **0..255 的浮点亮度**，不是 0..1。
// 与 smooth.hlsl 同一条约定：CPU 全程在灰阶域里算，常数（阈值 3.0 / 0.8、
// 软膝 12 / 10、clamp 255）都按这个域标定，换域就要重新推导每个常数。
// 只有最外层颜色各做一次 ×255 / ÷255。
//
// == 四趟的纹理编排（OBS 侧负责，见 obs-beauty.cpp 的 beauty_chain_detail）==
//   Lum    src + 历史 → A(.r=亮度 .g=新历史)      ← EMA 在这一趟里完成
//   Hist   A.g → 历史纹理（下一帧用）              ← emaMode==0 时跳过
//   BoxH   A → B(.r=box3h .g=box7h .b=亮度透传)
//   BoxV   B → A(.r=box3 .g=box7 .b=亮度透传)      ← A 此刻已被读空，可以覆写
//   Refill 颜色 + A + skinMask → 输出
// 亮度用 .b 一路透传，是为了只用两张平面纹理：Refill 需要**逐像素**亮度
// （不是盒子），而 BoxV 已经把 A 覆写掉了，不带过来就得开第三张。
//
// == OBS effect 方言（五条红线，见 tone.hlsl 文件头）=========================
//   1) 顶层不能有静态数组（本文件不用表）
//   2) 不能有数组形参
//   3) sampler 必须 sampler_state <name> { ... };
//   4) technique 入口必须带参
//   5) 矩阵参数名必须是 ViewProj，行向量 × 矩阵
// ===========================================================================

uniform float4x4 ViewProj;

// ---- 纹理 -----------------------------------------------------------------
uniform texture2d image;      // Lum: 原图 / BoxH: 亮度平面 / BoxV: boxH 结果 /
                              // Refill: 着色后的颜色（磨皮+色调的输出）
uniform texture2d image2;     // Lum: 上一帧亮度历史
uniform texture2d image3;     // Refill: 平面（.r box3 / .g box7 / .b 亮度）
uniform texture2d skinMask;   // Refill: 皮肤掩码（R8，0..1）

// 全流程都在 1:1 像素中心采样，必须 Point；Clamp 对应 CPU 的 clampi(x, 0, n-1)。
sampler_state pointTex {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform float2 texel;         // 1/w, 1/h

// ---- Lum 趟：时序 EMA ------------------------------------------------------
// emaMode 0 = 不做 EMA（历史照旧不推进，亮度原样透传）
//         1 = copyOnly（首帧 / 运动帧：历史=当前亮度，亮度**不量化**）
//         2 = 混合（静止帧：历史与亮度都取 (cur·b + prev·(1−b)) 的 8bit 量化值）
//
// 注意 0 与 1 的差别只在「历史写不写」，1 与 2 的差别在「亮度量化不量化」。
// CPU 那边的三个分支逐字对应（beauty_smooth.cpp 的 detailRefill EMA 段），
// 少一个分支就会在首帧或运动帧上多出 ±1 灰阶的差。
uniform int emaMode;
uniform int emaB256;          // beta × 256（四舍五入后取整）
uniform int emaIB256;         // 256 − emaB256

// ---- Refill 趟：四档权重与门槛 --------------------------------------------
uniform float dFine;          // 脸内细频增益
uniform float dMid;           // 脸内中频增益（0..3）
uniform float dGFine;         // 全局细频增益
uniform float dGMid;          // 全局中频增益
uniform float dThrFine;       // 细频软阈值（削噪）
uniform float dThrMid;
uniform float dThrFineG;      // 全局那一路的阈值（可与脸内不同）
uniform float dThrMidG;
uniform float dKneeFine;      // 软膝：|增量| 超过它按 k/|v| 衰减，防硬边过冲
uniform float dKneeMid;

// ⚠ 无掩码时的语义**与磨皮相反**，这是 CPU 侧的真实行为，不是笔误：
//   · surface3RgbBand（磨皮）收 nullptr 时 m 恒 255 —— 「没检出脸就全帧磨」；
//   · detailRefill（细节）收 nullptr 时 m 恒 **0** —— `mrow ? ... : 0.0f`。
// 后果：没有掩码时脸内增益全为 0、只有全局增益生效（(1−0)=1）。
// 如果这里跟着磨皮用 255，全局锐化会在「这一帧没检出脸」时整段失效 ——
// 正是参考实现 3.0.13 修过的那个「掩码一丢背景锐度跟着跳档」，core 不能重演。
uniform int noMask;           // 1 = 引擎没给掩码（此时 m 必须按 0 算）

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
// Lum：0..255 亮度 + 可选时序 EMA
// ---------------------------------------------------------------------------
// CPU 口径（detailRefill 开头）：
//     lum = 0.299·R + 0.587·G + 0.114·B      （R/G/B 是 0..255 的整数）
// EMA（beta < 0.999）：
//     cur = round(lum); prev = 历史;
//     v = (cur·b256 + prev·ib256 + 128) >> 8;  历史 = v; 亮度 = v
float4 PS_Lum(vertex_data vert_in) : TARGET {
    float3 c = image.Sample(pointTex, vert_in.uv).rgb * 255.0;
    float lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;

    // ⚠ .b 必须携带**逐像素亮度**（= 送进盒式的那份），因为 BoxV 之后 .r 会变成
    // box3、原亮度丢失，Refill 的 hf = lum − box3 只能靠 .b 一路透传过来。
    // 3.1.55 之前这里三个分支都写 .b = 0.0，Refill 拿到 lum=0 → hf = −box3
    // （约 −178），整张脸被压暗 ~1.4 灰阶而非锐化；3.1.56 由 GPU 细节对拍抓出并修正。
    if (emaMode == 0)
        return float4(lum, 0.0, lum, 1.0);

    if (emaMode == 1) {
        // copyOnly：亮度保持浮点原值（与「完全不开 EMA」逐字节相同），
        // 历史写成量化值，供下一帧混合时用。
        float h = floor(lum + 0.5);
        return float4(lum, h, lum, 1.0);
    }

    int cur  = (int)floor(lum + 0.5);
    int prev = (int)floor(image2.Sample(pointTex, vert_in.uv).r * 255.0 + 0.5);
    int v    = (cur * emaB256 + prev * emaIB256 + 128) >> 8;
    float fv = (float)v;
    return float4(fv, fv, fv, 1.0);
}

// ---------------------------------------------------------------------------
// Hist：把 Lum 算出的新历史写进历史纹理
// ---------------------------------------------------------------------------
// 单独一趟而不是在 Lum 里直接写出，是因为渲染目标不能同时被采样：
// Lum 要读**上一帧**的历史，历史纹理这一帧只能由另一趟来写。
float4 PS_Hist(vertex_data vert_in) : TARGET {
    float h = image.Sample(pointTex, vert_in.uv).g;
    return float4(h * (1.0 / 255.0), 0.0, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// BoxH：水平 3 tap + 7 tap 一次算完，亮度透传到 .b
// ---------------------------------------------------------------------------
// CPU 的 boxPlaneH 是滑动窗 + 每 32 列重算（抗 float 漂移）。这里每像素
// 独立求和，等价于「每像素都重算」，误差只差在末位 —— 与 smooth.hlsl 的
// PS_BoxH 同一取舍。
// 3 tap 直接复用 7 tap 循环里的中间三项，省两次采样。
float4 PS_BoxH(vertex_data vert_in) : TARGET {
    float2 dx = float2(texel.x, 0.0);

    float v1 = image.Sample(pointTex, vert_in.uv - dx).r;
    float v2 = image.Sample(pointTex, vert_in.uv).r;
    float v3 = image.Sample(pointTex, vert_in.uv + dx).r;

    float s3 = v1 + v2 + v3;
    float s7 = s3
             + image.Sample(pointTex, vert_in.uv - dx * 2.0).r
             + image.Sample(pointTex, vert_in.uv + dx * 2.0).r
             + image.Sample(pointTex, vert_in.uv - dx * 3.0).r
             + image.Sample(pointTex, vert_in.uv + dx * 3.0).r;

    float lum = image.Sample(pointTex, vert_in.uv).b;
    return float4(s3 * (1.0 / 3.0), s7 * (1.0 / 7.0), lum, 1.0);
}

// ---------------------------------------------------------------------------
// BoxV：垂直方向同样两档，输出最终 box3 / box7
// ---------------------------------------------------------------------------
float4 PS_BoxV(vertex_data vert_in) : TARGET {
    float2 dy = float2(0.0, texel.y);

    // .r 是水平 box3 → 纵向 3 tap → 最终 box3
    // .g 是水平 box7 → 纵向 7 tap → 最终 box7
    // 两档必须各走各的：box3 与 box7 是两个不同尺度的带，混了 hf/hm 就全错。
    float a1 = image.Sample(pointTex, vert_in.uv - dy).r;
    float a2 = image.Sample(pointTex, vert_in.uv).r;
    float a3 = image.Sample(pointTex, vert_in.uv + dy).r;
    float box3 = (a1 + a2 + a3) * (1.0 / 3.0);

    float b1 = image.Sample(pointTex, vert_in.uv - dy).g;
    float b2 = image.Sample(pointTex, vert_in.uv).g;
    float b3 = image.Sample(pointTex, vert_in.uv + dy).g;
    float box7 = (b1 + b2 + b3
                  + image.Sample(pointTex, vert_in.uv - dy * 2.0).g
                  + image.Sample(pointTex, vert_in.uv + dy * 2.0).g
                  + image.Sample(pointTex, vert_in.uv - dy * 3.0).g
                  + image.Sample(pointTex, vert_in.uv + dy * 3.0).g) *
                 (1.0 / 7.0);

    float lum = image.Sample(pointTex, vert_in.uv).b;
    return float4(box3, box7, lum, 1.0);
}

// ---------------------------------------------------------------------------
// 软阈值 / 软膝（逐字照抄 CPU 的 softThrF / softKneeF）
// ---------------------------------------------------------------------------
float softThr(float v, float thr) {
    if (thr <= 0.0)
        return v;
    float a = abs(v);
    if (a <= thr)
        return 0.0;
    return (v < 0.0) ? -(a - thr) : (a - thr);
}

float softKnee(float v, float k) {
    if (k <= 0.0)
        return v;
    float a = abs(v);
    if (a <= k)
        return v;
    float c = k + (a - k) * k / a;
    return (v < 0.0) ? -c : c;
}

// ---------------------------------------------------------------------------
// Refill：把增量加回颜色
// ---------------------------------------------------------------------------
// CPU 侧（detailRefill 的落笔段）：
//     m  = mask/255
//     hf = lum − box3          hm = box3 − box7
//     d  = Σ 权重 × softKnee(softThr(带, 阈值), 膝)
//     dst += d      （三通道同加，alpha 不动）
// 脸内(f)/全局(g) 两路各有自己的阈值；阈值相同时 CPU 只算一次，这里也照做
// （同一个 kF 复用，省两次函数调用 —— 数值完全一致）。
//
// 权重全 0 时直接返回颜色本身：CPU 那边是 `continue`（一个字节都不动），
// 等价。这个分支在「只有脸内锐化」的默认配置下覆盖大半幅画面（背景），
// 值得留。
float4 PS_Refill(vertex_data vert_in) : TARGET {
    float4 col = image.Sample(pointTex, vert_in.uv);
    float3 c   = col.rgb * 255.0;

    float m  = (noMask != 0) ? 0.0 : skinMask.Sample(pointTex, vert_in.uv).r;

    float3 plane = image3.Sample(pointTex, vert_in.uv).rgb;
    float l3  = plane.r;
    float l7  = plane.g;
    float lum = plane.b;

    float fwF = m * dFine;
    float fwG = (1.0 - m) * dGFine;
    float mwF = m * dMid;
    float mwG = (1.0 - m) * dGMid;

    if (fwF == 0.0 && fwG == 0.0 && mwF == 0.0 && mwG == 0.0)
        return col;

    float hf = lum - l3;
    float hm = l3 - l7;

    float d = 0.0;
    if (fwF != 0.0 || fwG != 0.0) {
        float kF = softKnee(softThr(hf, dThrFine), dKneeFine);
        if (fwF != 0.0)
            d += fwF * kF;
        if (fwG != 0.0)
            d += fwG * ((dThrFineG == dThrFine)
                            ? kF
                            : softKnee(softThr(hf, dThrFineG), dKneeFine));
    }
    if (mwF != 0.0 || mwG != 0.0) {
        float kM = softKnee(softThr(hm, dThrMid), dKneeMid);
        if (mwF != 0.0)
            d += mwF * kM;
        if (mwG != 0.0)
            d += mwG * ((dThrMidG == dThrMid)
                            ? kM
                            : softKnee(softThr(hm, dThrMidG), dKneeMid));
    }
    if (d == 0.0)
        return col;

    float3 v = clamp(c + d, 0.0, 255.0);
    // CPU 是 `(unsigned char)v` —— **截断**，不是四舍五入。写进 RGBA8 渲染目标
    // 时硬件会四舍五入，所以这里先 floor 一次再交给它，否则每像素会多 1 灰阶。
    v = floor(v);
    return float4(v * (1.0 / 255.0), col.a);
}

// ---------------------------------------------------------------------------
// technique：一阶段一个（texrender 不能在一个 technique 内换渲染目标）
// ---------------------------------------------------------------------------
technique LumTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Lum(vert_in);
    }
}

technique HistTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Hist(vert_in);
    }
}

technique BoxHTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_BoxH(vert_in);
    }
}

technique BoxVTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_BoxV(vert_in);
    }
}

technique RefillTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Refill(vert_in);
    }
}
