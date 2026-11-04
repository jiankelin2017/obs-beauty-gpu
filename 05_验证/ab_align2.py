#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两臂录制的精确对齐（序列互相关版）。

为什么不用单帧匹配
------------------
第一版对每一帧单独在 ±4 帧里找最像的，结果偏移在 1/2/3 之间随机跳，
最佳与次优只差 1.41 倍 —— 这段素材说话动作慢，相邻帧本身就几乎一样，
单帧无法唯一确定。而错 1 帧就足以让「嘴型」这类快动作产生巨大假差异。

做法
----
把每帧压成一个低维特征（非脸区的 4×8 块均值，32 维），然后**按序列**求
让 Σ‖F_A[i+δ] − F_B[i]‖² 最小的 δ。几十帧的联合模式是唯一的，误差曲线
会有一个明确的谷底；谷底与次谷的比值就是对齐的置信度。

非脸区仍然是关键：美颜只动脸，背景/字幕/桌面是同一输入，用它们对齐不会
把要测的差异也最小化掉。
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

# 匹配区：画面**下半部**。上面是脸/脖子（美颜会动它），下面是桌面与手 ——
# 手在动、桌面静止，美颜完全不碰，正合适做互相关。第一版把 4×8 网格铺满
# 全画面并剔除脸区，结果几乎每格都跟脸重叠，特征全是 NaN。
MATCH_Y0 = 900
FEAT = (16, 16)      # 匹配区降采样后的特征网格


def feat(g):
    sub = g[MATCH_Y0:, :]
    s = cv2.resize(sub, FEAT, interpolation=cv2.INTER_AREA)
    return s.flatten().astype(np.float64)


def read_feats(path, limit=0):
    cap = cv2.VideoCapture(path)
    feats, grays = [], []
    i = 0
    while True:
        ok, f = cap.read()
        if not ok:
            break
        g = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
        feats.append(feat(g))
        grays.append(g)
        i += 1
        if limit and i >= limit:
            break
    cap.release()
    return np.array(feats), grays


def score(Fa, Fb, delta, na, nb, b0=0):
    """比较 Fb[b0 : b0+nb] 与 Fa[b0+delta : b0+delta+nb]，返回平均平方差。"""
    ia = np.arange(nb) + delta + b0
    ok = (ia >= 0) & (ia < na)
    if ok.sum() < 20:
        return float("inf"), 0
    seg = Fb[b0:b0 + nb]
    d = Fa[ia[ok]] - seg[ok]
    return float((d * d).mean()), int(ok.sum())


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo.mp4")
    ap.add_argument("--b", default="rms4_combo.mp4")
    ap.add_argument("--span", type=int, default=12)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args(argv)

    pa, pb = os.path.join(OUT, a.a), os.path.join(OUT, a.b)
    Fa, ga = read_feats(pa, a.limit)
    Fb, gb = read_feats(pb, a.limit)
    na, nb = len(Fa), len(Fb)
    print("A %d 帧  B %d 帧  特征 %d 维（非脸区）" % (na, nb, Fa.shape[1]))

    curve = {}
    for d in range(-a.span, a.span + 1):
        s, k = score(Fa, Fb, d, na, nb)
        curve[d] = s
    best = min(curve, key=curve.get)
    # 次优：离开最优点至少 2 帧的最小值，避免相邻帧把谷底摊平
    second = min((v for k, v in curve.items() if abs(k - best) >= 2), default=float("inf"))
    print("最优偏移 δ=%d（A 的第 i+δ 帧 对应 B 的第 i 帧）" % best)
    print("  误差 %.5f，次优 %.5f，比值 %.2fx" % (curve[best], second,
                                                second / max(1e-12, curve[best])))
    near = sorted(curve.items(), key=lambda kv: kv[1])[:5]
    print("  最近 5 个：%s" % [(k, round(v, 5)) for k, v in near])

    # 线段级校验：把序列切 4 段，各自求最优，看是否一致（一致性 = 没有中途漂移）
    segs = []
    NSEG = 6
    for s0 in range(NSEG):
        lo, hi = s0 * nb // NSEG, (s0 + 1) * nb // NSEG
        sub = {}
        for d in range(best - 4, best + 5):
            v, k = score(Fa, Fb, d, na, hi - lo, lo)
            sub[d] = v
        b2 = min(sub, key=sub.get)
        sd = sorted(sub.values())
        ratio = sd[1] / max(1e-12, sd[0]) if len(sd) > 1 else float("nan")
        segs.append(int(b2))
        print("  段%d（帧 %3d..%3d）：偏移 %+d，误差 %.1f，次优 %.1f（%.2fx）"
              % (s0, lo, hi - 1, b2, sub[b2], sd[1], ratio))
    stable = len(set(segs)) == 1

    dst = os.path.join(OUT, "align2_%s_%s.json"
                       % (os.path.splitext(a.a)[0], os.path.splitext(a.b)[0]))
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump({"a": a.a, "b": a.b, "delta": int(best),
                   "curve": {str(k): v for k, v in curve.items()},
                   "seg_best": segs, "stable": stable}, fh, ensure_ascii=False,
                  indent=2)
    print("分段一致：%s；写入 %s" % ("是" if stable else "否", dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
