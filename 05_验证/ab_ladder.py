#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""可见度标尺：把「差异有多大」换成「多少灰阶的鼓包看得出来」。

为什么需要它
------------
`ab_match.py` 给出的是**相对量**（≥4 灰阶面积 1.4%、单帧最大差 15）。这个数字
没法直接回答「人看不看得见」—— 因为同一个峰值，摊在 200 px 宽上是背景亮度起伏，
压在 2 px 上是脏点。真正决定可见性的是**空间梯度**（灰阶/像素），不是峰值。

所以这里做一个绝对标尺：在**真实画面的原始帧**上，按实测的半高宽（63 px）叠加
不同幅度的平滑鼓包（0/6/12/18/24 灰阶），1:1 裁切堆放。看完就知道「像我们这样的
差异，要多大才轮到肉眼」—— 这是把主观判断锚在物理量上的唯一办法。

口径
----
- 鼓包用高斯，σ 由半高宽反推：FWHM = 2.3548σ → σ = FWHM/2.3548 = 26.8 px。
- 只加在额头带（y 与实测差异峰一致），x 居中，避免顺便改了画面整体亮度。
- 全部写 8 bit PNG，1:1 裁切，不缩放 —— 缩放会抹掉高频、改变可见性结论。

用法
----
    py -3 ab_ladder.py --a max_combo_t2.mp4 --b rms4_combo_t2.mp4
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

CROP = (150, 720, 60, 780)          # 脸区 x0,x1,y0,y1
FWHM = 63.0                          # 实测差异垂直剖面半高宽（px）
SIGMA = FWHM / 2.3548


