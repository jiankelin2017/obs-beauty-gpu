// ===========================================================================
// mask.hlsl —— 皮肤掩码 / 眼睛保护（5b-2 切片 1，未接线）
//
// 移植自 beauty_core.cpp 的 buildMaskInto()（L2274-2600），目标是**逐像素等价**：
// 宿主用同样的整数几何 + 同样的浮点公式，算出来的 8-bit 掩码与 CPU 差 ≤ 1 灰阶。
//
// == 为什么把「整数几何」留在 CPU =========================================
// CPU 的边界全是**整数截断**：(int)(cx - hw)、x1 - x、featherRamp(y - y0 + 1, fe)…
// HLSL 的 float→int 是 round-toward-zero，但 MSVC 的 (int) 也是截断 —— 看起来一样，
// 可一旦表达式里有 `+0.5` 或负数，两边就会分叉。所以本 shader **一个几何量都不自己算**：
// x0/y0/x1/y1/fe/nx0/ny0/nx1/ny1 全部由宿主按 CPU 原式算好后以 float 形式传进来
// （值本身就是整数，浮点能精确表示），shader 只做逐像素部分。
// 这是切片 1 的核心约定，删掉它 = 把等价性交给运气。
//
// == 切片 1 的能力边界（重要）==============================================
//   · 只支持 **1 张脸**：多脸时宿主整帧回退 CPU（不做 blend_op=max 的合并，
//     OBS 的 effect 解析器对 blend_op 的字符串支持在本机没有源码可查，不冒险）。
//   · 支持 maskShape 的 bit0（脸用椭圆）/ bit1（脖子用椭圆），
//     **不支持 bit2（脖子逐行跟肤色扫描）** —— 那是顺序语义，宿主见到就回退 CPU。
//   · 不做 noMask 的语义：无脸时宿主不画这张纹理，改为绑定全 0/全 255 的常量纹理
//     （与 3.1.51 的 host 侧行为一致，见 obs-beauty.cpp:1796）。
//
// == OBS effect 方言五条红线（踩过，别再犯）================================
//   1) 顶层不能有 `static const` 数组（本文件无表）
//   2) 函数不能有数组形参
//   3) sampler 必须 `sampler_state <name> { ... };`（无 uniform、无 =）
//   4) technique 入口必须带参
//   5) 矩阵参数名必须 `ViewProj`，且 `mul(float4(pos.xyz,1.0), ViewProj)`
//
// == 值域约定 =============================================================
// 内部一律用 **0..255 的整数域**（int），最后一步才 ×(1/255) 写进 R8 —— 与 CPU
// 的 `unsigned char` 同尺度。**不要**在中途改成 0..1 浮点再取整：CPU 的
// `(wgt*feather + 128) >> 8` 是整数右移，浮点乘 1/256 在 .5 边界会差 1。
// ===========================================================================

uniform float4x4 ViewProj;

