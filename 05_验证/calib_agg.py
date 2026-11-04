# -*- coding: utf-8 -*-
"""标定「16 个块怎么合并成一个数」（2026-11-04，2.12.0 前置 / P1-1c）。

为什么需要它
------------
2.11.0 的判定是：16×16 格取格内最大 → 4×4 格为一块取均值 → **16 块取最大**。
真实素材（真人说话，`cam_real`）上判动率 42.8%、`nopix` 只剩 33.5%、往返只省 18%，
而根因是「取最大」对局部微动极敏感：脸上任何一小块动，整帧就判动 ——
可这时人脸位置根本没变，检测结果完全没过期（`PROGRESS §9.32.6`）。

本脚本用 `motion_model.py` 里那条唯一实现，一次解码算四种块聚合，
回答「换成哪一种，能把『说话』判成静止、又把『真运动』与『人脸进出』留在动里」。

    口径 max     —— 16 块取最大（2.11.x 现状，引擎默认）
    口径 top4    —— 16 块里最大的 4 块取均值
    口径 rms     —— 16 块的均方根 sqrt(Σm²/16)（2.12.0 候选）
    口径 median  —— 16 块的中位数（对照组，预期失效）

用法
----
    py -3 calib_agg.py                       # 默认四组素材
    py -3 calib_agg.py --files a.mp4 b.avi
    py -3 calib_agg.py --thr 6 --interp linear
"""

from __future__ import annotations

import argparse
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from motion_model import DEFAULT_THR, motion_small  # noqa: E402

ASSETS = os.path.join(HERE, "assets")
AGGS = ("max", "top4", "rms", "median")

# 默认素材：每一行是一类「希望判动率高/低」的样本
DEFAULT_SETS = [
    ("真实-说话（希望低）", "cam_real_1080x1920.mp4"),
    ("合成-真运动（希望高）", "motion_720p.mp4"),
    ("合成-静止（希望低）", "flash_k0_720p.avi"),
    ("事件-脸消失1帧（希望命中）", "drop_k1_720p.avi"),
    ("事件-脸消失2帧（希望命中）", "drop_k2_720p.avi"),
    ("事件-脸闪1帧（希望命中）", "flash_k1_720p.avi"),
]


def metric_all(prev, cur):
    """两张 64×64 亮度图 → 四种块聚合的判定值（与引擎同一条数学）。"""
    grid, blk = 16, 4
    side = cur.shape[0]
    step = side // grid
    d = np.abs(cur.astype(np.int32) - prev.astype(np.int32)).astype(np.float64)
    d = d[:step * grid, :step * grid]
    cells = d.reshape(grid, step, grid, step).max(axis=(1, 3))
    blocks = cells.reshape(grid // blk, blk, grid // blk, blk).mean(axis=(1, 3))
    flat = blocks.ravel()
    return {
        "max": float(flat.max()),
        "top4": float(np.sort(flat)[-4:].mean()),
        "rms": float(np.sqrt(np.mean(flat * flat))),
        "median": float(np.median(flat)),
    }


def scan(path, interp="linear", limit=None):
    """一次解码 → 每帧四口径的判定值序列。"""
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        return None
    ser = {k: [] for k in AGGS}
    prev, n = None, 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        n += 1
        if limit and n > limit:
            break
        cur = motion_small(frame, interp)
        if prev is not None:
            for k, v in metric_all(prev, cur).items():
                ser[k].append(v)
        prev = cur
    cap.release()
    if not ser["max"]:
        return None
    return n, ser


def stats(v, thr):
    a = np.asarray(v, dtype=np.float64)
    if not len(a):
        return None
    return {
        "hot": float((a > thr).mean()) * 100.0,
        "p50": float(np.percentile(a, 50)),
        "p90": float(np.percentile(a, 90)),
        "mx": float(a.max()),
    }


SWEEP = (5, 6, 7, 8, 10, 12)


def sweep_line(v, agg):
    """同一序列在若干阈值下的判动率 —— 用来判断「换口径要不要跟着换阈值」。"""
    a = np.asarray(v, dtype=np.float64)
    return "  ".join("%2d:%5.1f%%" % (t, float((a > t).mean()) * 100.0)
                     for t in SWEEP)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="*", help="自定义素材（覆盖默认表，标签用文件名）")
    ap.add_argument("--thr", type=int, default=DEFAULT_THR)
    ap.add_argument("--interp", default="linear", choices=("linear", "area"))
    ap.add_argument("--limit", type=int, default=0, help="每段最多算多少帧（0=全量）")
    ap.add_argument("--sweep", action="store_true",
                    help="追加打印 max/rms 在各阈值下的判动率（重标用）")
    args = ap.parse_args(argv)

    sets = ([(os.path.basename(f), f) for f in args.files] if args.files
            else [(lab, os.path.join(ASSETS, fn)) for lab, fn in DEFAULT_SETS])

    print("阈值 %d，第二段缩放 %s；数值 = 判动%% / p50 / p90 / max"
          % (args.thr, args.interp))
    print()
    head = "%-26s %6s |" % ("素材", "帧")
    for k in AGGS:
        head += " %-26s |" % (("[%s]" % k) if k != "max" else "[max] 现状")
    print(head)
    print("-" * (36 + 29 * len(AGGS)))

    for lab, path in sets:
        if not os.path.exists(path):
            print("%-26s %6s | 缺素材：%s" % (lab, "-", path))
            continue
        r = scan(path, args.interp, args.limit or None)
        if not r:
            print("%-26s %6s | 打不开" % (lab, "-"))
            continue
        n, ser = r
        line = "%-26s %6d |" % (lab, n)
        for k in AGGS:
            s = stats(ser[k], args.thr)
            line += " %5.1f%% %5.1f %5.1f %5.1f |" % (
                s["hot"], s["p50"], s["p90"], s["mx"])
        print(line)
        if args.sweep:
            print("%-26s %6s | max  " % ("", "") + sweep_line(ser["max"], "max"))
            print("%-26s %6s | rms  " % ("", "") + sweep_line(ser["rms"], "rms"))

    print()
    print("读法：")
    print("  · 真实-说话 一列越低越好（现状 max 约 52%%）；")
    print("  · 合成-真运动 一列必须仍是 100%%（p50 离阈值越远越稳）；")
    print("  · 事件两行看的是「有没有命中」，不是比率高低 —— 每次事件只有 1–2 帧")
    print("    过渡，判动率天然只有百分之几，关键是**别掉到 0**。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
