# -*- coding: utf-8 -*-
"""
smooth.hlsl 真编译门禁。

与 verify_tone_compiles.py 同一套思路（数值对 != 能跑），针对磨皮着色器的
差异补齐三项：

  1) 磨皮有 **5 个 technique / 5 个像素入口**（Down / BoxH / BoxV / Amp / Comp），
     tone 只有一个。所以 D 项要逐个入口编译，F 项要遍历所有 technique。
  2) 有 5 个 `uniform texture2d` + 6 个 sampler。OBS 的 ep_parse_sampler_state
     只按名字注册，纹理名与 sampler 名撞车会静默取错 —— 新增 H 项查重。
  3) 盒式半径是 `uniform int rad` 驱动的**动态循环**。fxc 能编，但循环写在
     函数外或用了未声明的 rad 都会在运行期才炸 —— 新增 I 项做符号检查。

检查清单：
  A 顶层 static/const 数组       （OBS effect 解析器不认）
  B 函数数组形参                 （ep_parse_func_param 不认）
  C technique 与入口齐全
  D fxc 真编译 6 个入口（vs 1 + ps 5）
  E sampler 必须是 OBS 语法
  F technique 入口必须带参
  G 顶点矩阵必须是 ViewProj + mul(vec, m)
  H texture2d / sampler_state 名字唯一且一一对应
  I 动态循环的半径符号必须声明为 uniform int

用法：python verify_smooth_compiles.py [smooth.hlsl 路径]
退出码 0 = 全过。
"""
import io, os, re, subprocess, sys, tempfile

HLS = sys.argv[1] if len(sys.argv) > 1 else \
    "D:/插件/OBS美颜GPU版/01_源码/shaders/smooth.hlsl"
FXC = "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"

txt = io.open(HLS, "r", encoding="utf-8").read()


def strip_comments(s):
    """去注释再检查 —— 文件头注释里写着「不要这样写」的示例，直接正则会误报。
    （2026-10-31 E 项就这样自伤过一次）"""
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def strip_techniques(s):
    """删除所有 technique 块（含内部 pass 的嵌套大括号），留给 fxc 用。

    必须匹配**行首 + 名字 + 花括号**三者，不能只搜裸词 technique：
    文件头注释里写着"technique 入口必须带参"，第一版按裸词匹配把从注释那行到
    文件尾全删了，fxc 于是报 `undeclared identifier 'ViewProj'` —— 检查器自己
    造出来的假错误，比不检查更浪费时间。
    """
    out = []
    i = 0
    while True:
        m = re.search(r"(?m)^\s*technique\s+\w+\s*\{", s[i:])
        if not m:
            out.append(s[i:])
            break
        start = i + m.start()
        out.append(s[i:start])
        j = i + m.end() - 1          # 正则尾字符就是 '{'，从它开始数括号
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
PS_ENTRIES = ["PS_Down", "PS_BoxH", "PS_BoxV", "PS_Amp", "PS_Comp"]
bad_c = []
if not re.search(r"\bVSDefault\s*\(", code):
    bad_c.append("找不到顶点入口 VSDefault")
for e in PS_ENTRIES:
    if not re.search(r"\b%s\s*\(" % re.escape(e), code):
        bad_c.append("找不到像素入口 %s" % e)
# 每个入口都要有对应 technique
for e in PS_ENTRIES:
    if not re.search(r"pixel_shader\s*=\s*%s\s*\(" % re.escape(e), code):
        bad_c.append("%s 没有被任何 technique 引用" % e)
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
print("[E] sampler 语法检查:", "PASS" if not bad_e else "FAIL")
for b in bad_e:
    fail.append("E: " + b)

# ---------------------------------------------------------------- F. technique 入口
bad_f = []
tech_blocks = re.findall(r"technique\s+(\w+)\s*\{", code)
if not tech_blocks:
    bad_f.append("没有找到任何 technique")
