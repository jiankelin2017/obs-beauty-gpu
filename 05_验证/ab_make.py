#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成人眼 A/B 的动态产物（按帧号配对版）。

产物
----
  ab_side.mp4     并排全画面（各 540×960），最常见的样子
  ab_face.mp4     脸区放大并排
  ab_flicker.mp4  同位置交替 A/B（每 3 帧一切 ≈10 Hz）—— 最灵敏
  ab_slow.mp4     同位置交替，每 30 帧（1 s）一切 —— 看长期印象
  ab_brow.mp4     差异最强那块（发际线带）的特写交替 —— 有的放矢

为什么要用帧号配对
------------------
两臂的录制起点相差 2.5 帧，且各自有重复/跳帧；不配对就必然错位，而错 1 帧
在说话素材上就是整张脸的假差异（实测会把 1.5% 读成 6%，把 19 灰阶读成 165）。
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
from ab_keys import imwrite_u  # noqa: E402


def put(img, text, scale=1.1):
    cv2.putText(img, text, (12, 36), cv2.FONT_HERSHEY_SIMPLEX, scale,
                (0, 0, 0), 5, cv2.LINE_AA)
    cv2.putText(img, text, (12, 36), cv2.FONT_HERSHEY_SIMPLEX, scale,
                (255, 255, 255), 2, cv2.LINE_AA)
    return img


def wr(path, frames, fps=30):
    if not frames:
        return None
    h, w = frames[0].shape[:2]
    vw = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
    if not vw.isOpened():
        return None
    for f in frames:
        vw.write(f)
    vw.release()
    return path


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--match", default="out/ab/match_max_rms4.json")
    ap.add_argument("--a", default="max_combo_v2.mp4")
    ap.add_argument("--b", default="rms4_combo_v2.mp4")
    ap.add_argument("--reps", type=int, default=2)
    a = ap.parse_args(argv)

    rows = json.load(open(os.path.join(HERE, a.match), encoding="utf-8"))["rows"]
    ai = [r["a"] for r in rows]
    bi = [r["b"] for r in rows]
    want_a, want_b = set(ai), set(bi)

    def grab(name, want):
        cap = cv2.VideoCapture(os.path.join(OUT, name))
        m = {}
        i = 0
        while True:
            ok, f = cap.read()
            if not ok:
                break
            if i in want:
                m[i] = f
            i += 1
        cap.release()
        return m
    A = grab(a.a, want_a)
    B = grab(a.b, want_b)
    pairs = [(A[x], B[y], rows[k]) for k, (x, y) in enumerate(zip(ai, bi))
             if x in A and y in B]
    print("配对 %d 帧（%.1f s @30fps）" % (len(pairs), len(pairs) / 30.0))

    W, H = 540, 960
    rx0, rx1, ry0, ry1 = 150, 720, 60, 780

    # 差异最强点 → 决定特写窗口
    best = max(pairs, key=lambda p: p[2]["a4"])
    d = np.abs(cv2.cvtColor(best[0], cv2.COLOR_BGR2GRAY).astype(np.int16)
               - cv2.cvtColor(best[1], cv2.COLOR_BGR2GRAY).astype(np.int16))
    sm = cv2.blur(d.astype(np.float32), (15, 15))
    yy, xx = np.unravel_index(int(sm.argmax()), sm.shape)
    yy, xx = int(yy), int(xx)
    bx0, bx1 = max(0, xx - 190), min(1080, xx + 190)
    by0, by1 = max(0, yy - 105), min(1920, yy + 105)
    print("差异最强点 (%d,%d)，特写窗口 x[%d,%d] y[%d,%d]"
          % (xx, yy, bx0, bx1, by0, by1))

    def seq(tag):
        return [p[0] if tag == "A" else p[1] for p in pairs]

    # 1) 并排全画面
    fs = []
    for fa, fb, _ in pairs:
        la = cv2.resize(fa, (W, H), interpolation=cv2.INTER_AREA)
        lb = cv2.resize(fb, (W, H), interpolation=cv2.INTER_AREA)
        row = np.hstack([put(la.copy(), "A"), put(lb.copy(), "B")])
        cv2.line(row, (W, 0), (W, H), (255, 255, 255), 2)
        fs.append(row)
    print("  side     ", wr(os.path.join(OUT, "ab_side.mp4"), fs))

    # 2) 脸区放大并排
    fs = []
    for fa, fb, _ in pairs:
        ca = fa[ry0:ry1, rx0:rx1]
        cb = fb[ry0:ry1, rx0:rx1]
        za = cv2.resize(ca, (W, H), interpolation=cv2.INTER_CUBIC)
        zb = cv2.resize(cb, (W, H), interpolation=cv2.INTER_CUBIC)
        row = np.hstack([put(za.copy(), "A"), put(zb.copy(), "B")])
        cv2.line(row, (W, 0), (W, H), (255, 255, 255), 2)
        fs.append(row)
    print("  face     ", wr(os.path.join(OUT, "ab_face.mp4"), fs))

    # 3) 闪烁（每 3 帧切）
    fs = []
    for k in range(len(pairs)):
        src = pairs[k][0] if (k // 3) % 2 == 0 else pairs[k][1]
        tag = "A" if (k // 3) % 2 == 0 else "B"
        c = cv2.resize(src[ry0:ry1, rx0:rx1], (W, H), interpolation=cv2.INTER_CUBIC)
        fs.append(put(c.copy(), tag))
    print("  flicker  ", wr(os.path.join(OUT, "ab_flicker.mp4"), fs))

    # 4) 慢速交替（每 30 帧切）
    fs = []
    for k in range(len(pairs)):
        src = pairs[k][0] if (k // 30) % 2 == 0 else pairs[k][1]
        tag = "A" if (k // 30) % 2 == 0 else "B"
        c = cv2.resize(src[ry0:ry1, rx0:rx1], (W, H), interpolation=cv2.INTER_CUBIC)
        fs.append(put(c.copy(), tag))
    print("  slow     ", wr(os.path.join(OUT, "ab_slow.mp4"), fs))

    # 5) 发际线带特写交替
    fs = []
    for k in range(len(pairs)):
        src = pairs[k][0] if (k // 3) % 2 == 0 else pairs[k][1]
        tag = "A" if (k // 3) % 2 == 0 else "B"
        c = src[by0:by1, bx0:bx1]
        c = cv2.resize(c, (760, 420), interpolation=cv2.INTER_NEAREST)
        fs.append(put(c.copy(), tag, 1.0))
    print("  brow     ", wr(os.path.join(OUT, "ab_brow.mp4"), fs))

    # 静态图：发际线带的 A/B 上下堆叠（原尺寸 2 倍）
    ca = best[0][by0:by1, bx0:bx1]
    cb = best[1][by0:by1, bx0:bx1]
    z = lambda im: cv2.resize(im, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST)  # noqa: E731
    imwrite_u(os.path.join(OUT, "keys2/brow_AB.png"),
              np.vstack([put(z(ca).copy(), "A"), put(z(cb).copy(), "B")]))
    print("  发际线对照 %s" % os.path.join(OUT, "keys2/brow_AB.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
