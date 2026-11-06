#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
发布一致性离线闸门（不需要 OBS，不需要跑画面）。

回答一个每次发布前都得问、而以前只能靠人眼的问题：
    「我手里这个 dll，到底是哪份源码编出来的？装到机器上的、打进安装包的，
      跟它是同一个文件吗？」

对应 `docs/CHANGELOG.md` 的 N13 / N15 两条工程债（旧编号映射见 `docs/INDEX.md` §4）：
    N13 安装包会与构建产物脱节（2.9.1 装了一个月，Setup.exe 还停在 2.9.0）
    N14 工具不校验「dll 版本 == 源码 BEAUTY_BUILD_STR」

用法:
    py -3 05_验证/check_release.py                # 全量检查，有问题退出码 1
    py -3 05_验证/check_release.py --strict       # WARN 也算失败（发布前用这个）
    py -3 05_验证/check_release.py --no-installed # 跳过「装机 vs 构建」比对
    py -3 05_验证/check_release.py --json out.json

检查项（每项独立判定，互不掩盖）:
    1. 版本单源    01_源码/beauty_params.h 的 BEAUTY_BUILD_STR 能被解析
    2. 构建产物    02_构建/obs-beauty.dll 里**真的有**这个版本字符串（不是靠文件名猜）
    3. 装机        装机 dll 与构建 dll 逐字节相同
    4. 安装包      07_安装包/obs-beauty.dll 与构建 dll 逐字节相同
    5. 着色器      源码 / 安装包 / 装机 / solo 目录 四份 6 个 .hlsl 全部逐字节相同
    6. 开关表      开关总表.md ⊆ 源码 getenv()，且「源码有、表里没有、别的文档提到」= 0
    7. 安装路径    setup.cpp 的 plug/shad 必须由 %PROGRAMDATA% 插件根派生（3.0.1 修 N13 第二种形态）
                   3.2.2 起判据改为「赋值链可追到 root」：允许 binDir 这类中间变量，
                   但 plug 必须落在 bin\64bit 下、且不得是 Program Files 字面量
    8. 安装包版本串 Setup.exe 内真的含当前版本（ASCII / UTF-16LE）
    9. 安装包载荷   直接从 Setup.exe 的 RCDATA 读回 dll + 运行时 + 人脸模型 + 许可 +
                   6 个 shader，与磁盘权威副本逐字节相同（3.2.2 起 5 类 10 项）

    7-9 是 3.0.1 新增：3.0.0 时 Setup.exe 只被检查「文件存在」，于是它装到
    Program Files 旧目录（OBS 32.x 根本不加载）也能一路 9 PASS 发布出去。

为什么第 2 项要看二进制内容而不是 `dll_version()`：
    撞版本号是最常见的翻车方式 —— 源码 bump 了，但编译命令没重跑，dll 还是旧的，
    文件名/时间戳都像新的。只有从二进制里搜字符串，才知道**这份 dll 自报几版**。
