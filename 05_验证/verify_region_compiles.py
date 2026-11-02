# -*- coding: utf-8 -*-
"""
region.hlsl 真编译门禁（五官段切片 4a）。

与 verify_mask_compiles.py 同一套骨架。五官的椭圆族全是「加法增益 + 软边」，
它的错误形态是**画面只是略变**，所以静态检查盯的是数值与顺序：

  A 顶层 static/const              B 函数数组形参
  C technique/入口齐全             D fxc 真编译（vs + ps）
  E sampler 必须是 OBS 语法 + Point F technique 入口必须带参
  G 顶点矩阵必须是 ViewProj + mul(vec, m)
  H 纹理/sampler 名唯一 + 必须有 image
  I 椭圆距离必须是「旋转投影 + d2>=1 跳过 + sqrt」
  J 软边必须是 1 - smoothStep(feather, 1, d)
  K shade 加权必须是 (1 - shade*Y/255)，且 Y 的系数与 CPU 同值
  L 参数纹理每椭圆 3 texel、采样必须落在 texel 中心（+0.5）
  M acc 必须在循环**外**才乘掩码、clamp ±48、且只取整一次

用法：python verify_region_compiles.py [region.hlsl 路径]
退出码 0 = 全过。
"""
import io, os, re, subprocess, sys, tempfile

HLS = sys.argv[1] if len(sys.argv) > 1 else \
    "D:/插件/OBS美颜GPU版/01_源码/shaders/region.hlsl"
FXC = "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"

txt = io.open(HLS, "r", encoding="utf-8").read()


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def strip_techniques(s):
    out = []
    i = 0
    while True:
        m = re.search(r"(?m)^\s*technique\s+\w+\s*\{", s[i:])
        if not m:
            out.append(s[i:])
            break
        start = i + m.start()
        out.append(s[i:start])
        j = i + m.end() - 1
        depth = 0
        while j < len(s):
            if s[j] == "{":
                depth += 1
            elif s[j] == "}":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        i = j
    return "".join(out)


def body_of(name):
    m = re.search(r"(?m)^\s*(?:float4|float3|float2|float|int|void)\s+%s\s*\([^)]*\)\s*"
                  r"(?::\s*TARGET\s*)?\{" % re.escape(name), code)
    if not m:
        return None
    body = code[m.end():]
    depth, k = 1, 0
    while k < len(body) and depth > 0:
        if body[k] == "{":
            depth += 1
        elif body[k] == "}":
            depth -= 1
        k += 1
    return body[:k]


code = strip_comments(txt)
fail = []

# ------------------------------------------------------------------ A
bad_a = [l.strip() for l in code.splitlines()
         if re.match(r"^\s{0,3}(static|const)\b", l)]
print("[A] 顶层数组检查:", "PASS" if not bad_a else "FAIL")
for b in bad_a:
    fail.append("A: 顶层出现 static/const：%s" % b)

# ------------------------------------------------------------------ B
bad_b = [m.group(1).strip() for m in
         re.finditer(r"(?m)^\s*(?:float[1-4]?|void|int|bool|vertex_data)\s+\w+\s*\(([^)]*)\)",
                     code) if "[" in m.group(1)]
print("[B] 数组形参检查:", "PASS" if not bad_b else "FAIL")
for b in bad_b:
    fail.append("B: 数组形参：%s" % b)

# ------------------------------------------------------------------ C
PS_ENTRIES = ["PS_Region", "PS_FoldMaxH", "PS_FoldMaxV", "PS_FoldApply",
              "PS_SpotSumH", "PS_SpotMeanV", "PS_SpotApply"]
TECH = ["RegionEllipseTech", "RegionFoldMaxHTech", "RegionFoldMaxVTech",
        "RegionFoldApplyTech", "RegionSpotSumHTech", "RegionSpotMeanVTech",
        "RegionSpotApplyTech"]
bad_c = []
if not re.search(r"\bVSDefault\s*\(", code):
    bad_c.append("找不到顶点入口 VSDefault")
for e in PS_ENTRIES:
    if not re.search(r"float4\s+%s\s*\(" % re.escape(e), code):
        bad_c.append("找不到像素入口 %s" % e)
    if not re.search(r"pixel_shader\s*=\s*%s\s*\(" % re.escape(e), code):
        bad_c.append("%s 没有被 technique 引用" % e)
for t in TECH:
    if not re.search(r"technique\s+%s\s*\{" % re.escape(t), code):
        bad_c.append("找不到 technique %s（宿主按名字取）" % t)