// 源帧。只被 PS_Skin 采样（PS_Eye 用不到，但宿主仍需绑 —— OBS 会对未绑参数报警）。
uniform texture2d image;
sampler_state imageTex {
    Filter   = Point;      // 逐像素一一对应，绝不能插值
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform float2 size;       // (w, h)，整数像素尺寸
uniform float2 texel;      // (1/w, 1/h)，保留给将来的 ROI 版本

// ---- 单张脸的几何（全部是 CPU 算好的整数，见文件头）-----------------------
uniform float4 faceRect;   // x0, y0, x1, y1   —— CPU: (int)(cx-hw) 等，已 clamp
uniform float4 faceGeom;   // cx, cy, a(=hw), b(=hh)
uniform float  faceFe;     // 羽化像素（CPU: (int)(f.w*0.08+0.5)，≤64）
// 脖子通道的循环上下界**随 maskShape 变**，宿主必须按模式算好再传：
//   矩形分支（bit1 未置）: y ∈ [y1, cy+hh+f.h*len)
//   椭圆分支（bit1 置）  : y ∈ [clamp(cyN−B), clamp(cyN+B))，其中 cyN = y1
// 后者会**伸进脸框区域**（B 从 y1 起算），所以两条分支不能共用一个 ny0 ——
// 共用会把椭圆上半截切掉，掩码在脖子上沿缺一块，而且不报错。
uniform float4 neckRect;   // nx0, nyLo, nx1, nyHi —— 按上面两种模式之一填
uniform float4 neckEll;    // A(=f.w*halfW), B(=f.h*len), cyN(=y1), unused
uniform float  neckFe;     // 脖子羽化像素（CPU 按脖子自身宽度算）
uniform int    maskShape;  // bit0=脸椭圆, bit1=脖子椭圆（bit2 不支持，宿主回退）

// ---- 肤色椭圆（C20 自适应中心，由 CPU 的 estimateSkinCenter 给）-----------
uniform float4 skinEllipse; // cb, cr, ru, rv

// ---- 眼睛保护（只画眼睛，不画嘴：与参考实现结构一致，见 CPU 注释 L2574）----
uniform float4 eye0;        // cx, cy, rx, ry
uniform float4 eye1;

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

// CPU 的 smoothStep()（beauty_core.cpp:171）。b<=a 的退化分支必须保留：
// CPU 在 featherRamp 里会传 fe=0，那时它走的是「不是 smoothstep」的另一条路。
float smoothStep(float a, float b, float x)
{
    if (b <= a) return (x < a) ? 0.0 : 1.0;
    float t = (x - a) / (b - a);
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// CPU 的 featherRamp()（beauty_core.cpp:783）。CPU 用 64×64 查表省除法，
// 表本身是 `t*t*(3-2t)*256+0.5` 下取整 —— 这里按公式直算，结果相同。
// 早退顺序必须与 CPU 一致：fe<=0 → 256，dist<=0 → 0，dist>=fe → 256。
int featherRamp(int dist, int fe)
{
    if (fe <= 0) return 256;
    if (dist <= 0) return 0;
    if (dist >= fe) return 256;
    float t = (float)dist / (float)fe;
    return (int)(t * t * (3.0 - 2.0 * t) * 256.0 + 0.5);
}

// CPU 的 skinWeight()（beauty_core.cpp:954）。三个部分缺一不可：
//   ① CbCr 平面椭圆距离（0.6..1.3 软过渡）
//   ② 亮度下限门控（暗部不算皮肤）
//   ③ 亮度上限门控（过曝不算：鼻梁/额头高光磨平会变死白）
// 系数抄错任何一位都不会报错、只会「效果略弱」，所以下面每个常数都被
// verify_mask_compiles.py 的 [I]/[J] 钉死。
int skinWeight(float3 c)
{
    float y  = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    float cb = -0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;
    float cr = 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;

    float du = (cb - skinEllipse.x) / skinEllipse.z;
    float dv = (cr - skinEllipse.y) / skinEllipse.w;
    float d  = sqrt(du * du + dv * dv);

    float m = 1.0 - smoothStep(0.6, 1.3, d);
    m *= smoothStep(0.12, 0.30, y) * (1.0 - smoothStep(0.94, 0.99, y));
    if (m <= 0.0) return 0;
    int v = (int)(m * 255.0 + 0.5);
    return (v > 255) ? 255 : v;
}

// 落笔：CPU 是 `(wgt*feather + 128) >> 8`（整数右移，不是 /256 浮点）。
int paintWeight(int wgt, int feather)
{
    if (wgt <= 0) return 0;
    if (feather >= 256) return wgt;
    return (wgt * feather + 128) >> 8;
}

float4 PS_Skin(vert_out vert_in) : TARGET
{
    int2 p = int2(floor(vert_in.uv * size));
    int x = p.x;
    int y = p.y;

    int x0 = (int)(faceRect.x + 0.5);
    int y0 = (int)(faceRect.y + 0.5);
    int x1 = (int)(faceRect.z + 0.5);
    int y1 = (int)(faceRect.w + 0.5);
    int fe = (int)(faceFe + 0.5);

    int nx0 = (int)(neckRect.x + 0.5);
    int nyLo = (int)(neckRect.y + 0.5);
    int nx1 = (int)(neckRect.z + 0.5);
    int nyHi = (int)(neckRect.w + 0.5);
    int feN = (int)(neckFe + 0.5);

    int v = 0;

    // ---- 脸 ------------------------------------------------------------
    if (x >= x0 && x < x1 && y >= y0 && y < y1) {
        int feather = -1;
        if ((maskShape & 1) != 0) {
            // 椭圆距离场：与 CPU 同一近似（(1-q)*rEff），rEff = 0.5*(a+b)。
            // 注意 CPU 是「先算 1/(a*a) 再乘」，这里照抄 —— 直接写 dx*dx/(a*a)
            // 会让除法与倒数乘法在末位上分叉，逐像素对拍会零星差 1。
            float a = faceGeom.z;
            float b = faceGeom.w;
            float rEff = 0.5 * (a + b);
            float ia2 = 1.0 / (a * a);
            float ib2 = 1.0 / (b * b);
            float dx = (float)x + 0.5 - faceGeom.x;
            float dy = (float)y + 0.5 - faceGeom.y;
            float q = sqrt(dx * dx * ia2 + dy * dy * ib2);
            if (q < 1.0) {
                int dpx = (int)((1.0 - q) * rEff + 0.5);
                feather = featherRamp(dpx, fe);
            }
        } else {
            // 矩形 + 四边羽化。CPU 的取法：先 min(fTop, fx)，再**仅在**脖子横向
            // 范围之外才用 fBot —— 下巴正下方交给脖子通道续上，否则会出一条横线。
            int fTop = featherRamp(y - y0 + 1, fe);
            int fBot = featherRamp(y1 - y, fe);
            int dl = x - x0 + 1;
            int dr = x1 - x;
            int fx = featherRamp(min(dl, dr), fe);
            feather = min(fTop, fx);
            if ((x < nx0 || x >= nx1) && fBot < feather) feather = fBot;
        }

        if (feather >= 0) {
            float3 c = floor(image.Sample(imageTex, vert_in.uv).rgb * 255.0 + 0.5) * (1.0 / 255.0);
            v = max(v, paintWeight(skinWeight(c), feather));
        }
    }

    // ---- 脖子 ----------------------------------------------------------
    if (x >= nx0 && x < nx1 && y >= nyLo && y < nyHi) {
        int feather = -1;
        if ((maskShape & 2) != 0) {
            float A = neckEll.x;
            float B = neckEll.y;
            float rEff = 0.5 * (A + B);
            float ia2 = 1.0 / (A * A);
            float ib2 = 1.0 / (B * B);
            float dx = (float)x + 0.5 - faceGeom.x;
            float dy = (float)y + 0.5 - neckEll.z;
            float q = sqrt(dx * dx * ia2 + dy * dy * ib2);
            if (q < 1.0) {
                int dpx = (int)((1.0 - q) * rEff + 0.5);
                feather = featherRamp(dpx, feN);
            }
        } else {
            int fBot = featherRamp(nyHi - y, feN);
            int dl = x - nx0 + 1;
            int dr = nx1 - x;
            int fx = featherRamp(min(dl, dr), feN);
            feather = min(fBot, fx);
        }

        if (feather >= 0) {
            float3 c = floor(image.Sample(imageTex, vert_in.uv).rgb * 255.0 + 0.5) * (1.0 / 255.0);
            v = max(v, paintWeight(skinWeight(c), feather));
        }
    }

    // R8 目标：只写 R。×(1/255) 让 unorm 量化回同一个整数。
    return float4((float)v * (1.0 / 255.0), 0.0, 0.0, 1.0);
}

// 眼睛保护椭圆。CPU 把**平方距离**直接喂 smoothstep(0.35,1.0)（不是 sqrt），
// 抄成 sqrt 会让保护范围缩成原来的一半 —— 而且不报错，只是眼下被磨糊。
int protectEllipse(float2 p, float4 e)
{
    if (e.z < 0.5 || e.w < 0.5) return 0;
    float du = ((float)p.x + 0.5 - e.x) / e.z;
    float dv = ((float)p.y + 0.5 - e.y) / e.w;
    float q = du * du + dv * dv;
    if (q >= 1.0) return 0;
    float t = smoothStep(0.35, 1.0, q);
    return (int)((1.0 - t) * 255.0 + 0.5);
}

float4 PS_Eye(vert_out vert_in) : TARGET
{
    int2 p = int2(floor(vert_in.uv * size));
    int v = max(protectEllipse(float2(p), eye0), protectEllipse(float2(p), eye1));
    return float4((float)v * (1.0 / 255.0), 0.0, 0.0, 1.0);
}

technique MaskSkinTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Skin(vert_in);
    }
}

technique MaskEyeTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Eye(vert_in);
    }
}
