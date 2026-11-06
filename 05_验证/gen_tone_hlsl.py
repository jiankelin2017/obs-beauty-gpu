# -*- coding: utf-8 -*-
"""
从 beauty_freq.cpp 的色调 LUT 原始数据生成 01_源码/shaders/tone.hlsl。

为什么要生成而不是手抄：
  kClarityLut5 / kWhitenLut5 各 5*3*17 = 255 个数字，手写极易错位。
  此脚本把原始数据原样嵌进 Python（与 C++ 同源），再逐字吐成 HLSL 的
  `static const float` 展平数组，保证 GPU 与 CPU 的色调变换逐字节一致
  （这是守住 79.0 分对齐成果的关键）。

2026-10-31 修版（两个致命缺陷，原版根本加载不了）：
  1) OBS 的 effect 解析器（libobs/graphics/effect-parser.c: ep_parse_other）
     **不支持顶层** `static const float X[255] = {...};` —— specifier 只认
     property/const/uniform，"static" 会被当成类型名，随后 expected ';' 报错，
     gs_effect_create_from_file() 直接失败。因此数组必须写在**函数体内**
     （ep_parse_func_contents 把函数体逐 token 原样拷进 shader 文本，不经参数解析）。
  2) 原版 `lut17(tbl + base, v)` 是 C 指针运算，HLSL 不支持，fxc 报
     X3022 "scalar, vector or matrix expected"。改成显式下标 `tbl[base + i]`。
  另：原版 `toneCh()` 拿数组当形参，OBS 的 ep_parse_func_param 不认 `[N]` 形参，
  所以新版本索性不再用数组形参，插值直接内联（编译器会把 3 通道循环展开）。

输出：tone.hlsl —— OBS effect 格式，包含：
  * 函数体内展平的 CLARITY[255] / WHITEN[255] / ROSE_G[17] / ROSE_B[17]
  * PSMain：采样原色 + 皮肤掩码，按 clarity→whiten→rosy 顺序施加，
            仅皮肤区（mask>0）按 mask/255 缩放，与 CPU 的 toneColor 对齐。
"""
import io, os

# ---- 原始数据（逐字抄自 beauty_freq.cpp L481-539）----
# 顺序：[level][channel][anchor]，anchor = 0,16,...,240,255（17 个）
CLARITY = [
    [ # panel 0 = identity
      [0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255],
      [0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255],
      [0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255],
    ],
    [ # 0.25
      [0,16,32,47,62,78,95,112,129,146,162,179,195,210,225,241,255],
      [0,16,31,46,62,79,95,112,129,146,163,179,194,209,224,240,255],
      [0,16,31,46,62,79,95,112,129,146,163,179,195,209,225,240,255],
    ],
    [ # 0.5
      [0,16,32,46,60,77,94,112,129,147,165,182,198,212,227,241,255],
      [0,16,29,44,60,77,95,113,131,149,166,182,195,211,225,240,255],
      [0,16,29,44,60,77,95,112,131,149,166,182,197,211,226,240,255],
    ],
    [ # 0.75
      [0,16,32,45,58,75,93,111,130,149,167,185,200,215,229,241,255],
      [0,16,28,42,58,76,94,113,133,151,169,185,197,212,225,241,255],
      [0,16,28,42,58,76,94,113,132,151,169,185,200,212,227,241,255],
    ],
    [ # 1.0
      [0,16,32,43,56,74,92,111,131,151,170,188,203,217,230,242,255],
      [0,16,26,40,56,74,94,114,134,154,172,188,199,214,226,241,255],
      [0,16,26,41,56,74,93,113,133,153,171,188,203,214,228,241,255],
    ],
]

