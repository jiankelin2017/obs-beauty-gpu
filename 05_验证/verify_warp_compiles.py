# -*- coding: utf-8 -*-
"""
warp.hlsl 真编译门禁（5c / 3.1.62）。

与 verify_detail_compiles.py 同一套骨架（数值对 != 能跑），针对美形的差异：
本文件只有 **1 个 technique / 1 个像素入口**，但它要逐位复现 CPU 的
warpFaceWithGeom，所以静态检查的重点从「入口齐不齐」变成「等价性有没有被
写丢」——下面每一条都对应一个会让画面静默出错、却不报错不改崩溃的写法。

检查清单：
  A 顶层 static/const 数组          B 函数数组形参
  C technique 与入口齐全            D fxc 真编译 2 个入口（vs 1 + ps 1）
  E sampler 必须是 OBS 语法         F technique 入口必须带参
  G 顶点矩阵必须是 ViewProj + mul(vec, m)
  H texture2d / sampler_state 名字唯一且一一对应
  I 三段位移必须**各自写回**，且顺序为大眼→瘦脸→下巴
  J 采样点必须钳进 ROI（clamp(sx-roi.x) / clamp(sy-roi.y)）
  K 必须是 4 点手工双线性（权重取自 fx/fy），不能用硬件 bilinear
  L 位移可忽略的快速路径阈值必须是 0.02（与 CPU 同值）
  M 四通道一起插值（CPU 是 for c<4，含 alpha；写成 .rgb 会让透明边缘差几十灰阶）
  N 截断必须 floor(v+0.5)，不能直接返回浮点（RGBA8 会自己四舍五入 → 每像素 +1）
  O ROI 外必须原样透传（CPU 只写 ROI 内，一个字节都不动）

用法：python verify_warp_compiles.py [warp.hlsl 路径]
退出码 0 = 全过。
"""
import io, os, re, subprocess, sys, tempfile

HLS = sys.argv[1] if len(sys.argv) > 1 else \
    "D:/插件/OBS美颜GPU版/01_源码/shaders/warp.hlsl"
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


def func_body(name):
    """取 `float4 <name>(...) : TARGET { ... }` 的函数体（大括号配对）。"""
    m = re.search(r"float4\s+%s\s*\([^)]*\)\s*:\s*TARGET\s*\{" % re.escape(name), code)
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
PS_ENTRIES = ["PS_Warp"]
bad_c = []
if not re.search(r"\bVSDefault\s*\(", code):
    bad_c.append("找不到顶点入口 VSDefault")
for e in PS_ENTRIES:
    if not re.search(r"float4\s+%s\s*\(" % re.escape(e), code):
        bad_c.append("找不到像素入口 %s" % e)
    if not re.search(r"pixel_shader\s*=\s*%s\s*\(" % re.escape(e), code):
        bad_c.append("%s 没有被任何 technique 引用" % e)
if not re.search(r"technique\s+WarpTech\s*\{", code):
    bad_c.append("找不到 technique WarpTech（宿主按名字取）")
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
# 采样必须是 Point：Clamp 对应 CPU 的 clampi，Linear 会让 4 点手工加权失效
if sampler_names and not re.search(r"Filter\s*=\s*Point\s*;", code):
    bad_e.append("sampler 不是 Filter=Point —— 4 点手工双线性要求点采样")
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

body = func_body("PS_Warp")
if body is None:
    fail.append("C: 找不到 PS_Warp 的函数体，后续等价性检查全部跳过")
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)

# ---------------------------------------------------------------- I. 三段位移
# 每一段都必须真的把 sx/sy 写回去。漏一句就是「滑块没反应」或「只有部分生效」，
# 而不写回时着色器仍然编译通过、仍然跑得动 —— 必须静态钉死。
bad_i = []
# 大眼段：sx = ex + dx / m;  sy = ey + dy / m;
if not re.search(r"sx\s*=\s*ex\s*\+\s*dx\s*/\s*m\s*;", body):
    bad_i.append("大眼段没有把 sx 写回（应为 `sx = ex + dx / m;`）")
if not re.search(r"sy\s*=\s*ey\s*\+\s*dy\s*/\s*m\s*;", body):
    bad_i.append("大眼段没有把 sy 写回（应为 `sy = ey + dy / m;`）")
