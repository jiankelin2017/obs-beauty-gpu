# -*- coding: utf-8 -*-
"""
mask.hlsl 真编译门禁（5b-2 切片 1）。

与 verify_warp_compiles.py 同一套骨架：静态检查钉「结构/数值/写法」，
fxc 钉「能编译」，**逐像素等价仍然只能靠装机跑 M1 对拍**（编译过 ≠ 值算对，
这条教训来自 T1 的 `.b=0`）。

本文件的检查针对掩码特有的、**错了不报错只是画面略变**的写法：

  A 顶层 static/const              B 函数数组形参
  C technique/入口齐全（2 个）      D fxc 真编译 3 个入口（vs + 2 ps）
  E sampler 必须是 OBS 语法 + Point F technique 入口必须带参
  G 顶点矩阵必须是 ViewProj         H 纹理/sampler 名唯一 + 必须有 image
  I 肤色 YCbCr 六个系数必须与 CPU 同值
  J 椭圆判据 0.6/1.3 + 亮度门控 0.12/0.30/0.94/0.99
  K 羽化三条早退 + smoothstep 公式
  L 落笔必须是整数右移 (wgt*feather+128)>>8（不是 /256 浮点）
  M 几何量必须来自 uniform，且以 (int)(x+0.5) 还原整数
  N 眼睛保护必须把**平方距离**喂 smoothstep(0.35,1.0)，且不得出现 sqrt
  O 脸/脖子必须用 max() 合并（不得 += 叠加）

用法：python verify_mask_compiles.py [mask.hlsl 路径]
退出码 0 = 全过。
"""
import io, os, re, subprocess, sys, tempfile

HLS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "01_源码", "shaders", "mask.hlsl")
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
    """取任意 `ret name(...) { ... }` 的函数体（大括号配对）。"""
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

# ---------------------------------------------------------------- A. 顶层数组
bad_a = []
for line in code.splitlines():
    if re.match(r"^\s{0,3}(static|const)\b", line):
        bad_a.append("顶层出现 static/const 声明：%s" % line.strip())
print("[A] 顶层数组检查:", "PASS" if not bad_a else "FAIL")
for b in bad_a:
    fail.append("A: " + b)

# ---------------------------------------------------------------- B. 数组形参
bad_b = []
for m in re.finditer(
        r"(?m)^\s*(?:float[1-4]?|void|int|bool|vertex_data)\s+\w+\s*\(([^)]*)\)", code):
    if "[" in m.group(1):
        bad_b.append("函数形参里出现数组：(%s)" % m.group(1).strip())
print("[B] 数组形参检查:", "PASS" if not bad_b else "FAIL")
for b in bad_b:
    fail.append("B: " + b)

# ---------------------------------------------------------------- C. technique
PS_ENTRIES = ["PS_Skin", "PS_Eye"]
TECH = ["MaskSkinTech", "MaskEyeTech"]
bad_c = []
if not re.search(r"\bVSDefault\s*\(", code):
    bad_c.append("找不到顶点入口 VSDefault")
for e in PS_ENTRIES:
    if not re.search(r"float4\s+%s\s*\(" % re.escape(e), code):
        bad_c.append("找不到像素入口 %s" % e)
    if not re.search(r"pixel_shader\s*=\s*%s\s*\(" % re.escape(e), code):
        bad_c.append("%s 没有被任何 technique 引用" % e)
for t in TECH:
    if not re.search(r"technique\s+%s\s*\{" % re.escape(t), code):
        bad_c.append("找不到 technique %s（宿主按名字取）" % t)
print("[C] technique/入口检查:", "PASS" if not bad_c else "FAIL")
for b in bad_c:
    fail.append("C: " + b)

# ---------------------------------------------------------------- E. sampler 语法
bad_e = []
if re.search(r"uniform\s+sampler_state", code):
    bad_e.append("出现 `uniform sampler_state`（OBS 语法无 uniform 前缀）")
if re.search(r"=\s*sampler_state", code):
    bad_e.append("出现 `= sampler_state {...}`（OBS 语法无 = 赋值）")
sampler_names = []
for m in re.finditer(r"(?m)^\s*sampler_state\s+(\w+)\s*\{", code):
    sampler_names.append(m.group(1))
    tail = code[m.end():]
    if not re.match(r"[^}]*\}\s*;", tail):
        bad_e.append("sampler_state %s 的块没有以 `};` 收尾" % m.group(1))
if not sampler_names:
    bad_e.append("没有找到任何 OBS 语法的 sampler_state 块")
