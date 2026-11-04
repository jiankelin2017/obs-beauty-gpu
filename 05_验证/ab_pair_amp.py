#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按帧号配对两段人眼 A/B 录制，算「带幅度」（P2-5 用）。

为什么不改 ab_amp.py
--------------------
`ab_amp.py` 是写死三对（agg / ctl / 正对照）+ 写死竖屏脸区的，是 §9.35 那次
判决的一次性脚本。改它等于动历史口径，以后回看分不清「当时怎么算的」。
本脚本只做通用那一段：**给两段录制和一块区域，输出逐帧带幅度与门限统计**。

口径与 ab_amp 完全一致（照抄 band_amp）
--------------------------------------
    逐通道取最大差 → 7×7 摊平 → 区域峰值
原因见 ab_amp.py 头部：rms 的掩码位移会带动色度，先转灰度会互相抵消一部分；
标尺上的鼓包是三通道同加的纯亮度变化，只有「逐通道最大」这一口径能与标尺对齐。

门限（来自 §9.35 的人眼 A/B 标尺）
----------------------------------
    ≤ 6 灰阶  不可检测（标尺第 1 档刚好到门限）
    6~9      过渡带
    > 9      需要重新评估默认值

用法
----
    py -3 05_验证/ab_pair_amp.py --a max_combo_p25_720p60.mp4 \\
                                 --b rms_combo_p25_720p60.mp4 --tag max_vs_rms
    py -3 05_验证/ab_pair_amp.py --a off_combo_p25_720p60.mp4 \\
                                 --b max_combo_p25_720p60.mp4 --tag off_vs_max

⚠ 条码位宽要与素材一致（480 帧内容 → 9 格）。脚本按录制里读到的最大帧号
   自动判断，不需要手给。
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
sys.path.insert(0, HERE)

import make_ab_clip as MC  # noqa: E402


def scan(path, bits):
    """扫全片，返回 ({内容帧号: 帧}, 帧号序列, 总帧数, 不可读帧数)。"""
    MC.BITS = bits
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit("打不开：%s" % path)
    got, seq, bad, n = {}, [], 0, 0
    while True:
        ok, f = cap.read()
        if not ok:
            break
        n += 1
        c = MC.read_code(f)
        if c < 0:
            bad += 1
            continue
        seq.append(c)
        got.setdefault(c, f)
    cap.release()
    return got, seq, n, bad


def triple_index(seq):
    """内容帧号 -> 它出现处的 (前一帧号, 后一帧号) 集合。

    为什么需要（2026-11-04，60 fps 格）：素材 60 fps 与渲染 60 fps 是两个独立
    时钟，必然周期性重复/跳过素材帧 —— 实测同一内容帧号被渲染两次（如 318、
    323、339 各出现两遍），**而且两臂重复的位置不同**。于是「同一帧号」在两臂
    对应的是滤镜状态不同的两个瞬间，差异可达 5~7 灰阶，与要测的信号同量级。
    用前后邻居帧号做指纹，只保留两臂上下文一致的帧，才能把时序歧义排除。
    """
    t = {}
    for i in range(1, len(seq) - 1):
        t.setdefault(seq[i], set()).add((seq[i - 1], seq[i + 1]))
    return t


