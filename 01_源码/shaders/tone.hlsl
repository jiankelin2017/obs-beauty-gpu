// ===========================================================================
// tone.hlsl —— 皮肤色调变换（清晰 / 美白 / 红润）GPU 着色器
//
// 移植自 beauty_freq.cpp 的 toneColor()（L582-635）+ LUT 数据（L481-539）。
// LUT 数值来自 05_验证/gen_tone_hlsl.py（只覆盖到 2.x 的模板）。
// **2026-11-05 更正**：本文件此后已手工扩展过多次（P3 的 3D-LUT 段、seg 参数、
// 以及本轮的 toneGlobal），生成器已不再与文件同步 —— 重跑它会把这些段落**整段
// 抹掉**。改 LUT 数值仍以生成器为准，但不要再直接跑它覆盖本文件；
// 需要重建时先把下面这些段落补进模板。
//
// ⚠ OBS effect 解析器的两条硬约束（踩过，别再犯）：
//   1) 顶层不能出现 `static const float X[N] = {...};` —— effect-parser.c 的
//      ep_parse_other 只认 property/const/uniform 三个 specifier，"static" 会被
//      当成类型名，随后 expected ';' 报错 → gs_effect_create_from_file() 失败。
//      所以 4 张表都写在 PSMain 函数体内（函数体是逐 token 原样拷贝，不解析）。
//   2) 不能有数组形参（ep_parse_func_param 不认 `float t[17]`），也不能用
//      `tbl + base` 这种指针运算（HLSL 报 X3022）。因此插值直接内联、显式下标。
//   3) sampler 必须用 OBS 自己的语法 `sampler_state <name> { ... };`，
//      不能写 HLSL 标准的 `uniform sampler_state x = sampler_state { ... };`
//      （ep_parse_sampler_state 只认「名字 + { ... } + ;」，没有 uniform 前缀、
//      也没有 = 赋值）。写成 HLSL 语法时 fxc 能过、OBS 运行期才报
//      "Invalid type 'sampler_state' used for assignment"。
//   4) technique 里的入口必须**带参数**：`vertex_shader = VSDefault(vert_in);`
//      不能写 `VSDefault()`。OBS 把括号里的 token 原样拼进生成的 HLSL，
//      空括号就成了零参调用，D3D 编译报
//      "X3013: 'VSDefault': no matching 0 parameter function"。
//   5) 顶点矩阵的参数名必须是 `ViewProj`，**不是 ViewProjMatrix**，且必须按
//      OBS 的约定写 `mul(float4(pos.xyz, 1.0), ViewProj)`（向量左、矩阵右）。
//      D3D11 后端按名字 "ViewProj" 自动绑定（d3d11-shader.cpp:82
//      gs_shader_get_param_by_name(this, "ViewProj")）。名字写错**不报任何错**，
//      只是矩阵永远停在单位阵 -> 顶点坐标 [0..cx] 被当成 NDC 直接丢掉
//      （NDC 只有 -1..1）-> 整屏黑，而 gs_texrender_get_texture() 照样返回
//      一张有效纹理，所以上层完全看不出是「没画上」还是「算错了」。
//
// 数学口径（与 CPU 逐字节对齐，守住 79.0 分）：
//   * 纯逐像素 LUT 查表，无邻域依赖。
//   * 施加顺序：清晰 → 美白 → 红润（先抬对比再提亮，与 CPU 完全一致）。
//   * 仅皮肤区（mask>0）被修改，且按 mask/255 缩放。
//   * 写回前 clamp[0,255] 再 +0.5 取整，与 CPU 的 (unsigned char)(v+0.5f) 同。
// ===========================================================================

uniform float4x4 ViewProj;
uniform texture2d   image;              // 输入帧（RGBA，0..1）
sampler_state imageTex {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};
uniform texture2d   mask;               // 皮肤掩码（单通道，值 0..255 存在 R）
sampler_state maskTex {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};
uniform float clarity;                  // 面板 0..100 → 调用方换算成 0..1
uniform float whiten;
uniform float rosy;