print("[C] technique/入口检查:", "PASS" if not bad_c else "FAIL")
for b in bad_c:
    fail.append("C: " + b)

# ------------------------------------------------------------------ E
bad_e = []
if re.search(r"uniform\s+sampler_state", code):
    bad_e.append("出现 `uniform sampler_state`")
if re.search(r"=\s*sampler_state", code):
    bad_e.append("出现 `= sampler_state {...}`")
sampler_names = []
for m in re.finditer(r"(?m)^\s*sampler_state\s+(\w+)\s*\{", code):
    sampler_names.append(m.group(1))
    if not re.match(r"[^}]*\}\s*;", code[m.end():]):
        bad_e.append("sampler_state %s 没有以 `};` 收尾" % m.group(1))
if not sampler_names:
    bad_e.append("没有 OBS 语法的 sampler_state")
# 三张纹理都必须点采样：掩码/参数表逐像素对齐，插值会把相邻椭圆混进来
for m in re.finditer(r"(?m)^\s*sampler_state\s+(\w+)\s*\{([^}]*)\}", code):
    if not re.search(r"Filter\s*=\s*Point", m.group(2)):
        bad_e.append("sampler %s 不是 Filter=Point" % m.group(1))
print("[E] sampler 语法检查:", "PASS" if not bad_e else "FAIL")
for b in bad_e:
    fail.append("E: " + b)

# ------------------------------------------------------------------ F
bad_f = []
for tname in re.findall(r"technique\s+(\w+)\s*\{", code):
    m = re.search(r"technique\s+%s\s*\{" % re.escape(tname), code)
    body = code[m.end():]
    depth, k = 1, 0
    while k < len(body) and depth > 0:
        if body[k] == "{":
            depth += 1
        elif body[k] == "}":
            depth -= 1
        k += 1
    body = body[:k]
    for kind, pat in (("vertex_shader", r"vertex_shader\s*=\s*(\w+)\s*\(([^)]*)\)"),
                      ("pixel_shader", r"pixel_shader\s*=\s*(\w+)\s*\(([^)]*)\)")):
        mm = re.search(pat, body)
        if not mm:
            bad_f.append("%s 里 %s 缺失" % (tname, kind))
        elif not mm.group(2).strip():
            bad_f.append("%s 的 %s 是空参数调用" % (tname, kind))
print("[F] technique 入口调用检查:", "PASS" if not bad_f else "FAIL")
for b in bad_f:
    fail.append("F: " + b)

# ------------------------------------------------------------------ G
bad_g = []
if not re.search(r"uniform\s+float4x4\s+ViewProj\s*;", code):
    bad_g.append("缺少 `uniform float4x4 ViewProj;`")
if re.search(r"\bViewProjMatrix\b", code):
    bad_g.append("出现 ViewProjMatrix")
for _ in re.finditer(r"mul\s*\(\s*(ViewProj\w*)\s*,", code):
    bad_g.append("mul 顺序反了（应为 mul(vec, ViewProj)）")
print("[G] 顶点矩阵检查:", "PASS" if not bad_g else "FAIL")
for b in bad_g:
    fail.append("G: " + b)

# ------------------------------------------------------------------ H
bad_h = []
tex_names = re.findall(r"(?m)^\s*uniform\s+texture2d\s+(\w+)\s*;", code)
for group in (tex_names, sampler_names):
    dup = set(n for n in group if group.count(n) > 1)
    if dup:
        bad_h.append("重名：%s" % ", ".join(sorted(dup)))
clash = set(tex_names) & set(sampler_names)
if clash:
    bad_h.append("纹理与 sampler 撞名：%s" % ", ".join(sorted(clash)))
for need in ("image", "skinMask", "ellipseTex"):
    if need not in tex_names:
        bad_h.append("缺少纹理 `%s`" % need)
print("[H] 纹理/sampler 名检查:", "PASS" if not bad_h else "FAIL")
for b in bad_h:
    fail.append("H: " + b)

body = body_of("PS_Region")
if body is None:
    fail.append("C: 找不到 PS_Region 函数体，I/J/K/L/M 跳过")
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)

# ------------------------------------------------------------------ I
bad_i = []
if not re.search(r"u\s*=\s*\(\s*dx\s*\*\s*B\.x\s*\+\s*dy\s*\*\s*B\.y\s*\)\s*/\s*A\.z", body):
    bad_i.append("旋转投影 u 不是 (dx*cosT + dy*sinT)/rx")