def band_amp(fa, fb, region):
    """带幅度：逐像素取通道最大差 → 7×7 摊平 → 区域峰值（与 ab_amp 同口径）。"""
    x0, y0, x1, y1 = region
    d = np.abs(fa.astype(np.int16) - fb.astype(np.int16)).max(axis=2)
    d = d.astype(np.float32)
    ds = cv2.blur(d, (7, 7))[y0:y1, x0:x1]
    return float(ds.max()), float(ds.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="A 臂录制（相对 out/ab）")
    ap.add_argument("--b", required=True, help="B 臂录制（相对 out/ab）")
    ap.add_argument("--crop", default="", help="x0,y0,x1,y1；默认全画面")
    ap.add_argument("--bits", type=int, default=0, help="条码格数；0=自动")
    ap.add_argument("--strict", action="store_true",
                    help="只配对两臂前后邻居帧号一致的帧（60fps 素材必开）")
    ap.add_argument("--tag", default="", help="输出文件名后缀")
    a = ap.parse_args()

    pa, pb = os.path.join(OUT, a.a), os.path.join(OUT, a.b)
    bits = a.bits
    if not bits:
        # 先按 9 格扫一遍，读不到就退回 8 格 —— 比让用户手给可靠
        for b in (9, 8, 10):
            MC.BITS = b
            cap = cv2.VideoCapture(pa)
            ok, f = cap.read()
            cap.release()
            if ok and MC.read_code(f) >= 0:
                bits = b
                break
        bits = bits or 9
    print("条码 %d 格" % bits)

    fa, sa, na, ba = scan(pa, bits)
    fb, sb, nb, bb = scan(pb, bits)
    print("A %s：%d 帧，条码不可读 %d，可配对 %d" % (a.a, na, ba, len(fa)))
    print("B %s：%d 帧，条码不可读 %d，可配对 %d" % (a.b, nb, bb, len(fb)))
    common = sorted(set(fa) & set(fb))
    if a.strict:
        ta, tb = triple_index(sa), triple_index(sb)
        # 两个条件缺一不可：
        #   len(...)==1  —— 该内容帧在两臂都只被渲染一次（排除重复渲染）
        #   交集非空      —— 前后邻居帧号对得上（排除跳帧）
        common = [c for c in common
                  if len(ta.get(c, ())) == 1 and len(tb.get(c, ())) == 1
                  and (ta[c] & tb[c])]
        print("严格模式：只保留两臂「前后邻居帧号一致」的帧")
    if not common:
        raise SystemExit("没有共同帧号 —— 条码位宽或素材循环范围不对")
    nc = min(len(fa), len(fb))
    print("共同内容帧 %d 个（两臂各 %d/%d）" % (len(common), len(fa), len(fb)))

    h, w = fa[common[0]].shape[:2]
    region = ((0, 0, w, h) if not a.crop
              else tuple(int(x) for x in a.crop.split(",")))
    print("区域 %s（画面 %dx%d）" % (str(region), w, h))

    peaks, means = [], []
    for c in common:
        pk, mn = band_amp(fa[c], fb[c], region)
        peaks.append(pk)
        means.append(mn)
    peaks = np.array(peaks)
    means = np.array(means)

    print("\n=== %s vs %s（%d 对）===" % (a.a, a.b, len(common)))
    print("  带幅度(通道max)  p50 %.2f  p90 %.2f  p99 %.2f  max %.2f  mean %.2f"
          % (np.percentile(peaks, 50), np.percentile(peaks, 90),
             np.percentile(peaks, 99), peaks.max(), peaks.mean()))
    print("  脸区均值差        p50 %.3f  p90 %.3f  max %.3f"
          % (np.percentile(means, 50), np.percentile(means, 90), means.max()))
    print("  超门限帧：>6 %d/%d (%.1f%%)   >9 %d/%d (%.1f%%)"
          % (int((peaks > 6).sum()), len(peaks), 100.0 * (peaks > 6).mean(),
             int((peaks > 9).sum()), len(peaks), 100.0 * (peaks > 9).mean()))

    tag = a.tag or "%s_vs_%s" % (
        os.path.basename(a.a).split("_")[0], os.path.basename(a.b).split("_")[0])
    jp = os.path.join(OUT, "amp_%s.json" % tag)
    json.dump({"a": a.a, "b": a.b, "bits": bits, "strict": a.strict,
               "region": region,
               "pairs": len(common),
               "peak_p50": float(np.percentile(peaks, 50)),
               "peak_p90": float(np.percentile(peaks, 90)),
               "peak_max": float(peaks.max()),
               "frac_gt6": float((peaks > 6).mean()),
               "frac_gt9": float((peaks > 9).mean()),
               # codes 与 per_frame 一一对应（2026-11-04 加）：
               # 人眼判片时要能回答「看第几秒」。闪烁片的帧序是按**非严格**
               # 的 common 排的，per_frame 却是按**严格**过滤后的子序列排的，
               # 两者下标不等长（338 vs 426）。不存内容帧号就只能靠猜，
               # 所以这里把帧号一起落盘，由下游换算成时间点。
               "codes": common,
               "per_frame": [round(float(x), 3) for x in peaks]},
              open(jp, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("  写入 %s" % jp)
    del nc


if __name__ == "__main__":
    main()
