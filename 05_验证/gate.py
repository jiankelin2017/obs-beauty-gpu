#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
S0 性能门禁（一条命令出可比指标）。

复用语料：05_验证/phase0_parse_log.py 的解析器（同一目录，直接 import）。
拼写「性能 WxH：下载 ...」与「seg 分项 ...」行的口径与此脚本完全一致。

用法:
    python gate.py [日志路径 ...] [--save 基线.json] [--cmp 基线.json]

  · 不传路径: 自动扫描 %AppData%\\obs-studio\\logs\\ 下最新的 .txt 日志。
  · --save:   把每个分辨率的「中位数往返 / 掩码% / 对齐%」固化成基线 JSON。
  · --cmp:    与固化基线比，逐分辨率报 Δ 往返中位数（S0 门槛 ≤ 0.15 ms）。

输出每分辨率一行（S0 门禁规定的六项 + chain + err）:
    分辨率 | 窗口数 | 往返中位(ms) | 掩码%中位 | 对齐%中位 | chain命中/询问 | 插件warning | 插件error
"""
import os
import re
import sys
import json

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import phase0_parse_log as P  # 复用既有解析器

PLUGIN_WARN = re.compile(r"\[obs-beauty\].*warning", re.IGNORECASE)
PLUGIN_ERR = re.compile(r"\[obs-beauty\].*error", re.IGNORECASE)


def med(vals):
    """中位数（None 自动剔除）。"""
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2.0


def count_plugin_logs(path):
    w = e = 0
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if PLUGIN_WARN.search(line):
                    w += 1
                if PLUGIN_ERR.search(line):
                    e += 1
    except Exception:
        pass
    return w, e


def gate_rows(perf, logpaths):
    """按分辨率聚合出门禁六项。"""
    by_res = {}
    for r in perf:
        by_res.setdefault(r["res"], []).append(r)
    rows = []
    for res in sorted(by_res):
        arr = by_res[res]
        n = len(arr)
        tot = med([r["tot"] for r in arr])
        cover = med([r["cover"] for r in arr])
        align = med([r["align"] for r in arr])
        # chain 计数：取该分辨率下 seg 行里最新的 chain= 命中/询问
        chain = None
        for r in arr:
            s = r.get("seg")
            if s and s.get("chain_hit") is not None:
                chain = (s["chain_hit"], s["chain_call"])
        w = e = 0
        for lp in logpaths:
            cw, ce = count_plugin_logs(lp)
            w += cw
            e += ce
        rows.append((res, n, tot, cover, align, chain, w, e))
    return rows


def fmt(v, spec=".2f"):
    return ("%" + spec) % v if v is not None else "-"


def main():
    argv = sys.argv[1:]
    save = None
    cmpf = None
    if "--save" in argv:
        i = argv.index("--save")
        save = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if "--cmp" in argv:
        i = argv.index("--cmp")
        cmpf = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    paths = [a for a in argv if not a.startswith("--")]

    if not paths:
        d = P.scan_default_log()
        if not d:
            print("未找到 OBS 日志，请手动传入路径。", file=sys.stderr)
            return 1
        print("自动选用日志: %s" % d)
        paths = [d]
    paths = P.expand_paths(paths)

    perf, _segs = [], []
    for p in paths:
        a, b = P.parse_file(p)
        perf.extend(a)
        _segs.extend(b)

    if not perf:
        print("未匹配到任何 '性能 WxH：下载 ...' 行。请确认:")
        print("  1) 启动 OBS 前设置了 BEAUTY_PROFILE=1（现有 720p/4K 启动器已默认开启）")
        print("  2) 滤镜参数非全 0（全 0 会 skip_sdk 直通，引擎项=0 但仍会打性能行）")
        print("  3) 至少跑满 180 帧（约 3 秒 @60fps = 1 个窗口；S0 取 3 窗口）")
        return 2

    rows = gate_rows(perf, paths)
    print("\n【S0 门禁】窗口数 / 往返中位 / 掩码% / 对齐% / chain命中·询问 / 插件warning / 插件error")
    hdr = "%-10s %5s %10s %8s %8s %14s %10s %9s" % (
        "分辨率", "窗口", "往返中位", "掩码%", "对齐%", "chain", "warn", "err")
    print(hdr)
    print("-" * len(hdr))
    for res, n, tot, cov, al, chain, w, e in rows:
        ch = ("%d/%d" % chain) if chain else "-"
        print("%-10s %5d %10s %8s %8s %14s %10d %9d" % (
            res, n, fmt(tot), fmt(cov), fmt(al), ch, w, e))

    if save:
        base = {res: {"n": n, "tot": tot, "cover": cov, "align": al}
                for res, n, tot, cov, al, chain, w, e in rows}
        with open(save, "w", encoding="utf-8") as f:
            json.dump(base, f, ensure_ascii=False, indent=2)
        print("\n基线已固化: %s" % save)

    if cmpf:
        if not os.path.exists(cmpf):
            print("基线文件不存在: %s" % cmpf, file=sys.stderr)
            return 3
        with open(cmpf, encoding="utf-8") as f:
            base = json.load(f)
        print("\n【可重复性比对 vs %s】 门槛 Δ 往返中位 ≤ 0.15 ms" % cmpf)
        ok = True
        for res, n, tot, cov, al, chain, w, e in rows:
            if res not in base:
                print("  %s: 基线无此分辨率，跳过" % res)
                continue
            dt = abs((tot or 0) - (base[res]["tot"] or 0))
            flag = "PASS" if dt <= 0.15 else "FAIL"
            if flag == "FAIL":
                ok = False
            print("  %-10s 当前 %.2f  基线 %.2f  Δ %.2f  -> %s" % (
                res, tot or 0, base[res]["tot"] or 0, dt, flag))
        print("\nREPEATABILITY: " + ("ALL PASS" if ok else "FAILED（先修测量，不进 S1）"))
        return 0 if ok else 4

    return 0


if __name__ == "__main__":
    sys.exit(main())