# 掩码必须逐像素取源图：Linear 会把边界外插成邻居均值，掩码边缘整圈 ±1
if sampler_names and not re.search(r"Filter\s*=\s*Point\s*;", code):
    bad_e.append("sampler 不是 Filter=Point —— 掩码必须逐像素一一对应")
print("[E] sampler 语法检查:", "PASS" if not bad_e else "FAIL")
for b in bad_e:
    fail.append("E: " + b)

# ---------------------------------------------------------------- F. technique 入口
bad_f = []
tech_blocks = re.findall(r"technique\s+(\w+)\s*\{", code)
if not tech_blocks:
    bad_f.append("没有找到任何 technique")
for tname in tech_blocks:
    m = re.search(r"technique\s+%s\s*\{" % re.escape(tname), code)
    body = code[m.end():]
    depth = 1
    k = 0
    while k < len(body) and depth > 0:
        if body[k] == "{":
            depth += 1
        elif body[k] == "}":
            depth -= 1
        k += 1
    body = body[:k]
    for kind, pat in (("vertex_shader", r"vertex_shader\s*=\s*(\w+)\s*\(([^)]*)\)"),
                      ("pixel_shader",  r"pixel_shader\s*=\s*(\w+)\s*\(([^)]*)\)")):
        mm = re.search(pat, body)
        if not mm:
            bad_f.append("%s 里 %s 没有赋值语句" % (tname, kind))
            continue
        fname, args = mm.group(1), mm.group(2).strip()
        if not args:
            bad_f.append("%s 的 %s = %s() 是空参数调用" % (tname, kind, fname))
print("[F] technique 入口调用检查:", "PASS" if not bad_f else "FAIL")
for b in bad_f:
    fail.append("F: " + b)

# ---------------------------------------------------------------- G. 顶点矩阵
bad_g = []
if not re.search(r"uniform\s+float4x4\s+ViewProj\s*;", code):
    bad_g.append("缺少 `uniform float4x4 ViewProj;`（参数名必须是 ViewProj）")
if re.search(r"\bViewProjMatrix\b", code):
    bad_g.append("出现 `ViewProjMatrix` —— D3D11 后端只认 `ViewProj`")
for m in re.finditer(r"mul\s*\(\s*(ViewProj\w*)\s*,", code):
    bad_g.append("mul 顺序反了：OBS 约定是 mul(vec, ViewProj)")
print("[G] 顶点矩阵检查:", "PASS" if not bad_g else "FAIL")
for b in bad_g:
    fail.append("G: " + b)

# ---------------------------------------------------------------- H. 名字唯一
bad_h = []
tex_names = re.findall(r"(?m)^\s*uniform\s+texture2d\s+(\w+)\s*;", code)
dup_tex = set([n for n in tex_names if tex_names.count(n) > 1])
if dup_tex:
    bad_h.append("texture2d 重名：%s" % ", ".join(sorted(dup_tex)))
dup_smp = set([n for n in sampler_names if sampler_names.count(n) > 1])
if dup_smp:
    bad_h.append("sampler_state 重名：%s" % ", ".join(sorted(dup_smp)))
clash = set(tex_names) & set(sampler_names)
if clash:
    bad_h.append("纹理名与 sampler 名撞车：%s" % ", ".join(sorted(clash)))
if tex_names and not sampler_names:
    bad_h.append("有 texture2d 却没有任何 sampler_state")
if "image" not in tex_names:
    bad_h.append("主输入纹理必须叫 `image`（OBS 的 gs_effect_set_texture 约定）")
print("[H] 纹理/sampler 名检查:", "PASS" if not bad_h else "FAIL")
for b in bad_h:
    fail.append("H: " + b)

skin_body = body_of("skinWeight")
if skin_body is None:
    fail.append("I: 找不到 skinWeight 函数体，I/J 两项跳过")
    skin_body = ""

# ---------------------------------------------------------------- I. 肤色系数
# 这六个数抄错任何一位都不报错，只是「磨皮范围略变」。CPU 的 y/cb/cr 三行见
# beauty_core.cpp:967-969，必须逐系数相同。
bad_i = []
# 整个表达式一起钉：符号、系数、运算对象顺序，少一样都算错。
# 只查「字面量出现过」是不够的 —— `+ 0.331264` 与 `- 0.331264` 都能匹配到子串，
# 但一个是错的。
TERMS = [
    ("Y 行", r"y\s*=\s*0\.299\s*\*\s*c\.r\s*\+\s*0\.587\s*\*\s*c\.g\s*\+\s*0\.114\s*\*\s*c\.b"),
    ("Cb 行", r"cb\s*=\s*-0\.168736\s*\*\s*c\.r\s*-\s*0\.331264\s*\*\s*c\.g\s*\+\s*0\.5\s*\*\s*c\.b"),
    ("Cr 行", r"cr\s*=\s*0\.5\s*\*\s*c\.r\s*-\s*0\.418688\s*\*\s*c\.g\s*-\s*0\.081312\s*\*\s*c\.b"),
]
for label, pat in TERMS:
    if not re.search(pat, skin_body):
        bad_i.append("%s 与 CPU 不一致（应为 %s）" % (label, pat))
