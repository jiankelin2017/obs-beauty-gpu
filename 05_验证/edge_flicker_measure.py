#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""阶段 0：真机（素材源播放）边界带闪烁的**客观**度量。

要回答的问题
------------
§9.42 的判决全部立在**仿真片**上：静止画面 + 把掩码整体平移。真机上，掩码偏移
大的帧恰好是画面自己在动的帧 —— 运动掩蔽（motion masking）会压掉边界带的可见度。
所以先要量一件事：**真实画面下，效果落点在边界带上每帧跳多少灰阶。**

为什么用「开/关两臂相减」而不是直接看开臂
------------------------------------------
D(t) = |open(t) − raw(t)|（逐像素通道最大差）把「画面本身在变」整个消掉 —— 因为
同一帧开与关的输入是同一张画面。D 剩下的就只有「效果在这个像素落了几灰阶」。

掩码稳定时 D 在边界带上恒定；掩码抖一次（检测框跳、检测提交间隔），D 就在边界带
上跳一次。于是：

    闪烁幅度 = D 的帧间差 ΔD 在边界带上的峰值
    口径 = 逐通道最大差 → 7×7 摊平 → 区域峰值（与 §9.35/§9.42 完全一致）

这个数可以直接与 §9.42.11 标定的门限 **6.02 灰阶**比。

分区（三个对照）
---------------
- **脸内**  框内缩 35%：λ=1 恒定，ΔD 是「运动地基」——它有多大，就说明画面运动
            本身能造出多大假信号
- **边界带** 距框边 < 0.12 脸宽：掩码抖动的作用区（羽化 0.08 脸宽，取 0.12 覆盖）
- **背景**  框外扩 0.5 脸宽之外：无效果区，应 ≈0，是噪声底

三区都只在「D > 1 灰阶」的像素上统计 —— 掩码是肤色椭圆 + 脖子通道，矩形框角
本来就没效果，不排除会把均值稀释掉。

框从哪来
--------
`out/budget_cache.npz` 的 `boxes` 是 `cam_real_1080x1920.mp4` 全片逐帧 YuNet 结果
（2975 帧，(x, y, w, h)，原片像素坐标）。素材段 `[1421, 1661)` 的框直接按下标取，
不重跑检测 —— 与 §9.34/§9.44 用的是同一份几何。

用法
----
    py -3 05_验证/edge_flicker_measure.py                 # 用默认两臂
    py -3 05_验证/edge_flicker_measure.py --open X --raw Y --tag mytag

两臂录制（`ab_record.record_once`）：
    open 臂  cfg=combo    raw 臂  cfg=raw（`set_config(c, None)` → 关滤镜）
    集合 `beauty_edge_probe`（播放 `assets/ab_clip_1080x1920.avi`，1080x1920@30）
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
sys.path.insert(0, HERE)

import ab_pair_amp as PA          # noqa: E402  条码扫描、带幅度口径

BOX_NPZ = os.path.join(OUT, "budget_cache.npz")
F0 = 1421                          # 素材段在原片里的起始帧（make_ab_clip 的默认值）

THR = 6.02                         # §9.42.11 标定的闪烁门限（灰阶）
BLUR = (7, 7)                      # 与 ab_pair_amp.band_amp 同口径


# ---------------------------------------------------------------- 区域

def zones(box, shape):
    """按一个脸框切出「脸内 / 边界带 / 背景」三块布尔掩码。

    box 是 `budget_cache` 的 (x, y, w, h)。边界带取框内、距最近边 < 0.12w 的部分；
    脸内取框内缩 0.35w。两者都不含框外，所以掩码的脖子通道（伸到框下）只有在
    框内那段会被统计 —— 宁可少算，不要把背景混进来。
    """
    h, w = shape
    x, y, bw, bh = box
    x0, y0 = int(round(x)), int(round(y))
    x1, y1 = int(round(x + bw)), int(round(y + bh))
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w - 1, x1), min(h - 1, y1)
    if x1 - x0 < 40 or y1 - y0 < 40:
        return None, None, None

    u = np.arange(w, dtype=np.int32)[None, :]
    v = np.arange(h, dtype=np.int32)[:, None]
    inside = (u >= x0) & (u < x1) & (v >= y0) & (v < y1)

    band = 0.12 * bw
    # u 是 (1,w)、v 是 (h,1)，逐个 np.minimum 会自动广播成 (h,w)；
    # np.minimum.reduce 不行（不同 shape 的数组 list 不构成同构数组）
    dist = np.minimum(np.minimum(u - x0, x1 - 1 - u),
                      np.minimum(v - y0, y1 - 1 - v)).astype(np.float32)
    edge = inside & (dist < band)

    shrink = 0.35 * bw
    core = inside & (dist > shrink)

    pad = 0.5 * bw
    outside = (u < x0 - pad) | (u > x1 + pad) | (v < y0 - pad) | (v > y1 + pad)
    return core, edge, outside