if not re.search(r"t2\s*=\s*\(\s*-dx\s*\*\s*B\.y\s*\+\s*dy\s*\*\s*B\.x\s*\)\s*/\s*A\.w", body):
    bad_i.append("旋转投影 t2 不是 (-dx*sinT + dy*cosT)/ry")
if not re.search(r"d2\s*=\s*u\s*\*\s*u\s*\+\s*t2\s*\*\s*t2", body):
    bad_i.append("缺 d2 = u² + t²")
if not re.search(r"if\s*\(\s*d2\s*>=\s*1\.0\s*\)\s*continue\s*;", body):
    bad_i.append("缺 `d2 >= 1` 跳过（椭圆外一个字节都不该动）")
if not re.search(r"sqrt\s*\(\s*d2\s*\)", body):
    bad_i.append("缺 sqrt(d2) —— 软边吃的是距离，不是平方")
print("[I] 椭圆距离检查:", "PASS" if not bad_i else "FAIL")
for b in bad_i:
    fail.append("I: " + b)

# ------------------------------------------------------------------ J
bad_j = []
if not re.search(r"1\.0\s*-\s*smoothStep\s*\(\s*B\.w\s*,\s*1\.0\s*,\s*d\s*\)", body):
    bad_j.append("软边必须是 `1 - smoothStep(feather, 1.0, d)`（B.w = feather）")
if not re.search(r"g\s*=\s*B\.z\s*\*\s*fall", body):
    bad_j.append("增益必须是 `gain * fall`（B.z = gain）")
print("[J] 软边检查:", "PASS" if not bad_j else "FAIL")
for b in bad_j:
    fail.append("J: " + b)

# ------------------------------------------------------------------ K
bad_k = []
# Y 的系数与 CPU 同值，且必须**先回到 0..255 域**再算（0..1 域会让 shade 失效 ——
# 2026-11-01 第一次 R1 对拍就是这么红的）。系数写在共享函数 lumOf 里。
lum_body = body_of("lumOf") or ""
if not lum_body:
    bad_k.append("找不到 lumOf 函数体")
elif not re.search(r"floor\s*\(\s*c\s*\*\s*255\.0\s*\+\s*0\.5\s*\)", lum_body):
    bad_k.append("lumOf 必须先把 0..1 颜色还原成 0..255 字节值")
elif not re.search(r"0\.299\s*\*\s*b\.r\s*\+\s*0\.587\s*\*\s*b\.g\s*\+\s*0\.114\s*\*\s*b\.b",
                   lum_body):
    bad_k.append("亮度系数必须与 CPU 同值 (0.299/0.587/0.114)")
if not re.search(r"Y\s*=\s*lumOf\s*\(\s*src\s*\)", body):
    bad_k.append("PS_Region 里 Y 必须来自 lumOf(src)（同一域）")
if not re.search(r"g\s*\*=\s*\(\s*1\.0\s*-\s*C\.x\s*\*\s*\(\s*Y\s*\*\s*\(\s*1\.0\s*/\s*255\.0\s*\)\s*\)", body):
    bad_k.append("shade 加权必须是 `g *= 1 - shade*(Y/255)`（C.x = shade）")
print("[K] 亮度/shade 检查:", "PASS" if not bad_k else "FAIL")
for b in bad_k:
    fail.append("K: " + b)

# ------------------------------------------------------------------ L
bad_l = []
if not re.search(r"k\s*\*\s*3", body):
    bad_l.append("参数纹理必须每椭圆 3 个 texel（k*3）")
for off in (0, 1, 2):
    pat = r"\(float\)\(\s*k\s*\*\s*3" + (r"\s*\+\s*%d" % off if off else "") + r"\s*\)\s*\+\s*0\.5"
    if not re.search(pat, body):
        bad_l.append("第 %d 个 texel 的采样没有 +0.5 落到中心" % off)
# SampleLevel 吃归一化 UV：少了 / REGION_PARAM_W 会被 Clamp 夹到最后一个 texel，
# 表现是「一个椭圆都没加」而**不报错**（2026-11-01 R1 首跑就是这么翻车的）。
if not re.search(r"/\s*REGION_PARAM_W", body):
    bad_l.append("参数表 UV 必须除以纹理宽度（归一化），否则全被 clamp 到末尾")
if not re.search(r"#define\s+REGION_PARAM_W\s+64\.0", code):
    bad_l.append("缺 `#define REGION_PARAM_W 64.0`（要与宿主建纹理的宽度一致）")
print("[L] 参数纹理检查:", "PASS" if not bad_l else "FAIL")
for b in bad_l:
    fail.append("L: " + b)