WHITEN = [
    [ # panel 0 = identity
      [0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255],
      [0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255],
      [0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255],
    ],
    [ # 0.25
      [0,16,32,49,65,82,99,115,132,148,164,179,195,210,225,241,255],
      [0,16,33,49,66,82,99,116,132,148,163,179,194,209,225,241,255],
      [0,16,32,49,66,83,100,116,133,149,164,179,195,210,225,240,255],
    ],
    [ # 0.5
      [0,16,32,50,67,85,102,119,136,152,167,182,197,212,227,241,255],
      [0,16,33,51,68,85,103,119,136,152,167,182,196,211,225,241,255],
      [0,16,32,51,68,86,104,121,137,153,168,182,198,211,227,241,255],
    ],
    [ # 0.75
      [0,16,32,50,68,87,105,122,140,156,171,185,200,214,228,242,254],
      [0,16,34,52,69,87,106,123,140,156,170,185,197,212,226,242,255],
      [0,16,32,52,70,89,108,125,142,158,172,186,201,213,228,241,255],
    ],
    [ # 1.0
      [0,16,32,51,70,89,108,126,144,160,174,188,203,216,230,242,254],
      [0,16,35,54,71,90,109,126,143,159,174,187,199,214,227,242,255],
      [0,16,32,53,72,92,112,129,146,162,176,189,204,215,230,242,255],
    ],
]

ROSE_G = [0,0,-4,-4,-5,-5,-5,-5,-4,-4,-3,-3,-1,-1,0,0,0]
ROSE_B = [0,0, 2, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,0,0]

# 展平成 1D（与 HLSL 索引一致：idx = level*51 + channel*17 + anchor）
def flatten(table):
    out = []
    for lvl in table:
        for ch in lvl:
            out.extend(ch)
    assert len(out) == len(table) * 3 * 17
    return out

CLARITY_FLAT = flatten(CLARITY)
WHITEN_FLAT = flatten(WHITEN)

def arr_str(name, vals, indent="    "):
    # 每行 17 个（= 一档一通道），便于和 C++ 的 [level][channel] 对照
    lines = []
    for i in range(0, len(vals), 17):
        chunk = vals[i:i+17]
        lines.append(indent + "    " + ",".join("%3d" % v for v in chunk) + ",")
    body = "\n".join(lines)
    return "%sstatic const float %s[%d] = {\n%s\n%s};\n" % (indent, name, len(vals), body, indent)

def signed_arr_str(name, vals, indent="    "):
    body = ",".join("%3d" % v for v in vals)
    return "%sstatic const float %s[%d] = { %s };\n" % (indent, name, len(vals), body)

HEADER = """// ===========================================================================
// tone.hlsl —— 皮肤色调变换（清晰 / 美白 / 红润）GPU 着色器
//
// 移植自 beauty_freq.cpp 的 toneColor()（L582-635）+ LUT 数据（L481-539）。
// 由 05_验证/gen_tone_hlsl.py 生成，**不要手改**：LUT 数值一律以生成器为准。
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

float4 PSMain(vertex_data vert_in) : TARGET {
    // ---- LUT 数据（展平：idx = level*51 + channel*17 + anchor）----
    // 必须留在函数体内：见文件头的约束 1。
%s%s%s%s
    float4 src = image.Sample(imageTex, vert_in.uv);
    float  m   = mask.Sample(maskTex, vert_in.uv).r * 255.0f;   // 0..255
    float3 rgb = src.rgb * 255.0f;                           // 色调运算走 0..255 空间
    float  sk  = m * (1.0f / 255.0f);

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
    return float4(rgb / 255.0f, 1.0f);
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
"""

hlsl = HEADER % (
    arr_str("CLARITY", CLARITY_FLAT, "    "),
    arr_str("WHITEN", WHITEN_FLAT, "    "),
    signed_arr_str("ROSE_G", ROSE_G, "    "),
    signed_arr_str("ROSE_B", ROSE_B, "    "),
)

out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "01_源码", "shaders")
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.join(out_dir, "tone.hlsl")
io.open(out_path, "w", encoding="utf-8", newline="\n").write(hlsl)
print("wrote", out_path, "bytes=", len(hlsl))
print("CLARITY rows =", len(CLARITY_FLAT)//17, "WHITEN rows =", len(WHITEN_FLAT)//17)
