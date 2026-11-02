#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
S0 性能门禁（一条命令出可比指标）。

复用语料：05_验证/phase0_parse_log.py 的解析器（同一目录，直接 import）。
拼写「性能 WxH：下载 ...」与「seg 分项 ...」行的口径与此脚本完全一致。

用法:
    python gate.py [日志路径 ...] [--save 基线.json] [--cmp 基线.json]
                   [--tol-transfer 0.15] [--tol-engine-pct 5] [--skip-transfer]

  · 不传路径: 自动扫描 %AppData%\\obs-studio\\logs\\ 下最新的 .txt 日志。
  · --save:   把每个分辨率的「下载 / 上传 / 引擎 / 往返 / 掩码% / 对齐%」固化成基线 JSON。
  · --cmp:    与固化基线比，**拆项**判定（口径 A，2026-11-02 拍板）：

        搬运项 = 下载 + 上传   Δ ≤ --tol-transfer（默认 0.15 ms）
        引擎项                 Δ ≤ max(0.10 ms, --tol-engine-pct% × 基线引擎)（默认 5%）
        往返                   **不判定**，只作参考打印

为什么要拆项（PROGRESS 9.1.1 的实测结论）：
  往返 = 下载 + 引擎 + 上传。其中「下载」是 gs_stagesurface_map() 的**排队等待**，
  等的是上一帧排进 GPU 队列的全部工作，同一轮内相邻窗口的极差就有 720p 1.4 ms、
  4K 1.3 ms —— 拿它当 0.15 ms 门槛的判据时，门槛比噪声底还小，跨轮必然 FAIL。
  拆开之后，搬运项（纯 memcpy/DMA 量级）与引擎项（CPU 算力量级）各断言一件事。

注意：跨会话/跨日的绝对值不可横比（9.10 脚注）。要证明「某改动更快」，用
ab_pair.py 的交替配对 A/B（口径 C），不要拿两个会话的中位数相减。

输出每分辨率一行:
    分辨率 | 窗口数 | 往返中位 | 搬运(l+u) | 引擎 | 掩码%中位 | 对齐%中位 | chain | warn | err
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

# 口径 A 的默认门槛
TOL_TRANSFER = 0.15     # ms（下载 + 上传）
TOL_ENGINE_PCT = 5.0    # %；720p 及以下
TOL_ENGINE_FLOOR = 0.10  # ms；引擎项本身只有 0.3~5 ms，5% 会小于噪声，故设地板
# 4K 单独定容差（2026-11-02 实测）。理由不是"大分辨率就该宽容"，而是实测发现
# 4K 的「引擎」项里含**磨皮钩子的 map 等待**：同一二进制四次采集的 `seg 磨皮`
# 在 1.44~2.31 ms 之间摆（掩码段 2.58~2.82 才是纯 CPU），引擎项因此 4.03~5.22。
# 5% 的容差在 4K 上等于要求测量精确到 0.25 ms，而实际底噪就有 0.85 ms。
# 取 25%：只抓大退化（把 4K 引擎打高 1 ms 以上），0.3 ms 级差异交给 ab_pair.py。
TOL_ENGINE_PCT_4K = 25.0


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
    """按分辨率聚合出门禁指标（每项都是中位数）。"""
    by_res = {}
    for r in perf:
        by_res.setdefault(r["res"], []).append(r)
    rows = []
    for res in sorted(by_res):
        arr = by_res[res]
        dl = med([r["dl"] for r in arr])
        up = med([r["up"] for r in arr])
        tr = None if (dl is None or up is None) else dl + up
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
        rows.append({
            "res": res, "n": len(arr),
            "tot": med([r["tot"] for r in arr]),
            "dl": dl, "up": up, "tr": tr,
            "pr": med([r["pr"] for r in arr]),
            "cover": med([r["cover"] for r in arr]),
            "align": med([r["align"] for r in arr]),
            "chain": chain, "warn": w, "err": e,
        })
    return rows


def fmt(v, spec=".2f"):
    return ("%" + spec) % v if v is not None else "-"


def engine_tol(base_pr, res=None, pct=None):
    """引擎项容差：pct% 与 0.10 ms 取大者；4K 默认 25%（见 TOL_ENGINE_PCT_4K）。"""
    if base_pr is None:
        return TOL_ENGINE_FLOOR
    if pct is None:
        pct = TOL_ENGINE_PCT_4K if (res or "").startswith("3840") else TOL_ENGINE_PCT
    return max(TOL_ENGINE_FLOOR, abs(base_pr) * pct / 100.0)