# ------------------------------------------------------------------ M
# CPU 的次序是「循环里只累加 → 循环外 ×mask → clamp ±48 → 一次取整」。
# 一旦把 ×mask 或取整写进循环，重叠椭圆就会各自取整，立体那 8 个椭圆会累积误差。
bad_m = []
if not re.search(r"acc\s*\+=\s*g", body):
    bad_m.append("循环里应只做 `acc += g`")
if not re.search(r"acc\s*\*=\s*mw", body):
    bad_m.append("缺循环外的 `acc *= mw`")
if not re.search(r"clamp\s*\(\s*acc\s*,\s*-48\.0\s*,\s*48\.0\s*\)", body):
    bad_m.append("缺 `clamp(acc, -48, 48)`（与 CPU 同一防御上限）")
if not re.search(r"floor\s*\(\s*src\s*\*\s*255\.0\s*\+\s*acc\s*\+\s*0\.5\s*\)", body):
    bad_m.append("取整必须是 `floor(src*255 + acc + 0.5)`（等价 lroundf；见 3.1.23 的负增益 bug）")
if re.search(r"lround|round\s*\(", body):
    bad_m.append("出现 round/lround —— HLSL 的 round 是四舍六入五成双，与 CPU 的 lroundf 不同")
print("[M] 累加/取整次序检查:", "PASS" if not bad_m else "FAIL")
for b in bad_m:
    fail.append("M: " + b)

# ------------------------------------------------------------------ N
# 法令纹：三个最容易「看起来对、其实错」的点。
bad_n = []
fold_apply = body_of("PS_FoldApply") or ""
foldh = body_of("PS_FoldMaxH") or ""
foldv = body_of("PS_FoldMaxV") or ""
if not fold_apply:
    bad_n.append("找不到 PS_FoldApply 函数体")
else:
    # ① 限制因子必须是 max(mask, 区域权重)。写成乘掩码会把最该提亮的那条线掐掉
    #    （CPU 注释：corr(掩码, 参照Δ) = −0.66）。
    if not re.search(r"max\s*\(\s*mw\s*,\s*wgt\s*\)", fold_apply):
        bad_n.append("限制因子必须是 `max(mw, wgt)`，不是乘掩码")
    # ② Δ 必须封顶，且斜率乘的是「暗差」而不是亮度。
    if not re.search(r"min\s*\(\s*fSlope\s*\*\s*dRaw\s*,\s*fMaxLift\s*\)", fold_apply):
        bad_n.append("Δ 必须是 `min(slope*dRaw, maxLift)`")
    if not re.search(r"dRaw\s*=\s*LR\s*-\s*Y", fold_apply):
        bad_n.append("暗差必须是 `dRaw = LR - Y`")
    # ③ 暗线闸门（响应宽度收敛到真正褶皱线）
    if not re.search(r"smoothStep\s*\(\s*fGateLo\s*,\s*fGateHi\s*,\s*dRaw\s*\)", fold_apply):
        bad_n.append("缺暗线闸门 `smoothStep(fGateLo, fGateHi, dRaw)`")
    if not re.search(r"wgt\s*<=\s*0\.0", fold_apply):
        bad_n.append("缺 `wgt <= 0` 早退（区域外一个字节都不该动）")
if not foldh:
    bad_n.append("找不到 PS_FoldMaxH")
elif not re.search(r"px\.x\s*\+\s*\(float\)k", foldh):
    bad_n.append("横向 max 没有沿 x 走窗口")
if not foldv:
    bad_n.append("找不到 PS_FoldMaxV")
elif not re.search(r"px\.y\s*\+\s*\(float\)k", foldv):
    bad_n.append("纵向 max 没有沿 y 走窗口")
print("[N] 法令纹语义检查:", "PASS" if not bad_n else "FAIL")
for b in bad_n:
    fail.append("N: " + b)

# ------------------------------------------------------------------ O
# 祛斑：均值必须是「真均值」（分母按实际元素数），判据与限幅也要钉住。
bad_o = []
spot_apply = body_of("PS_SpotApply") or ""
spot_v = body_of("PS_SpotMeanV") or ""
spot_h = body_of("PS_SpotSumH") or ""
if not spot_apply:
    bad_o.append("找不到 PS_SpotApply 函数体")
