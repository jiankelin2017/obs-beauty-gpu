// ===========================================================================
// region.hlsl —— 五官段：椭圆增益族（切片 4a，family A）
//
// 移植自 beauty_region.cpp 的 applyEllipses()（L64-170）。这一族覆盖：
//   五官立体 8 个椭圆、亮眼 / 去眼袋、法令纹下瓣 —— 它们的作用方式完全相同：
//   **每个椭圆贡献一个加法增益，最后统一乘皮肤掩码、统一 clamp、统一取整。**
//
// == 为什么用「参数纹理」而不是 uniform 数组 =============================
// OBS 的 effect 方言对顶层数组素来不友好（E3 的坑：顶层 static const 数组直接失败）。
// 每帧的椭圆数量还随脸数/参数变（8~14 个），塞进常量缓冲要么数组、要么十几个
// 独立 uniform 手写展开。所以走第三条路：**宿主把椭圆列表写进一张 64×1 的
// RGBA32F 纹理**，shader 循环采样它。
//   · 没有数组声明，方言风险归零；
//   · 顺序天然保持（k 递增），与 CPU 的 `for k` 完全一致；
//   · 每个椭圆 3 个 texel：A=(cx,cy,rx,ry) B=(cosT,sinT,gain,feather) C=(shade,_,_,_)
//   · 全部是**绝对像素坐标**（CPU 已经做过 faceX + e.cx*faceW），shader 不算几何。
//
// == 为什么必须单趟 ======================================================
// CPU 是「把所有椭圆的 acc 加完 → ×mask → clamp ±48 → 一次取整」。
// 若拆成「每椭圆一趟 + 加法混合」，每一步都会各自取整，重叠区的误差会随椭圆数
// 线性累积（立体恰好 8 个椭圆大量重叠）。所以必须一趟算完再取整。
//
// == 取整 ================================================================
// CPU 用 lroundf（四舍五入、半数远离零），并修过一个真 bug（3.1.23）：
// 负增益时用 `(int)(v - 0.5)` 会**系统性多减 1 档**。这里用 floor(v+0.5)：
// 对 v>=0 与 lroundf 等价；v<0 时结果会被 clamp 到 0，不影响。
// ===========================================================================

uniform float4x4 ViewProj;

