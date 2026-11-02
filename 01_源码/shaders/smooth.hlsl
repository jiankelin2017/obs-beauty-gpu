// ===========================================================================
// smooth.hlsl —— 磨皮（双尺度带通 + 振幅门控）GPU 着色器
//
// 移植自 beauty_smooth.cpp 的 surface3RgbBand()（L1087-1259，core 1.4.0）。
// 结构见 C23 §12：B1 窄 / B2 宽两个不同尺度的 3 次盒式，D = B2 − B1 是带通，
// 振幅门控 w = 1/(1+(a/T)^gwp) 负责「越平越磨、越有结构越保守」。
//
// == 值域约定（最重要的一条）================================================
// 中间纹理用 RGBA16F（创建失败时由宿主回退 RGBA32F），**装 0..255 的浮点值**，不是 0..1。
// 16F 是 3.1.76-p3 为绕过 RTX 5060 上 32F 渲染目标 + Linear 采样触发的 device-lost 崩溃而改。
// 理由：CPU 端 surface3RgbBand 全程用 float 但值域是「灰阶」(0..255)，所有常数
// （T=6.5、inv255、clamp 255、+0.5 取整）都按这个域写。GPU 沿用同一值域，
// 公式可以逐字照抄，不必重新推导每个常数的尺度 —— 那些"顺手改成 0..1"的地方
// 正是历史上数值对不上的来源。
// 只有最外层输入(采样 in_tex)与最终输出各做一次 ×255 / ÷255。
//
// == OBS effect 方言的五条红线（踩过，别再犯；详见 tone.hlsl 文件头）==========
//   1) 顶层不能有 `static const float X[N] = {...};`（本文件不用表，无风险）
//   2) 不能有数组形参
//   3) sampler 必须 `sampler_state <name> { ... };`，无 uniform、无 =
//   4) technique 入口必须带参：`VSDefault(vert_in)` / `PS_xxx(vert_in)`
//   5) 矩阵参数名必须是 `ViewProj`，且 `mul(float4(pos.xyz,1.0), ViewProj)`
//
// == 渲染顺序（OBS 侧编排，共 21 次 draw，除 Comp 外都在半分辨率）============
//   Down   ×1   全分辨率 → 半分辨率，2×2 盒式（对齐 CPU 的整数 2×2 抽样）
//   BoxH/V ×6   B1 = 3 趟可分离盒式，半径 r1
//   BoxH/V ×6   B2 = 3 趟可分离盒式，半径 r2
//   Amp    ×1   a = |C.g − B2.g|，只测绿通道（C23 §3.1 ③）
//   BoxH/V ×6   amp = 3 趟可分离盒式，半径 ra
//   Comp   ×1   全分辨率，①→②→③→I→eye，clamp + 0.5 取整
//
// 盒式为什么不合成 2D：CPU 的 bandBoxN 就是 H,V,H,V,H,V 交替（可分离），
// 分开做不仅与它同构，浮点求和顺序也最接近。合成 2D 核数学等价但顺序不同，
// 逐像素对拍的误差会白白变大。
// ===========================================================================

uniform float4x4 ViewProj;

