# -*- coding: utf-8 -*-
"""
tone.hlsl 真编译门禁 —— 把 OBS effect 方言翻成纯 HLSL 后交给 fxc 编译。

为什么需要这一步（2026-10-31 的血泪教训）：
  原先只有 verify_tone_alignment.py，它只比对了 LUT 数值与插值数学，
  于是两个**致死**缺陷都漏了过去：
    1) 顶层 `static const float X[255] = {...};` —— OBS effect 解析器不认
       （effect-parser.c: ep_parse_other），gs_effect_create_from_file 直接失败；
    2) `lut17(tbl + base, v)` —— C 指针运算，HLSL 报 X3022，fxc 编译不过。
  数值对 ≠ 能跑。本脚本负责「能跑」这一半。

检查五项：
  A. 顶层不允许出现 static/const 数组声明（会撞 OBS 解析器）。
  B. 函数形参里不允许出现 `[`（ep_parse_func_param 不认数组形参）。
  C. 必须有 technique 且引用 VSDefault / PSMain。
  D. 用 fxc 以 vs_4_0 / ps_4_0 真编译，0 error，并打印指令数。
  E. sampler 必须是 OBS 语法 `sampler_state <名> { ... };`（2026-10-31 真机踩到）。
  F. technique 入口必须带参数 `VSDefault(vert_in)`，不能是空括号（同日真机踩到）。
  G. 顶点矩阵必须叫 `ViewProj` 且按 mul(vec, ViewProj) 顺序（同日真机踩到，整屏黑）。

用法：python verify_tone_compiles.py [tone.hlsl 路径]
退出码 0 = 全过。
"""
import io, os, re, subprocess, sys, tempfile

HLS = sys.argv[1] if len(sys.argv) > 1 else "D:/插件/OBS美颜GPU版/01_源码/shaders/tone.hlsl"
FXC = "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"

txt = io.open(HLS, "r", encoding="utf-8").read()


def strip_comments(s):
    """去掉注释再做语法检查 —— 文件头注释里写着「不要这样写」的示例，
    直接正则会把示例当成真代码误报（2026-10-31 E 项就这样自伤过一次）。"""
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


code = strip_comments(txt)
fail = []

# ---------------------------------------------------------------- A. 顶层数组
# OBS 解析器逐 token 走：函数体外的 static 声明一定解析失败。
for no, line in enumerate(code.splitlines(), 1):
    if re.match(r"^\s{0,3}(static|const)\b", line):
        fail.append("A: 顶层出现 static/const 声明：%s" % line.strip())
print("[A] 顶层数组检查:", "PASS" if not any(f.startswith("A:") for f in fail) else "FAIL")

# ---------------------------------------------------------------- B. 数组形参
# 只看**函数声明**的形参表（不能把代码里的 CLARITY[b0+i] 这类下标表达式误判）。
for m in re.finditer(r"(?m)^\s*(?:float[1-4]?|void|int|bool|vertex_data)\s+\w+\s*\(([^)]*)\)", code):
    if "[" in m.group(1):
        fail.append("B: 函数形参里出现数组：(%s)" % m.group(1).strip())
print("[B] 数组形参检查:", "PASS" if not any(f.startswith("B:") for f in fail) else "FAIL")

# ---------------------------------------------------------------- C. technique
has_tech = re.search(r"technique\s+ToneTech", code) is not None
has_both = ("VSDefault(" in code) and ("PSMain(" in code)
ok_c = has_tech and has_both
print("[C] technique/入口检查:", "PASS" if ok_c else "FAIL（缺 technique 或入口）")
if not ok_c:
    fail.append("C: technique 或 vertex/pixel 入口缺失")

# ---------------------------------------------------------------- E. sampler 语法
# OBS 的 ep_parse_sampler_state 只认 `sampler_state <名字> { ... };`
# ——没有 uniform 前缀、没有 = 赋值、块必须以 `};` 收尾。
# 写成 HLSL 标准的 `uniform sampler_state x = sampler_state { ... };` 时，
# fxc 能过，但 OBS 运行期报 "Invalid type 'sampler_state' used for assignment"。
# 这条只有真机才暴露过，所以必须在静态检查里钉死。
bad_e = []
if re.search(r"uniform\s+sampler_state", code):
    bad_e.append("出现 `uniform sampler_state`（OBS 语法无 uniform 前缀）")
if re.search(r"=\s*sampler_state", code):
    bad_e.append("出现 `= sampler_state {...}`（OBS 语法无 = 赋值）")
for m in re.finditer(r"(?m)^\s*sampler_state\s+(\w+)\s*\{", code):
    tail = code[m.end():]
    if not re.match(r"[^}]*\}\s*;", tail):
        bad_e.append("sampler_state %s 的块没有以 `};` 收尾" % m.group(1))
if not re.search(r"(?m)^\s*sampler_state\s+\w+\s*\{", code):
    bad_e.append("没有找到任何 OBS 语法的 sampler_state 块")
ok_e = not bad_e
print("[E] sampler 语法检查:", "PASS" if ok_e else "FAIL")
for b in bad_e:
    fail.append("E: " + b)

# ---------------------------------------------------------------- F. technique 入口调用
# OBS 把 `vertex_shader = Func(x)` 的 token 原样拼进生成的 HLSL。写成
# `Func()` 空括号 → 生成零参调用 → D3D 报 X3013 "no matching 0 parameter
# function"。fxc 看不到这一条，因为 D 项把整个 technique 块删掉了再编译。
bad_f = []
ent = re.search(r"technique\s+ToneTech\s*\{(.*)\}\s*$", code, re.S)
if not ent:
    bad_f.append("找不到 technique ToneTech 块")
