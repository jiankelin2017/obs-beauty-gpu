#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按帧号精确配对 + 差异统计。

为什么可以这样配对
------------------
素材每帧右下角烧了 8 位帧号条码（`make_ab_clip.stamp_code`），录制里 100% 可读。
于是「A 的第几帧」和「B 的第几帧」不再靠时间猜，而是直接按**内容帧号**配对：
同一个内容帧号在两臂里各取第一次出现的那一帧。

这一下解决了三个坑
------------------
1. 两臂起点相差 2.5 帧（StartRecord 延迟不同）→ 不再需要整数偏移；
2. OBS 渲染会重复/跳过素材帧 → 帧号直接暴露（实测 rms4 臂 78 次重复、72 次跳帧，
   max 臂几乎 1:1），过去这会被当成"美颜差异"；
3. 对齐本身可验证 → 配对后残差应当落在 0 附近。

用法
----
    py -3 ab_match.py --a max_combo_v2.mp4 --b rms4_combo_v2.mp4
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
sys.path.insert(0, HERE)
from make_ab_clip import read_code  # noqa: E402

CROP = (150, 720, 60, 780)     # 脸区


def scan(path):
    """返回 (帧号列表, 灰度图列表)。"""
    cap = cv2.VideoCapture(path)
    codes, grays = [], []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        codes.append(read_code(f))
        grays.append(cv2.cvtColor(f, cv2.COLOR_BGR2GRAY))
    cap.release()
    return codes, grays


def first_map(codes):
    """帧号 → 第一次出现的索引。"""
    m = {}
    for i, c in enumerate(codes):
        if c >= 0 and c not in m:
            m[c] = i
    return m


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo_v2.mp4")
    ap.add_argument("--b", default="rms4_combo_v2.mp4")
    ap.add_argument("--json", default="")
    a = ap.parse_args(argv)

    ca, ga = scan(os.path.join(OUT, a.a))
    cb, gb = scan(os.path.join(OUT, a.b))
    ma, mb = first_map(ca), first_map(cb)
    common = sorted(set(ma) & set(mb))
    print("A %d 帧（有效帧号 %d 个）  B %d 帧（%d 个）  共同帧号 %d 个"
          % (len(ca), len(ma), len(cb), len(mb), len(common)))
    if not common:
        sys.exit("没有共同帧号，无法配对")

    # 配对质量：同一个帧号在两臂里是否真的像同一次渲染（残差应当很小）
    x0, x1, y0, y1 = CROP
    area = float((x1 - x0) * (y1 - y0))
    rows = []
    for n in common:
        ia, ib = ma[n], mb[n]
        da = ga[ia].astype(np.int16)
        db = gb[ib].astype(np.int16)
        full = np.abs(da - db)
        sub = full[y0:y1, x0:x1]
        rows.append({"n": n, "a": ia, "b": ib,
                     "a4": 100.0 * float((sub >= 4).sum()) / area,
                     "a8": 100.0 * float((sub >= 8).sum()) / area,
                     "mx": int(sub.max()), "mean": float(sub.mean()),
                     "off_full": float(full.mean())})

    a4 = np.array([r["a4"] for r in rows])
    a8 = np.array([r["a8"] for r in rows])
    mx = np.array([r["mx"] for r in rows])
    mn = np.array([r["mean"] for r in rows])
    of = np.array([r["off_full"] for r in rows])

    print("\n== %s vs %s（按帧号配对，%d 对）" % (a.a, a.b, len(rows)))
    print("   全画面平均差  p50 %.3f  p90 %.3f（含条码区，可当对齐自检）"
          % (np.median(of), np.percentile(of, 90)))
    print("   脸区 ≥4 灰阶面积  p50 %6.2f%%  p90 %6.2f%%  max %6.2f%%"
          % (np.median(a4), np.percentile(a4, 90), a4.max()))
    print("   脸区 ≥8 灰阶面积  p50 %6.2f%%  p90 %6.2f%%  max %6.2f%%"
          % (np.median(a8), np.percentile(a8, 90), a8.max()))
    print("   脸区单帧最大差    p50 %5d     p90 %5d     max %5d"
          % (np.median(mx), np.percentile(mx, 90), mx.max()))
    print("   脸区均值差        p50 %6.2f   p90 %6.2f"
          % (np.median(mn), np.percentile(mn, 90)))
    print("   几乎无差异(<0.01%%)的帧 %d/%d = %.0f%%"
          % (int((a4 < 0.01).sum()), len(a4), 100.0 * (a4 < 0.01).mean()))

    # 重复/跳帧统计，说明两臂的时间线本身不同
    for tag, c in (("A", ca), ("B", cb)):
        arr = np.array([x for x in c if x >= 0])
        d = np.diff(arr)
        reps = int((d == 0).sum())
        skips = int((d == 2).sum())
        print("   %s 时间线：重复 %d 次、跳过 1 帧 %d 次、其他 %d 次"
              % (tag, reps, skips, int((~np.isin(d, (0, 1, 2))).sum())))

    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({"a": a.a, "b": a.b, "rows": rows}, fh, ensure_ascii=False)
        print("   写入 %s" % a.json)
    return rows


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
