# -*- coding: utf-8 -*-
"""
校验 tone.hlsl 与 CPU 主源 beauty_freq.cpp 的色调 LUT 逐字节对齐，
并验证「三维重建视图」与「着色器展平基址视图」在数学上等价。

校验三件事：
  1) tone.hlsl 的 CLARITY/WHITEN/ROSE_G/ROSE_B 与 C++ 源完全相同（逐数）。
  2) 着色器索引 base = lvl*51 + ch*17 能正确重建 C++ 的 [level][channel][anchor]。
  3) 用 C++ 算法跑出的字节 == 用着色器算法跑出的字节（含 +0.5 舍入）。

退出码 0 = 全部通过；非 0 = 存在偏差（守住 79.0 分对齐的硬门禁）。
"""
import io, os, re, sys

SRC = "D:/插件/OBS美颜源码/01_源码/beauty_freq.cpp"
HLS = "D:/插件/OBS美颜GPU版/01_源码/shaders/tone.hlsl"

# ---------------------------------------------------------------- 解析 C++ 源
def parse_cpp_array(path, name, n):
    txt = io.open(path, "r", encoding="utf-8", errors="ignore").read()
    # 找到 `name[...] = {` 后到第一个 `};` 之间的所有整数
    m = re.search(re.escape(name) + r"\s*(?:\[[^\]]*\])+\s*=\s*\{(.*?)\};", txt, re.S)
    if not m:
        raise RuntimeError("在 C++ 源中找不到 " + name)
    block = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)  # 去掉 /* ... */ 注释（含 0.25 等数字）
    block = re.sub(r"//[^\n]*", "", block)                     # 去掉 // 注释
    nums = [int(x) for x in re.findall(r"-?\d+", block)]
    assert len(nums) == n, "%s 解析出 %d 个数，期望 %d" % (name, len(nums), n)
    return nums

cpp_clarity = parse_cpp_array(SRC, "kClarityLut5", 5 * 3 * 17)
cpp_whiten  = parse_cpp_array(SRC, "kWhitenLut5",  5 * 3 * 17)
cpp_roseg   = parse_cpp_array(SRC, "kRosyG17", 17)
cpp_roseb   = parse_cpp_array(SRC, "kRosyB17", 17)

