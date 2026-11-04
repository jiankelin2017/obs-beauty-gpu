#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""阶段 0：真机（采集卡）单臂的边界带时域跳变度量。

为什么是「单臂」而不是像素材源那样两臂相减
------------------------------------------
真机上人没法把同一段动作原样做两遍，所以拿不到「同一帧的开/关」。改用**静止段**：
人端坐不动时画面内容几乎恒定，此时边界带若还有跳变，就只剩两个来源 ——
传感器/编码噪声（背景区可量）和**掩码抖动**（检测框每次重检抖几像素 → 掩码平移
→ 羽化带里的 λ 整体挪一次）。这就是 §9.42 仿真片想模拟、但在真机上到底成不成立
的那个问题。

为什么按检测周期分相
--------------------
滤镜默认 `dper=7`（每 7 帧重检一次，日志 `dper=` 可读）。若边界带的跳变确实来自
「重检 → 掩码跳」，它必然锁相在帧号 mod 7 上：相位 0 的那几帧大、其余小。反之若
各相位同量级，说明跳变是连续噪声，与掩码无关。这一项比均值更能定性。

口径
----
与 §9.35 / §9.42 一致：**逐通道最大差 → 7×7 摊平 → 区域峰值**，门限 **6.02 灰阶**
（§9.42.11 的 α 标定）。真机录制是 8bit NV12 进 OBS、x264 lossless 出 mp4，
亮度通道无损，所以统计只用亮度 Y，避开 4:2:0 色度下采样。

用法
----
    py -3 05_验证/edge_live_measure.py --video out/live_edge/xxx.mp4
    py -3 05_验证/edge_live_measure.py --video X --box 850,263,200,280
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
THR = 6.02                    # §9.42.11 标定的 5 Hz 闪烁门限（灰阶）
BLUR = (7, 7)                 # 与 ab_pair_amp.band_amp 同口径
BOX = (850, 263, 200, 280)    # 2026-11-04 目视标定：发际线→下巴、左颊→右颊
SEGS = {"still": (5, 25), "slow": (28, 48), "fast": (51, 71)}
PERIOD = 7                    # 日志 dper=7


def zones(box, shape):
    """脸内 / 边界带 / 背景三块布尔掩码（与素材源那支同口径）。"""
    h, w = shape
    x, y, bw, bh = box
    x0, y0 = max(0, int(x)), max(0, int(y))
    x1, y1 = min(w - 1, int(x + bw)), min(h - 1, int(y + bh))
    if x1 - x0 < 40 or y1 - y0 < 40:
        return None, None, None
    u = np.arange(w, dtype=np.int32)[None, :]
    v = np.arange(h, dtype=np.int32)[:, None]
    inside = (u >= x0) & (u < x1) & (v >= y0) & (v < y1)
    band = 0.12 * bw
    dist = np.minimum(np.minimum(u - x0, x1 - 1 - u),
                      np.minimum(v - y0, y1 - 1 - v)).astype(np.float32)
    edge = inside & (dist < band)
    core = inside & (dist > 0.35 * bw)
    pad = 0.5 * bw
    bg = (u < x0 - pad) | (u > x1 + pad) | (v < y0 - pad) | (v > y1 + pad)
    return core, edge, bg


def reg_stats(v, thr=THR):
    v = np.asarray(v, dtype=np.float32)
    if v.size == 0:
        return {"n": 0}
    return {"n": int(v.size),
            "p50": float(np.percentile(v, 50)),
            "p90": float(np.percentile(v, 90)),
            "p99": float(np.percentile(v, 99)),
            "max": float(v.max()),
            "over_thr_pct": float((v > thr).mean() * 100.0)}


def luma(fr):
    # 用整数近似 BT.709 亮度，只做相对比较，不追求绝对准确
    b, g, r = fr[:, :, 0], fr[:, :, 1], fr[:, :, 2]
    return (0.2126 * r + 0.7152 * g + 0.0722 * b).astype(np.float32)


