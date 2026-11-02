#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""磨皮「趟数 → 耗时」斜率探针（S5 决策用，2026-11-03）。

回答一个问题：4K 上磨皮 `seg 磨皮` 的 1.2 ms 里，多少是**固定成本**
（CPU 提交 draw + 每次 RT begin/end 的驱动开销），多少是**随像素走**的
（半分辨率纹理的读写带宽 + 采样）。

为什么要问：S5（通道打包）只减 draw/RT 切换次数，不减总像素工作量。
如果耗时主要是像素项，S5 的收益就有硬上限 —— 判据得据此重定，
不能照抄「往返再降 30%」。

做法：`BEAUTY_SMOOTH_PASSES` 把趟数锁成 1/2/3（draw = 5 + 6·趟），
每档跑 `--rounds` 轮，取 `seg 磨皮` 窗口中位数；再对「draw 数 → 毫秒」
做最小二乘，斜率 = 每次 draw（含它的 RT 切换）值多少钱。

用法:
    py -3 05_验证/slope_smooth.py                 # 720p + 4K，趟数 1/2/3，各 2 轮
    py -3 05_验证/slope_smooth.py --rounds 3 --passes 1 3
    py -3 05_验证/slope_smooth.py --only 4K

注意：这是**诊断**，不是产品配置 —— 趟数少 = 平滑不足，画面会变。
"""
import argparse
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import s0_lock as L
import soak_s2 as S
import phase0_parse_log as P

SECONDS = 25.0


def one(log, res_key):
    """从日志里取该分辨率的 (磨皮中位, 引擎中位, 往返中位, 窗口数)。

    ⚠ `phase0_parse_log` 的 seg 是**两层**：r["seg"] 是 seg 行记录本身，
    真正的分项在 r["seg"]["seg"]["磨皮"]。2026-11-03 第一版探针就错取成
    r["seg"]["磨皮"] → 全部「解析不到 seg 磨皮」，白跑了 12 轮。
    """
    perf, _ = P.parse_file(log)
    rows = [r for r in perf if r["res"] == res_key]
    if not rows:
        return None
    sm, eng, tt = [], [], []
    for r in rows:
        s = (r.get("seg") or {}).get("seg") or {}
        if s.get("磨皮") is not None:
            sm.append(s["磨皮"])
        if r.get("pr") is not None:
            eng.append(r["pr"])
        if r.get("tot") is not None:
            tt.append(r["tot"])
    if not sm:
        return None
    return (st.median(sm), st.median(eng) if eng else None,
            st.median(tt) if tt else None, len(rows))


def fit(xs, ys):
    """最小二乘过不过原点都给的简单线性拟合：y = a + b·x。"""
    n = len(xs)
    if n < 2:
        return None, None
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    if den <= 1e-9:
        return None, None
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den
    return my - b * mx, b


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--passes", nargs="+", type=int, default=[1, 2, 3])
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--only", choices=("720p", "4K"))
    ap.add_argument("--seconds", type=float, default=SECONDS)
    args = ap.parse_args(argv)

    out = {}          # res -> passes -> [磨皮]
    eng = {}
    tt = {}
    S.keep_awake(True)
    try:
        for r in range(1, args.rounds + 1):
            for name, coll, canvas in L.TARGETS:
                if args.only and args.only != name:
                    continue
                key = "%sx%s" % (canvas[0], canvas[1])
                for np_ in args.passes:
                    envx = {"BEAUTY_PROFILE": "1", "BEAUTY_SMOOTH_PASSES": str(np_)}
                    print("\n---- 第 %d 轮 / %s / 趟数=%d ----" % (r, name, np_))
                    log, note = L.one_run(coll, canvas, args.seconds, envx)
                    if not log:
                        print("  !! %s" % note)
                        continue
                    m = one(log, key)
                    if not m:
                        print("  !! 解析不到 seg 磨皮（%s）" % log)
                        continue
                    sm, pr, tot, n = m
                    print("  磨皮 %.2f  引擎 %s  往返 %s  窗口 %d"
                          % (sm, pr, tot, n))
                    out.setdefault(key, {}).setdefault(np_, []).append(sm)
                    eng.setdefault(key, {}).setdefault(np_, []).append(pr)
                    tt.setdefault(key, {}).setdefault(np_, []).append(tot)
    finally:
        S.keep_awake(False)

    print("\n=========== 趟数 → 耗时 ===========")
    for key, per in out.items():
        print("\n%s" % key)
        print("  趟数   draw  磨皮(中位)   引擎     往返    样本")
        xs, ys = [], []
        for np_ in sorted(per):
            v = per[np_]
            draw = 5 + 6 * np_
            sm = st.median(v)
            pr = st.median([x for x in (eng[key][np_] or []) if x is not None] or [0])
            tm = st.median([x for x in (tt[key][np_] or []) if x is not None] or [0])
            print("  %-5d  %-5d  %8.2f  %7.2f  %7.2f  %d"
                  % (np_, draw, sm, pr, tm, len(v)))
            xs.append(draw)
            ys.append(sm)
        a, b = fit(xs, ys)
        if a is not None:
            dmax = max(xs)
            cur = a + b * dmax
            print("  拟合：磨皮 = %.2f + %.4f × draw  →  每 draw（含 RT 切换）≈ %.1f µs"
                  % (a, b, b * 1000.0))
            print("        当前 %d draw：提交/切换 ≈ %.2f ms（占 %.0f%%），"
                  "随像素走的 GPU 项 ≈ %.2f ms"
                  % (dmax, b * dmax, 100.0 * b * dmax / max(cur, 1e-9), a))


if __name__ == "__main__":
    main()
