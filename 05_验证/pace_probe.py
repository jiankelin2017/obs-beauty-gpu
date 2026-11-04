#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""帧节拍探针：录到的源帧有没有被重复/跳过。

背景（§9.35.7）
--------------
按帧号配对做 A/B 时发现：`rms`/`rms4` 的录制里大量源帧被重复或跳过，`max` 几乎没有。
这件事与掩码拖影无关，但会污染「差异」的统计（错 1 帧在说话素材上就是整张脸的假差异），
所以必须单独量清楚。

两个口径必须分开
----------------
「码值差 == 0」**不等于**「重复帧」：
  · 真重复：相邻两帧像素平均差 < 0.2（实测 0.01–0.05，只剩编码噪声）；
  · 伪重复：码值相同但像素差 40–90 —— 那是条码被美颜改写导致误读，不是重复。
`max` 臂里就有 8 处码值相同却像素差 46–86 的，不区分就会把它算成抖动。

为什么同时报「跳帧」
--------------------
真重复与跳帧**数量接近**说明平均帧率是对的、只是采样相位在来回滑（hold 一下再追一帧）；
若只有重复没有跳帧，那是真的在丢帧。两者含义完全不同，不能只看一个数。

用法
----
    py -3 pace_probe.py max_combo_t2.mp4 rms_combo_t2.mp4 rms4_combo_t2.mp4
"""

from __future__ import annotations

import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out", "ab")
sys.path.insert(0, HERE)
from make_ab_clip import read_code  # noqa: E402

DUP_PX = 0.2       # 相邻帧像素平均差低于此值 → 判为真重复


def probe(path):
    cap = cv2.VideoCapture(path)
    codes, diffs = [], []
    prev = None
    while True:
        ok, f = cap.read()
        if not ok:
            break
        g = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY).astype(np.int16)
        codes.append(read_code(f))
        diffs.append(0.0 if prev is None else float(np.abs(g - prev).mean()))
        prev = g
    cap.release()

    c = np.array(codes)
    d = np.diff(c)
    same = np.where(d == 0)[0]          # 码值差 0
    skip = np.where(d == 2)[0]          # 码值差 2 = 跳了一帧
    dup = [i for i in same if diffs[i + 1] < DUP_PX]
    fake = [i for i in same if diffs[i + 1] >= DUP_PX]
    return {
        "file": os.path.basename(path), "frames": len(c),
        "dup": len(dup), "dup_idx": dup,
        "fake": len(fake), "skip": len(skip),
        "other": int((~np.isin(d, (0, 1, 2))).sum()),
        "dup_rate": 100.0 * len(dup) / max(1, len(c) - 1),
    }


def main(argv=None):
    names = argv or sys.argv[1:]
    if not names:
        sys.exit("给几个 mp4 文件名（相对 out/ab）")
    print("%-24s %6s %6s %6s %6s %8s %8s"
          % ("文件", "帧", "真重复", "误读", "跳帧", "重复率", "重复间隔"))
    for n in names:
        r = probe(os.path.join(OUT, n))
        gap = "-"
        if len(r["dup_idx"]) >= 2:
            gaps = np.diff(r["dup_idx"])
            gap = "%.1f±%.1f" % (float(np.median(gaps)), float(np.std(gaps)))
        print("%-24s %6d %6d %6d %6d %7.1f%% %8s"
              % (r["file"][:24], r["frames"], r["dup"], r["fake"],
                 r["skip"], r["dup_rate"], gap))
    return 0


if __name__ == "__main__":
    sys.exit(main())
