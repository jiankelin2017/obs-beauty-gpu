#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分段对齐 + 差异统计，并对「同臂两份录制」给出对齐残差底。

为什么需要这个脚本
------------------
OBS 的渲染帧与素材帧不是严格一一对应：两臂的时间基相差约 **半帧**，最优整数
偏移在不同时段于 +2 / +3 之间交替（实测 12 段里 7 段 +2、5 段 +3）。整数帧
对齐因此存在最多 0.5 帧的残差，而说话时 1 帧嘴型就差很多 —— 这个残差会伪装
成「美颜差异」。

所以必须先量「对齐残差底」：拿**同一臂的两份录制**（内容逐字节相同，只是
起录时刻不同），走完全相同的对齐流程，它给出的差异就是这套方法的噪声底。
真实两臂的差异必须**显著高于**这个底，否则什么都不能说。

用法
----
    py -3 ab_stat.py --a off_combo.mp4 --b off_combo_n1.mp4      # 噪声底
    py -3 ab_stat.py --a max_combo.mp4 --b rms4_combo.mp4        # 待测
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out", "ab")

CROP = (150, 720, 60, 780)     # 脸区（1080×1920）


def read_gray(path):
    cap = cv2.VideoCapture(path)
    out = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        out.append(cv2.cvtColor(f, cv2.COLOR_BGR2GRAY))
    cap.release()
    return out


def best_offset(A, B, s, e, span=6):
    """段内最优整数偏移：A[i+off] 对 B[i]。用全画面均值差。"""
    sc = {}
    for off in range(span + 1):
        acc = []
        for i in range(s, e):
            j = i + off
            if 0 <= j < len(A):
                acc.append(float(np.abs(A[j].astype(np.int16) - B[i].astype(np.int16)).mean()))
        sc[off] = float(np.mean(acc)) if acc else 9e9
    b = min(sc, key=sc.get)
    second = sorted(sc.values())[1]
    return b, sc[b], second


def run(a_file, b_file, seg=30, skip_mark=True):
    A = read_gray(os.path.join(OUT, a_file))
    B = read_gray(os.path.join(OUT, b_file))
    n = min(len(A), len(B))
    x0, x1, y0, y1 = CROP
    area = float((x1 - x0) * (y1 - y0))

    rows = []
    for s in range(0, n, seg):
        e = min(s + seg, n)
        off, err, err2 = best_offset(A, B, s, e)
        for i in range(s, e):
            j = i + off
            if not (0 <= j < len(A)):
                continue
            d = np.abs(A[j][y0:y1, x0:x1].astype(np.int16)
                       - B[i][y0:y1, x0:x1].astype(np.int16))
            rows.append({"i": i, "off": off, "seg": s,
                         "a4": 100.0 * float((d >= 4).sum()) / area,
                         "a8": 100.0 * float((d >= 8).sum()) / area,
                         "mx": int(d.max()), "mean": float(d.mean()),
                         "geo": err})
    return rows, A, B


def summarize(tag, rows):
    a4 = np.array([r["a4"] for r in rows])
    a8 = np.array([r["a8"] for r in rows])
    mx = np.array([r["mx"] for r in rows])
    mn = np.array([r["mean"] for r in rows])
    offs = np.array([r["off"] for r in rows])
    uniq, cnt = np.unique(offs, return_counts=True)
    print("== %s（%d 帧）" % (tag, len(rows)))
    print("   偏移分布 %s" % dict(zip(uniq.tolist(), cnt.tolist())))
    print("   ≥4 灰阶面积  p50 %6.2f%%  p90 %6.2f%%  max %6.2f%%"
          % (np.median(a4), np.percentile(a4, 90), a4.max()))
    print("   ≥8 灰阶面积  p50 %6.2f%%  p90 %6.2f%%  max %6.2f%%"
          % (np.median(a8), np.percentile(a8, 90), a8.max()))
    print("   单帧最大差    p50 %5d     p90 %5d     max %5d"
          % (np.median(mx), np.percentile(mx, 90), mx.max()))
    print("   脸区均值差    p50 %6.2f   p90 %6.2f" % (np.median(mn), np.percentile(mn, 90)))
    return {"tag": tag, "frames": len(rows),
            "a4_p50": float(np.median(a4)), "a4_p90": float(np.percentile(a4, 90)),
            "a4_max": float(a4.max()),
            "mx_p50": int(np.median(mx)), "mx_max": int(mx.max()),
            "mean_p50": float(np.median(mn))}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo.mp4")
    ap.add_argument("--b", default="rms4_combo.mp4")
    ap.add_argument("--seg", type=int, default=30)
    ap.add_argument("--tag", default="")
    ap.add_argument("--json", default="")
    a = ap.parse_args(argv)

    rows, _, _ = run(a.a, a.b, a.seg)
    tag = a.tag or ("%s vs %s" % (a.a, a.b))
    s = summarize(tag, rows)
    # 分段偏移表，用来看漂移是不是单向的
    segs = {}
    for r in rows:
        segs.setdefault(r["seg"], r["off"])
    print("   分段偏移 %s" % [segs[k] for k in sorted(segs)])
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({"summary": s, "rows": rows}, fh, ensure_ascii=False)
        print("   写入 %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