uniform texture2d image;          // 当前颜色（磨皮/色调之后）—— 读 Y 与 RGB
sampler_state imageTex {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform texture2d skinMask;       // 皮肤软掩码（0..1，5b-2 之后也是 GPU 纹理）
sampler_state skinMaskTex {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

// 椭圆参数表：64×1 RGBA32F，每 3 个 texel 描述一个椭圆，见文件头。
// ⚠ SampleLevel 吃的是**归一化 UV**，不是纹素坐标。写成 (k*3+0.5, 0.5) 会被
// Clamp 夹到最后一个 texel（全 0）—— 表现是「GPU 一个椭圆都没加，差异恰好
// 等于 CPU 的效果量」。2026-11-01 第一次 R1 就是这个（maxdiff=6、6% 像素）。
#define REGION_PARAM_W 64.0
uniform texture2d ellipseTex;
sampler_state ellipseTexS {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform int   ellipseCount;       // 本帧有效椭圆数
uniform float2 texSize;           // (w, h)

// ---- 法令纹（切片 4b）----------------------------------------------------
uniform int   lrRadius;           // 邻域最大半径 1..24
uniform float fSlope;             // Δ = min(maxLift, slope·(LR−Y))
uniform float fMaxLift;
uniform float fGateLo;            // 暗差闸门 smoothstep(lo, hi, dRaw)
uniform float fGateHi;

struct vert_in {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct vert_out {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

vert_out VSDefault(vert_in vert_in)
{
    vert_out o;
    o.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    o.uv  = vert_in.uv;
    return o;
}

// CPU 的 smoothStep()（beauty_region.cpp:55）—— 注意与 mask.hlsl 里那份同式，
// b<=a 的退化分支必须保留（feather 可能被 clamp 到 0.95，但仍是 b>a）。
float smoothStep(float a, float b, float x)
{
    if (b <= a) return (x < a) ? 0.0 : 1.0;
    float t = (x - a) / (b - a);
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float lumOf(float3 c)
{
    // 与 CPU 同域：先回到 0..255 字节值再算 Y
    float3 b = floor(c * 255.0 + 0.5);
    return 0.299 * b.r + 0.587 * b.g + 0.114 * b.b;
}

float4 PS_Region(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float3 src = image.Sample(imageTex, uv).rgb;
    float  mw  = skinMask.Sample(skinMaskTex, uv).r;

    // 掩码为 0 的像素 CPU 直接 continue（一个字节都不碰）。这里返回原值即可。
    if (mw <= 0.0) {
        return float4(src, 1.0);
    }

    // 像素中心坐标 —— CPU 全部用 x+0.5 / y+0.5（对小而锐的特征差半格就看得出来）。
    float2 p = uv * texSize;          // 已在像素中心
    // ⚠ Y 必须与 CPU **同域**（0..255 字节值）。写成 0.299*src.r（0..1 域）会让
    // 下面的 shade 加权几乎失效 —— 2026-11-01 第一次 R1 对拍就是这样红的：
    // maxdiff=6、8.4% 像素、avg 1.6，而所有 shade>0 的椭圆都偏亮。
    float  Y = lumOf(src);

    float acc = 0.0;
    int n = ellipseCount;
    for (int k = 0; k < n; k++) {
        // 每椭圆 3 个 texel：A/B/C。
        // 这里必须用 **SampleLevel(..., 0)** 而不是 Sample：Sample 是"梯度指令"，
        // 放在迭代次数随运行期变化的循环里，fxc 会报 X3511「无法展开循环」并拒绝编译
        // （2026-11-01 第一次门禁就是这么红的）。SampleLevel 显式给 0 级，不依赖导数，
        // 与点采样在本用途上完全等价（我们本来就要 texel 中心的值）。
        float4 A = ellipseTex.SampleLevel(ellipseTexS, float2(((float)(k * 3) + 0.5) / REGION_PARAM_W, 0.5), 0.0);
        float4 B = ellipseTex.SampleLevel(ellipseTexS, float2(((float)(k * 3 + 1) + 0.5) / REGION_PARAM_W, 0.5), 0.0);
        float4 C = ellipseTex.SampleLevel(ellipseTexS, float2(((float)(k * 3 + 2) + 0.5) / REGION_PARAM_W, 0.5), 0.0);

        float dx = p.x - A.x;
        float dy = p.y - A.y;
        float u  = (dx * B.x + dy * B.y) / A.z;    // cosT, sinT
        float t2 = (-dx * B.y + dy * B.x) / A.w;
        float d2 = u * u + t2 * t2;
        if (d2 >= 1.0) continue;
        float d = sqrt(d2);
        // 软边：内圈满强度（d < feather），外圈 smoothstep 归零
        float fall = 1.0 - smoothStep(B.w, 1.0, d);   // B.w = feather
        float g = B.z * fall;                          // B.z = gain
        // shade 越大越只提亮暗部（眼白/牙齿/高光不该继续提亮）
        if (C.x > 0.0)
            g *= (1.0 - C.x * (Y * (1.0 / 255.0)));
        acc += g;
    }

    if (acc != 0.0) {
        acc *= mw;
        acc = clamp(acc, -48.0, 48.0);   // 与 CPU 同一个防御上限
        float3 v = floor(src * 255.0 + acc + 0.5);   // 见文件头的取整说明
        return float4(clamp(v, 0.0, 255.0) * (1.0 / 255.0), 1.0);
    }
    return float4(src, 1.0);
}

technique RegionEllipseTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Region(vert_in);
    }
}

// ===========================================================================
// 切片 4b：法令纹填平（beauty_region.cpp:applyFoldRemoval，L182-329）
//
// CPU 三步：亮度平面 → 可分离邻域 max（先横后纵）= LR →
//          Δ = min(maxLift, slope·max(0, LR−Y)) · gate · wgt · max(mw, wgt)，
//          然后 `p[c] += Δ`（这里 Δ>0，所以 (int)(p+d+0.5) 就是四舍五入）。
//
// 注意两个反直觉点（抄错会让效果"看不见"或"糊到脸颊上"）：
//   1. **限制因子是 max(皮肤掩码, 区域权重)**，不是乘掩码。褶皱越深肤色判定越低，
//      直接乘会在最该提亮的那条线上把效果掐掉（实测 corr(掩码, 参照Δ) = −0.66）。
//   2. **边缘是"跳过"而不是 clamp**。max 滤波跳过的语义与 Clamp 采样在这里等价：
//      窗口一定包含边缘像素本身，clamp 只是把它的值重复一遍，max 不变。
// ===========================================================================

// 横向 max：输入 = 当前颜色，输出 R = 窗口内最大亮度
// （lumOf 已在文件上方定义一次，这里不再重复 —— HLSL 不允许重定义。）
float4 PS_FoldMaxH(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float2 px = uv * texSize;
    float mx = -1.0e20;
    int r = lrRadius;
    for (int k = -24; k <= 24; k++) {
        if (k < -r || k > r) continue;
        float2 q = float2(clamp(px.x + (float)k, 0.5, texSize.x - 0.5), px.y);
        float v = lumOf(image.SampleLevel(imageTex, q / texSize, 0.0).rgb);
        if (v > mx) mx = v;
    }
    return float4(mx * (1.0 / 255.0), 0.0, 0.0, 1.0);
}

// 纵向 max：输入 = 上一趟的 R（0..1 归一化），输出 R = LR/255
float4 PS_FoldMaxV(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float2 px = uv * texSize;
    float mx = -1.0e20;
    int r = lrRadius;
    for (int k = -24; k <= 24; k++) {
        if (k < -r || k > r) continue;
        float2 q = float2(px.x, clamp(px.y + (float)k, 0.5, texSize.y - 0.5));
        float v = image.SampleLevel(imageTex, q / texSize, 0.0).r;
        if (v > mx) mx = v;
    }
    return float4(mx, 0.0, 0.0, 1.0);
}

// 区域权重：各椭圆软边之和，截到 1（CPU 的同名循环，L290-305）
float ellipseWgt(float2 p, int n)
{
    float wgt = 0.0;
    for (int k = 0; k < n; k++) {
        float4 A = ellipseTex.SampleLevel(ellipseTexS, float2(((float)(k * 3) + 0.5) / REGION_PARAM_W, 0.5), 0.0);
        float4 B = ellipseTex.SampleLevel(ellipseTexS, float2(((float)(k * 3 + 1) + 0.5) / REGION_PARAM_W, 0.5), 0.0);
        float dx = p.x - A.x;
        float dy = p.y - A.y;
        float u  = (dx * B.x + dy * B.y) / A.z;
        float t2 = (-dx * B.y + dy * B.x) / A.w;
        float d2 = u * u + t2 * t2;
        if (d2 >= 1.0) continue;
        wgt += 1.0 - smoothStep(B.w, 1.0, sqrt(d2));
    }
    return min(wgt, 1.0);
}

// 施加：Δ 加上去。image = 当前颜色，image2 = LR（R 通道存 LR/255）
uniform texture2d lrTex;
sampler_state lrTexS {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

float4 PS_FoldApply(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float2 p  = uv * texSize;
    float3 src = image.Sample(imageTex, uv).rgb;
    float  mw  = skinMask.Sample(skinMaskTex, uv).r;
    float  wgt = ellipseWgt(p, ellipseCount);
    if (wgt <= 0.0)
        return float4(src, 1.0);

    float Y  = lumOf(src);
    float LR = lrTex.Sample(lrTexS, uv).r * 255.0;
    float dRaw = LR - Y;
    if (dRaw <= 0.0)
        return float4(src, 1.0);

    float gate = smoothStep(fGateLo, fGateHi, dRaw);
    float d = min(fSlope * dRaw, fMaxLift);
    float lim = max(mw, wgt);          // ← 见文件头的第 1 条
    d *= gate * wgt * lim;
    if (d <= 0.0)
        return float4(src, 1.0);

    float3 v = floor(src * 255.0 + d + 0.5);
    return float4(clamp(v, 0.0, 255.0) * (1.0 / 255.0), 1.0);
}

technique RegionFoldMaxHTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_FoldMaxH(vert_in);
    }
}

technique RegionFoldMaxVTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_FoldMaxV(vert_in);
    }
}

technique RegionFoldApplyTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_FoldApply(vert_in);
    }
}