else:
    if not re.search(r"ad\s*<\s*sThrLo\s*\|\|\s*ad\s*>\s*sThrHi", spot_apply):
        bad_o.append("斑点判据必须是 `ad < thrLo || ad > thrHi → 不动`")
    if not re.search(r"pull\s*=\s*clamp\s*\(\s*-sK\s*\*\s*mw\s*\*\s*dev\s*,\s*-24\.0\s*,\s*24\.0\s*\)",
                     spot_apply):
        bad_o.append("pull 必须是 `clamp(-k*mw*dev, ±24)`")
    if not re.search(r"dev\s*=\s*L\s*-\s*mean", spot_apply):
        bad_o.append("偏差必须是 `dev = L - mean`")
    if not re.search(r"mw\s*<=\s*0\.0", spot_apply):
        bad_o.append("缺 `mw <= 0` 早退（掩码外不该动）")
if not spot_v:
    bad_o.append("找不到 PS_SpotMeanV 函数体")
else:
    # 窗口必须在 **ROI 边界** clamp，不是画面边界（CPU 的 ROI 是 bounds_）。
    if not spot_h or not re.search(r"clamp\(\s*px\.x \+ \(float\)k,\s*roiMin\.x", spot_h):
        bad_o.append("横向窗口必须 clamp 到 roiMin/roiMax（不是 texSize）")
    if not re.search(r"clamp\(\s*px\.y \+ \(float\)k,\s*roiMin\.y", spot_v):
        bad_o.append("纵向窗口必须 clamp 到 roiMin/roiMax")
    # 分母必须是实际行数×列数，不能是 win²
    if not re.search(r"sum\s*/\s*\(\s*nx\s*\*\s*ny\s*\)", spot_v):
        bad_o.append("均值必须除以实际参与元素数 `nx*ny`（win² 会让边缘整片判成斑点）")
    if not re.search(r"nx\s*=\s*floor\s*\(\s*min\s*\(\s*px\.x", spot_v):
        bad_o.append("nx 必须按 clamp 后的实际列数算")
    if not re.search(r"ny\s*=\s*floor\s*\(\s*min\s*\(\s*px\.y", spot_v):
        bad_o.append("ny 必须按 clamp 后的实际行数算")
print("[O] 祛斑语义检查:", "PASS" if not bad_o else "FAIL")
for b in bad_o:
    fail.append("O: " + b)

# ------------------------------------------------------------------ D
def to_plain_hlsl(src):
    out = strip_techniques(src)
    out = re.sub(r"\btexture2d\b", "Texture2D", out)
    out = re.sub(r"(?m)^\s*sampler_state\s+(\w+)\s*\{[^}]*\}\s*;",
                 r"SamplerState \1;", out)
    out = re.sub(r":\s*TARGET\b", ": SV_Target", out)
    return out


plain = to_plain_hlsl(txt)
tmpdir = tempfile.mkdtemp(prefix="regionchk_")
plain_path = os.path.join(tmpdir, "region_plain.hlsl")
io.open(plain_path, "w", encoding="utf-8", newline="\n").write(plain)

if not os.path.exists(FXC):
    print("[D] fxc 真编译: SKIP（找不到 %s）" % FXC)
else:
    all_ok = True
    for prof, entry in [("vs_4_0", "VSDefault")] + [("ps_4_0", e) for e in PS_ENTRIES]:
        asm = os.path.join(tmpdir, "%s_%s.asm" % (entry, prof))
        obj = os.path.join(tmpdir, "%s_%s.bin" % (entry, prof))
        p = subprocess.run([FXC, "/nologo", "/T", prof, "/E", entry,
                            "/Fc" + asm, "/Fo" + obj, plain_path],
                           capture_output=True, text=True)
        out = (p.stdout or "") + (p.stderr or "")
        inst = ""
        if os.path.exists(asm):
            m = re.search(r"// Approximately (\d+) instruction slots",
                          io.open(asm, "r", errors="ignore").read())
            if m:
                inst = "（%s 条指令）" % m.group(1)
        ok = (p.returncode == 0) and ("error" not in out.lower())
        print("[D] fxc %-7s /E %-11s %s%s" % (prof, entry, "PASS" if ok else "FAIL", inst))
        if not ok:
            all_ok = False
            fail.append("D: fxc %s 失败：\n%s" % (entry, out.strip()))
    print("[D] 汇总:", "PASS" if all_ok else "FAIL")

if fail:
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)
print("\n全部通过：region.hlsl 语法可被 OBS 加载、入口能被 HLSL 编译器接受。")
print("注意：这只证明「能编译 + 公式没抄错」，逐像素等价必须真机跑 R1 对拍。")
sys.exit(0)
