#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验 obs-beauty-ui.dll 的每一个导入符号，是否都存在于 OBS 自带的 DLL 里。

用途：确认「用 Qt 6.8.3 开发包编译出来的插件」能真的被 OBS 32.1.1 加载。
原理：解析新 DLL 的导入表 -> 逐个目标 DLL 解析导出表 -> 求差集。
      差集为空 = 动态链接阶段不会因缺符号失败。

用法：python verify-ui-imports.py [待检查的dll]

3.2.4：本脚本随面板源码一起引入 GPU 仓库。默认目标改为本目录的
obs-beauty-ui.dll（build_ui.ps1 的产物），因此不带参数即可直接跑。
它是「面板能不能被这台机器的 OBS 加载」的唯一离线判据 —— 面板是 Qt 插件，
编译用的 Qt 与 OBS 自带的 Qt 若 ABI 不符，症状不是报错而是菜单压根不出现，
和这次要修的 bug 长得一模一样，靠肉眼看日志区分不开。
"""

import os
import re
import subprocess
import sys

DUMPBIN = (r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC"
           r"\14.51.36231\bin\Hostx64\x64\dumpbin.exe")
OBS_BIN = r"C:\Program Files\obs-studio\bin\64bit"
HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "obs-beauty-ui.dll")


def dump(*args):
    out = subprocess.run([DUMPBIN] + list(args), capture_output=True, text=True, errors="replace")
    return out.stdout


def parse_imports(path):
    """-> {dll_name_lower: set(imported_symbol)}"""
    text = dump("/imports", path)
    cur = None
    table = {}
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"^([A-Za-z0-9_.\-]+\.dll)$", s)
        if m:
            cur = m.group(1).lower()
            table.setdefault(cur, set())
            continue
        m = re.match(r"^[0-9A-Fa-f]+\s+(\S+)$", s)
        if m and cur:
            sym = m.group(1)
            if sym.lower() in ("import", "name"):
                continue
            table[cur].add(sym)
    return table


def parse_exports(path):
    text = dump("/exports", path)
    names = set()
    for line in text.splitlines():
        m = re.match(r"\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)", line)
        if m:
            names.add(m.group(1))
    return names


def main():
    if not os.path.isfile(TARGET):
        print("找不到 %s" % TARGET)
        return 1
    print("待检文件：%s" % TARGET)
    print("大小：    %d 字节" % os.path.getsize(TARGET))
    print()

    imports = parse_imports(TARGET)
    if not imports:
        print("导入表解析失败")
        return 1

    total_missing = 0
    for dll in sorted(imports):
        if dll.startswith("kernel32") or dll.startswith("api-ms-") or dll.startswith("vcruntime") \
                or dll.startswith("msvcp") or dll.startswith("ucrtbase"):
            continue
        need = imports[dll]
        p = os.path.join(OBS_BIN, dll)
        if not os.path.isfile(p):
            print("%-24s 在 OBS 目录下找不到该 DLL！" % dll)
            total_missing += len(need)
            continue
        have = parse_exports(p)
        missing = sorted(n for n in need if n not in have)
        total_missing += len(missing)
        flag = "OK" if not missing else "缺失 %d" % len(missing)
        print("%-24s 需要 %3d 个符号  %s" % (dll, len(need), flag))
        for n in missing[:8]:
            print("      缺: %s" % n)

    print()
    if total_missing == 0:
        print("结论：全部导入符号都能在 OBS 自带 DLL 中解析 -> 插件可正常加载")
        return 0
    print("结论：有 %d 个符号缺失 -> 需要改用与 OBS 匹配的 Qt 版本重新编译" % total_missing)
    return 1


if __name__ == "__main__":
    sys.exit(main())