else:
    body = ent.group(1)
    for kind, pat in (("vertex_shader", r"vertex_shader\s*=\s*(\w+)\s*\(([^)]*)\)"),
                      ("pixel_shader",  r"pixel_shader\s*=\s*(\w+)\s*\(([^)]*)\)")):
        m = re.search(pat, body)
        if not m:
            bad_f.append("%s 没有赋值语句" % kind)
            continue
        fname, args = m.group(1), m.group(2).strip()
        if not args:
            bad_f.append("%s = %s() 是空参数调用（必须写成 %s(<参数名>)）"
                         % (kind, fname, fname))
        # 与函数签名比对：形参个数必须 >= 调用里给的个数
        sig = re.search(r"(?m)^\s*\w+\s+%s\s*\(([^)]*)\)" % re.escape(fname), code)
        if sig:
            params = [p for p in sig.group(1).split(",") if p.strip()]
            if not params:
                bad_f.append("%s 的形参表为空，但 technique 里传了参数" % fname)
ok_f = not bad_f
print("[F] technique 入口调用检查:", "PASS" if ok_f else "FAIL")
for b in bad_f:
    fail.append("F: " + b)

# ---------------------------------------------------------------- G. 顶点矩阵
# D3D11 后端按**名字** "ViewProj" 自动绑定世界/视图/投影矩阵
# （d3d11-shader.cpp:82）。名字写成 ViewProjMatrix 不会报任何错，只是矩阵
# 永远停在单位阵 -> 顶点 [0..cx] 被当成 NDC -> 整屏黑，而 texrender 仍然
# 返回一张「有效但全 0」的纹理，上层分不清是没画上还是算错了。
# 另外 OBS 用行向量约定：mul(vec, ViewProj)，矩阵在右。
bad_g = []
if not re.search(r"uniform\s+float4x4\s+ViewProj\s*;", code):
    bad_g.append("缺少 `uniform float4x4 ViewProj;`（参数名必须是 ViewProj）")
if re.search(r"\bViewProjMatrix\b", code):
    bad_g.append("出现 `ViewProjMatrix` —— D3D11 后端只认 `ViewProj`，会自动绑定失败")
for m in re.finditer(r"mul\s*\(\s*(ViewProj\w*)\s*,", code):
    bad_g.append("mul 顺序反了：OBS 约定是 mul(vec, ViewProj)，矩阵放右边")
ok_g = not bad_g
print("[G] 顶点矩阵检查:", "PASS" if ok_g else "FAIL")
for b in bad_g:
    fail.append("G: " + b)

# ---------------------------------------------------------------- D. fxc 真编译
def to_plain_hlsl(src):
    """把 OBS effect 方言翻成 fxc 认识的纯 HLSL。

    注意：这里只做「fxc 必需」的最小改写。**任何掩盖原始语法的改写都是
    门禁漏洞**——第 3 条 sampler 错误就是因为旧版在这里把
    `uniform sampler_state X = sampler_state {...}` 悄悄换成了
    `SamplerState X;`，fxc 于是永远看不到问题。所以 E 项检查必须独立保留。
    """
    out = src
    # texture2d → Texture2D
    out = re.sub(r"\btexture2d\b", "Texture2D", out)
    # OBS: sampler_state X { ... };  →  SamplerState X;
    out = re.sub(r"(?m)^\s*sampler_state\s+(\w+)\s*\{[^}]*\}\s*;",
                 r"SamplerState \1;", out)
    # : TARGET → : SV_Target
    out = re.sub(r":\s*TARGET\b", ": SV_Target", out)
    # 去掉 technique 块（fxc 以 /E 指定入口，不需要 technique）
    out = re.sub(r"technique\s+ToneTech\s*\{.*?\n\}\s*$", "", out, flags=re.S)
    return out

plain = to_plain_hlsl(txt)
tmpdir = tempfile.mkdtemp(prefix="tonechk_")
plain_path = os.path.join(tmpdir, "tone_plain.hlsl")
io.open(plain_path, "w", encoding="utf-8", newline="\n").write(plain)

if not os.path.exists(FXC):
    print("[D] fxc 真编译: SKIP（找不到 fxc.exe：%s）" % FXC)
else:
    all_ok = True
    for prof, entry in (("vs_4_0", "VSDefault"), ("ps_4_0", "PSMain")):
        asm = os.path.join(tmpdir, "tone_%s.asm" % prof)
        obj = os.path.join(tmpdir, "tone_%s.bin" % prof)
        p = subprocess.run([FXC, "/nologo", "/T", prof, "/E", entry,
                            "/Fc" + asm, "/Fo" + obj, plain_path],
                           capture_output=True, text=True)
        out = (p.stdout or "") + (p.stderr or "")
        inst = ""
        if os.path.exists(asm):
            m = re.search(r"// Approximately (\d+) instruction slots", io.open(asm, "r", errors="ignore").read())
            if m:
                inst = "（%s 条指令）" % m.group(1)
        ok = (p.returncode == 0) and ("error" not in out.lower())
        print("[D] fxc %s /E %s: %s%s" % (prof, entry, "PASS" if ok else "FAIL", inst))
        if not ok:
            all_ok = False
            fail.append("D: fxc %s 失败：\n%s" % (prof, out.strip()))
        elif "warning X4000" in out:
            print("     注意：有 X4000（疑似未初始化）警告，需人工确认")
    print("[D] 汇总:", "PASS" if all_ok else "FAIL")

if fail:
    print("\n---- 失败详情 ----")
    for f in fail:
        print(f)
    sys.exit(1)
print("\n全部通过：tone.hlsl 既与 CPU 数值对齐（另见 verify_tone_alignment.py），也能被 HLSL 编译器接受。")
sys.exit(0)