def imwrite_u(path, img):
    """cv2.imwrite 对非 ASCII 路径会静默失败，统一走 imencode。"""
    ext = os.path.splitext(path)[1] or ".png"
    ok, buf = cv2.imencode(ext, img)
    if not ok:
        raise RuntimeError("编码失败：%s" % path)
    with open(path, "wb") as fh:
        fh.write(buf.tobytes())
    return path


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
    ap.add_argument("--gain", nargs="*", type=float, default=None,
                    help="手工指定档位；不给就按实测幅度自动取半幅/实幅/2倍/4倍")
    a = ap.parse_args(argv)

    with open(os.path.join(HERE, a.json), encoding="utf-8") as fh:
        rows = json.load(fh)["rows"]
    worst = max(rows, key=lambda r: r["a4"])
    print("最差帧 n=%d  脸区 ≥4 面积 %.2f%%  单帧最大差 %d"
          % (worst["n"], worst["a4"], worst["mx"]))

    fa = frames_by_code(os.path.join(OUT, a.a))
    fb = frames_by_code(os.path.join(OUT, a.b))
    n = worst["n"]
    if n not in fa or n not in fb:
        sys.exit("帧号 %d 在某一臂里找不到" % n)
    A = fa[n]
    B = fb[n]

    # 用真实差异定位鼓包中心。先做 7×7 均值，把编码噪声的单像素尖峰压掉 ——
    # 否则定位会落到孤立噪声点上，标尺的幅度基准就失真了。
    # ⚠ 单像素最大差（17~23）**不能**当作差异幅度：那是条码/边缘处的编码噪声，
    # 摊平之后的「带幅度」才是人眼看到的那一层。
    x0, x1, y0, y1 = CROP
    d = np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2).astype(np.float32)
    ds = cv2.blur(d, (7, 7))
    sub = ds[y0:y1, x0:x1]
    iy, ix = np.unravel_index(int(np.argmax(sub)), sub.shape)
    ypk, xpk = y0 + int(iy), x0 + int(ix)
    amp0 = float(sub[iy, ix])
    print("差异带：y=%d x=%d，7×7 摊平后幅度 %.1f 灰阶（单像素最大差 %d 是编码噪声）"
          % (ypk, xpk, amp0, worst["mx"]))

    h, w = A.shape[:2]
    yy = np.arange(h, dtype=np.float32)[:, None]
    xx = np.arange(w, dtype=np.float32)[None, :]
    g = np.exp(-(((yy - ypk) ** 2) / (2 * SIGMA ** 2)
                 + ((xx - xpk) ** 2) / (2 * (SIGMA * 3) ** 2))).astype(np.float32)
    g3 = g[:, :, None]          # 三通道同加，避免只抬一个通道导致偏色

    # 裁一条穿过峰值的横带，1:1 堆放；带宽要够看见缓坡的整个起伏
    cy0, cy1 = max(0, ypk - 90), min(h, ypk + 130)
    cx0, cx1 = x0, x1
    rungs = a.gain
    if not rungs:
        # 默认档位按实测幅度定：半幅 / 实幅 / 2 倍 / 4 倍。
        # 这样一眼能看出「实幅这一档看不看得出」，而不是拿一组无关的数去猜。
        rungs = [0, amp0 * 0.5, amp0, amp0 * 2, amp0 * 4]
    rungs = [round(float(v), 1) for v in rungs]
    tiles = []
    for amp in rungs:
        img = A.astype(np.float32) + g3 * amp
        img = np.clip(img, 0, 255).astype(np.uint8)
        t = img[cy0:cy1, cx0:cx1].copy()
        # 左上角写字：幅度，避免靠记忆对号
        lab = "+%.1f 灰阶" % amp if amp > 0 else "原始（0）"
        cv2.rectangle(t, (0, 0), (170, 26), (0, 0, 0), -1)
        cv2.putText(t, lab, (6, 19), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                    (255, 255, 255), 1, cv2.LINE_AA)
        tiles.append(t)
        # 每隔 2 px 画一条 1 px 的弱参考线？不画 —— 会引入额外的边缘线索
    stack = np.vstack(tiles)
    p1 = imwrite_u(os.path.join(OUT, "ladder_1to1.png"), stack)
    print("1:1 标尺 -> %s  (%dx%d)" % (p1, stack.shape[1], stack.shape[0]))

    # 再来一张 2 倍放大（只看峰值附近），用于确认坡是不是真的缓
    z = []
    for amp in rungs:
        img = np.clip(A.astype(np.float32) + g3 * amp, 0, 255).astype(np.uint8)
        t = img[max(0, ypk - 40):ypk + 50, cx0:cx1]
        z.append(cv2.resize(t, None, fx=2.0, fy=2.0, interpolation=cv2.INTER_NEAREST))
    zs = np.vstack(z)
    p2 = imwrite_u(os.path.join(OUT, "ladder_2x.png"), zs)
    print("2x 标尺 -> %s  (%dx%d)" % (p2, zs.shape[1], zs.shape[0]))

    # 客观自检：加了 amp 之后，实际落地的梯度是多少灰阶/px
    for amp in rungs:
        col = g[:, xpk] * amp
        slope = float(np.abs(np.diff(col)).max())
        print("  +%5.1f 灰阶：峰值 %.1f，垂直相邻像素最大跳变 %.2f 灰阶/px"
              % (amp, float(col.max()), slope))

    # 紧凑横排：只取鼓包中心 320×220，2 倍放大后并排 —— 堆叠版会被看图界面
    # 等比缩小，缩小本身就是低通滤波，正好会把「缓坡」抹平，把结论看反。
    panels = []
    for amp in (0.0, amp0, amp0 * 4):
        img = np.clip(A.astype(np.float32) + g3 * amp, 0, 255).astype(np.uint8)
        y0p, x0p = max(0, ypk - 110), max(0, xpk - 160)
        t = img[y0p:y0p + 220, x0p:x0p + 320]
        t = cv2.resize(t, None, fx=2.0, fy=2.0, interpolation=cv2.INTER_NEAREST)
        cv2.rectangle(t, (0, 0), (200, 34), (0, 0, 0), -1)
        cv2.putText(t, ("0" if amp == 0 else "+%.1f" % amp) + " 灰阶", (8, 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 1, cv2.LINE_AA)
        panels.append(t)
    vs = np.hstack(panels)
    p3 = imwrite_u(os.path.join(OUT, "ladder_vs.png"), vs)
    print("横排对比 -> %s  (%dx%d)" % (p3, vs.shape[1], vs.shape[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