// ===========================================================================
// 切片 4c：祛斑痘（beauty_region.cpp:applySpotFix，L331-432）
//
// CPU：亮度平面（取自**当前 dst**）→ 半径 ≤6 的盒均值（边界 clamp、分母按
// **实际参与元素数**）→ dev = L − mean，|dev| ∈ [thrLo, thrHi] 才动 →
// pull = clamp(−k·mw·dev, ±24) → 取整加回。
//
// 两个必须照抄的点：
//   1. **分母不是 win²**，是 (实际行数 × 实际列数)。CPU 注释记过一个真 bug：
//      分母写成 win² 时 ROI 四边均值偏小、dev 被撑大，边缘整片被判成斑点。
//   2. 盒均值是**真均值**，所以不能用 Clamp 采样偷懒 —— Clamp 会重复边缘像素，
//      对 max 无害，对均值会系统性偏。这里用「求和 + 实际计数」两趟算出真均值。
// ===========================================================================

// 祛斑的作用域：[roiMin, roiMax) 的**像素中心**范围。CPU 的盒均值在 ROI 边界就 clamp，
// 不是画面边界 —— 在整帧上算会让 ROI 边缘一圈的均值偏（2026-11-01 实测 2.2% 像素）。
// ⚠ OBS 的 effect-parser **不接受一行声明两个 uniform**
// （`uniform float2 a, b;` 会报 Expected 'type'，2026-11-01 实测）。必须一行一个。
uniform float2 roiMin;
uniform float2 roiMax;
uniform int   sRadius;    // 1..6
uniform float sK;         // 强度
uniform float sThrLo;     // 斑点判据下限（细纹理不动）
uniform float sThrHi;     // 上限（真边缘不动）