def main():
    argv = sys.argv[1:]
    save = None
    cmpf = None
    tol_tr = TOL_TRANSFER
    tol_pct = None          # None = 按分辨率走默认（720p 5% / 4K 25%）
    # 实测结论（2026-11-02 20:2x，同一二进制三个会话）：
    #   搬运项 720p 1.36 / 0.69 / 1.08（跨会话 Δ0.67，单会话内 0.96→2.02 也有 1.06）
    #   引擎项 720p 0.53 / 0.53 / 0.53、4K 3.85 / 3.97 / 3.92（Δ≤0.12，3.1%）
    # 即：搬运项本身不可跨会话重复，它只能在 C（交替配对 A/B）里判定。
    # 因此默认保留 A 的字面判定，同时给 --skip-transfer 切到「A+C 合成口径」。
    skip_tr = "--skip-transfer" in argv
    if skip_tr:
        argv = [a for a in argv if a != "--skip-transfer"]
    for key in ("--save", "--cmp", "--tol-transfer", "--tol-engine-pct"):
        if key in argv:
            i = argv.index(key)
            if i + 1 >= len(argv):
                print("%s 需要参数值" % key, file=sys.stderr)
                return 1
            val = argv[i + 1]
            argv = argv[:i] + argv[i + 2:]
            if key == "--save":
                save = val
            elif key == "--cmp":
                cmpf = val
            elif key == "--tol-transfer":
                tol_tr = float(val)
            else:
                tol_pct = float(val)
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
    print("\n【S0 门禁】口径 A（拆项）：搬运 Δ≤%.2f ms、引擎 Δ≤max(%.2f, 5%%/4K 25%%)"
          % (tol_tr, TOL_ENGINE_FLOOR))
    hdr = "%-10s %5s %9s %10s %8s %8s %8s %12s %7s %6s" % (
        "分辨率", "窗口", "往返中位", "搬运l+u", "引擎", "掩码%", "对齐%", "chain", "warn", "err")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        ch = ("%d/%d" % r["chain"]) if r["chain"] else "-"
        print("%-10s %5d %9s %10s %8s %8s %8s %12s %7d %6d" % (
            r["res"], r["n"], fmt(r["tot"]), fmt(r["tr"]), fmt(r["pr"]),
            fmt(r["cover"]), fmt(r["align"]), ch, r["warn"], r["err"]))

    if save:
        base = {r["res"]: {"n": r["n"], "tot": r["tot"], "tr": r["tr"],
                           "dl": r["dl"], "up": r["up"], "pr": r["pr"],
                           "cover": r["cover"], "align": r["align"],
                           "gate": "A+C", "tol_transfer": tol_tr,
                           "tol_engine_pct": tol_pct}
                for r in rows}
        with open(save, "w", encoding="utf-8") as f:
            json.dump(base, f, ensure_ascii=False, indent=2)
        print("\n基线已固化: %s" % save)

    if cmpf:
        if not os.path.exists(cmpf):
            print("基线文件不存在: %s" % cmpf, file=sys.stderr)
            return 3
        with open(cmpf, encoding="utf-8") as f:
            base = json.load(f)
        print("\n【可重复性比对 vs %s】（口径 A：引擎 720p %g%% / 4K %g%%%s）" % (
            cmpf, TOL_ENGINE_PCT if tol_pct is None else tol_pct,
            TOL_ENGINE_PCT_4K,
            "，搬运项豁免 → 由 ab_pair.py 判定" if skip_tr else ""))
        print("  %-9s %9s %9s %8s %9s %8s %8s  %s" % (
            "分辨率", "搬运现值", "搬运基线", "Δ搬运", "引擎现值", "Δ引擎", "引擎容差", "判定"))
        ok = True
        for r in rows:
            b = base.get(r["res"])
            if not b:
                print("  %-9s 基线无此分辨率，跳过" % r["res"])
                continue
            btr = b.get("tr")
            bpr = b.get("pr")
            if btr is None or bpr is None:
                # 旧格式基线（只有 tot）：退化为往返比对，明确提示不参与判定
                dt = abs((r["tot"] or 0) - (b.get("tot") or 0))
                print("  %-9s 旧格式基线（无拆项），往返 Δ %.2f 仅供参考 -> SKIP"
                      % (r["res"], dt))
                continue
            dtr = abs((r["tr"] or 0) - btr)
            dpr = abs((r["pr"] or 0) - bpr)
            etol = engine_tol(bpr, r["res"], tol_pct)
            f_tr = skip_tr or dtr <= tol_tr
            f_pr = dpr <= etol
            # 健康项：有 error 即 FAIL；掩码占比偏移 > 1 个百分点视为画面条件变了
            f_hd = (r["err"] == 0)
            dcv = abs((r["cover"] or 0) - (b.get("cover") or 0))
            f_cv = dcv <= 1.0
            verdict = "PASS" if (f_tr and f_pr and f_hd and f_cv) else "FAIL"
            if verdict == "FAIL":
                ok = False
            tags = ""
            if not f_tr:
                tags += "（搬运超差）"
            if not f_pr:
                tags += "（引擎超差）"
            if not f_hd:
                tags += "（插件 error %d 条）" % r["err"]
            if not f_cv:
                tags += "（掩码占比偏 %.2f pp）" % dcv
            print("  %-9s %9s %9s %8s %9s %8s %8s  %s%s" % (
                r["res"], fmt(r["tr"]), fmt(btr), fmt(dtr),
                fmt(r["pr"]), fmt(dpr), fmt(etol), verdict, tags))
        print("\nREPEATABILITY(A): " + ("ALL PASS" if ok else "FAILED（先修测量，不进下一阶段）"))
        print("  往返不参与判定 —— 它含 map 的排队等待，跨轮抖动 ±0.5~1 ms（见 9.1.1）。")
        if skip_tr:
            print("  搬运项已豁免（--skip-transfer）：它的跨会话差可达 0.67 ms，"
                  "只能在 ab_pair.py 的配对 A/B 里判定。")
        print("  要证明『某改动更快』请用 ab_pair.py 的交替配对 A/B（口径 C）。")
        return 0 if ok else 4

    return 0


if __name__ == "__main__":
    sys.exit(main())