// ==== 方案 E / P3：3D-LUT 色彩（默认全关，逐字节走原路径）=================
// 与 SDK tex_combine 的差别：不把四波段硬塞进一张 1600 高纹理，
// 而是每张 LUT 独立一张纹理 + lutBand 指定波段，少一组出错点。
// lut3dMix = 0 时下面这些 uniform 一个都不会被读（分支外），
// 宿主仍需绑定：OBS 对未绑参数会报警，用 1×1 白纹理兜底即可。
uniform texture2d   lutTex;
sampler_state lutTexS {
    Filter   = Linear;
    AddressU = Clamp;      // L6：必须 Clamp，Repeat 会在 R/G 越界处镜像出接缝
    AddressV = Clamp;
};
uniform float4 lutDim;      // x=dim(蓝切片数) y=raw(行) z=col(列) w=预留
uniform float4 lutBand;     // xy = (波段 y 起点, 纹理总高)；zw 预留。
// 用 float4 而非 float2：宿主侧统一走 gs_effect_set_vec4，
// 不必为这一个参数引 vec2 头文件，也避免 OBS 对 float2 的宽度处理差异。
uniform float  lut3dMix;    // 0 = 现有一维表（默认）；1 = 完全走 3D-LUT
uniform float  toneEarlyOut;// 0 = 关（默认）；1 = 三路全关时早退
// 2026-11-05（复刻 SDK）：1 = 清晰/美白/红润整帧施加，不乘皮肤掩码（默认）；
// 0 = 旧行为（只在掩码内施加，掩码边界会变成一条可见色界）。
// SDK 的 p34/p57/p58/p139 里 whitten/clarity 是纯色彩 LUT，不读 tex_mask；
// 只有 surfaceblur_mix 读 tex_mask.a。所以这个开关必须与 CPU toneColor 同步。
uniform float  toneGlobal;

struct vertex_data {
    float4 pos   : POSITION;
    float2 uv    : TEXCOORD0;
};

vertex_data VSDefault(vertex_data vert_in) {
    vertex_data vsout;
    vsout.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    vsout.uv  = vert_in.uv;
    return vsout;
}

// 方案 E / P3：3D-LUT 采样（跨蓝切片三线性）。
// 不用 sampler 形参、不用 [unroll]、不用 fmod —— 本仓库 effect-parser 零先例。
float2 lutGrid(float b, float col)
{
    float q = floor(b / col);
    return float2(b - q * col, q);
}

float3 sampleLut3d(float3 c)
{
    float dim = lutDim.x, raw = lutDim.y, col = lutDim.z;
    float3 tc = clamp(c, 0.0f, 1.0f);
    float  b  = tc.b * (dim - 1.0f);
    float  b0 = floor(b);
    float  fr = b - b0;
    float2 q0 = lutGrid(b0, col);
    float2 q1 = lutGrid(b0 + 1.0f, col);
    float2 inv = float2(1.0f / col, 1.0f / raw);
    float2 p0 = q0 * inv
              + float2(0.5f / (col * dim), 0.5f / (raw * dim))
              + float2((1.0f / col - 1.0f / (col * dim)) * tc.r,
                       (1.0f / raw - 1.0f / (raw * dim)) * tc.g);
    float2 p1 = p0 + (q1 - q0) * inv;
    p0.y = (p0.y * lutBand.y + lutBand.x) / 1600.0f;
    p1.y = (p1.y * lutBand.y + lutBand.x) / 1600.0f;
    return lerp(lutTex.Sample(lutTexS, p0).rgb,
                lutTex.Sample(lutTexS, p1).rgb, fr);
}