if not re.search(r"\(int\)\s*\(\s*m\s*\*\s*255\.0\s*\+\s*0\.5\s*\)", skin_body):
    bad_i.append("肤色权重取整必须是 `(int)(m*255.0+0.5)`（与 CPU 同值域）")
print("[I] 肤色系数检查:", "PASS" if not bad_i else "FAIL")
for b in bad_i:
    fail.append("I: " + b)

# ---------------------------------------------------------------- J. 椭圆/亮度门控
bad_j = []
if not re.search(r"smoothStep\s*\(\s*0\.6\s*,\s*1\.3\s*,", skin_body):
    bad_j.append("椭圆判据必须是 smoothStep(0.6, 1.3, d)")
if not re.search(r"smoothStep\s*\(\s*0\.12\s*,\s*0\.30\s*,", skin_body):
    bad_j.append("暗部门控必须是 smoothStep(0.12, 0.30, y)")
if not re.search(r"1\.0\s*-\s*smoothStep\s*\(\s*0\.94\s*,\s*0\.99\s*,", skin_body):
    bad_j.append("过曝门控必须是 (1 - smoothStep(0.94, 0.99, y))")
print("[J] 椭圆/亮度门控检查:", "PASS" if not bad_j else "FAIL")
for b in bad_j:
    fail.append("J: " + b)

# ---------------------------------------------------------------- K. 羽化
feather_body = body_of("featherRamp") or ""
bad_k = []
if not re.search(r"if\s*\(\s*fe\s*<=\s*0\s*\)\s*return\s+256\s*;", feather_body):
    bad_k.append("缺少第一条早退：fe<=0 → 256")
if not re.search(r"if\s*\(\s*dist\s*<=\s*0\s*\)\s*return\s+0\s*;", feather_body):
    bad_k.append("缺少第二条早退：dist<=0 → 0")
if not re.search(r"if\s*\(\s*dist\s*>=\s*fe\s*\)\s*return\s+256\s*;", feather_body):
    bad_k.append("缺少第三条早退：dist>=fe → 256")
if not re.search(r"t\s*\*\s*t\s*\*\s*\(\s*3\.0\s*-\s*2\.0\s*\*\s*t\s*\)\s*\*\s*256\.0\s*\+\s*0\.5",
                 feather_body):
    bad_k.append("羽化公式必须是 t*t*(3-2t)*256+0.5（与 CPU 的查表同式）")
if not re.search(r"\(float\)\s*dist\s*/\s*\(float\)\s*fe", feather_body):
    bad_k.append("t 必须是 (float)dist / (float)fe（整数除法会直接变 0）")
print("[K] 羽化公式检查:", "PASS" if not bad_k else "FAIL")
for b in bad_k:
    fail.append("K: " + b)

# ---------------------------------------------------------------- L. 落笔取整
paint_body = body_of("paintWeight") or ""
bad_l = []
if not re.search(r"\*\s*feather\s*\+\s*128\s*\)\s*>>\s*8", paint_body):
    bad_l.append("落笔必须是 `(wgt*feather + 128) >> 8`（整数右移，与 CPU 的 unsigned char 同口径）")
if re.search(r"feather\s*/\s*256", paint_body):
    bad_l.append("出现 `feather / 256` —— 浮点除法在 .5 边界会与整数右移差 1")
if not re.search(r"if\s*\(\s*feather\s*>=\s*256\s*\)\s*return\s+wgt\s*;", paint_body):
    bad_l.append("缺少 feather>=256 的原值直通（CPU 有这条分支）")
print("[L] 落笔取整检查:", "PASS" if not bad_l else "FAIL")
for b in bad_l:
    fail.append("L: " + b)

ps_body = body_of("PS_Skin")
if ps_body is None:
    fail.append("C: 找不到 PS_Skin 的函数体，M/N/O 全部跳过")
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)

