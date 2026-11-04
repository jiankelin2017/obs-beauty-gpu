# -*- coding: utf-8 -*-
"""引擎「运动判定」的 CPU 复算 —— 全库唯一一份实现（2026-11-04，2.11.1）。

为什么要单独成模块
------------------
这条链有三处极易写错的地方，散在多个脚本里必然漂：

1. **亮度**：`(54r + 183g + 19b) >> 8`，与 `beauty_core.cpp` 里那行逐位相同（BT.601
   的整数近似）。用 `cv2.cvtColor(..., COLOR_BGR2GRAY)` 会差 1–2 级。
2. **缩放是两段**：宿主先把整帧 letterbox 进 640×640（`scale = min(640/W, 640/H)`，
   居中，四周补黑，见 `obs-beauty.cpp:5339`），方案C 再把这张 640×640 缩到 64×64。
   直接 720p→64×64 是**另一条数学**：内容铺满全图、格内像素数从 16 变成 400，
   算出来的噪声底会低一个数量级。
3. **聚合**：16×16 格 → 每格取「逐像素 |Δ| 的最大值」（B 口径，2.11.0 现状）或
   均值（C 口径，已否决）→ 4×4 格为一块取均值 → 16 个块再聚合。
   **与阈值 6 比的就是这个数**（`detectPeriod` 里 `res > thr`）。
   块聚合口径由 `agg` 决定（2.12.0 新增，对应开关 `BEAUTY_DET_ADAPT_AGG`）：
     · `max` —— 16 块取最大（2.11.x 现状，默认）
     · `rms` —— sqrt(Σm²/16)（2.12.0 候选，对局部微动不敏感）
   四种口径的对比见 `calib_agg.py` 与 `PROGRESS §9.32.7`。

第二段缩放有两种近似，结果差得不小，两个都给出来：
  · `linear` —— GPU 双线性 10:1 缩小不做 box 平均，采样点稀疏，**噪声保留得多**，
    这是**保守侧**（真实引擎走这条）；
  · `area`   —— 理想 box 平均，噪声被压 √100 倍，是**乐观侧**（给个下界对照）。
两边都过阈值，才算真的过。

用法
----
    from motion_model import motion_small, metric, predict
    res = predict("assets/drop_k1_720p.avi", interp="linear")
    print(res["hot_ratio"])
"""

from __future__ import annotations

import cv2
import numpy as np

THUMB_SIDE = 640      # 宿主缩略图边长（kThumbSide）
MOTION_SIDE = 64      # 方案C 运动小图边长（kMotionSide）
GRID = 16             # 16×16 格
BLK = 4               # 4×4 格 = 一块
DEFAULT_THR = 6       # detAdaptThr 默认值


def luma(frame):
    """BGR uint8 → 亮度 int32，与引擎逐位相同。"""
    b = frame[:, :, 0].astype(np.int32)
    g = frame[:, :, 1].astype(np.int32)
    r = frame[:, :, 2].astype(np.int32)
    return (54 * r + 183 * g + 19 * b) >> 8


def letterbox(frame, side=THUMB_SIDE):
    """整帧 letterbox 进 side×side，四周补黑 —— 与宿主 beauty_thumb_build 同算法。"""
    h, w = frame.shape[:2]
    scale = min(side / float(w), side / float(h))
    nw = min(int(w * scale + 0.5), side)
    nh = min(int(h * scale + 0.5), side)
    ox = (side - nw) // 2
    oy = (side - nh) // 2
    out = np.zeros((side, side, 3), np.uint8)
    if nw > 0 and nh > 0:
        out[oy:oy + nh, ox:ox + nw] = cv2.resize(
            frame, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return out


def motion_small(frame, interp="linear"):
    """整帧 → 引擎判定用的 64×64 亮度小图。"""
    lb = letterbox(frame)
    flag = cv2.INTER_AREA if interp == "area" else cv2.INTER_LINEAR
    sm = cv2.resize(lb, (MOTION_SIDE, MOTION_SIDE), interpolation=flag)
    return luma(sm)


def metric(prev, cur, mode="max", agg="max"):
    """两张 64×64 亮度图 → 与阈值比较的那个数。

    mode 控「格内统计」（max = 2.11.x 现状；mean = 已否决的 C 口径）。
    agg  控「16 个块怎么合并成一个数」（max / rms / top4 / median）。
    引擎里 `BEAUTY_DET_ADAPT_AGG` 只切 agg，不切 mode。
    """
    d = np.abs(cur.astype(np.int32) - prev.astype(np.int32)).astype(np.float64)
    bh = MOTION_SIDE // GRID
    bw = MOTION_SIDE // GRID
    d = d[:bh * GRID, :bw * GRID]
    cells = d.reshape(GRID, bh, GRID, bw)
    if mode == "mean":
        cells = cells.mean(axis=(1, 3))
    else:
        cells = cells.max(axis=(1, 3))
    blocks = cells.reshape(GRID // BLK, BLK, GRID // BLK, BLK).mean(axis=(1, 3))
    flat = blocks.ravel()
    if agg == "rms":
        return float(np.sqrt(np.mean(flat * flat)))
    if agg == "top4":
        return float(np.sort(flat)[-4:].mean())
    if agg == "median":
        return float(np.median(flat))
    return float(flat.max())


def predict_frames(frames, interp="linear", mode="max", agg="max"):
    """逐帧序列 → 每帧的判定值（第一帧没有前驱，跳过）。"""
    res, prev = [], None
    for f in frames:
        cur = motion_small(f, interp)
        if prev is not None:
            res.append(metric(prev, cur, mode, agg))
        prev = cur
    return res


def predict(path, interp="linear", mode="max", agg="max", thr=DEFAULT_THR,
            limit=None):
    """读一个视频文件，返回判定值序列与统计。

    `hot_ratio` 就是 OBS 日志里 `判动 hit/calls` 的离线预测值：
    判定值超过阈值 = 这一帧会被判成「在动」= 这一帧不会省掉回读。
    """
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        return {"path": path, "error": "打不开"}
    res, prev, n = [], None, 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        n += 1
        if limit and n > limit:
            break
        cur = motion_small(frame, interp)
        if prev is not None:
            res.append(metric(prev, cur, mode, agg))
        prev = cur
    cap.release()
    if not res:
        return {"path": path, "frames": n, "error": "帧数不足"}
    arr = np.asarray(res, dtype=np.float64)
    hot = int((arr > thr).sum())
    return {
        "path": path,
        "frames": n,
        "res": res,
        "p50": float(np.percentile(arr, 50)),
        "p90": float(np.percentile(arr, 90)),
        "mx": float(arr.max()),
        "hot": hot,
        "calls": len(res),
        "hot_ratio": hot / float(len(res)),
    }


def fmt_pred(r, key_ratio="hot_ratio"):
    """一行的短格式：判动% / p50 / p90 / max。"""
    if "error" in r:
        return r["error"]
    return "%5.1f%%  %5.1f %5.1f %5.1f" % (
        r[key_ratio] * 100.0, r["p50"], r["p90"], r["mx"])