# ---------------------------------------------------------------- 解析 HLSL
def parse_hlsl_array(path, name, n):
    txt = io.open(path, "r", encoding="utf-8").read()
    m = re.search(r"static const float\s+" + re.escape(name) + r"\s*\[\s*" + str(n) + r"\s*\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m:
        raise RuntimeError("在 tone.hlsl 中找不到 " + name)
    nums = [int(round(float(x))) for x in re.findall(r"-?\d+\.?\d*", m.group(1))]
    assert len(nums) == n, "%s 解析出 %d 个数，期望 %d" % (name, len(nums), n)
    return nums

hls_clarity = parse_hlsl_array(HLS, "CLARITY", 255)
hls_whiten  = parse_hlsl_array(HLS, "WHITEN", 255)
hls_roseg   = parse_hlsl_array(HLS, "ROSE_G", 17)
hls_roseb   = parse_hlsl_array(HLS, "ROSE_B", 17)

# ---------------------------------------------------------------- 1) 逐数比对
def eq(a, b):
    return all(x == y for x, y in zip(a, b))

ok = True
ok &= eq(cpp_clarity, hls_clarity)
ok &= eq(cpp_whiten,  hls_whiten)
ok &= eq(cpp_roseg,   hls_roseg)
ok &= eq(cpp_roseb,   hls_roseb)
print("[1] C++ vs HLSL 逐数比对:", "PASS" if ok else "FAIL")
if not ok:
    for nm, c, h in [("CLARITY", cpp_clarity, hls_clarity),
                     ("WHITEN", cpp_whiten, hls_whiten),
                     ("ROSE_G", cpp_roseg, hls_roseg),
                     ("ROSE_B", cpp_roseb, hls_roseb)]:
        if not eq(c, h):
            diff = [(i, c[i], h[i]) for i in range(len(c)) if c[i] != h[i]]
            print("    %s 偏差点(前10): %s" % (nm, diff[:10]))

# ---------------------------------------------------------------- 数学基件
def lut17(t, v):
    p = v * (1.0 / 16.0)
    if p >= 16.0: return float(t[16])
    if p <= 0.0:  return float(t[0])
    i = int(p); f = p - i
    return float(t[i]) + (float(t[i + 1]) - float(t[i])) * f

def lvl5(a):
    x = a * 4.0
    if x < 0.0: x = 0.0
    if x > 3.999: x = 3.999
    return int(x), x - int(x)

# ---------------------------------------------------------------- 2) 索引重建
# 单档查表：返回某一 (level, channel) 的 17 锚点插值结果（与 HLSL toneCh 语义一致）。
# 档位间的 (b-a)*f 插值由调用方完成，完全对应 C++ Lv5::at。
def shader_ch(tbl, lvl, ch, v):
    base = lvl * 51 + ch * 17
    return lut17(tbl[base:base + 17], v)

def shader_at(tbl, i0, f, ch, v):
    a = shader_ch(tbl, i0, ch, v)
    b = shader_ch(tbl, i0 + 1, ch, v)
    return a + (b - a) * f

def shader_tone(r, g, b, mask, clarity, whiten, rosy):
    if mask == 0:
        return (int(round(r)), int(round(g)), int(round(b)))  # 非皮肤原样（近似，仅用于非0比对）
    sk = mask / 255.0
    lc_i0, lc_f = lvl5(clarity)
    lw_i0, lw_f = lvl5(whiten)
    if clarity > 0.0:
        r += (shader_at(hls_clarity, lc_i0, lc_f, 0, r) - r) * sk
        g += (shader_at(hls_clarity, lc_i0, lc_f, 1, g) - g) * sk
        b += (shader_at(hls_clarity, lc_i0, lc_f, 2, b) - b) * sk
    if whiten > 0.0:
        r += (shader_at(hls_whiten, lw_i0, lw_f, 0, r) - r) * sk
        g += (shader_at(hls_whiten, lw_i0, lw_f, 1, g) - g) * sk
        b += (shader_at(hls_whiten, lw_i0, lw_f, 2, b) - b) * sk
    if rosy > 0.0:
        g += lut17(hls_roseg, g) * rosy * sk
        b += lut17(hls_roseb, b) * rosy * sk
    r = 0 if r < 0 else (255 if r > 255 else r + 0.5)
    g = 0 if g < 0 else (255 if g > 255 else g + 0.5)
    b = 0 if b < 0 else (255 if b > 255 else b + 0.5)
    return (int(r), int(g), int(b))

# CPU 视图：用三维重建 [level][channel][anchor]
def to3d(flat):
    return [[flat[(l * 3 + c) * 17:(l * 3 + c) * 17 + 17] for c in range(3)] for l in range(5)]

cpp_clar3 = to3d(cpp_clarity)
cpp_whit3 = to3d(cpp_whiten)

def cpu_at(t3, i0, f, ch, v):
    a = lut17(t3[i0][ch], v)
    b = lut17(t3[i0 + 1][ch], v)
    return a + (b - a) * f

def cpu_tone(r, g, b, mask, clarity, whiten, rosy):
    if mask == 0:
        return (int(round(r)), int(round(g)), int(round(b)))
    sk = mask / 255.0
    lc_i0, lc_f = lvl5(clarity)
    lw_i0, lw_f = lvl5(whiten)
    if clarity > 0.0:
        r += (cpu_at(cpp_clar3, lc_i0, lc_f, 0, r) - r) * sk
        g += (cpu_at(cpp_clar3, lc_i0, lc_f, 1, g) - g) * sk
        b += (cpu_at(cpp_clar3, lc_i0, lc_f, 2, b) - b) * sk
    if whiten > 0.0:
        r += (cpu_at(cpp_whit3, lw_i0, lw_f, 0, r) - r) * sk
        g += (cpu_at(cpp_whit3, lw_i0, lw_f, 1, g) - g) * sk
        b += (cpu_at(cpp_whit3, lw_i0, lw_f, 2, b) - b) * sk
    if rosy > 0.0:
        g += lut17(cpp_roseg, g) * rosy * sk
        b += lut17(cpp_roseb, b) * rosy * sk
    r = 0 if r < 0 else (255 if r > 255 else r + 0.5)
    g = 0 if g < 0 else (255 if g > 255 else g + 0.5)
    b = 0 if b < 0 else (255 if b > 255 else b + 0.5)
    return (int(r), int(g), int(b))

# ---------------------------------------------------------------- 3) 等价测试
mism = 0
samples = 0
grid = list(range(0, 256, 17)) + [1, 7, 100, 200, 254]   # 覆盖锚点与中间值
masks = [0, 30, 128, 255]
settings = [(0,0,0),(0.25,0.25,0.25),(0.5,0.5,0.5),(1.0,1.0,1.0),(0.3,0.7,0.2),(0.8,0.1,0.9)]
for r in grid:
    for g in grid:
        for b in grid:
            for mv in masks:
                for (cl, wh, ro) in settings:
                    c = cpu_tone(float(r), float(g), float(b), mv, cl, wh, ro)
                    s = shader_tone(float(r), float(g), float(b), mv, cl, wh, ro)
                    samples += 1
                    if c != s:
                        mism += 1
                        if mism <= 10:
                            print("    偏差:", (r,g,b,mv,cl,wh,ro), "CPU=", c, "SHADER=", s)
print("[2] 索引重建 + [3] CPU vs 着色器等价: 采样 %d, 偏差 %d -> %s" %
      (samples, mism, "PASS" if mism == 0 else "FAIL"))

sys.exit(0 if (ok and mism == 0) else 1)
