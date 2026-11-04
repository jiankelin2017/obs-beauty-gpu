# -*- coding: utf-8 -*-
"""离线标定「运动判定」的三种格内统计口径（2026-11-04，2.11.1 前置）。

为什么需要它
------------
2.11.0 把运动判定搬到了 64×64 GPU 小图，但格内取的是「逐像素 |Δ| 的最大值」。
在**合成 PNG 素材**上（逐帧完全相同）两臂都是 dper=7，看起来没问题；换成
**真实视频源**（mjpg 逐帧独立压缩，每帧噪声不同）后，`nopix=0/180` ——
判定每帧都认为「在动」，省帧收益直接归零，闪脸素材也因此测不出「晚一帧」。

本脚本用同一条数学在 CPU 上复算三种口径，回答一个问题：
    哪一种「格内统计」既有足够分离度（运动中 → 大），又不会被逐帧噪声顶穿？
      A 老路径  ：格 = 单点 |Δ|（640×640 缩略图，stride 采样）
      B 2.11.0 ：格 = 格内逐像素 |Δ| 的**最大**值
      C 候选   ：格 = 格内逐像素 |Δ| 的**均值**
三者后面的聚合完全一样：4×4 格为一块取均值 → 16 个块取最大 → 与阈值比。

用法
----
    py -3 calib_motion_stat.py                     # 全部素材
    py -3 calib_motion_stat.py --files a.mp4 b.avi
"""

from __future__ import annotations

import argparse
import glob
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
GRID = 16          # 16×16 格
BLK = 4            # 4×4 格 = 一块


def imread_luma(frame):
    """BGR → BT.601 近似整数亮度，与引擎里那行 `(54r+183g+19b)>>8` 一致。"""
    b, g, r = frame[:, :, 0].astype(np.int32), frame[:, :, 1].astype(np.int32), \
        frame[:, :, 2].astype(np.int32)
    return (54 * r + 183 * g + 19 * b) >> 8


def grid_metrics(prev, cur):
    """返回三种口径的 16×16 格差值矩阵。prev/cur 为同尺寸 luma。"""
    h, w = cur.shape
    bh, bw = h // GRID, w // GRID
    a = np.zeros((GRID, GRID), np.float64)      # 单点
    b = np.zeros((GRID, GRID), np.float64)      # 格内最大 |Δ|
    c = np.zeros((GRID, GRID), np.float64)      # 格内平均 |Δ|
    d = np.abs(cur.astype(np.int32) - prev.astype(np.int32)).astype(np.float64)
    for gy in range(GRID):
        for gx in range(GRID):
            y0, x0 = gy * bh, gx * bw
            cell = d[y0:y0 + bh, x0:x0 + bw]
            if cell.size == 0:
                continue
            a[gy, gx] = cell[0, 0]
            b[gy, gx] = cell.max()
            c[gy, gx] = cell.mean()
    return a, b, c


def block_max(m):
    """4×4 格为一块取均值，返回 16 个块里的最大值 —— 与引擎里那段完全一致。"""
    out = 0.0
    for by in range(0, GRID, BLK):
        for bx in range(0, GRID, BLK):
            v = m[by:by + BLK, bx:bx + BLK].mean()
            if v > out:
                out = v
    return out


def scan(path, limit=None):
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        print("  打不开：%s" % path)
        return None
    prev = None
    rows = {"A": [], "B": [], "C": []}
    n = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        n += 1
        if limit and n > limit:
            break
        # 与 GPU 那条路一致：先把画面缩到 64×64 再算（最近邻 ≈ 单次点采样）
        small = cv2.resize(frame, (64, 64), interpolation=cv2.INTER_NEAREST)
        cur = imread_luma(small)
        if prev is not None:
            a, b, c = grid_metrics(prev, cur)
            rows["A"].append(block_max(a))
            rows["B"].append(block_max(b))
            rows["C"].append(block_max(c))
        prev = cur
    cap.release()
    return n, rows


def pct(v, q):
    return float(np.percentile(v, q)) if len(v) else float("nan")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="*")
    ap.add_argument("--limit", type=int, default=300, help="每段最多算多少帧")
    args = ap.parse_args(argv)

    files = args.files or sorted(
        glob.glob(os.path.join(ASSETS, "flash_k*_720p.avi")) +
        glob.glob(os.path.join(ASSETS, "motion_720p.mp4")) +
        glob.glob(os.path.join(ASSETS, "plate_720p.avi")))
    if not files:
        sys.exit("assets/ 下没有可比素材，先跑 make_flash_assets.py")

    print("每段取前 %d 帧；数值 = 16 个块均值的最大值（引擎实际与阈值 6 比的就是它）"
          % args.limit)
    print("口径 A=格内单点  B=格内最大|Δ|（2.11.0 现状）  C=格内平均|Δ|（候选）")
    print()
    print("%-26s %5s | %-23s | %-23s | %-23s" %
          ("素材", "帧", "A 单点  p50/p90/max", "B 最大  p50/p90/max",
           "C 平均  p50/p90/max"))
    print("-" * 112)
    for path in files:
        r = scan(path, args.limit)
        if not r:
            continue
        n, rows = r
        cells = []
        for key in "ABC":
            v = rows[key]
            cells.append("%5.1f %5.1f %5.1f" % (pct(v, 50), pct(v, 90),
                                                max(v) if v else float("nan")))
        print("%-26s %5d | %-23s | %-23s | %-23s"
              % (os.path.basename(path), n, cells[0], cells[1], cells[2]))
    print()
    print("读法：p90 就是「有 10%% 的帧会越过它」的水平。阈值 6 要卡在")
    print("      静止素材的 p90 之上、运动素材的 p50 之下，才算有分离度。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
