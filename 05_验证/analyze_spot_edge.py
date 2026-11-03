#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P0-10 定性：GPU 五官段祛斑在 ROI 边界的窗口实现差异（离线，不开 OBS）。

背景
----
`BEAUTY_REGION_CMP=1` 的同帧对拍在 `--only spot` 下报 maxdiff=4 / 178 像素。
判别实验（本轮的 A 步）：
  · k→0（BEAUTY_REG_SPOT=1e-6）→ 差异 0     ⇒ 无坐标/掩码错位等系统性偏差
  · k=0.05 → 174 像素，bbox 只有 y=632~633 两行
  · k=0.10 → 178 像素，bbox y=472~633
  · k=1.00 → 227 像素，bbox y=351~633，maxdiff 37
差异的「底座」不随 k 变化、且贴在一条水平带上 ⇒ 指向 ROI 边界的两行。

假设
----
CPU（beauty_region.cpp:386-414）用**截断窗口**：yA=max(y-r,0)、yB=min(y+r,bh-1)，
分子只加 yA..yB，分母 (yB-yA+1) —— 与「实际参与元素数」一致。
GPU（region.hlsl:319-351）用 **clamp 重复窗口 + 去重分母**：采样点
clamp(px+k, roiMin+0.5, roiMax-0.5)，边缘的采样会**重复**同一个像素；
而分母 nx/ny 按 clamp 后的**去重行数**算。于是边缘 r 行内分子多算了重复像素，
均值偏大 ⇒ dev 偏小 ⇒ pull 偏小 ⇒ 与 CPU 差 1~4 灰阶。

工具做法
----
读插件落盘的原图与掩码（`BEAUTY_M1_DUMP_DIR`），离线复算两种窗口实现，
看「末端差异」是否复现出对拍报的那组数字与 bbox 形状。

用法
----
    py -3 05_验证/analyze_spot_edge.py --roi 535,218,849,634
    py -3 05_验证/analyze_spot_edge.py --roi 535,218,849,634 --k 0.05
