# -*- coding: utf-8 -*-
"""把「掩码旧了几帧」换算成「掩码错了多少像素」（2026-11-04，2.12.0 / P1-1c 结账用）。

为什么需要它
------------
2.12.0 把 16 块的合并口径从 `max` 换成 `RMS`，真实说话素材上：
    · 判动率 42.7% → 7.1%
    · 提交次数 1195 → 422
    · `nopix` 33.6% → 76.5%
    · 往返中位 **1.57 → 0.86 ms（−45%）**
代价也随之出现，而且日志里看得很清楚：
    · `align%`（掩码与本帧同源的帧占比，等价于提交率）**68.5% → 23.5%**

`align%` 只说「掩码更新的频率低了」，不说「低到什么程度会看出来」。
真正要回答的是：**这一帧用的掩码，相对本帧应有的位置，偏了多少像素？**

方法（全离线，不启 OBS）
------------------------
在同一段素材上同时跑三件事，口径与引擎逐条对齐：
    1. 逐帧 YuNet 检测框（`face_box_drift.py` 同一套 letterbox / 阈值 / NMS）；
    2. 逐帧 64×64 运动小图（`motion_model.py` 那条唯一实现）；
    3. `detectPeriod` + `detectDue` 的状态机复现（含 `detAdaptHot` 续期与
       `maskAge_ >= every` 的 off-by-one），分别喂 max / rms 两种判定值。
于是每帧都有一条「这份掩码描述的是 t − lag 帧」的 lag 序列，把 lag 落到
第 1 步的框序列上，就得到**掩码位置偏差**。与掩码羽化宽度（0.08 × 脸宽，
源码 `kMaskFeatherRatio`）比：
偏差远小于羽化宽度 = 渐变带吃掉它；远超 = 掩码边界会跟错位置。

用法
----
    py -3 mask_lag_sim.py                        # 默认 cam_real 全片
    py -3 mask_lag_sim.py --limit 600            # 只看前 600 帧
    py -3 mask_lag_sim.py --thr 6 --interp linear
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import face_box_drift as D          # noqa: E402  检测口径（唯一实现）
from motion_model import motion_small  # noqa: E402

DEFAULT = os.path.join(HERE, "assets", "cam_real_1080x1920.mp4")
GRID, BLK = 16, 4
STILL = 7          # detAdaptStill 8 → still = detAdaptStill - 1
HOT = 3            # detAdaptHot


def metric_pair(prev, cur):
    """两张 64×64 亮度图 → (max 口径, rms 口径) 的判定值。"""
    step = cur.shape[0] // GRID
    d = np.abs(cur.astype(np.int32) - prev.astype(np.int32)).astype(np.float64)
    d = d[:step * GRID, :step * GRID]
    cells = d.reshape(GRID, step, GRID, step).max(axis=(1, 3))
    blocks = cells.reshape(GRID // BLK, BLK, GRID // BLK, BLK).mean(axis=(1, 3))
    flat = blocks.ravel()
    return float(flat.max()), float(np.sqrt(np.mean(flat * flat)))


def simulate(vals, thr, still=STILL):
    """detectPeriod + detectDue 的状态机。返回每帧的 lag（掩码描述的是 t-lag 帧）。

    约定：lag = maskAge_ + 1 —— 提交发生在 maskAge_==every 那一帧，worker 下一帧
    才到货，所以「本帧拿到的掩码」描述的是上一次提交那一帧，再往回差一个异步帧。
    `still` 是引擎里那个 every 值，= detAdaptStill - 1。
    """
    lag, age, hot = [], 0, 0
    for v in vals:
        every = 0 if (v > thr or hot > 0) else still
        if v > thr:
            hot = HOT
        elif hot > 0:
            hot -= 1
        if age >= every:
            age = 0
            lag.append(1)          # 本帧刚提交，用的是上一帧那份（异步 1 帧）
        else:
            age += 1
            lag.append(age + 1)
        if every == 0 and age == 0:
            lag[-1] = 1
    return lag


def run(path, limit, thr, interp):
    model = D.find_model()
    det = cv2.FaceDetectorYN.create(model, "", (D.DET_SIDE, D.DET_SIDE),
                                    D.CONF, D.NMS, 5000)
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit("打不开：%s" % path)
    boxes, vals_max, vals_rms = [], [], []
    prev = None
    n = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        n += 1
        if limit and n > limit:
            break
        boxes.append(D.detect_boxes(det, frame))
        cur = motion_small(frame, interp)
        if prev is not None:
            a, b = metric_pair(prev, cur)
            vals_max.append(a)
            vals_rms.append(b)
        prev = cur
    cap.release()
    widths = [b[2] for b in boxes if b is not None]
    fw = float(np.median(widths)) if widths else float("nan")
    return n, boxes, vals_max, vals_rms, fw


def report(name, boxes, vals, thr, fw, still=STILL):
    lag = simulate(vals, thr, still)
    # boxes[i] 对应第 i 帧；判定值从第 1 帧才有 → vals[j] 对应第 j+1 帧
    off = 1
    dxy, iou = [], []
    for j, lg in enumerate(lag):
        t = j + off
        src = t - lg
        if src < 0 or t >= len(boxes):
            continue
        p, q = boxes[src], boxes[t]
        if p is None or q is None:
            continue
        dxy.append(((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5)
        iou.append(D._iou(p, q))
    feather = D.FEATHER_K * fw
    hot = 100.0 * sum(1 for v in vals if v > thr) / max(1, len(vals))
    print("  %-4s 判动 %5.1f%% | 掩龄 p50 %4.1f p90 %4.1f max %2d | "
          "偏差 p50 %5.2f p90 %5.2f p99 %5.2f max %6.2f px | "
          "超羽化 %.1f%% | IoU p50 %.3f"
          % (name, hot,
             float(np.percentile(lag, 50)), float(np.percentile(lag, 90)),
             int(max(lag)),
             float(np.percentile(dxy, 50)), float(np.percentile(dxy, 90)),
             float(np.percentile(dxy, 99)), max(dxy),
             100.0 * sum(1 for v in dxy if v > feather) / max(1, len(dxy)),
             float(np.percentile(iou, 50))))
    return {"hot": hot, "lag": lag, "dxy": dxy, "iou": iou}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", default=DEFAULT)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--thr", type=int, default=6)
    ap.add_argument("--interp", default="linear", choices=("linear", "area"))
    ap.add_argument("--json", default="",
                    help="把逐帧 lag/偏差 落盘，供 mask_off_probe 的曲线做加权")
    ap.add_argument("--stills", nargs="*", type=int, default=[8],
                    help="要扫的 detAdaptStill（面板语义 8/6/4…），可给多个")
    args = ap.parse_args(argv)

    n, boxes, vm, vr, fw = run(args.file, args.limit, args.thr, args.interp)
    feather = D.FEATHER_K * fw
    print("== %s" % os.path.basename(args.file))
    print("   %d 帧，脸宽中位 %.0f px，羽化宽度 %.1f px，阈值 %d，%s 缩放"
          % (n, fw, feather, args.thr, args.interp))
    print("   「偏差」= 本帧该用的掩码位置 与 实际用的那份（lag 帧前）之间的距离")
    r_save = {}
    for st in args.stills:
        print("\n-- detAdaptStill = %d --" % st)
        for lab, vv in (("max", vm), ("rms", vr)):
            r_save["%s@%d" % (lab, st)] = report(lab, boxes, vv, args.thr, fw, st - 1)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"file": os.path.basename(args.file), "frames": n,
                       "face_w": fw, "feather": feather, "thr": args.thr,
                       "interp": args.interp,
                       "arms": {k: {"hot": v["hot"],
                                    "lag": [int(x) for x in v["lag"]],
                                    "dxy": [float(x) for x in v["dxy"]]}
                                for k, v in r_save.items()}},
                      fh, ensure_ascii=False)
        print("\n   逐帧数据写入 %s" % args.json)
    print()
    print("读法：把「超羽化」看成「这一帧的掩码边界明显跟错了位置」的比例。")
    print("      羽化 %.1f px 是渐变带的宽度，带内的偏移会被抹平；" % feather)
    print("      超出它的那部分才可能被看见。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