def reg_stats(vals, thr=THR):
    if vals.size == 0:
        return {"n": 0}
    return {
        "n": int(vals.size),
        "p50": float(np.percentile(vals, 50)),
        "p90": float(np.percentile(vals, 90)),
        "p99": float(np.percentile(vals, 99)),
        "max": float(vals.max()),
        "over_thr_pct": float((vals > thr).mean() * 100.0),
    }


MIN_FRAC = 0.01     # 区域内「D>1」的像素不足 1% 面积就整帧跳过


def zone_peak(dd, reg, valid, area):
    """区域内 ΔD 峰值；有效像素太少就返回 None。

    为什么要有这个门槛（2026-11-04 首跑踩到）：背景区 D 均值只有 0.03 灰阶，
    但 `valid = D>1` 会挑出极少数噪声点，它们的 ΔD 能到 12 灰阶 —— 于是一个
    本该是 0 的区域报出「12.59 峰值 / 10.2% 超门限」。样本少的区域必须先判空。
    """
    m = reg & valid
    if int(m.sum()) < max(50, area * MIN_FRAC):
        return None
    return float(cv2.blur(dd, BLUR)[m].max())


# ---------------------------------------------------------------- 主体

def run(open_path, raw_path, tag, f0, bits, skip=20):
    go, so, no, bo = PA.scan(open_path, bits)
    gr, sr, nr, br = PA.scan(raw_path, bits)
    print("open：%d 帧（不可读 %d），raw：%d 帧（不可读 %d）" % (no, bo, nr, br))
    common = sorted(set(go) & set(gr))
    if not common:
        raise SystemExit("两臂没有共同帧号 —— 条码位宽不对？（--bits）")
    print("共同帧号 %d 个（%d…%d）" % (len(common), common[0], common[-1]))

    # 时序歧义：同一帧号在录制里出现两次时，两臂可能取到不同的瞬间。
    # 用 triple_index 只保留「前后邻居一致」的帧（与 ab_pair_amp 同法）。
    to, tr = PA.triple_index(so), PA.triple_index(sr)
    keep = [f for f in common if f in to and f in tr and (to[f] & tr[f])]
    print("剔掉时序歧义后 %d 个" % len(keep))

    bz = np.load(BOX_NPZ, allow_pickle=True)
    boxes = np.stack([np.asarray(b, float).ravel() for b in bz["boxes"]])

    acc = {"core": [], "edge": [], "bg": []}
    acc_d = {"core": [], "edge": [], "bg": []}
    prev = None          # (fid, D_flat)
    pairs = 0
    max_trace = []

    for fid in keep:
        fa = go[fid]
        fb = gr[fid]
        d = np.abs(fa.astype(np.int16) - fb.astype(np.int16)).max(axis=2).astype(np.float32)
        idx = f0 + fid
        if idx >= len(boxes):
            continue
        core, edge, bg = zones(boxes[idx], d.shape)
        if core is None:
            prev = None
            continue
        valid = d > 1.0            # 效果真的落在这里
        # 1) D 本身（效果幅度）
        acc_d["core"].append(float(d[core].mean()))
        acc_d["edge"].append(float(d[edge & valid].mean()) if (edge & valid).any() else 0.0)
        acc_d["bg"].append(float(d[bg].mean()))
        # 2) ΔD（时域跳变）—— 只跟**相邻内容帧**比
        if prev is not None and fid == prev[0] + 1:
            done = pairs
            pairs += 1
            if done < skip:
                # 预热段：掩码刚重建 / 滤镜刚启用，第一对帧能到 12.8 灰阶（首跑实测）。
                # 这不是闪烁，是启动瞬态，必须剔掉否则整张表被它拉高。
                prev = (fid, d)
                continue
            dd = np.abs(d - prev[1]).astype(np.float32)
            vals = {}
            for k, reg in (("core", core), ("edge", edge), ("bg", bg)):
                v = zone_peak(dd, reg, valid, int(reg.sum()))
                if v is not None:
                    acc[k].append(v)
                    vals[k] = v
            # 框位移：用来判「ΔD 是运动带来的，还是掩码节奏带来的」
            pb = boxes[f0 + prev[0]]
            move = float(np.hypot(boxes[idx][0] - pb[0], boxes[idx][1] - pb[1]))
            if "edge" in vals:
                max_trace.append({"fid": int(fid), "t": round(fid / 30.0, 2),
                                  "core": round(vals.get("core", 0.0), 3),
                                  "edge": round(vals["edge"], 3),
                                  "bg": round(vals.get("bg", 0.0), 3),
                                  "move": round(move, 2)})
        prev = (fid, d)

    # 「ΔD 是运动带来的还是掩码节奏带来的」——按框位移分档看边界带 ΔD。
    # 若各档同量级 → 与运动无关（掩码节奏主导）；若单调上升 → 运动主导。
    by_move = {}
    if max_trace:
        e = np.array([x["edge"] for x in max_trace])
        m = np.array([x["move"] for x in max_trace])
        for lo, hi, name in ((0.0, 1.0, "<1 px"), (1.0, 3.0, "1–3 px"),
                             (3.0, 6.0, "3–6 px"), (6.0, 1e9, ">6 px")):
            sel = (m >= lo) & (m < hi)
            if sel.sum() >= 5:
                by_move[name] = {"n": int(sel.sum()),
                                 "p50": float(np.median(e[sel])),
                                 "max": float(e[sel].max())}
    corr = corr_core = None
    if len(max_trace) > 10:
        e = np.array([x["edge"] for x in max_trace])
        c = np.array([x["core"] for x in max_trace])
        m = np.array([x["move"] for x in max_trace])
        if m.std() > 1e-6:
            if e.std() > 1e-6:
                corr = float(np.corrcoef(e, m)[0, 1])
            if c.std() > 1e-6:
                corr_core = float(np.corrcoef(c, m)[0, 1])

    rep = {
        "tag": tag,
        "open": os.path.basename(open_path),
        "raw": os.path.basename(raw_path),
        "pairs": pairs,
        "thr": THR,
        "D_mean": {k: (float(np.mean(v)) if v else 0.0) for k, v in acc_d.items()},
        "delta_d": {k: reg_stats(np.array(v)) for k, v in acc.items()},
        "corr_edge_vs_move": corr,
        "corr_core_vs_move": corr_core,
        "skip": skip,
        "edge_by_move": by_move,
        "trace": max_trace,
    }
    return rep