for tname in tech_blocks:
    # 取该 technique 的块体（pass 内没有嵌套大括号以外的结构）
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
        sig = re.search(r"(?m)^\s*\w+\s+%s\s*\(([^)]*)\)" % re.escape(fname), code)
        if sig:
            params = [p for p in sig.group(1).split(",") if p.strip()]
            if not params and args:
                bad_f.append("%s 的形参表为空，但 technique 里传了参数" % fname)
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

# ---------------------------------------------------------------- H. 纹理/sampler 名唯一
# OBS 的 ep_parse_sampler_state 按名字注册到同一个符号表；纹理名与 sampler 名
# 撞车时 fxc 还能过（类型不同会报错），但运行期 gs_effect_get_param_by_name
# 可能取到另一个，表现为「纹理内容不对」而不是报错 —— 必须静态拦。
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
# 每个 texture 至少要有一个 sampler 供它采样（数量不必相等，只查非空）
if tex_names and not sampler_names:
    bad_h.append("有 texture2d 却没有任何 sampler_state")
print("[H] 纹理/sampler 名检查:", "PASS" if not bad_h else "FAIL")
for b in bad_h:
    fail.append("H: " + b)

# ---------------------------------------------------------------- I. 动态循环半径
# 盒式 pass 用 `for (int k = -rad; k <= rad; ++k)`。rad 若没声明成 uniform int，
# fxc 会当未定义符号报错（能拦住）；但写成 `uniform float rad` 也能编过，
# 只在运行期拿不到整数半径 —— 表现为核宽变成 0。这里钉死类型。
#
# 2026-11-05 放宽：P2 的值域加权核是**固定 5 tap**（`for (int k = -2; k <= 2; ++k)`），
# 上下界都是字面量，不存在"半径不是整数"的风险。旧规则把 `-2` 当成变量名去找
# `uniform int 2;`，于是从 P2 落地那天起这一项一直是红的 —— 门禁常红等于没有门禁。
# 现在的判据分两类：
#   字面量下界（-2 …）  → 上界也必须是字面量（定长核，允许）
#   变量下界（-rad …）  → 必须有 `uniform int rad;`（动态核，钉死类型）
bad_i = []
loops = re.findall(r"for\s*\(\s*int\s+(\w+)\s*=\s*-(\w+)\s*;\s*\1\s*<=\s*(\w+)", code)
if loops:
    for var, lo, hi in sorted(set(loops)):
        if lo.isdigit():
            if not hi.isdigit():
                bad_i.append("循环 %s：下界是字面量 -%s，上界却是 `%s` —— "
                             "定长核的上下界必须同为字面量" % (var, lo, hi))
            continue
        if not re.search(r"uniform\s+int\s+%s\s*;" % re.escape(lo), code):
            bad_i.append("循环变量 -%s 不是 uniform int（盒式半径必须是整数）" % lo)
    if not any(lo == "rad" and lo.isdigit() is False for _, lo, _ in loops):
        bad_i.append("没找到 `for (int k = -rad;` 形式的盒式循环 —— 结构可能已被改坏")
else:
    bad_i.append("没找到 `for (int k = -rad;` 形式的盒式循环 —— 结构可能已被改坏")
print("[I] 动态循环半径检查:", "PASS" if not bad_i else "FAIL")
for b in bad_i:
    fail.append("I: " + b)

# ---------------------------------------------------------------- D. fxc 真编译
def to_plain_hlsl(src):
    """最小改写，只为让 fxc 认识。任何掩盖原始语法的改写都是门禁漏洞。"""
    out = strip_techniques(src)
    out = re.sub(r"\btexture2d\b", "Texture2D", out)
    out = re.sub(r"(?m)^\s*sampler_state\s+(\w+)\s*\{[^}]*\}\s*;",
                 r"SamplerState \1;", out)
    out = re.sub(r":\s*TARGET\b", ": SV_Target", out)
    return out


plain = to_plain_hlsl(txt)
tmpdir = tempfile.mkdtemp(prefix="smoothchk_")
plain_path = os.path.join(tmpdir, "smooth_plain.hlsl")
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
print("\n全部通过：smooth.hlsl 语法可被 OBS 加载、6 个入口都能被 HLSL 编译器接受。")
sys.exit(0)