def run(video, box, segs, period, thr=THR, skip=10):
    cap = cv2.VideoCapture(video)
    if not cap.isOpened():
        raise SystemExit("打不开 %s" % video)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    W = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    H = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    core, edge, bg = zones(box, (H, W))
    if core is None:
        raise SystemExit("脸框越界")

    # 逐帧只算一次 Y，按段落收集
    want = {}
    for name, (t0, t1) in segs.items():
        for i in range(int(t0 * fps), min(int(t1 * fps), n)):
            want[i] = name

    acc = {s: {"core": [], "edge": [], "bg": []} for s in segs}
    phase = {s: {} for s in segs}       # 相位 -> edge 峰值列表
    trace = []
    prev = None
    i = 0
    while True:
        ok, fr = cap.read()
        if not ok:
            break
        if i not in want:
            prev = None          # 段落外不跨段求差
            i += 1
            continue
        seg = want[i]
        y = luma(fr)
        if prev is not None and prev[0] == i - 1 and prev[1] == seg:
            d = cv2.blur(np.abs(y - prev[2]).astype(np.float32), BLUR)
            if i % int(fps) >= skip or (i - int(segs[seg][0] * fps)) >= skip:
                acc[seg]["core"].append(float(d[core].max()))
                acc[seg]["edge"].append(float(d[edge].max()))
                acc[seg]["bg"].append(float(d[bg].max()))
                ph = i % period
                phase[seg].setdefault(ph, []).append(float(d[edge].max()))
                if seg == "still":
                    trace.append({"i": i, "t": round(i / fps, 2), "ph": ph,
                                  "core": round(float(d[core].max()), 2),
                                  "edge": round(float(d[edge].max()), 2),
                                  "bg": round(float(d[bg].max()), 2)})
        prev = (i, seg, y)
        i += 1
    cap.release()

    rep = {"video": os.path.basename(video), "size": [W, H], "fps": fps, "frames": n,
           "box": list(box), "thr": thr, "period": period, "segs": {},
           "phase": {}, "trace": trace}
    for s in segs:
        rep["segs"][s] = {k: reg_stats(v) for k, v in acc[s].items()}
        rep["phase"][s] = {int(k): float(np.mean(v)) for k, v in phase[s].items()}
    return rep


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--box", default="%d,%d,%d,%d" % BOX)
    ap.add_argument("--period", type=int, default=PERIOD)
    ap.add_argument("--tag", default="live")
    a = ap.parse_args(argv)

    box = tuple(float(x) for x in a.box.split(","))
    rep = run(a.video, box, SEGS, a.period)
    os.makedirs(os.path.join(OUT, "logs"), exist_ok=True)
    jp = os.path.join(OUT, "logs", "edge_live_%s.json" % a.tag)
    with open(jp, "w", encoding="utf-8") as fh:
        json.dump(rep, fh, ensure_ascii=False, indent=2)

    print("=" * 74)
    print("%s  %dx%d  %.0f fps  框 %s  门限 %.2f 灰阶"
          % (rep["video"], rep["size"][0], rep["size"][1], rep["fps"], box, THR))
    print("%-6s %-6s %6s %7s %7s %7s %7s %8s"
          % ("段", "区域", "n", "p50", "p90", "p99", "max", ">门限%"))
    for s in SEGS:
        for k, nm in (("core", "脸内"), ("edge", "边界带"), ("bg", "背景")):
            st = rep["segs"][s][k]
            if not st.get("n"):
                print("%-6s %-6s %6s" % (s, nm, "无样本"))
                continue
            print("%-6s %-6s %6d %7.2f %7.2f %7.2f %7.2f %7.1f%%"
                  % (s, nm, st["n"], st["p50"], st["p90"], st["p99"], st["max"],
                     st["over_thr_pct"]))
    print("\n按帧号 mod %d 分相的边界带均值（判跳变是否锁相在检测节奏上）：" % a.period)
    for s in SEGS:
        ph = rep["phase"][s]
        if not ph:
            continue
        mx = max(ph.values()) or 1.0
        bar = "  ".join("%d:%5.2f" % (k, ph[k]) for k in sorted(ph))
        top = max(ph, key=ph.get)
        print("  %-6s %s   ← 峰值相位 %d（%.2f）" % (s, bar, top, ph[top]))
        print("         相位差 %.2f 倍（最大/最小）"
              % (mx / max(1e-6, min(ph.values()))))
    print("→ %s" % jp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
