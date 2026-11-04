#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""P2-6 路 A 离线评估：把「帧数预算」换成「位移预算」扫一遍。

背景（PROGRESS §9.43）
----
引擎里「判动」与「掩码更新时机」是同一个决定（`beauty_core.cpp:2658`），
rms 省下的就是提交次数；而代价的单位是 **像素**（掩龄 × 每帧位移），
不是帧。所以真正的中间路不是「换个聚合口径」，而是 **静止期给位移设上限**：
累计运动量超过预算就提前提交，画面真冻住时仍按 `detAdaptStill` 慢慢来。

三条规则族
----
    R1  hot 看 max，预算也累计 max   —— 只换预算单位，判动口径不变
    R2  hot 看 rms，预算累计 max     —— 省回读（rms 的好处）+ 位移兜底（安全网）
    R3  hot 看 rms，预算也累计 rms   —— 全 rms，预算也按 rms 记

    budget = inf 时 R1 必须逐值复现已发布的 `max` 行、R3 必须复现 `rms` 行
    （§9.34 的表：max 判动 52.5% / 偏移 p90 9.05 / 超羽化 0.5%；
     rms 10.2% / 16.84 / 7.9%）。复现不了就说明本脚本的状态机写错了，
     **此时任何新数字都不作数** —— 脚本会直接把断言打出来。

指标
----
    提交率      —— 真正的成本货币（每次提交 = 一次 YuNet + 一次 1.6 MB 回读）
    掩龄        —— 掩码描述的是几帧前的画面
    偏移 px     —— 本帧该用的掩码位置 与 实际用的那份 的距离（代价本身）
    超羽化%     —— 偏移超过羽化宽度的帧占比（§9.34 的可见性代理）

用法
----
    py -3 mask_budget_sweep.py                     # 默认全片，扫一串预算
    py -3 mask_budget_sweep.py --limit 600         # 先粗看
    py -3 mask_budget_sweep.py --refresh           # 重算检测（默认读缓存）
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import cv2                       # noqa: E402
import face_box_drift as D       # noqa: E402
import mask_lag_sim as L         # noqa: E402  状态机基线（必须与它逐值一致）
from motion_model import motion_small  # noqa: E402

CACHE = os.path.join(HERE, "out", "budget_cache.npz")


# ---------------------------------------------------------------- 数据准备
def load_series(path, limit, interp, refresh=False):
    """逐帧（检测框 / max 判定值 / rms 判定值 / 脸宽 / fps），带 npz 缓存。"""
    key = "%s|%d|%s" % (os.path.basename(path), limit, interp)
    if not refresh and os.path.exists(CACHE):
        z = np.load(CACHE, allow_pickle=True)
        if str(z["key"]) == key:
            return (list(z["boxes"]), z["vm"], z["vr"],
                    float(z["fw"]), float(z["fps"]))

    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit("打不开：%s" % path)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    cap.release()

    n, boxes, vm, vr, fw = L.run(path, limit, 6, interp)
    z = {"key": key, "boxes": np.array(boxes, dtype=object),
         "vm": np.asarray(vm), "vr": np.asarray(vr),
         "fw": fw, "fps": fps}
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    np.savez(CACHE, **z)
    return boxes, np.asarray(vm), np.asarray(vr), fw, fps


# ---------------------------------------------------------------- 状态机
def simulate_budget(v_hot, v_acc, thr, still, hot_n, budget):
    """detectPeriod 的状态机 + 位移预算。budget=inf 时退化成原规则。

    与原版（mask_lag_sim.simulate）唯一的差别是第三个「提交」理由：
    静止期累计的 `v_acc` 超过 budget → 本帧照样提交。累计量在提交时清零。
    """
    lag, age, hot = [], 0, 0
    acc = 0.0
    for vh, va in zip(v_hot, v_acc):
        over = (acc + va) > budget
        every = 0 if (vh > thr or hot > 0 or over) else still
        if vh > thr:
            hot = hot_n
        elif hot > 0:
            hot -= 1
        if age >= every:
            age = 0
            acc = 0.0
            lag.append(1)
        else:
            age += 1
            acc += va            # 没提交的这一帧，位移照样累积
            lag.append(age + 1)
    return lag


