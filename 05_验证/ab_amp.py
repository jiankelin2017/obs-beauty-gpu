#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「标尺档位」与「实测差异幅度」放在同一把尺上。

为什么要这一步
--------------
`ab_jnd.py` 给出的是**标尺**：+6 / +9 / +12… 灰阶的高斯鼓包，人眼从第几档开始看得见。
但那个数字要能用来判决，前提是「实测差异」也能换算成同一口径的灰阶幅度。
之前说的 12.8 灰阶是「7×7 摊平后的峰值」，而标尺上的幅度是「高斯峰值」——
两者量纲接近但不完全一致，必须用同一套算子重算一遍，否则是拿两个尺子比长短。

本脚本做两件事
--------------
1. 校验 `ab_jnd_h264.mp4` 的完整性：逐档量「闪烁两态之间的带幅度」。
2. 对三对真录制（agg / ctl / 正对照）逐帧算同一口径的带幅度，直接对标尺门限。

⚠ 口径必须钉死，否则是拿两把尺子比长短
--------------------------------------
同一个差异，两种算子能差 40%：

    逐通道取最大再 7×7 摊平  ->  12.76 灰阶   ← ab_ladder / ab_jnd 定位用的口径
    先转灰度再差再 7×7 摊平  ->   8.92 灰阶

原因是 `rms` 造成的掩码位移会带动**色度**一起变：某个通道涨 12、另一个通道跌几点，
灰度加权后正好互相抵消一部分。标尺上的鼓包是**三通道同加**的纯亮度变化，
它的通道最大 = 灰度差，所以只有「逐通道最大」这一口径能与标尺对齐。
本脚本对三对录制统一用逐通道最大，灰度差只作参考列。

用法
----
    py -3 ab_amp.py
"""

from __future__ import annotations

import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out", "ab")
sys.path.insert(0, HERE)
from make_ab_clip import read_code   # noqa: E402

# 标尺参数，必须与 ab_jnd.py 一致
RUN = 3
STILL = 15
FLICK = 90
GAP = 30
PER = STILL + FLICK + GAP
RUNGS = [0.0, 6.0, 9.0, 12.0, 15.0, 18.0, 24.0, 36.0]

# 脸区：与 ab_jnd.py 定位差异带时用的窗口一致
X0, X1, Y0, Y1 = 150, 720, 60, 780


def band_amp(fa, fb, region=(X0, Y0, X1, Y1)):
    """带幅度：逐像素取通道最大差 → 7×7 摊平 → 区域峰值。

    摊平是为了压掉 H.264 的单像素噪声（rung 0 的整帧最大差也有 4），
    但 7×7 远小于 63 px 的带宽，不会抹掉那条带本身。
    """
    x0, y0, x1, y1 = region
    d = np.abs(fa.astype(np.int16) - fb.astype(np.int16)).max(axis=2).astype(np.float32)
    g = cv2.cvtColor(fa, cv2.COLOR_BGR2GRAY).astype(np.int16)
    h = cv2.cvtColor(fb, cv2.COLOR_BGR2GRAY).astype(np.int16)
    dg = np.abs(g - h).astype(np.float32)
    ds = cv2.blur(d, (7, 7))[y0:y1, x0:x1]
    dgs = cv2.blur(dg, (7, 7))[y0:y1, x0:x1]
    return float(ds.max()), float(dgs.max()), float(ds.mean())


def check_ladder(path):
    """校验标尺片：逐档量闪烁两态之间的带幅度。"""
    cap = cv2.VideoCapture(path)
    frames = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        frames.append(f)
    cap.release()
    if not frames:
        return None
    print("== 标尺片 %s：%d 帧（应 %d 帧）  %.2f s"
          % (os.path.basename(path), len(frames), len(RUNGS) * PER,
             len(frames) / 30.0))
    print("   %-5s %-8s %-10s %-10s %-10s %-8s"
          % ("档", "设计幅度", "带幅度(通道max)", "灰度带幅度", "脸区均值差", "保留率"))
    rows = []
    for i, amp in enumerate(RUNGS):
        b = i * PER
        # 静止段最后一帧 vs 第一个「亮态」帧
        lo = frames[b + STILL - 1]
        hi = frames[b + STILL + RUN]
        ba, bg, mn = band_amp(hi, lo)
        rows.append((amp, ba, bg, mn))
        print("   %-5.0f %-8.0f %-16.2f %-10.2f %-10.2f %-8s"
              % (amp, amp, ba, bg, mn,
                 "—" if amp == 0 else "%.0f%%" % (100 * ba / amp)))
    return rows



def peak_amp(pairs):
    """对若干对（A 路径, B 路径, json 路径）逐帧算 7×7 摊平后的峰值差。"""
    for a, b, js in pairs:
        jp = os.path.join(HERE, js)
        if not os.path.exists(jp):
            print("== %s：缺 %s，跳过" % (a, js))
            continue
        rows = json.load(open(jp, encoding="utf-8"))["rows"]
        want = set(r["n"] for r in rows)
        # 两路都只留需要的帧。彩色要留 —— 灰度会把色度差抹掉（见文件头说明）
        fa, fb = {}, {}
        for path, store in ((a, fa), (b, fb)):
            cap = cv2.VideoCapture(os.path.join(OUT, path))
            while True:
                ok, f = cap.read()
                if not ok:
                    break
                c = read_code(f)
                if c in want and c not in store:
                    store[c] = f
            cap.release()
        common = [n for n in want if n in fa and n in fb]
        va, vg, vm = [], [], []
        for n in common:
            ba, bg, mn = band_amp(fa[n], fb[n])
            va.append(ba)
            vg.append(bg)
            vm.append(mn)
        va, vg = np.array(va), np.array(vg)
        print("== %-18s vs %-18s  配对 %3d 帧" % (a, b, len(common)))
        print("   带幅度(通道max)：p50 %6.2f  p75 %6.2f  p90 %6.2f  max %6.2f"
              % (np.percentile(va, 50), np.percentile(va, 75),
                 np.percentile(va, 90), va.max()))
        print("   灰度带幅度（参考）：p50 %6.2f  p90 %6.2f  max %6.2f"
              % (np.percentile(vg, 50), np.percentile(vg, 90), vg.max()))
        for th in (6.0, 9.0):
            print("   带幅度 ≥%.0f 的帧占比：%5.1f%%" % (th, 100.0 * float((va >= th).mean())))
    return True


def main():
    print("### 一、标尺片自检")
    lad = check_ladder(os.path.join(OUT, "ab_jnd_h264.mp4"))
    if lad and lad[0][1] != 0.0:
        print("   ⚠ 0 档整帧最大差 = %.0f，不是 0 —— 判读不可信" % lad[0][1])
    print()
    print("### 二、真录制的差异幅度（同一口径：7×7 摊平峰值）")
    peak_amp([
        ("max_combo_t2.mp4", "rms4_combo_t2.mp4", "out/ab/match_t2.json"),
        ("max_combo_v2.mp4", "max_combo_t2.mp4", "out/ab/match_maxmax.json"),
        ("ctrl_nofilter.mp4", "ctrl_strong.mp4", "out/ab/ctrl.json"),
    ])
    return 0


if __name__ == "__main__":
    sys.exit(main())