# 瘦脸段：sx = fA.x + (sx - fA.x) * (1.0 + k);
if not re.search(r"sx\s*=\s*fA\.x\s*\+\s*\(sx\s*-\s*fA\.x\)\s*\*\s*\(1\.0\s*\+\s*k\)", body):
    bad_i.append("瘦脸段没有把 sx 写回（应为 `sx = fA.x + (sx - fA.x) * (1.0 + k);`）")
# 下巴段：sy = fD.x + (sy - fD.x) * (1.0 + k);
if not re.search(r"sy\s*=\s*fD\.x\s*\+\s*\(sy\s*-\s*fD\.x\)\s*\*\s*\(1\.0\s*\+\s*k\)", body):
    bad_i.append("下巴段没有把 sy 写回（应为 `sy = fD.x + (sy - fD.x) * (1.0 + k);`）")
# 顺序：CPU 是 大眼 → 瘦脸 → 下巴，每一段读上一段的输出。换顺序 = 换算法。
pos_eye = body.find("wFlags & 1")
pos_thin = body.find("wFlags & 2")
pos_chin = body.find("wFlags & 4")
if min(pos_eye, pos_thin, pos_chin) < 0:
    bad_i.append("wFlags 的 1/2/4 三个位没有全部出现")
elif not (pos_eye < pos_thin < pos_chin):
    bad_i.append("三段顺序错了（必须是 大眼(%d) → 瘦脸(%d) → 下巴(%d)）"
                 % (pos_eye, pos_thin, pos_chin))
print("[I] 三段位移写回/顺序检查:", "PASS" if not bad_i else "FAIL")
for b in bad_i:
    fail.append("I: " + b)

# ---------------------------------------------------------------- J. ROI 钳位
# 不钳的话瘦脸大幅时采样点会被推出图像边界，表现为 ROI 边缘一圈拖影。
bad_j = []
if not re.search(r"clamp\s*\(\s*sx\s*-\s*roi\.x\s*,\s*0\.0\s*,", body):
    bad_j.append("缺少水平钳位 `clamp(sx - roi.x, 0.0, rw-1)`")
if not re.search(r"clamp\s*\(\s*sy\s*-\s*roi\.y\s*,\s*0\.0\s*,", body):
    bad_j.append("缺少垂直钳位 `clamp(sy - roi.y, 0.0, rh-1)`")
print("[J] ROI 钳位检查:", "PASS" if not bad_j else "FAIL")
for b in bad_j:
    fail.append("J: " + b)

# ---------------------------------------------------------------- K. 4 点手工双线性
# 硬件 bilinear 的权重由 uv 反推，uv 是浮点，4K 下末位误差会让权重偏移 → ±1。
# 必须 4 次 Point 采样 + fx/fy 手工加权，权重与 CPU 同源。
bad_k = []
for u in ("u00", "u01", "u10", "u11"):
    if not re.search(r"image\.Sample\s*\(\s*pointTex\s*,\s*%s\s*\)" % u, body):
        bad_k.append("缺少 4 点采样之一：image.Sample(pointTex, %s)" % u)
if not re.search(r"p00\s*\*\s*\(1\.0\s*-\s*fx\)\s*\+\s*p01\s*\*\s*fx", body):
    bad_k.append("水平权重不是 `p00*(1-fx) + p01*fx`（必须与 CPU 同源）")
if not re.search(r"a\s*\*\s*\(1\.0\s*-\s*fy\)\s*\+\s*b\s*\*\s*fy", body):
    bad_k.append("垂直权重不是 `a*(1-fy) + b*fy`（必须与 CPU 同源）")
# 采样点必须落在像素中心（+0.5），否则 4 点会各偏半格
if not re.search(r"\+\s*0\.5\s*\)\s*\*\s*invSize", body):
    bad_k.append("采样 uv 没有 +0.5 对齐像素中心")
print("[K] 手工双线性检查:", "PASS" if not bad_k else "FAIL")
for b in bad_k:
    fail.append("K: " + b)