# ---------------------------------------------------------------- M. 几何来自 uniform
# 等价性的核心约定：几何（矩形/羽化宽度）由 CPU 按原式算好，shader 只 +0.5 还原整数。
# 一旦 shader 自己用 cx-hw 之类的浮点表达式再截断，就会和 MSVC 的截断分叉。
bad_m = []
for v in ("faceRect.x", "faceRect.z", "faceRect.y", "faceRect.w",
          "neckRect.x", "neckRect.z", "neckRect.y", "neckRect.w"):
    if not re.search(r"\(int\)\s*\(\s*%s\s*\+\s*0\.5\s*\)" % re.escape(v), ps_body):
        bad_m.append("几何量 %s 没有以 (int)(x+0.5) 还原整数" % v)
if re.search(r"\(int\)\s*\([^)]*(?:faceGeom\.z|faceGeom\.w)\s*[-+]", ps_body):
    bad_m.append("shader 自己在算矩形边界（应全部由宿主算好）")
if not re.search(r"faceFe\s*\+\s*0\.5", ps_body):
    bad_m.append("faceFe 没有以 (int)(fe+0.5) 还原")
print("[M] 几何来源检查:", "PASS" if not bad_m else "FAIL")
for b in bad_m:
    fail.append("M: " + b)

# ---------------------------------------------------------------- N. 眼睛保护
eye_body = body_of("protectEllipse") or ""
bad_n = []
if not re.search(r"du\s*\*\s*du\s*\+\s*dv\s*\*\s*dv", eye_body):
    bad_n.append("眼睛保护必须用平方距离 q = du*du + dv*dv")
if re.search(r"sqrt", eye_body):
    bad_n.append("眼睛保护里出现 sqrt —— CPU 是把**平方距离**直接喂 smoothstep")
if not re.search(r"smoothStep\s*\(\s*0\.35\s*,\s*1\.0\s*,\s*q\s*\)", eye_body):
    bad_n.append("眼睛保护必须是 smoothStep(0.35, 1.0, q)")
if not re.search(r"\(int\)\s*\(\s*\(\s*1\.0\s*-\s*t\s*\)\s*\*\s*255\.0\s*\+\s*0\.5\s*\)", eye_body):
    bad_n.append("保护权重取整必须是 (int)((1-t)*255.0+0.5)")
print("[N] 眼睛保护检查:", "PASS" if not bad_n else "FAIL")
for b in bad_n:
    fail.append("N: " + b)

# ---------------------------------------------------------------- O. max 合并
# CPU 是「脸框与脖子通道重叠处取较大值」。写成 += 或覆盖会让重叠区被抬亮/变暗。
bad_o = []
if len(re.findall(r"v\s*=\s*max\s*\(\s*v\s*,", ps_body)) < 2:
    bad_o.append("脸与脖子两段都必须 `v = max(v, ...)` 合并（CPU 是 max 语义）")
if re.search(r"v\s*\+=", ps_body):
    bad_o.append("出现 `v +=` —— 重叠区会被叠加抬亮，CPU 是 max")
print("[O] max 合并检查:", "PASS" if not bad_o else "FAIL")
for b in bad_o:
    fail.append("O: " + b)

# ---------------------------------------------------------------- D. fxc 真编译
def to_plain_hlsl(src):
    out = strip_techniques(src)
    out = re.sub(r"\btexture2d\b", "Texture2D", out)
    out = re.sub(r"(?m)^\s*sampler_state\s+(\w+)\s*\{[^}]*\}\s*;",
                 r"SamplerState \1;", out)
    out = re.sub(r":\s*TARGET\b", ": SV_Target", out)
    return out


plain = to_plain_hlsl(txt)
tmpdir = tempfile.mkdtemp(prefix="maskchk_")
plain_path = os.path.join(tmpdir, "mask_plain.hlsl")
io.open(plain_path, "w", encoding="utf-8", newline="\n").write(plain)

if not os.path.exists(FXC):
    print("[D] fxc 真编译: SKIP（找不到 fxc.exe：%s）" % FXC)
else:
    all_ok = True
    jobs = [("vs_4_0", "VSDefault")] + [("ps_4_0", e) for e in PS_ENTRIES]
    for prof, entry in jobs:
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
        print("[D] fxc %-7s /E %-9s %s%s"
              % (prof, entry, "PASS" if ok else "FAIL", inst))
        if not ok:
            all_ok = False
            fail.append("D: fxc %s 失败：\n%s" % (entry, out.strip()))
    print("[D] 汇总:", "PASS" if all_ok else "FAIL")

if fail:
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)
print("\n全部通过：mask.hlsl 语法可被 OBS 加载、3 个入口都能被 HLSL 编译器接受。")
print("注意：这只证明「能编译 + 公式没抄错」，逐像素等价必须在真机跑 M1 对拍。")
sys.exit(0)