"""
import argparse
import os

import numpy as np

DUMP = os.path.join(os.environ.get("TEMP", "C:/Windows/Temp"), "beauty_m1dump")


def load(cx, cy):
    src = np.fromfile(os.path.join(DUMP, "region_src_rgba_%dx%d.raw" % (cx * 4, cy)),
                      dtype=np.uint8).reshape(cy, cx, 4)
    mask = np.fromfile(os.path.join(DUMP, "region_mask_%dx%d.raw" % (cx, cy)),
                       dtype=np.uint8).reshape(cy, cx)
    return src, mask


def lum_of(src):
    f = src[:, :, :3].astype(np.float32)
    return 0.299 * f[:, :, 0] + 0.587 * f[:, :, 1] + 0.114 * f[:, :, 2]


def box_mean_truncate(L, roi, r):
    """CPU：截断窗口，分母 = 实际参与的元素数（不重复）。

    beauty_region.cpp:386-414 是在 **ROI 局部坐标**里 clamp 的：
    yA=max(ly-r,0)、yB=min(ly+r,bh-1)，所以分母的上下界是 0 / bh-1，
    不是全帧的 0 / H-1 —— 这一点正是与 GPU 分道的地方。
    """
    x0, y0, x1, y1 = roi
    H, W = L.shape
    bw, bh = x1 - x0, y1 - y0
    ys = np.arange(H)
    ly = ys - y0
    # 关键：窗口外的行**根本不计入**（不是 clamp 到边界再计入）
    in_roi = (ly >= 0) & (ly < bh)
    acc = np.zeros((H, W), dtype=np.float32)
    for dy in range(-r, r + 1):
        yy = ly + dy
        valid = in_roi & (yy >= 0) & (yy < bh)
        acc += np.where(valid[:, None], L[np.clip(ys + dy, y0, y1 - 1), :], 0.0)
    ny = (np.clip(ly + r, 0, bh - 1) - np.clip(ly - r, 0, bh - 1) + 1)
    ny = np.where(in_roi, ny, 1).astype(np.float32)[:, None]
    out = np.zeros_like(L)
    xs = np.arange(W)
    lx = xs - x0
    in_roi_x = (lx >= 0) & (lx < bw)
    for dx in range(-r, r + 1):
        xx = lx + dx
        valid = in_roi_x & (xx >= 0) & (xx < bw)
        out += np.where(valid[None, :], acc[:, np.clip(xs + dx, x0, x1 - 1)], 0.0)
    nx = (np.clip(lx + r, 0, bw - 1) - np.clip(lx - r, 0, bw - 1) + 1)
    nx = np.where(in_roi_x, nx, 1).astype(np.float32)[None, :]
    return out / (nx * ny)


def box_mean_clamp_repeat(L, roi, r):
    """GPU：clamp 重复采样（分子含重复像素），分母 = 去重行数。"""
    x0, y0, x1, y1 = roi
    H, W = L.shape
    acc = np.zeros((H, W), dtype=np.float32)
    ys = np.arange(H)
    for dy in range(-r, r + 1):
        acc += L[np.clip(ys + dy, y0, y1 - 1), :]
    # 去重行数：floor(min(y+r, y1-0.5)) - floor(max(y-r, y0+0.5)) + 1
    yc = ys + 0.5
    ny = np.floor(np.minimum(yc + r, y1 - 0.5)) - \
        np.floor(np.maximum(yc - r, y0 + 0.5)) + 1.0
    ny = np.maximum(ny, 1.0)[:, None]
    out = np.zeros_like(L)
    xs = np.arange(W)
    for dx in range(-r, r + 1):
        out += acc[:, np.clip(xs + dx, x0, x1 - 1)]
    xc = xs + 0.5
    nx = np.floor(np.minimum(xc + r, x1 - 0.5)) - \
        np.floor(np.maximum(xc - r, x0 + 0.5)) + 1.0
    nx = np.maximum(nx, 1.0)[None, :]
    return out / (nx * ny)


def apply_spot(src, mask, L, mean, roi, k, thr_lo, thr_hi):
    """与 beauty_region.cpp:450-476 同一套施加规则。"""
    x0, y0, x1, y1 = roi
    out = src[:, :, :3].astype(np.float32).copy()
    mw = mask.astype(np.float32) / 255.0
    dev = L - mean
    ad = np.abs(dev)
    hit = (mw > 0) & (ad >= thr_lo) & (ad <= thr_hi)
    pull = np.clip(-k * mw * dev, -24.0, 24.0)
    region = np.zeros_like(hit)
    region[y0:y1, x0:x1] = True
    hit &= region
    out[hit] += pull[hit][:, None]
    out = np.clip(np.rint(out), 0, 255)
    res = src.copy()
    res[:, :, :3] = out.astype(np.uint8)
    return res, int(hit.sum())


def diff_report(a, b, roi):
    d = np.abs(a[:, :, :3].astype(np.int16) - b[:, :, :3].astype(np.int16))
    w = d.max(axis=2)
    ys, xs = np.nonzero(w)
    if not len(ys):
        return 0, 0, None, 0.0
    return int(w.max()), int(len(ys)), (int(xs.min()), int(ys.min()),
                                        int(xs.max()), int(ys.max())), \
        float(w[ys, xs].mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roi", required=True, help="x0,y0,x1,y1（对拍日志的 ROI）")
    ap.add_argument("--canvas", default="1280x720")
    ap.add_argument("--k", type=float, default=0.10)
    ap.add_argument("--r", type=int, default=2)
    ap.add_argument("--thr-lo", type=float, default=1.4)
    ap.add_argument("--thr-hi", type=float, default=30.0)
    args = ap.parse_args()

    cx, cy = [int(v) for v in args.canvas.lower().split("x")]
    roi = tuple(int(v) for v in args.roi.split(","))
    src, mask = load(cx, cy)
    L = lum_of(src)

    mean_cpu = box_mean_truncate(L, roi, args.r)
    mean_gpu = box_mean_clamp_repeat(L, roi, args.r)

    md = np.abs(mean_cpu - mean_gpu)
    x0, y0, x1, y1 = roi
    roi_mask = np.zeros_like(md, dtype=bool)
    roi_mask[y0:y1, x0:x1] = True
    nz = int(((md > 1e-6) & roi_mask).sum())
    print("窗口均值：截断 vs clamp 重复（只统计 ROI 内）")
    print("  不同的像素   : %d" % nz)
    if nz:
        ys, xs2 = np.nonzero((md > 1e-6) & roi_mask)
        print("  bbox         : (%d,%d)-(%d,%d)" % (xs2.min(), ys.min(),
                                                    xs2.max(), ys.max()))
        print("  最大均值偏差 : %.3f 灰阶" % md[roi_mask].max())

    cpu_out, cpu_hit = apply_spot(src, mask, L, mean_cpu, roi, args.k,
                                  args.thr_lo, args.thr_hi)
    gpu_out, gpu_hit = apply_spot(src, mask, L, mean_gpu, roi, args.k,
                                  args.thr_lo, args.thr_hi)

    mx, n, bbox, avg = diff_report(cpu_out, gpu_out, roi)
    print("")
    print("=== 两种窗口实现的结果差异（k=%.3f r=%d thr=[%.1f,%.1f]）==="
          % (args.k, args.r, args.thr_lo, args.thr_hi))
    print("  命中像素     : CPU %d / GPU %d" % (cpu_hit, gpu_hit))
    print("  maxdiff      : %d" % mx)
    print("  差异像素     : %d" % n)
    if bbox:
        print("  bbox         : (%d,%d)-(%d,%d)" % bbox)
        print("  非零处平均偏差: %.2f 灰阶" % avg)
        y0, y1 = roi[1], roi[3]
        print("  ROI 上下边   : y0=%d y1=%d → 边缘 r=%d 内的差异行 %s"
              % (y0, y1, args.r,
                 sorted(set([int(v) for v in np.nonzero(
                     np.abs(cpu_out[:, :, :3].astype(np.int16) -
                            gpu_out[:, :, :3].astype(np.int16)).max(axis=2))[0]]))[:6]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