float4 PSMain(vertex_data vert_in) : TARGET {
    // ---- LUT 数据（展平：idx = level*51 + channel*17 + anchor）----
    // 必须留在函数体内：见文件头的约束 1。
    static const float CLARITY[255] = {
          0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255,
          0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255,
          0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255,
          0, 16, 32, 47, 62, 78, 95,112,129,146,162,179,195,210,225,241,255,
          0, 16, 31, 46, 62, 79, 95,112,129,146,163,179,194,209,224,240,255,
          0, 16, 31, 46, 62, 79, 95,112,129,146,163,179,195,209,225,240,255,
          0, 16, 32, 46, 60, 77, 94,112,129,147,165,182,198,212,227,241,255,
          0, 16, 29, 44, 60, 77, 95,113,131,149,166,182,195,211,225,240,255,
          0, 16, 29, 44, 60, 77, 95,112,131,149,166,182,197,211,226,240,255,
          0, 16, 32, 45, 58, 75, 93,111,130,149,167,185,200,215,229,241,255,
          0, 16, 28, 42, 58, 76, 94,113,133,151,169,185,197,212,225,241,255,
          0, 16, 28, 42, 58, 76, 94,113,132,151,169,185,200,212,227,241,255,
          0, 16, 32, 43, 56, 74, 92,111,131,151,170,188,203,217,230,242,255,
          0, 16, 26, 40, 56, 74, 94,114,134,154,172,188,199,214,226,241,255,
          0, 16, 26, 41, 56, 74, 93,113,133,153,171,188,203,214,228,241,255,
    };
    static const float WHITEN[255] = {
          0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255,
          0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255,
          0, 16, 32, 48, 64, 80, 96,112,128,144,160,176,192,208,224,240,255,
          0, 16, 32, 49, 65, 82, 99,115,132,148,164,179,195,210,225,241,255,
          0, 16, 33, 49, 66, 82, 99,116,132,148,163,179,194,209,225,241,255,
          0, 16, 32, 49, 66, 83,100,116,133,149,164,179,195,210,225,240,255,
          0, 16, 32, 50, 67, 85,102,119,136,152,167,182,197,212,227,241,255,
          0, 16, 33, 51, 68, 85,103,119,136,152,167,182,196,211,225,241,255,
          0, 16, 32, 51, 68, 86,104,121,137,153,168,182,198,211,227,241,255,
          0, 16, 32, 50, 68, 87,105,122,140,156,171,185,200,214,228,242,254,
          0, 16, 34, 52, 69, 87,106,123,140,156,170,185,197,212,226,242,255,
          0, 16, 32, 52, 70, 89,108,125,142,158,172,186,201,213,228,241,255,
          0, 16, 32, 51, 70, 89,108,126,144,160,174,188,203,216,230,242,254,
          0, 16, 35, 54, 71, 90,109,126,143,159,174,187,199,214,227,242,255,
          0, 16, 32, 53, 72, 92,112,129,146,162,176,189,204,215,230,242,255,
    };
    static const float ROSE_G[17] = {   0,  0, -4, -4, -5, -5, -5, -5, -4, -4, -3, -3, -1, -1,  0,  0,  0 };
    static const float ROSE_B[17] = {   0,  0,  2,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0 };

    float4 src = image.Sample(imageTex, vert_in.uv);

    // E/P3 早退（默认关）。注意：现有代码有两条返回路径 —— sk<=0 时不量化，
    // sk>0 且三路全关时仍会走 floor(rgb+0.5) 量化。这里按 sk<=0 的表达式写，
    // 因此 toneEarlyOut=1 与 sk>0 的旧路径并不逐字节相同，启用前须做 A/B 对拍。
    // 默认 0，关闭时这一整段都不进，逐字节不变。
    if (toneEarlyOut > 0.5f &&
        clarity <= 0.0f && whiten <= 0.0f && rosy <= 0.0f) {
        float3 e0 = src.rgb * 255.0f;
        return float4(e0 / 255.0f, 1.0f);
    }

    float  m   = mask.Sample(maskTex, vert_in.uv).r * 255.0f;   // 0..255
    float3 rgb = src.rgb * 255.0f;                           // 色调运算走 0..255 空间
    // toneGlobal=1：整帧施加（复刻 SDK，无掩码边界）；=0：旧行为。
    float  sk  = (toneGlobal > 0.5f) ? 1.0f : m * (1.0f / 255.0f);

    // 非皮肤：一个字节都不动（对应 CPU toneColor 的 `if (mv == 0) continue;`）
    if (sk <= 0.0f)
        return float4(rgb / 255.0f, 1.0f);

    // ---- 档位插值（对应 C++ Lv5：level 0..1 → 表索引 0..4，上界夹到 3.999）----
    float xc = clamp(clarity, 0.0f, 1.0f) * 4.0f;
    if (xc > 3.999f) xc = 3.999f;
    int   ci = (int)xc;
    float cf = xc - (float)ci;

    float xw = clamp(whiten, 0.0f, 1.0f) * 4.0f;
    if (xw > 3.999f) xw = 3.999f;
    int   wi = (int)xw;
    float wf = xw - (float)wi;

    // ---- 清晰 → 美白（逐通道；顺序不能换：先抬对比再提亮）----
    // 通道循环次数是常量，编译器会展开，所以 rgb[c] 的下标最终是常量。
    for (int c = 0; c < 3; c++) {
        if (clarity > 0.0f) {
            float v = rgb[c];
            float p = v * (1.0f / 16.0f);          // 17 锚点、步长 16
            int   i = (int)p;
            if (i < 0)  i = 0;
            if (i > 15) i = 15;                    // 最后一档锚点 15→16
            float f  = p - (float)i;
            int   b0 = ci * 51 + c * 17;
            int   b1 = b0 + 51;
            float l0 = CLARITY[b0 + i] + (CLARITY[b0 + i + 1] - CLARITY[b0 + i]) * f;
            float l1 = CLARITY[b1 + i] + (CLARITY[b1 + i + 1] - CLARITY[b1 + i]) * f;
            float tgt = l0 + (l1 - l0) * cf;
            rgb[c] = v + (tgt - v) * sk;
        }
    }
    for (int c2 = 0; c2 < 3; c2++) {
        if (whiten > 0.0f) {
            float v = rgb[c2];
            float p = v * (1.0f / 16.0f);
            int   i = (int)p;
            if (i < 0)  i = 0;
            if (i > 15) i = 15;
            float f  = p - (float)i;
            int   b0 = wi * 51 + c2 * 17;
            int   b1 = b0 + 51;
            float l0 = WHITEN[b0 + i] + (WHITEN[b0 + i + 1] - WHITEN[b0 + i]) * f;
            float l1 = WHITEN[b1 + i] + (WHITEN[b1 + i + 1] - WHITEN[b1 + i]) * f;
            float tgt = l0 + (l1 - l0) * wf;
            rgb[c2] = v + (tgt - v) * sk;
        }
    }

    // ---- 红润（只压绿、微升蓝；与 CPU 的 dlt17 同）----
    if (rosy > 0.0f) {
        float pg = rgb.g * (1.0f / 16.0f);
        int   ig = (int)pg;
        if (ig < 0)  ig = 0;
        if (ig > 15) ig = 15;
        float fg = pg - (float)ig;
        float dg = ROSE_G[ig] + (ROSE_G[ig + 1] - ROSE_G[ig]) * fg;

        float pb = rgb.b * (1.0f / 16.0f);
        int   ib = (int)pb;
        if (ib < 0)  ib = 0;
        if (ib > 15) ib = 15;
        float fb = pb - (float)ib;
        float db = ROSE_B[ib] + (ROSE_B[ib + 1] - ROSE_B[ib]) * fb;

        rgb.g += dg * rosy * sk;
        rgb.b += db * rosy * sk;
    }

    // 与 CPU toneColor 写回字节一致：先夹 [0,255]，再 +0.5 四舍五入（half-up），
    // 最后 /255。UNORM 纹理存储是 half-to-even，主动 +0.5 可消除 1-LSB 偏差。
    rgb = clamp(rgb, 0.0f, 255.0f);
    rgb = floor(rgb + 0.5f);
    float3 outRgb = rgb / 255.0f;

    // E/P3：3D-LUT 只在皮肤区生效，且 lut3dMix=0 时严格走原路径。
    if (lut3dMix > 0.0f) {
        float3 l3 = sampleLut3d(outRgb);
        outRgb = lerp(outRgb, lerp(outRgb, l3, sk), lut3dMix);
    }
    return float4(outRgb, 1.0f);
}

// OBS effect 必须至少声明一个 technique，gs_effect_create_from_file 才会接受。
technique ToneTech
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSMain(vert_in);
    }
}
