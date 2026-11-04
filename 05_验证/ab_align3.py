#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐帧对齐：动态规划求 B 的每一帧对应 A 的哪一帧。

为什么需要逐帧
--------------
OBS 以固定 30 fps 渲染、素材也按实时 30 fps 播放，两者时钟不必同相，于是
渲染过程中会周期性重复或跳过素材帧。实测两臂的同步标记间隔分别是 244 和
243 帧 —— 有一次漂移。按「分段常数偏移」对齐时，6 段里 2 段的残差高达
397/411（灰阶平方），另外 4 段只有 0.1 —— 说明偏移在段内就发生了变化。

判据
----
匹配区是画面**下半部**（桌面 + 手）：美颜只作用于脸，这块在两臂里是同一份
输入。所以对齐正确时残差应当接近 0（实测 0.1，即 RMS 0.3 灰阶），错位一帧
就是 400 量级。这个 4000 倍的差距使对齐本身可以被验证，不需要"信任"算法。

输出
----
`out/ab/align_dp_<a>_<b>.json`：`{"pairs": [[a_idx, b_idx], ...], "resid": [...]}`
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

MATCH_Y0 = 900      # 匹配区起点：下半部
FEAT = (16, 16)


def feat(g):
    return cv2.resize(g[MATCH_Y0:, :], FEAT,
                      interpolation=cv2.INTER_AREA).flatten().astype(np.float64)


def read_feats(path):
    cap = cv2.VideoCapture(path)
    out = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        out.append(feat(cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)))
    cap.release()
    return np.array(out)


def dp_align(Fa, Fb, dmin=0, dmax=9, lam=60.0):
    """返回 (pairs, resid)。pairs 是 [(a_idx, b_idx)]。

    lam 是「偏移变化一帧」的罚项。cost 单位是灰阶平方：对齐良好约 0.1，
    错一帧约 400，所以 lam=60 既能压住无意义的抖动，又允许真实漂移处切换。
    """
    na, nb = len(Fa), len(Fb)
    D = dmax - dmin + 1
    INF = 1e18
    cost = np.full((nb, D), INF)
    for i in range(nb):
        for k in range(D):
            j = i + dmin + k
            if 0 <= j < na:
                d = Fa[j] - Fb[i]
                cost[i, k] = float((d * d).mean())

    dp = cost[0].copy()
    back = np.zeros((nb, D), dtype=np.int32)
    for i in range(1, nb):
        for k in range(D):
            best, bk = dp[k], k
            if k > 0 and dp[k - 1] + lam < best:
                best, bk = dp[k - 1] + lam, k - 1
            if k < D - 1 and dp[k + 1] + lam < best:
                best, bk = dp[k + 1] + lam, k + 1
            dp[k] = cost[i, k] + best
            back[i, k] = bk

    k = int(np.argmin(dp))
    path = [k]
    for i in range(nb - 1, 0, -1):
        k = int(back[i, k])
        path.append(k)
    path.reverse()
    pairs, resid = [], []
    for i, k in enumerate(path):
        j = i + dmin + k
        if 0 <= j < na:
            pairs.append([int(j), int(i)])
            resid.append(float(cost[i, k]))
    return pairs, resid


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo.mp4")
    ap.add_argument("--b", default="rms4_combo.mp4")
    ap.add_argument("--lam", type=float, default=60.0)
    a = ap.parse_args(argv)

    Fa = read_feats(os.path.join(OUT, a.a))
    Fb = read_feats(os.path.join(OUT, a.b))
    print("A %d 帧  B %d 帧" % (len(Fa), len(Fb)))

    pairs, resid = dp_align(Fa, Fb, lam=a.lam)
    r = np.array(resid)
    offs = np.array([p[0] - p[1] for p in pairs])
    print("对齐 %d 对" % len(pairs))
    print("  残差 中位 %.3f  p90 %.3f  max %.3f  >10 的帧 %d"
          % (np.median(r), np.percentile(r, 90), r.max(), int((r > 10).sum())))
    uniq, cnt = np.unique(offs, return_counts=True)
    print("  偏移分布：%s" % dict(zip(uniq.tolist(), cnt.tolist())))
    # 平滑度为 0 才叫可信：偏移只在漂移点跳，其余时刻不变
    jumps = int((np.diff(offs) != 0).sum())
    print("  偏移变化次数 %d（%d 帧）" % (jumps, len(offs)))

    dst = os.path.join(OUT, "align_dp_%s_%s.json"
                       % (os.path.splitext(a.a)[0], os.path.splitext(a.b)[0]))
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump({"a": a.a, "b": a.b, "pairs": pairs,
                   "resid": resid, "lam": a.lam}, fh, ensure_ascii=False)
    print("写入 %s" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