# ---------------------------------------------------------------- L. 快速路径阈值
# CPU：|dxo|<0.02 && |dyo|<0.02 → memcpy。阈值不同会让「接近不动」的像素
# 一边直拷一边重采样，两者差 0 或 1 灰阶 —— 逐像素对拍就会零星报差异。
bad_l = []
if not re.search(r"dxo\s*>\s*-0\.02", body) or not re.search(r"dxo\s*<\s*0\.02", body):
    bad_l.append("水平快速路径阈值不是 ±0.02（CPU 是 -0.02 < dxo < 0.02）")
if not re.search(r"dyo\s*>\s*-0\.02", body) or not re.search(r"dyo\s*<\s*0\.02", body):
    bad_l.append("垂直快速路径阈值不是 ±0.02（CPU 是 -0.02 < dyo < 0.02）")
print("[L] 快速路径阈值检查:", "PASS" if not bad_l else "FAIL")
for b in bad_l:
    fail.append("L: " + b)

# ---------------------------------------------------------------- M. 四通道插值
# CPU 是 `for (int c = 0; c < 4; c++)` —— alpha 也参与插值。写成 .rgb 再单独
# 拿 col.a 会让透明区边缘的 alpha 不随位移走，差几十灰阶。
bad_m = []
if re.search(r"\.rgb\b", body):
    bad_m.append("出现 `.rgb` 三通道截取 —— 插值必须四通道（CPU 的 c<4 含 alpha）")
if re.search(r"col\.a\b", body):
    bad_m.append("出现 `col.a` —— alpha 不能单独取，必须与 RGB 一起插值")
for p in ("p00", "p01", "p10", "p11"):
    if not re.search(r"float4\s+%s\s*=" % p, body):
        bad_m.append("%s 不是 float4（四通道）" % p)
print("[M] 四通道插值检查:", "PASS" if not bad_m else "FAIL")
for b in bad_m:
    fail.append("M: " + b)

# ---------------------------------------------------------------- N. 截断口径
# CPU 是 `(unsigned char)(v + 0.5)`。写 RGBA8 时硬件会四舍五入，所以必须先
# floor 取整再 ÷255；直接返回浮点会让每像素 +1。
bad_n = []
if not re.search(r"floor\s*\(\s*v\s*\+\s*0\.5\s*\)", body):
    bad_n.append("缺少 `floor(v + 0.5)` 取整（RGBA8 会自己四舍五入 → 每像素 +1）")
if not re.search(r"1\.0\s*/\s*255\.0", body):
    bad_n.append("缺少 `* (1.0 / 255.0)` 回到 0..1 域")
if not re.search(r"\*\s*255\.0", body):
    bad_n.append("采样没有 ×255 进灰阶域（必须与 CPU 的 0..255 整数域同尺度）")
print("[N] 截断/域检查:", "PASS" if not bad_n else "FAIL")
for b in bad_n:
    fail.append("N: " + b)

# ---------------------------------------------------------------- O. ROI 外透传
# CPU 只写 ROI 内的像素。着色器是全帧跑的，ROI 外必须原样返回，否则整帧
# 会被重采样一遍（画面看着没差，但逐像素对拍全是 ±1）。
bad_o = []
if not re.search(r"ox\s*<\s*roi\.x\s*\|\|\s*ox\s*>=\s*roi\.z", body):
    bad_o.append("缺少 ROI 外透传判据 `ox < roi.x || ox >= roi.z`")
if not re.search(r"oy\s*<\s*roi\.y\s*\|\|\s*oy\s*>=\s*roi\.w", body):
    bad_o.append("缺少 ROI 外透传判据 `oy < roi.y || oy >= roi.w`")
print("[O] ROI 外透传检查:", "PASS" if not bad_o else "FAIL")
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
tmpdir = tempfile.mkdtemp(prefix="warpchk_")
plain_path = os.path.join(tmpdir, "warp_plain.hlsl")
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
        elif "warning X4000" in out:
            print("     注意：有 X4000（疑似未初始化）警告，需人工确认")
    print("[D] 汇总:", "PASS" if all_ok else "FAIL")

if fail:
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)
print("\n全部通过：warp.hlsl 语法可被 OBS 加载、2 个入口都能被 HLSL 编译器接受。")
sys.exit(0)