// ---- 纹理 -----------------------------------------------------------------
// image 是"通用输入槽"：Down 时装全分辨率源图，盒式/Amp 时装上一趟的中间层，
// Comp 时装全分辨率源图。同一个参数名在不同 draw 里绑不同纹理。
uniform texture2d image;
sampler_state imageTex {          // 线性：Comp 里做半分辨率 → 全分辨率的双线性
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform texture2d image2;         // Comp: B1（半分辨率，0..255）
sampler_state imageTex2 {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform texture2d image3;         // Comp: B2
sampler_state imageTex3 {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

uniform texture2d image4;         // Comp: amp
sampler_state imageTex4 {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

// 点采样：Down 与所有盒式 pass 的采样点都精确落在 texel 中心，不能有插值。
// 盒式的边界处理靠 AddressU/V = Clamp —— 正是 CPU 的 clampi(x+k, 0, bw-1)。
sampler_state boxTex {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

// 掩码：两张独立单通道纹理（3.1.51 从一张 GS_R8G8 拆开）。
//
// 拆的理由不是带宽，是**CPU 侧的交错开销**：合成一张时每帧要在 CPU 上跑一遍
// `for k: gb[2k]=mask[k]; gb[2k+1]=protect[k]`，720p 就是 92 万次迭代 + 写 1.8MB；
// 而引擎递进来的 mask / protect 本来就是两块连续缓冲，拆成两张就能
// `gs_texture_set_image` 直传，一次 CPU 拷贝都不做（见 obs-beauty.cpp 的
// 生产钩子）。上传字节数不变，省掉的是那趟内存写。
//
// ⚠ 采样值已是 0..1（GS_R8 是 UNORM），不要再 /255 —— 也别照抄 tone.hlsl 里
// 那个把 0..1 搬回 0..255 的 `* 255.0`，方向相反。
uniform texture2d skinMask;   // R = 皮肤掩码
sampler_state skinMaskS {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};
uniform texture2d eyeMask;    // R = 眼睛保护
sampler_state eyeMaskS {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

// ---- 标量参数 -------------------------------------------------------------
uniform float2 srcSize;   // 全分辨率 w, h
uniform float2 dstSize;   // 半分辨率 ww, wh
uniform float2 texel;     // 1/ww, 1/wh（盒式步长）
uniform int    rad;       // 当前盒式半径（随画幅变，CPU 侧算好）
uniform float  invBox;    // 1/(2·rad+1)

uniform float  gI;        // 强度 I（档位查表得来）
uniform float  gE;        // evenfactor e
uniform float  gT;        // 振幅门控阈值 T（灰阶）
uniform float  gGwp;      // 门控陡度指数
uniform float  gEyeK;     // 眼睛保护倍率 0..1（CPU 的 protectK/100）

struct vertex_data {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

vertex_data VSDefault(vertex_data vert_in) {
    vertex_data vsout;
    // 名字必须是 ViewProj、乘法必须是"行向量 × 矩阵"——写错不报错，只黑屏。
    vsout.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    vsout.uv  = vert_in.uv;
    return vsout;
}

// ---------------------------------------------------------------------------
// Down：全分辨率 → 半分辨率，2×2 盒式
// ---------------------------------------------------------------------------
// 与 CPU 逐字对齐（beauty_smooth.cpp L1150-1170）：
//     y0 = y*2, y1 = min(y0+1, h-1), x0 = x*2, x1 = min(x0+1, w-1)
//     out = (r0[x0] + r0[x1] + r1[x0] + r1[x1]) * 0.25
// 注意是**求和 ×0.25**，不是四个双线性平均。奇数宽高时最后一个块会重复
// 采到同一个像素（x1 == x0），CPU 那边也是这样夹的。
float4 PS_Down(vertex_data vert_in) : TARGET {
    int ox = (int)(vert_in.uv.x * dstSize.x);
    int oy = (int)(vert_in.uv.y * dstSize.y);

    int x0 = ox * 2;
    int y0 = oy * 2;
    int x1 = min(x0 + 1, (int)srcSize.x - 1);
    int y1 = min(y0 + 1, (int)srcSize.y - 1);

    float2 u00 = float2(((float)x0 + 0.5) / srcSize.x, ((float)y0 + 0.5) / srcSize.y);
    float2 u10 = float2(((float)x1 + 0.5) / srcSize.x, ((float)y0 + 0.5) / srcSize.y);
    float2 u01 = float2(((float)x0 + 0.5) / srcSize.x, ((float)y1 + 0.5) / srcSize.y);
    float2 u11 = float2(((float)x1 + 0.5) / srcSize.x, ((float)y1 + 0.5) / srcSize.y);

    float3 a = image.Sample(boxTex, u00).rgb;
    float3 b = image.Sample(boxTex, u10).rgb;
    float3 c = image.Sample(boxTex, u01).rgb;
    float3 d = image.Sample(boxTex, u11).rgb;

    // 0..1 → 0..255 的唯一入口（见文件头的值域约定）
    float3 s = (a + b + c + d) * 0.25 * 255.0;
    return float4(s, 1.0);
}

// ---------------------------------------------------------------------------
// 可分离盒式：水平 / 垂直
// ---------------------------------------------------------------------------
// CPU 的 bandBoxH/bandBoxV 对每一行/列做「窗口内 (2r+1) 个像素求平均」，
// 窗口越界时索引 clamp 到 [0, bw-1]。这里靠 boxTex 的 Clamp 寻址得到同一行为。
// 逐步累加而非滑动窗差分：CPU 用滑动窗是为了 O(1)/像素且每 32 列重算一次抗漂移；
// GPU 每像素独立求和，等于「每像素都重算」，反而更准 —— 差别只在 float 末位。
float4 PS_BoxH(vertex_data vert_in) : TARGET {
    float3 s = float3(0.0, 0.0, 0.0);
    for (int k = -rad; k <= rad; ++k)
        s += image.Sample(boxTex, vert_in.uv + float2((float)k * texel.x, 0.0)).rgb;
    return float4(s * invBox, 1.0);
}

float4 PS_BoxV(vertex_data vert_in) : TARGET {
    float3 s = float3(0.0, 0.0, 0.0);
    for (int k = -rad; k <= rad; ++k)
        s += image.Sample(boxTex, vert_in.uv + float2(0.0, (float)k * texel.y)).rgb;
    return float4(s * invBox, 1.0);
}

// ---------------------------------------------------------------------------
// Amp：振幅代理 a = |C.g − B2.g|
// ---------------------------------------------------------------------------
// 只测绿通道（C23 §3.1 ③）；用 |C−B2| 而不是 |B2−B1| 的理由见 C23 §12.3 ——
// 后者在 P=8 的振幅依赖是 1.0x，等于门控失效。
// image = 下采样后的源（C），image2 = B2 的最终结果。两者同分辨率，点采样。
float4 PS_Amp(vertex_data vert_in) : TARGET {
    float cg = image.Sample(boxTex, vert_in.uv).g;
    float bg = image2.Sample(boxTex, vert_in.uv).g;
    float d  = cg - bg;
    float a  = (d < 0.0) ? -d : d;
    return float4(a, a, a, 1.0);
}

// ---------------------------------------------------------------------------
// Comp：全分辨率合成
// ---------------------------------------------------------------------------
// 逐行照抄 CPU 的合成段（beauty_smooth.cpp L1243-1254）：
//     even = C − B1 + B2
//     even = C + (even − C)·skin                     ① mix(C, even, skin)
//     even += (B2 − even)·(skin·e)                   ② mix(even, B2, skin·e)
//     even = C + (even − C)·gw                       ③ 振幅门控
//     res  = C + (even − C)·I
//     out  = C + (res − C)·(1 − eye)
// ①②的顺序是确定的（C23 §3.3 有逐字源码）。③ 是本项目 3.1.17+ 的新增项。
//
// CPU 里 mask==0 有早退分支直接拷贝字节。数学上它等价于 skin=0 走完整公式
// （① ② ③ 都会把 even 退化成 C），所以这里不需要分支 —— 少一个分支对 GPU
// 反而更好，且避开了 wave 内分支发散。
float4 PS_Comp(vertex_data vert_in) : TARGET {
    float4 src = image.Sample(imageTex, vert_in.uv);
    float3 c   = src.rgb * 255.0;

    float  skin = skinMask.Sample(skinMaskS, vert_in.uv).r;
    float  eye  = eyeMask.Sample(eyeMaskS, vert_in.uv).r * gEyeK;

    float3 v1 = image2.Sample(imageTex2, vert_in.uv).rgb;   // B1（0..255）
    float3 v2 = image3.Sample(imageTex3, vert_in.uv).rgb;   // B2
    float  a  = image4.Sample(imageTex4, vert_in.uv).r;     // amp（0..255）

    float se      = skin * gE;
    float keepEye = 1.0 - eye;

    // ③ 振幅门控：w = 1/(1 + (a/T)^gwp)
    // gwp≈2 时走乘法（CPU 也是这么绕开 powf 的）；否则 pow。
    float at = a / gT;
    float ap = (gGwp > 1.99 && gGwp < 2.01) ? (at * at) : pow(max(at, 0.0), gGwp);
    float gw = 1.0 / (1.0 + ap);

    float3 even = c - v1 + v2;
    even = c + (even - c) * skin;        // ①
    even += (v2 - even) * se;            // ②
    even = c + (even - c) * gw;          // ③

    float3 res  = c + (even - c) * gI;
    float3 outv = c + (res - c) * keepEye;

    outv = clamp(outv, 0.0, 255.0);
    outv = floor(outv + 0.5);            // 同 CPU 的 (unsigned char)(v + 0.5f)
    return float4(outv * (1.0 / 255.0), src.a);
}

// ---------------------------------------------------------------------------
// technique：一个阶段一个，OBS 侧按阶段循环 draw（texrender 不能在一个
// technique 内换渲染目标，所以不能写成单 technique 多 pass）
// ---------------------------------------------------------------------------
technique DownTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Down(vert_in);
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

technique AmpTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Amp(vert_in);
    }
}

technique CompTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PS_Comp(vert_in);
    }
}
