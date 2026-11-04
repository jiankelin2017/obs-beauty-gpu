#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从两臂录制里挑关键帧，出「人眼对照」用的静态图。

挑帧依据：逐帧 `≥4 灰阶` 覆盖面积。取最差的几帧 + 中位帧 + 一帧几乎没有差异的，
三种放在一起，才能回答「是每帧都看得出来，还是只有个别帧」。

输出（`out/ab/keys/`）：
  kNN_side.png    左 A 右 B，脸区各放大 2 倍（最接近"实际看画面"的尺度）
  kNN_A.png / B   单臂原尺寸脸区（盲看用：不看标签也能比较两张）
  kNN_diff.png    绝对差 ×20 热图（物理量，不是给人看美观的）
  kNN_diff_x3.png 绝对差 ×3（把 4 灰阶放大到肉眼可辨，但不夸张）
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
KEYS = os.path.join(OUT, "keys")

sys.path.insert(0, HERE)
import ab_compare as C  # noqa: E402

# 脸 + 脖子，含边界带上下缘
CROP = (150, 720, 60, 780)


def imwrite_u(path, img):
    """cv2.imwrite 对非 ASCII 路径会静默失败（返回 False 不报错），
    项目路径里带中文，所以统一走 imencode + 二进制写。"""
    ext = os.path.splitext(path)[1] or ".png"
    ok, buf = cv2.imencode(ext, img)
    if not ok:
        raise RuntimeError("编码失败：%s" % path)
    with open(path, "wb") as fh:
        fh.write(buf.tobytes())
    return path


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", nargs=2, default=["max", "rms4"])
    ap.add_argument("--cfg", default="combo")
    ap.add_argument("--limit", type=int, default=236)
    ap.add_argument("--topn", type=int, default=3)
    a = ap.parse_args(argv)

    files = {arm: os.path.join(OUT, "%s_%s.mp4" % (arm, a.cfg)) for arm in a.arms}
    marks = {arm: C.find_mark(p) for arm, p in files.items()}
    print("标记帧：%s" % marks)
    if any(v < 0 for v in marks.values()):
        sys.exit("找不到同步标记")

    fa = C.read_frames(files[a.arms[0]], marks[a.arms[0]] + 3, a.limit)
    fb = C.read_frames(files[a.arms[1]], marks[a.arms[1]] + 3, a.limit)
    n = min(len(fa), len(fb))
    print("分析 %d 帧" % n)

    x0, x1, y0, y1 = CROP
    area = float((y1 - y0) * (x1 - x0))
    rows = []
    for i in range(n):
        da = C.gray(fa[i]).astype(np.int16)
        db = C.gray(fb[i]).astype(np.int16)
        d = np.abs(da - db)
        sub = d[y0:y1, x0:x1]
        rows.append({"i": i,
                     "a4": 100.0 * float((sub >= 4).sum()) / area,
                     "a8": 100.0 * float((sub >= 8).sum()) / area,
                     "mx": int(sub.max()),
                     "mean": float(sub.mean())})
    order = sorted(rows, key=lambda r: -r["a4"])

    picks = []
    for r in order[:a.topn]:
        picks.append(("worst%d" % (len(picks) + 1), r))
    med = order[len(order) // 2]
    picks.append(("median", med))
    quiet = order[-1]
    picks.append(("quiet", quiet))

    os.makedirs(KEYS, exist_ok=True)
    cv2.putText  # noqa: B018

    def lab(img, text, scale=1.4):
        v = img.copy()
        cv2.putText(v, text, (16, 46), cv2.FONT_HERSHEY_SIMPLEX, scale,
                    (0, 0, 0), 6, cv2.LINE_AA)
        cv2.putText(v, text, (16, 46), cv2.FONT_HERSHEY_SIMPLEX, scale,
                    (255, 255, 255), 2, cv2.LINE_AA)
        return v

    x0, x1, y0, y1 = CROP
    for tag, r in picks:
        i = r["i"]
        ca = fa[i][y0:y1, x0:x1]
        cb = fb[i][y0:y1, x0:x1]
        imwrite_u(os.path.join(KEYS, "k%02d_%s_A.png" % (i, tag)), ca)
        imwrite_u(os.path.join(KEYS, "k%02d_%s_B.png" % (i, tag)), cb)

        za = cv2.resize(ca, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST)
        zb = cv2.resize(cb, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST)
        side = np.hstack([lab(za, "A"), lab(zb, "B")])
        cv2.line(side, (za.shape[1], 0), (za.shape[1], za.shape[0]),
                 (255, 255, 255), 3)
        imwrite_u(os.path.join(KEYS, "k%02d_%s_side.png" % (i, tag)), side)

        d = np.abs(C.gray(fa[i]).astype(np.int16) - C.gray(fb[i]).astype(np.int16))
        sub = d[y0:y1, x0:x1]
        for mult, name in ((20, "diff"), (3, "diff_x3")):
            v = np.clip(sub.astype(np.float32) * mult, 0, 255).astype(np.uint8)
            v = cv2.resize(v, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST)
            if mult == 20:
                v = cv2.applyColorMap(v, cv2.COLORMAP_INFERNO)
            else:
                v = cv2.cvtColor(v, cv2.COLOR_GRAY2BGR)
            imwrite_u(os.path.join(KEYS, "k%02d_%s_%s.png" % (i, tag, name)),
                  lab(v, "|A-B| x%d" % mult))
        print("  %-8s 帧 %3d  ≥4:%5.2f%%  ≥8:%5.2f%%  max %3d  均值 %.2f"
              % (tag, i, r["a4"], r["a8"], r["mx"], r["mean"]))

    dist = {
        "frames": n,
        "a4_p50": float(np.median([r["a4"] for r in rows])),
        "a4_p90": float(np.percentile([r["a4"] for r in rows], 90)),
        "a4_max": max(r["a4"] for r in rows),
        "a4_zero": int(sum(1 for r in rows if r["a4"] < 0.01)),
        "picks": [{"tag": t, **r} for t, r in picks],
    }
    with open(os.path.join(KEYS, "dist.json"), "w", encoding="utf-8") as fh:
        json.dump(dist, fh, ensure_ascii=False, indent=2)

    print("\n≥4 灰阶面积分布（脸区）：p50 %.2f%%  p90 %.2f%%  max %.2f%%  几乎为 0 的帧 %d/%d"
          % (dist["a4_p50"], dist["a4_p90"], dist["a4_max"], dist["a4_zero"], n))
    print("关键帧图写入 %s" % KEYS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