def score(boxes, lag, feather):
    """lag → (掩龄分位, 偏移 px 分位, 超羽化%)。与 mask_lag_sim.report 同口径。"""
    off = 1
    dxy = []
    for j, lg in enumerate(lag):
        t = j + off
        src = t - lg
        if src < 0 or t >= len(boxes):
            continue
        p, q = boxes[src], boxes[t]
        if p is None or q is None:
            continue
        dxy.append(((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5)
    lag = np.asarray(lag, dtype=float)
    dxy = np.asarray(dxy, dtype=float)
    return {
        "lag_p50": float(np.percentile(lag, 50)),
        "lag_p90": float(np.percentile(lag, 90)),
        "lag_max": int(lag.max()),
        "d_p50": float(np.percentile(dxy, 50)),
        "d_p90": float(np.percentile(dxy, 90)),
        "d_p99": float(np.percentile(dxy, 99)),
        "over": 100.0 * float((dxy > feather).mean()),
        "dxy": dxy,
    }


def submit_rate(lag, fps):
    """提交率：lag==1 的帧就是「本帧刚提交」的那一批（异步 1 帧）。"""
    n = len(lag)
    return 100.0 * sum(1 for x in lag if x <= 1) / max(1, n)


# ---------------------------------------------------------------- 主流程
def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", default=L.DEFAULT)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--interp", default="linear", choices=("linear", "area"))
    ap.add_argument("--thr", type=int, default=6)
    ap.add_argument("--still", type=int, default=8, help="detAdaptStill（面板语义）")
    ap.add_argument("--refresh", action="store_true")
    ap.add_argument("--budgets", nargs="*", type=float,
                    default=[8, 12, 16, 20, 25, 30, 40, 55, 75, 110, 160, 240])
    a = ap.parse_args(argv)

    boxes, vm, vr, fw, fps = load_series(a.file, a.limit, a.interp, a.refresh)
    still = max(0, a.still - 1)
    feather = D.FEATHER_K * fw
    n = len(vm)

    print("== %s  %d 帧 @ %.1f fps，脸宽中位 %.0f px，羽化 %.1f px，thr=%d，still=%d"
          % (os.path.basename(a.file), n, fps, fw, feather, a.thr, still))
    print("   预算单位 = 累计的块差（1 px 平移约 8.1，见 beauty_core.cpp 的标定注释）")

    # ---- 自检：budget=inf 必须逐值复现已发布的两行 ----
    print("\n-- 自检（budget=inf 必须复现 §9.34 的 max / rms 两行）--")
    for lab, vh, va in (("max", vm, vm), ("rms", vr, vr)):
        mine = simulate_budget(vh, va, a.thr, still, L.HOT, float("inf"))
        ref = L.simulate(list(vh), a.thr, still)
        same = (list(mine) == list(ref))
        r = score(boxes, mine, feather)
        hot = 100.0 * float((np.asarray(vh) > a.thr).mean())
        print("   %-4s 判动 %5.1f%% | 提交 %5.1f%% | 掩龄 %2.0f/%2.0f/%2d | "
              "偏移 %5.2f/%6.2f px | 超羽化 %.1f%% | 与原版逐值一致: %s"
              % (lab, hot, submit_rate(mine, fps), r["lag_p50"], r["lag_p90"],
                 r["lag_max"], r["d_p50"], r["d_p90"], r["over"],
                 "是" if same else "**否**"))
        if not same:
            raise SystemExit("状态机与原版不一致，结论不可信 —— 先修脚本")

    # ---- 扫预算 ----
    rules = (("R1 max/预算max", vm, vm), ("R2 rms/预算max", vr, vm),
             ("R3 rms/预算rms", vr, vr))
    for name, vh, va in rules:
        print("\n-- %s（判动 %5.1f%%）--" % (name, 100.0 * float((np.asarray(vh) > a.thr).mean())))
        print("   %-8s %-8s %-7s %-13s %-19s %s"
              % ("预算", "≈px", "提交率", "掩龄 50/90/max", "偏移 50/90/99 px", "超羽化"))
        base = simulate_budget(vh, va, a.thr, still, L.HOT, float("inf"))
        rows = [(float("inf"), base)]
        for b in a.budgets:
            rows.append((b, simulate_budget(vh, va, a.thr, still, L.HOT, b)))
        for b, lag in rows:
            r = score(boxes, lag, feather)
            tag = "∞（=现状）" if b == float("inf") else "%6.1f" % b
            px = "  —  " if b == float("inf") else "%5.1f" % (b / 8.1)
            print("   %-8s %-8s %5.1f%%  %2.0f/%2.0f/%2d      %5.2f/%6.2f/%6.2f   %5.1f%%"
                  % (tag, px, submit_rate(lag, fps), r["lag_p50"], r["lag_p90"],
                     r["lag_max"], r["d_p50"], r["d_p90"], r["d_p99"], r["over"]))

    print("\n读法：目标线是 **偏移 p90 ≈ 9.05 px（= max 现状）**；"
          "提交率越低越省往返。")
    print("      只有当「p90 ≤ 9 px 且提交率远低于 max 的 52.5%+」同时成立，"
          "这条路才值得上真机。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
