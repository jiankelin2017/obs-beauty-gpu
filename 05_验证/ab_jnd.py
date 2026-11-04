#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""闪烁 JND 标尺：把「看不看得出」换成「多少灰阶的闪烁你看得出」。

为什么换成这个任务
------------------
先前的做法是给两支片（`max` vs `rms4`、`max` vs `max`）让你比较「哪支更闪」。
这是**辨别任务**（discrimination），要求在同一记忆里存两个感觉量再比大小 ——
人做这个很差，尤其是两支队都接近门限时。

这里换成**检测任务**（detection）：同一位置、同一形状的闪烁，幅度一档一档往上走，
你只需要回答「从第几档起我看得见」。检测任务的门限信噪比远低于辨别任务，
而且答案是**一个数**，不是一种感觉 —— 拿这个数去和实测的 12.8 灰阶比，就有了判决。

做法
----
- 底图：实测差异最大的那一帧（原分辨率裁 540×960，**1:1 不缩放**，缩放会低通、
  改变空间频率，结论就不可比）。
- 鼓包：与 `ab_ladder.py` 同一口径的高斯，σ 由 FWHM 63 px 反推，位置固定在实测
  差异带中心。
- 闪烁：每 3 帧切一次 ≈ 5 Hz（与 `ab_flicker` 同频）。
- 每档：0.5 s 静止（复位视觉）+ 3 s 闪烁；档间插 1 s 纯黑。
- 档位：0（阴性对照，必须看不见）+ 6/9/12/15/18/24/36 灰阶。

为什么要有 0 档
---------------
0 档两臂完全相同。若 0 档也「看见闪烁」，说明片子里有编码/播放伪影，
后面所有档位的判断都不可信 —— 这一档是整支片的自检。

用法
----
    py -3 ab_jnd.py --a max_combo_t2.mp4 --b rms4_combo_t2.mp4 --json out/ab/match_t2.json
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
from make_ab_clip import read_code   # noqa: E402
from ab_keys import imwrite_u        # noqa: E402

FWHM = 63.0
SIGMA = FWHM / 2.3548
CW, CH = 540, 960                    # 1:1 裁切尺寸，不缩放
RUN = 3                              # 每 3 帧切一次 = 5 Hz @30fps
STILL = 15                           # 每档前 0.5 s 静止，让视觉复位
FLICK = 90                           # 3 s 闪烁
GAP = 30                             # 档间 1 s 纯黑
RUNGS = [0.0, 6.0, 9.0, 12.0, 15.0, 18.0, 24.0, 36.0]


def label(img, text, scale=0.9):
    cv2.putText(img, text, (14, 40), cv2.FONT_HERSHEY_SIMPLEX, scale,
                (0, 0, 0), 6, cv2.LINE_AA)
    cv2.putText(img, text, (14, 40), cv2.FONT_HERSHEY_SIMPLEX, scale,
                (255, 255, 255), 2, cv2.LINE_AA)
    return img


def frames_by_code(path):
    cap = cv2.VideoCapture(path)
    out = {}
    while True:
        ok, f = cap.read()
        if not ok:
            break
        c = read_code(f)
        if c >= 0 and c not in out:
            out[c] = f
    cap.release()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo_t2.mp4")
    ap.add_argument("--b", default="rms4_combo_t2.mp4")
    ap.add_argument("--json", default="out/ab/match_t2.json")
    ap.add_argument("--out", default="ab_jnd.mp4")
    a = ap.parse_args(argv)

    rows = json.load(open(os.path.join(HERE, a.json), encoding="utf-8"))["rows"]
    worst = max(rows, key=lambda r: r["a4"])
    n = worst["n"]

    fa = frames_by_code(os.path.join(OUT, a.a))
    fb = frames_by_code(os.path.join(OUT, a.b))
    if n not in fa or n not in fb:
        sys.exit("帧号 %d 缺帧" % n)
    A, B = fa[n], fb[n]

    # 鼓包位置：用真实差异定位（7×7 摊平，避开单像素编码噪声）
    x0, x1, y0, y1 = 150, 720, 60, 780
    d = np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2).astype(np.float32)
    ds = cv2.blur(d, (7, 7))
    iy, ix = np.unravel_index(int(ds[y0:y1, x0:x1].argmax()), (y1 - y0, x1 - x0))
    ypk, xpk = y0 + int(iy), x0 + int(ix)
    print("底图帧 n=%d，鼓包中心 (%d,%d)，该处 7×7 幅度 %.1f 灰阶"
          % (n, xpk, ypk, float(ds[ypk, xpk])))

    # 1:1 裁切窗口：以鼓包为中心，避免把带子放到画面边缘
    cx0 = int(np.clip(xpk - CW // 2, 0, A.shape[1] - CW))
    cy0 = int(np.clip(ypk - 300, 0, A.shape[0] - CH))
    print("裁切窗口 x[%d,%d) y[%d,%d)（1:1，不缩放）"
          % (cx0, cx0 + CW, cy0, cy0 + CH))

    h, w = A.shape[:2]
    yy = np.arange(h, dtype=np.float32)[:, None]
    xx = np.arange(w, dtype=np.float32)[None, :]
    g = np.exp(-(((yy - ypk) ** 2) / (2 * SIGMA ** 2)
                 + ((xx - xpk) ** 2) / (2 * (SIGMA * 3) ** 2))).astype(np.float32)
    g3 = g[:, :, None]
    base = A[cy0:cy0 + CH, cx0:cx0 + CW]
    basef = base.astype(np.float32)

    frames = []
    for amp in RUNGS:
        hi = np.clip(basef + g3[cy0:cy0 + CH, cx0:cx0 + CW] * amp, 0, 255).astype(np.uint8)
        tag = ("阴性对照（两臂完全相同）" if amp == 0 else "+%.0f 灰阶" % amp)
        # 静止段：只用底图，让人眼复位
        for _ in range(STILL):
            frames.append(label(base.copy(), tag))
        # 闪烁段
        for k in range(FLICK):
            src = base if (k // RUN) % 2 == 0 else hi
            frames.append(label(src.copy(), tag))
        for _ in range(GAP):
            frames.append(np.zeros((CH, CW, 3), np.uint8))
        print("  档 +%4.0f 灰阶  %d 帧" % (amp, STILL + FLICK + GAP))

    path = os.path.join(OUT, a.out)
    vw = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"mp4v"), 30, (CW, CH))
    if not vw.isOpened():
        sys.exit("VideoWriter 打不开：%s" % path)
    for f in frames:
        vw.write(f)
    vw.release()
    print("总 %d 帧 = %.1f s -> %s" % (len(frames), len(frames) / 30.0, path))

    # 静态参考：把各档鼓包并排存一张，方便事后核对位置
    ref = []
    for amp in RUNGS:
        im = np.clip(basef + g3[cy0:cy0 + CH, cx0:cx0 + CW] * amp, 0, 255).astype(np.uint8)
        ref.append(label(im[300:600:2, ::2].copy(), "+%.0f" % amp, 0.6))
    imwrite_u(os.path.join(OUT, "jnd_ref.png"), np.vstack(ref))
    print("参考图 jnd_ref.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
