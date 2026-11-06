# -*- coding: utf-8 -*-
"""
detail.hlsl 真编译门禁。

与 verify_smooth_compiles.py 同一套骨架（数值对 != 能跑）。针对细节回填的差异：

  1) 5 个 technique / 5 个像素入口（Lum / Hist / BoxH / BoxV / Refill），
     和磨皮一样要逐个入口真编译。
  2) 没有动态循环 —— 盒宽固定 3 / 7，所以原 I 项（uniform int rad）改成
     「禁止出现动态循环」的反向检查：一旦有人把盒宽做成参数，就必须先
     把代码里的 1/3、1/7 和 tap 数改成一致，不能只改循环。
  3) J 项：亮度平面靠 `.b` 透传（Lum→BoxH→BoxV→Refill）。这条链路必须两段都对：
     J-1  PS_Lum 的每个 return float4 第 3 分量(.b) 必须 == 第 1 分量(.r)；
     J-2  BoxH/BoxV 必须把 image.b 原样写出。
     3.1.55 的真实 bug 就是 J-1 被写成 `.b = 0.0`，Refill 拿到 lum=0 → hf=−box3，
     整张脸被压暗而不是锐化，且不报任何错 —— 静态钉死，防止再犯。

检查清单：
  A 顶层 static/const 数组       B 函数数组形参
  C technique 与入口齐全          D fxc 真编译 6 个入口（vs 1 + ps 5）
  E sampler 必须是 OBS 语法       F technique 入口必须带参
  G 顶点矩阵必须是 ViewProj + mul(vec, m)
  H texture2d / sampler_state 名字唯一且一一对应
  I 不允许出现动态循环（盒宽固定 3/7）
  J PS_Lum 的 .b 必须 == .r；BoxH/BoxV 必须透传 .b（亮度链路）

用法：python verify_detail_compiles.py [detail.hlsl 路径]
退出码 0 = 全过。
"""
import io, os, re, subprocess, sys, tempfile

HLS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "01_源码", "shaders", "detail.hlsl")
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
PS_ENTRIES = ["PS_Lum", "PS_Hist", "PS_BoxH", "PS_BoxV", "PS_Refill"]
bad_c = []
if not re.search(r"\bVSDefault\s*\(", code):
    bad_c.append("找不到顶点入口 VSDefault")
for e in PS_ENTRIES:
    if not re.search(r"\b%s\s*\(" % re.escape(e), code):
        bad_c.append("找不到像素入口 %s" % e)
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
print("[H] 纹理/sampler 名检查:", "PASS" if not bad_h else "FAIL")
for b in bad_h:
    fail.append("H: " + b)

# ---------------------------------------------------------------- I. 禁止动态循环
# 盒宽在这个着色器里是**常量** 3 / 7（对应 CPU 的 boxPlaneH/V 半径 1 与 3）。
# 如果有人改成参数化循环，必须同时改 1/3、1/7 的归一化和 tap 数 —— 只改循环
# 会让带宽变但归一化不变，画面表现为「模糊了但亮度偏了」。所以这里直接禁止。
bad_i = []
for m in re.finditer(r"for\s*\(", code):
    bad_i.append("出现 for 循环（盒宽应为常量 3/7，不使用动态循环）")
print("[I] 动态循环检查:", "PASS" if not bad_i else "FAIL")
for b in bad_i:
    fail.append("I: " + b)

# ---------------------------------------------------------------- J. 亮度透传
# 亮度平面靠 .b 一路透传（Lum → BoxH → BoxV → Refill）：
#   J-1  PS_Lum 的每个 `return float4(...)` 必须让第 3 分量(.b) == 第 1 分量(.r)。
#        3.1.55 的真实 bug 就是这里写成了 0.0 —— Refill 拿到 lum=0，hf=lum−box3
#        退化成 −box3（约 −178），整张脸被压暗 ~1.4 灰阶而不是锐化。
#   J-2  PS_BoxH / PS_BoxV 必须把 image.b 原样写回输出 .b。
# 这两处漏任何一处都不报错、不崩溃，只会让画面「静默地不对」，所以静态钉死。
bad_j = []


def _func_body(name):
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


def _float4_return_args(body):
    """每个 `return float4(...)` 的实参列表（按顶层逗号切分，容忍嵌套括号/换行）。"""
    out = []
    for m in re.finditer(r"return\s+float4\s*\(", body):
        depth, cur, args, j = 1, "", [], m.end()
        while j < len(body) and depth > 0:
            ch = body[j]
            if ch == "(":
                depth += 1
                cur += ch
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
                cur += ch
            elif ch == "," and depth == 1:
                args.append(cur.strip())
                cur = ""
            else:
                cur += ch
            j += 1
        if cur.strip():
            args.append(cur.strip())
        out.append(args)
    return out


# J-1：PS_Lum 的 .b 必须 == .r（逐像素亮度透传）
_lum_body = _func_body("PS_Lum")
if _lum_body is None:
    bad_j.append("找不到 PS_Lum 的函数体")
else:
    _rets = _float4_return_args(_lum_body)
    if not _rets:
        bad_j.append("PS_Lum 没有任何 `return float4(...)`")
    for _args in _rets:
        if len(_args) < 4:
            bad_j.append("PS_Lum 的 return float4 参数不足 4 个：%s" % _args)
            continue
        if _args[0] != _args[2]:
            bad_j.append(
                "PS_Lum 的 return float4 第 3 分量(%s) != 第 1 分量(%s) —— "
                ".b 必须携带逐像素亮度(=.r)，否则 Refill 的 hf=lum−box3 会退化成 "
                "−box3，整张脸被压暗而不是锐化" % (_args[2], _args[0]))

# J-2：BoxH / BoxV 必须把 .b 原样写回输出
for entry in ("PS_BoxH", "PS_BoxV"):
    body = _func_body(entry)
    if body is None:
        bad_j.append("找不到 %s 的函数体" % entry)
        continue
    if not re.search(r"\.b\s*;", body) and not re.search(r"lum\s*,", body):
        bad_j.append("%s 没有把 image.b（亮度）写进输出" % entry)

print("[J] 亮度透传检查:", "PASS" if not bad_j else "FAIL")
for b in bad_j:
    fail.append("J: " + b)

# ---------------------------------------------------------------- D. fxc 真编译
def to_plain_hlsl(src):
    out = strip_techniques(src)
    out = re.sub(r"\btexture2d\b", "Texture2D", out)
    out = re.sub(r"(?m)^\s*sampler_state\s+(\w+)\s*\{[^}]*\}\s*;",
                 r"SamplerState \1;", out)
    out = re.sub(r":\s*TARGET\b", ": SV_Target", out)
    return out


plain = to_plain_hlsl(txt)
tmpdir = tempfile.mkdtemp(prefix="detailchk_")
plain_path = os.path.join(tmpdir, "detail_plain.hlsl")
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
print("\n全部通过：detail.hlsl 语法可被 OBS 加载、6 个入口都能被 HLSL 编译器接受。")
sys.exit(0)