"""
# 脚本自身版本（文档治理配套改动，与插件 BEAUTY_BUILD_STR 无关）
__version__ = "0.1.0"

import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SRC_DIR = os.path.join(ROOT, "01_源码")
BUILD_DLL = os.path.join(ROOT, "02_构建", "obs-beauty.dll")
PKG_DLL = os.path.join(ROOT, "07_安装包", "obs-beauty.dll")
INSTALLED_DLL = r"C:\ProgramData\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll"
SOLO_SHADER_DIR = r"D:\obs_beauty_gpu\shaders"
PARAMS_H = os.path.join(SRC_DIR, "beauty_params.h")
# 文档治理后开关总表统一在 docs/reference/（正文表格语义不变）
SWITCH_DOC = os.path.join(ROOT, "docs", "reference", "reference-switches.md")

SHADERS = ["smooth.hlsl", "tone.hlsl", "detail.hlsl", "warp.hlsl", "mask.hlsl",
           "region.hlsl"]

# 检查结果：(级别, 项, 说明)。级别 PASS / WARN / FAIL
RESULTS = []


def add(level, item, msg):
    RESULTS.append({"level": level, "item": item, "msg": msg})


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="ignore") as fp:
        return fp.read()


# ---------------------------------------------------------------- 1. 版本单源
def check_version_source():
    if not os.path.isfile(PARAMS_H):
        add("FAIL", "版本单源", "找不到 %s" % PARAMS_H)
        return None
    m = re.search(r'#define\s+BEAUTY_BUILD_STR\s+"([^"]+)"', read_text(PARAMS_H))
    if not m:
        add("FAIL", "版本单源", "beauty_params.h 里没有 BEAUTY_BUILD_STR 定义")
        return None
    ver = m.group(1)
    hits = []
    for name in sorted(os.listdir(SRC_DIR)):
        if name.endswith((".cpp", ".h")):
            n = len(re.findall(r'#define\s+BEAUTY_BUILD_STR', read_text(
                os.path.join(SRC_DIR, name))))
            if n:
                hits.append("%s x%d" % (name, n))
    if len(hits) != 1:
        add("FAIL", "版本单源", "BEAUTY_BUILD_STR 出现 %d 处（必须只有 1 处）：%s"
            % (len(hits), ", ".join(hits)))
    else:
        add("PASS", "版本单源", "BEAUTY_BUILD_STR = %s（唯一定义：%s）" % (ver, hits[0]))
    return ver


# ---------------------------------------------------------- 2. 构建产物含版本串
def check_build_contains(ver, path, label, missing_level="FAIL"):
    if not ver:
        return
    if not os.path.isfile(path):
        add(missing_level, label, "文件不存在：%s" % path)
        return
    blob = open(path, "rb").read()
    needles = [ver.encode("ascii"), ver.encode("utf-16-le")]
    found = [n for n in needles if n in blob]
    if not found:
        add("FAIL", label,
            "%s 里搜不到 \"%s\" —— 这份二进制不是当前源码编的（或版本宏没被引用）"
            % (os.path.basename(path), ver))
        return
    # 顺带抓出二进制里所有 x.y.z 形态的候选，撞版本号时一眼看出差在哪
    cands = sorted({s.decode("ascii") for s in re.findall(
        rb"\b\d+\.\d+\.\d+\b", blob)})
    if len(found) == 1:
        how = "ASCII"
    else:
        how = "ASCII + UTF-16LE"
    add("PASS", label, "%s 内含 \"%s\"（%s）；二进制里全部版本形态：%s"
        % (os.path.basename(path), ver, how, ", ".join(cands[:12])))


# ------------------------------------------------------- 3/4. 逐字节比对两份 dll
def check_same_dll(path, label, ref_md5, ref_size, missing_level="FAIL"):
    if not os.path.isfile(path):
        add(missing_level, label, "文件不存在：%s" % path)
        return
    size = os.path.getsize(path)
    d = md5(path)
    if d == ref_md5 and size == ref_size:
        add("PASS", label, "与构建产物相同（%s，%d B）" % (d, size))
    else:
        add("FAIL", label, "与构建产物**不同**：%s %d B ≠ 构建 %s %d B"
            % (d, size, ref_md5, ref_size))


# ------------------------------------------------------------------ 5. 着色器
def shader_paths():
    return [
        ("源码", os.path.join(SRC_DIR, "shaders")),
        ("安装包", os.path.join(ROOT, "07_安装包", "shaders")),
        ("装机", r"C:\ProgramData\obs-studio\plugins\obs-beauty\data\shaders"),
        ("solo", SOLO_SHADER_DIR),
    ]


def check_shaders():
    ref = {}
    src_dir = os.path.join(SRC_DIR, "shaders")
    for name in SHADERS:
        p = os.path.join(src_dir, name)
        if not os.path.isfile(p):
            add("FAIL", "着色器", "源码缺 %s" % name)
            continue
        ref[name] = md5(p)
    if not ref:
        return
    for label, d in shader_paths()[1:]:
        if not os.path.isdir(d):
            add("WARN", "着色器", "%s 目录不存在（跳过）：%s" % (label, d))
            continue
        bad = []
        for name, h in ref.items():
            p = os.path.join(d, name)
            if not os.path.isfile(p):
                bad.append("%s 缺失" % name)
            elif md5(p) != h:
                bad.append("%s 内容不同" % name)
        extra = [n for n in os.listdir(d)
                 if n.endswith(".hlsl") and n not in ref]
        if extra:
            bad.append("多余文件 %s" % ", ".join(sorted(extra)))
        if bad:
            add("FAIL", "着色器", "%s：%s" % (label, "；".join(bad)))
        else:
            add("PASS", "着色器", "%s：%d 个 .hlsl 全部与源码逐字节相同"
                % (label, len(ref)))


# ------------------------------------------------------------------ 6. 开关表
def check_switches():
    src = set()
    for name in sorted(os.listdir(SRC_DIR)):
        if not name.endswith((".cpp", ".h")):
            continue
        text = read_text(os.path.join(SRC_DIR, name))
        # 只看 BEAUTY_ 前缀：源码里还有 getenv("TEMP") 这类系统变量，
        # 它们不在本表口径内（本表只管本插件的操作开关）。
        src |= set(re.findall(r'getenv\("(BEAUTY_[A-Z_0-9]+)"\)', text))
    if not os.path.isfile(SWITCH_DOC):
        add("FAIL", "开关表", "找不到 %s" % SWITCH_DOC)
        return
    tbl = set(re.findall(r'^\| `(BEAUTY_[A-Z_0-9]+)`', read_text(SWITCH_DOC),
                         re.M))
    doc_missing = sorted(tbl - src)
    if doc_missing:
        add("FAIL", "开关表",
            "表里有、源码 getenv() 里没有（已删/改名/未实现）：%s"
            % ", ".join(doc_missing))
    else:
        add("PASS", "开关表", "表内 %d 条全部能在源码 getenv() 找到" % len(tbl))

    # 反方向：源码有、表里没有 —— 只有「别的文档/脚本提到过」才算漏收
    corpus = ""
    for sub in ("04_文档", "05_验证", "03_装机", "06_离线验证"):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            p = os.path.join(d, name)
            if os.path.isfile(p) and name.endswith(
                    (".md", ".py", ".ps1", ".bat", ".txt")):
                corpus += read_text(p)
    # 现行文档统一在 docs/；archive/ 是历史，含未入表的内部旋钮名，
    # 一旦纳入语料就会把「内部旋钮」误判成「文档提到却没进表」的漏收（假 FAIL）。
    docs_root = os.path.join(ROOT, "docs")
    if os.path.isdir(docs_root):
        for dirpath, dirnames, filenames in os.walk(docs_root):
            dirnames[:] = [d for d in dirnames if d != "archive"]
            for name in sorted(filenames):
                if name.endswith((".md", ".py", ".ps1", ".bat", ".txt")):
                    corpus += read_text(os.path.join(dirpath, name))
    for name in ("README.md", "PROGRESS.md"):   # 兼容治理前旧布局，文件移走后自动失效
        p = os.path.join(ROOT, name)
        if os.path.isfile(p):
            corpus += read_text(p)
    leaked = sorted(n for n in (src - tbl) if n in corpus)
    if leaked:
        add("FAIL", "开关表",
            "源码有、表里没有、但别的文档/脚本提到过 %d 条（按口径必须收进表）：%s"
            % (len(leaked), ", ".join(leaked)))
    else:
        add("PASS", "开关表", "没有「文档提到却没进表」的开关；未入表的 %d 条属纯内部旋钮"
            % len(src - tbl))


# ------------------------------------------- 7/8/9. Setup.exe 路径与内嵌载荷
SETUP_EXE = os.path.join(ROOT, "07_安装包", "Setup.exe")
SETUP_CPP = os.path.join(ROOT, "07_安装包", "setup.cpp")
# setup.rc 的资源 id -> 文件名（顺序与 rc 保持一致）
SETUP_RCDATA = {
    101: "obs-beauty.dll",
    102: "onnxruntime.dll",
    103: "yunet.onnx",
    104: "THIRD-PARTY-NOTICES.md",
    201: "detail.hlsl", 202: "mask.hlsl", 203: "region.hlsl",
    204: "smooth.hlsl", 205: "tone.hlsl", 206: "warp.hlsl",
}


def setup_payload_disk(name):
    """内嵌资源对应的磁盘权威副本。

    104 以仓库根的那份为准（它入库、受审查）；07_安装包/ 下的副本是构建时
    拷进去的中间物，比它自己没有意义。
    """
    if name == "obs-beauty.dll":
        return PKG_DLL
    if name == "THIRD-PARTY-NOTICES.md":
        return os.path.join(ROOT, name)
    if name.endswith(".hlsl"):
        return os.path.join(ROOT, "07_安装包", "shaders", name)
    return os.path.join(ROOT, "07_安装包", name)


def read_setup_resources():
    """只把 Setup.exe 当数据文件读，不执行它；返回 {id: bytes} 或 None。"""
    if sys.platform != "win32":
        return None
    import ctypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p,
                                 ctypes.c_uint32]
    k.LoadLibraryExW.restype = ctypes.c_void_p
    k.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p]
    k.FindResourceW.restype = ctypes.c_void_p
    k.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    k.SizeofResource.restype = ctypes.c_uint32
    k.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    k.LoadResource.restype = ctypes.c_void_p
    k.LockResource.argtypes = [ctypes.c_void_p]
    k.LockResource.restype = ctypes.c_void_p
    k.FreeLibrary.argtypes = [ctypes.c_void_p]
    mod = k.LoadLibraryExW(SETUP_EXE, None, 0x2)  # LOAD_LIBRARY_AS_DATAFILE
    if not mod:
        return None
    try:
        out = {}
        for rid in SETUP_RCDATA:
            r = k.FindResourceW(mod, rid, 10)     # 10 = RT_RCDATA
            if not r:
                return None
            sz = k.SizeofResource(mod, r)
            g = k.LoadResource(mod, r)
            p = k.LockResource(g) if g else None
            if not p or not sz:
                return None
            out[rid] = ctypes.string_at(p, sz)
        return out
    finally:
        k.FreeLibrary(mod)


def _wstring_expr(text, name):
    """取 `const std::wstring <name> = <expr>;` 的右值表达式，没有则 None。"""
    m = re.search(r"const\s+std::wstring\s+%s\s*=\s*([^;]+);" % re.escape(name),
                  text)
    return m.group(1).strip() if m else None


def _derives_from_root(text, name, depth=0):
    """name 的赋值表达式能否一路追到 root。

    允许经中间变量（3.2.2 把 bin 目录抽成了 binDir），否则把 `plug = root + ...`
    内联或拆开都会让闸门误判 —— 判据该是「派生自 %PROGRAMDATA% 插件根」这个
    语义，不是某一种写法。
    """
    if depth > 3:
        return False
    e = _wstring_expr(text, name)
    if e is None:
        return False
    if re.search(r"\broot\b", e):
        return True
    for ref in re.findall(r"\b([A-Za-z_]\w*)\b", e):
        if ref != name and _derives_from_root(text, ref, depth + 1):
            return True
    return False


def check_setup(ver):
    # (7) 路径：plug / shad 必须由 ProgramData 插件根派生，不能是旧布局
    if not os.path.isfile(SETUP_CPP):
        add("FAIL", "安装路径", "找不到 %s" % SETUP_CPP)
    else:
        t = read_text(SETUP_CPP)
        plug_e = _wstring_expr(t, "plug") or ""
        bin_e = _wstring_expr(t, "binDir") or ""
        ok = ("ProgramData" in t
              and "obs-studio\\\\plugins\\\\obs-beauty" in t
              and _derives_from_root(t, "plug")
              and _derives_from_root(t, "shad")
              and "obs-beauty.dll" in plug_e
              and ("\\\\bin\\\\64bit" in plug_e or "\\\\bin\\\\64bit" in bin_e)
              and "Program Files" not in plug_e)
        if ok:
            add("PASS", "安装路径",
                "setup.cpp 的 plug/shad 由 %PROGRAMDATA%\\obs-studio\\plugins\\obs-beauty 派生，与装机脚本一致")
        else:
            add("FAIL", "安装路径",
                "setup.cpp 仍指向 OBS 32.x 不加载的旧目录（或不再从 ProgramData 根派生）")

    if not os.path.isfile(SETUP_EXE):
        add("WARN", "安装包",
            "缺 Setup.exe —— 发布前要跑 07_安装包/build_setup.ps1")
        return

    # (8) 版本串
    check_build_contains(ver, SETUP_EXE, "安装包版本串", missing_level="WARN")

    # (9) 内嵌 payload 与磁盘 payload 逐字节相同（N13 的真正闭环）
    res = read_setup_resources()
    if res is None:
        add("FAIL", "安装包载荷",
            "无法从 Setup.exe 读回 RCDATA 资源（文件损坏 / 非 Windows）")
        return
    bad = []
    for rid, name in SETUP_RCDATA.items():
        disk = setup_payload_disk(name)
        if not os.path.isfile(disk):
            bad.append("%s 磁盘副本缺失" % name)
        elif hashlib.md5(res[rid]).hexdigest().upper() != md5(disk):
            bad.append("%s 内嵌≠磁盘" % name)
    if bad:
        add("FAIL", "安装包载荷", "；".join(bad))
    else:
        add("PASS", "安装包载荷",
            "Setup.exe 内嵌 %d 项（dll + 运行时 + 人脸模型 + 许可 + 6 shader）"
            "与磁盘副本逐字节相同" % len(SETUP_RCDATA))


# ---------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true", help="WARN 也算失败")
    ap.add_argument("--no-installed", action="store_true",
                    help="跳过「装机 vs 构建」比对（没装 / 只想查仓库时用）")
    ap.add_argument("--json", metavar="PATH", help="把结果写成 JSON")
    args = ap.parse_args()

    if not os.path.isdir(SRC_DIR):
        print("找不到源码目录：%s" % SRC_DIR)
        return 2

    ver = check_version_source()

    if not os.path.isfile(BUILD_DLL):
        add("FAIL", "构建产物", "找不到 %s —— 先跑 02_构建/build_gpu.ps1"
            % BUILD_DLL)
        build_md5 = build_size = None
    else:
        build_size = os.path.getsize(BUILD_DLL)
        build_md5 = md5(BUILD_DLL)
        check_build_contains(ver, BUILD_DLL, "构建产物")

    if build_md5:
        check_same_dll(PKG_DLL, "安装包", build_md5, build_size)
        if args.no_installed:
            add("WARN", "装机", "已跳过（--no-installed）")
        else:
            check_same_dll(INSTALLED_DLL, "装机", build_md5, build_size,
                           missing_level="WARN")
    else:
        # 构建产物缺失时，至少把「安装包 vs 装机」这两份互相比一次，
        # 这样即使编不了也能看出这两者有没有脱节（N13 的典型形态）
        if os.path.isfile(PKG_DLL) and os.path.isfile(INSTALLED_DLL):
            a, b = md5(PKG_DLL), md5(INSTALLED_DLL)
            add("PASS" if a == b else "FAIL", "安装包 vs 装机",
                ("一致（%s）" % a) if a == b else ("脱节：安装包 %s ≠ 装机 %s" % (a, b)))

    check_shaders()
    check_switches()

    check_setup(ver)

    # 报告 -------------------------------------------------------------
    w = max(len(r["item"]) for r in RESULTS) if RESULTS else 4
    print("=" * 78)
    print("发布一致性离线闸门  |  仓库：%s" % ROOT)
    print("=" * 78)
    for r in RESULTS:
        print("[%-4s] %-*s  %s" % (r["level"], w, r["item"], r["msg"]))
    n_fail = sum(1 for r in RESULTS if r["level"] == "FAIL")
    n_warn = sum(1 for r in RESULTS if r["level"] == "WARN")
    n_pass = sum(1 for r in RESULTS if r["level"] == "PASS")
    print("-" * 78)
    print("PASS %d / WARN %d / FAIL %d" % (n_pass, n_warn, n_fail))
    if n_fail:
        print("结论：不发布 —— 先修 FAIL 项。")
    elif n_warn:
        print("结论：可以装/可以测；发布前用 --strict 复核 WARN。")
    else:
        print("结论：一致。")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fp:
            json.dump({"version": ver, "results": RESULTS}, fp,
                      ensure_ascii=False, indent=2)
        print("JSON 已写：%s" % args.json)

    if n_fail or (args.strict and n_warn):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
