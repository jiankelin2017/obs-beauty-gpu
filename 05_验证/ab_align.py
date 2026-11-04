#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两臂录制的精确对齐。

问题
----
两次录制各自独立起播，起始时刻不同；素材循环播放，容器帧数虽然均匀
（PTS 恒 33.33 ms、354 帧），但**内容**会漂移 —— 实测两臂的同步标记间隔
分别是 244 和 243 帧，即中间有一处渲染重复/跳过了素材帧。所以按标记帧
+固定帧号对齐会错位 1 帧，而说话时嘴 1 帧就能动一大截，差异统计会被
对齐误差污染（第一版实测「最差帧 24% 面积 ≥4 灰阶」，其中大部分是这个）。

做法
----
用**非脸区**做匹配：美颜只作用于脸，背景/字幕/桌面在两臂里是同一个输入，
所以拿它们做互相关不会把待测的差异也最小化掉。对 B 的每一帧，在 A 的
±`--span` 帧内找「非脸区灰度差最小」的那一帧，得到逐帧映射。

输出一个 json：`{b_frame: a_frame}` 与偏移序列，供 ab_compare 使用。
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

# 脸 + 头发 + 脖子（1080×1920 尺度）。这块在匹配时要**排除**。
FACE_MASK = (100, 800, 0, 840)
DS = 4          # 降采样倍率，纯为提速（匹配精度不受影响，33ms 内画面变化很小）


def read_all(path):
    cap = cv2.VideoCapture(path)
    fr = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        fr.append(cv2.cvtColor(f, cv2.COLOR_BGR2GRAY))
    cap.release()
    return fr


def prep(g):
    """降采样 + 脸区置为常数（不参与匹配）。"""
    s = cv2.resize(g, None, fx=1.0 / DS, fy=1.0 / DS, interpolation=cv2.INTER_AREA)
    x0, x1, y0, y1 = [int(v / DS) for v in FACE_MASK]
    s = s.copy()
    s[y0:y1, x0:x1] = 0
    return s


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo.mp4")
    ap.add_argument("--b", default="rms4_combo.mp4")
    ap.add_argument("--span", type=int, default=4, help="搜索半径（帧）")
    ap.add_argument("--start-a", type=int, default=0)
    ap.add_argument("--start-b", type=int, default=0)
    ap.add_argument("--limit", type=int, default=300)
    a = ap.parse_args(argv)

    pa = os.path.join(OUT, a.a)
    pb = os.path.join(OUT, a.b)
    ga = read_all(pa)
    gb = read_all(pb)
    print("A %d 帧  B %d 帧" % (len(ga), len(gb)))

    sa = [prep(g) for g in ga]
    sb = [prep(g) for g in gb]

    rows = []
    for j in range(a.start_b, min(len(gb), a.start_b + a.limit)):
        best, bi = 1e18, None
        for off in range(-a.span, a.span + 1):
            i = j + a.start_a - a.start_b + off
            if i < 0 or i >= len(ga):
                continue
            d = float(np.abs(sb[j].astype(np.int16) - sa[i].astype(np.int16)).mean())
            if d < best:
                best, bi = d, i
        if bi is not None:
            rows.append({"b": j, "a": bi, "off": bi - j, "err": best})

    offs = np.array([r["off"] for r in rows])
    errs = np.array([r["err"] for r in rows])
    # 分段统计：偏移是分段常数（漂移点会跳一次）
    seg = []
    cur = offs[0]
    n = 0
    for o in offs:
        if o == cur:
            n += 1
        else:
            seg.append((int(cur), n))
            cur, n = o, 1
    seg.append((int(cur), n))
    print("偏移分布：%s" % {int(k): int((offs == k).sum()) for k in set(offs.tolist())})
    print("分段（偏移, 长度）：%s" % seg[:12])
    print("匹配误差 中位 %.3f  p90 %.3f" % (np.median(errs), np.percentile(errs, 90)))

    # 稳定判据：正确对齐时，匹配误差应显著低于偏移 ±1 的次优值
    second = []
    for r in rows[:60]:
        cand = []
        for o in (-1, 1):
            i = r["b"] + r["off"] + o
            if 0 <= i < len(sa):
                cand.append(float(np.abs(sb[r["b"]].astype(np.int16)
                                         - sa[i].astype(np.int16)).mean()))
        if cand:
            second.append(min(cand))
    if second:
        print("最佳 vs 次优：%.3f vs %.3f（比值 %.2fx）"
              % (np.median(errs), np.median(second),
                 np.median(second) / max(1e-9, np.median(errs))))

    dst = os.path.join(OUT, "align_%s_%s.json"
                       % (os.path.splitext(a.a)[0], os.path.splitext(a.b)[0]))
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump({"a": a.a, "b": a.b, "rows": rows}, fh, ensure_ascii=False)
    print("对齐表写入 %s" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