// 横向窗口**求和**（不归一化），R 通道存和。
//
// 2.9.1（P0-10）：窗口语义必须与 CPU **逐字一致** —— 这是本次修的那个 bug。
// CPU（beauty_region.cpp:386-414）是**截断窗口**：yA=max(ly-r,0)、yB=min(ly+r,bh-1)，
// 分子只加 yA..yB，范围外的行**不计入**。
// 旧写法是 clamp(px+k, roiMin+0.5, roiMax-0.5)：边缘的第 2r+1 次采样会**重复**
// 同一个像素计入分子，而纵向分母 nx/ny 是按去重后的数量算的 —— 于是 ROI 边缘
// r 行/列内均值偏大、dev 偏小、pull 偏小。实测 720p 差 178 像素（bbox 正好是
// ROI 底边两行），见 `05_验证/out/logs/P0-10_spot_edge_20261103.md`。
// 现在改成「越界直接 skip」，分子与分母都按同一批像素算。
float4 PS_SpotSumH(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float2 px = uv * texSize;
    int r = sRadius;
    int ix = (int)floor(px.x);
    int lo = max(ix - r, (int)roiMin.x);
    int hi = min(ix + r, (int)roiMax.x - 1.0);
    float sum = 0.0;
    for (int k = -6; k <= 6; k++) {
        int xx = ix + k;
        if (xx < lo || xx > hi) continue;   // 截断，不是 clamp
        sum += lumOf(image.SampleLevel(imageTex,
                                       float2((float)xx + 0.5, px.y) / texSize,
                                       0.0).rgb);
    }
    return float4(sum * (1.0 / 255.0), 0.0, 0.0, 1.0);
}

// 纵向求和 + 按实际参与元素数归一化 → 真均值。输入 = 上一趟的 R（0..1 尺度）。
float4 PS_SpotMeanV(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float2 px = uv * texSize;
    int r = sRadius;
    int iy = (int)floor(px.y);
    int lo = max(iy - r, (int)roiMin.y);
    int hi = min(iy + r, (int)roiMax.y - 1.0);
    float sum = 0.0;
    for (int k = -6; k <= 6; k++) {
        int yy = iy + k;
        if (yy < lo || yy > hi) continue;   // 截断，不是 clamp
        sum += image.SampleLevel(imageTex,
                                 float2(px.x, (float)yy + 0.5) / texSize,
                                 0.0).r;
    }
    // 实际参与的元素数（与上面两趟 skip 的集合完全是同一批）
    int ix = (int)floor(px.x);
    int loX = max(ix - r, (int)roiMin.x);
    int hiX = min(ix + r, (int)roiMax.x - 1.0);
    float nx = (float)(hiX - loX + 1);
    float ny = (float)(hi - lo + 1);
    return float4(sum / (nx * ny), 0.0, 0.0, 1.0);
}

// 施加：image = 当前颜色，image2 = 均值（R 通道，0..1 尺度）
uniform texture2d meanTex;
sampler_state meanTexS {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

float4 PS_SpotApply(vert_out vert_in) : TARGET
{
    float2 uv = vert_in.uv;
    float3 src = image.Sample(imageTex, uv).rgb;
    // ROI 外 CPU 根本不进循环
    float2 pp = uv * texSize;
    if (pp.x < roiMin.x || pp.x >= roiMax.x || pp.y < roiMin.y ||
        pp.y >= roiMax.y)
        return float4(src, 1.0);
    float  mw  = skinMask.Sample(skinMaskTex, uv).r;
    if (mw <= 0.0)
        return float4(src, 1.0);

    float L    = lumOf(src);
    float mean = meanTex.Sample(meanTexS, uv).r * 255.0;
    float dev  = L - mean;
    float ad   = abs(dev);
    // 细纹理（|dev| 太小）与真边缘（太大）都不动
    if (ad < sThrLo || ad > sThrHi)
        return float4(src, 1.0);

    float pull = clamp(-sK * mw * dev, -24.0, 24.0);
    float3 v = floor(src * 255.0 + pull + 0.5);
    return float4(clamp(v, 0.0, 255.0) * (1.0 / 255.0), 1.0);
}

technique RegionSpotSumHTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_SpotSumH(vert_in);
    }
}

technique RegionSpotMeanVTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_SpotMeanV(vert_in);
    }
}

technique RegionSpotApplyTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_SpotApply(vert_in);
    }
}