def run_static(open_path, raw_path, tag, skip=30, box=None):
    """静态素材（`beauty_still_real`）模式：不需要条码，两臂按帧序直接配。

    静态画面**没有任何内容运动**，所以 ΔD 里不含「运动」这个变量 ——
    它测到的就是纯粹的掩码抖动：检测框每次重检抖几像素 → 掩码跳 → 边界带跳。
    这是「真机静止时闪不闪」的直接答案，也是 §9.42 仿真片想模拟、但
    真实素材里根本找不到的那种条件（`cam_real` 全片帧间位移 p50 = 2.88 px，
    没有任何 60 帧以上的静止段）。
    """
    ca, cb = cv2.VideoCapture(open_path), cv2.VideoCapture(raw_path)
    if not ca.isOpened() or not cb.isOpened():
        raise SystemExit("打不开录制")
    acc = {"core": [], "edge": [], "bg": []}
    trace = []
    prev = None
    i = 0
    while True:
        ok1, fa = ca.read()
        ok2, fb = cb.read()
        if not (ok1 and ok2):
            break
        d = np.abs(fa.astype(np.int16) - fb.astype(np.int16)).max(axis=2).astype(np.float32)
        core, edge, bg = zones(box, d.shape)
        if core is None:
            break
        valid = d > 1.0
        if prev is not None and i >= skip:
            dd = np.abs(d - prev).astype(np.float32)
            vals = {}
            for k, reg in (("core", core), ("edge", edge), ("bg", bg)):
                v = zone_peak(dd, reg, valid, int(reg.sum()))
                if v is not None:
                    acc[k].append(v)
                    vals[k] = v
            if "edge" in vals:
                trace.append({"i": i, "t": round(i / 30.0, 2),
                              "core": round(vals.get("core", 0.0), 3),
                              "edge": round(vals["edge"], 3),
                              "bg": round(vals.get("bg", 0.0), 3)})
        prev = d
        i += 1
    ca.release(); cb.release()
    return {"tag": tag, "static": True, "pairs": i - skip, "skip": skip, "thr": THR,
            "delta_d": {k: reg_stats(np.array(v)) for k, v in acc.items()},
            "trace": trace}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--open", default=os.path.join(OUT, "ab", "edge_open.mp4"))
    ap.add_argument("--raw", default=os.path.join(OUT, "ab", "edge_raw.mp4"))
    ap.add_argument("--tag", default="edge0")
    ap.add_argument("--f0", type=int, default=F0, help="素材段在原片里的起始帧")
    ap.add_argument("--bits", type=int, default=8, help="条码位宽（240 帧内容 → 8）")
    ap.add_argument("--skip", type=int, default=20, help="剔除开头多少对预热帧")
    ap.add_argument("--static", action="store_true",
                    help="静态素材模式（无条码，两臂按帧序配）")
    ap.add_argument("--box", default="560,540,240,300",
                    help="--static 用的脸框 x,y,w,h（默认取 cam_real 段中位框）")
    a = ap.parse_args(argv)

    for p in (a.open, a.raw):
        if not os.path.isfile(p):
            raise SystemExit("找不到录制：%s" % p)

    if a.static:
        bx = [float(x) for x in a.box.split(",")]
        rep = run_static(a.open, a.raw, a.tag, a.skip, bx)
    else:
        rep = run(a.open, a.raw, a.tag, a.f0, a.bits, a.skip)
    os.makedirs(os.path.join(OUT, "logs"), exist_ok=True)
    jp = os.path.join(OUT, "logs", "edge_flicker_%s.json" % a.tag)
    with open(jp, "w", encoding="utf-8") as fh:
        json.dump(rep, fh, ensure_ascii=False, indent=2)

    print("=" * 68)
    print("配对 %d 对帧（剔除预热 %d 对）   门限 %.2f 灰阶（§9.42.11）"
          % (rep["pairs"] - rep["skip"], rep["skip"], THR))
    print("%-6s %8s %8s %8s %8s %10s" % ("区域", "p50", "p90", "p99", "max", ">门限%"))
    for k, name in (("core", "脸内"), ("edge", "边界带"), ("bg", "背景")):
        s = rep["delta_d"][k]
        if not s.get("n"):
            print("%-6s %s" % (name, "无样本"))
            continue
        print("%-6s %8.2f %8.2f %8.2f %8.2f %9.1f%%"
              % (name, s["p50"], s["p90"], s["p99"], s["max"], s["over_thr_pct"]))
    if "D_mean" in rep:
        print("效果幅度 D 均值（灰阶）：脸内 %.2f / 边界带 %.2f / 背景 %.2f"
              % (rep["D_mean"]["core"], rep["D_mean"]["edge"], rep["D_mean"]["bg"]))
    if rep.get("edge_by_move"):
        print("边界带 ΔD 按框位移分档（判它是不是运动撑起来的）：")
        for name, s in rep["edge_by_move"].items():
            print("  %-8s n=%3d  p50=%5.2f  max=%6.2f" % (name, s["n"], s["p50"], s["max"]))
    if rep.get("corr_edge_vs_move") is not None:
        print("corr(边界带 ΔD, 框位移) = %+.3f    corr(脸内 ΔD, 框位移) = %s"
              % (rep["corr_edge_vs_move"],
                 "%+.3f" % rep["corr_core_vs_move"] if rep.get("corr_core_vs_move") is not None else "n/a"))
    print("→ %s" % jp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
