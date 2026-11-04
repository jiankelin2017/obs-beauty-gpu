#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""闪烁门限标定：把两臂的**真实差异**按比例缩放，只改幅度、不改形态。

为什么要这个（2026-11-04，P2-5 判读之后）
----------------------------------------
§9.42.10 的判读只回答了一个二值问题：「60 fps 下看得见吗」——看得见。
但它没回答「多小才看不见」，而 §9.35.8 的 `+6 灰阶` 是用**合成鼓包**测的：
鼓包是圆滑的缓坡，而 `rms` 的真实差异是**整脸轮廓的细边**。形态不同，
门限不能直接搬。

这里换个做法：拿真实的 A / B 两帧，构造

    B' = A + α · (B − A)

α 从 1.0 降到 0.1。差异的**空间位置、形状、边缘朝向全部不变**，只有幅度线性缩小。
于是「哪一档开始看得见」就直接对应「这种形态的门限是多少灰阶」——
不再依赖鼓包与真实差异之间的类比。

为什么裁切而不缩放
------------------
§9.35.8 定的规矩：1:1 裁切，不缩放。缩放（哪怕双线性）会把细边抹平，
而细边正是 `rms` 差异的载体 —— 抹平等于偷偷降低了本实验的灵敏度。
裁切 440×600 一次把三件事办了：脸占画面比例变大、条码被裁出画面外、像素不重采样。

段结构（沿用 §9.35.8，好让两次判读可比）
--------------------------------------
每档 4.5 s = 0.5 s 静止（只有 A）+ 3.0 s 闪烁（A / B' 每 6 帧交替，5 Hz）+ 1.0 s 黑场。
所以第 n 段（n 从 0 起）的闪烁起点是 `4.5·n + 0.5` 秒。用户报「第几秒开始闪」即可定位档位。
第 0 段 α=0（B' ≡ A，两帧逐像素全等）是**阴性对照**：它必须看不见，
否则说明这套呈现方式自己在造闪烁，整支片作废。

用法
----
    py -3 ab_jnd_scale.py                       # 默认 max vs rms，帧号取 amp 里最大者
    py -3 ab_jnd_scale.py --code 216            # 指定帧号（零差异帧可验证对照）
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out", "ab")
FFMPEG = os.path.join(HERE, "tools", "ffmpeg.exe")
sys.path.insert(0, HERE)

import make_ab_clip as MC  # noqa: E402


def scan(path, bits, want):
    """只取需要的帧号，避免把整片读进内存。"""
    MC.BITS = bits
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit("打不开 %s" % path)
    got = {}
    while True:
        ok, f = cap.read()
        if not ok:
            break
        c = MC.read_code(f)
        if c >= 0 and c in want and c not in got:
            got[c] = f
        if len(got) == len(want):
            break
    cap.release()
    return got


def blank_code(f, bits=9):
    """涂黑右下角帧号条码。裁切通常会把它裁掉，但裁切参数一变就会漏回来。"""
    g = f.copy()
    h, w = g.shape[:2]
    x0 = w - bits * 18 - 16
    g[max(0, h - 56):, max(0, x0):] = 0
    return g


def band_peak(a, b):
    """带幅度：逐通道最大差 → 7×7 盒式摊平 → 区域峰值。

    与 §9.35 的 `ab_amp.py` 同口径，这样两个实验的灰阶数可以放在一起说。
    """
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2).astype(np.float32)
    return float(cv2.blur(d, (7, 7)).max()), float(d.max()), int((d > 6).sum())


FMTS = {
    # 标定片对编码噪声是零容忍的：最低档只有 1.2 灰阶，而 x264+yuv444p 的
    # RGB→YUV 取整就是 2 灰阶 —— 比信号还大。所以母版一律 FFV1（bgr0，写盘 0 灰阶差）。
    "ffv1": ("mkv", ["-c:v", "ffv1", "-pix_fmt", "bgr0"]),
    "x264": ("mp4", ["-c:v", "libx264", "-qp", "0", "-preset", "veryfast",
                     "-pix_fmt", "yuv444p"]),
}


def open_ffmpeg(dst, w, h, fps, fmt):
    _, enc = FMTS[fmt]
    cmd = [FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
           "-f", "rawvideo", "-pix_fmt", "bgr24",
           "-s", "%dx%d" % (w, h), "-r", str(fps), "-i", "-"] + enc + [dst]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="max_combo_ll60.mp4")
    ap.add_argument("--b", default="rms_combo_ll60.mp4")
    ap.add_argument("--amp", default="amp_ll60_max_rms.json",
                    help="用来挑默认帧号（取 per_frame 最大者）")
    ap.add_argument("--code", type=int, default=None, help="不指定则取 amp 里差异最大的帧")
    ap.add_argument("--bits", type=int, default=9)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--switch", type=int, default=6, help="60fps 下 6 帧 = 100 ms = 5 Hz")
    ap.add_argument("--alphas", default="0,0.1,0.25,0.5,1.0",
                    help="从低到高排（顺序效应：越往后越强，用户容易顺着报「从第几秒开始」）")
    ap.add_argument("--static", type=float, default=0.5, help="每档静止时长（秒）")
    ap.add_argument("--flicker", type=float, default=3.0, help="每档闪烁时长（秒）")
    ap.add_argument("--black", type=float, default=1.0, help="每档黑场（秒）")
    ap.add_argument("--crop", default="460,120,440,600",
                    help="x,y,w,h —— 1:1 裁切，不缩放（缩放会抹平细边）")
    ap.add_argument("--tag", default="jnd_scale_60")
    ap.add_argument("--fmt", default="ffv1", choices=sorted(FMTS),
                    help="ffv1=真无损 mkv（默认，标定必须用它）/ x264=无损 mp4（便利用，有 2 灰阶取整）")
    a = ap.parse_args()

    code = a.code
    if code is None:
        j = json.load(open(os.path.join(OUT, a.amp), encoding="utf-8"))
        i = int(np.argmax(j["per_frame"]))
        code, peak = j["codes"][i], j["per_frame"][i]
        print("用 %s 里差异最大的帧：帧号 %d（原始峰值 %.2f 灰阶）" % (a.amp, code, peak))

    fa = scan(os.path.join(OUT, a.a), a.bits, {code})
    fb = scan(os.path.join(OUT, a.b), a.bits, {code})
    if code not in fa or code not in fb:
        raise SystemExit("帧号 %d 在两臂里没有同时出现" % code)

    x, y, cw, ch = [int(v) for v in a.crop.split(",")]
    A_full, B_full = blank_code(fa[code], a.bits), blank_code(fb[code], a.bits)
    A = A_full[y:y + ch, x:x + cw].copy()
    B = B_full[y:y + ch, x:x + cw].copy()
    if A.shape[0] != ch or A.shape[1] != cw:
        raise SystemExit("裁切越界：%s，画面 %dx%d" % (a.crop, A_full.shape[1], A_full.shape[0]))
    h, w = A.shape[:2]

    alphas = [float(v) for v in a.alphas.split(",")]
    variants = []
    for al in alphas:
        if al <= 0.0:
            bp = A.copy()                      # 阴性对照：与 A 逐像素全等
        elif al >= 1.0:
            bp = B.copy()                      # 避免浮点往返引入 ±1
        else:
            bp = np.clip(np.round(A.astype(np.float32) +
                                  al * (B.astype(np.float32) - A.astype(np.float32))),
                         0, 255).astype(np.uint8)
        variants.append(bp)

    # 自检表：每档在裁切 ROI 上的实测带幅度（与 ab_amp.py 同口径）
    print("\n档   α     带幅度(7x7)  单像素max  >6面积  预期定位")
    for al, bp in zip(alphas, variants):
        b7, dm, n6 = band_peak(A, bp)
        seg = alphas.index(al)
        print("%2d  %-5.2f  %8.2f  %9d  %6d   闪烁起点 %.1f s"
              % (seg, al, b7, int(dm), n6, 4.5 * seg + 0.5))

    n_static = int(round(a.static * a.fps))
    n_flick = int(round(a.flicker * a.fps))
    n_black = int(round(a.black * a.fps))
    if n_flick % a.switch:
        raise SystemExit("闪烁段 %d 帧不是 switch=%d 的整数倍" % (n_flick, a.switch))

    dst = os.path.join(OUT, "%s.%s" % (a.tag, FMTS[a.fmt][0]))
    p = open_ffmpeg(dst, w, h, a.fps, a.fmt)
    black = np.zeros((h, w, 3), np.uint8)
    for bp in variants:
        for _ in range(n_static):
            p.stdin.write(A.tobytes())
        for i in range(n_flick):
            p.stdin.write((A if (i // a.switch) % 2 == 0 else bp).tobytes())
        for _ in range(n_black):
            p.stdin.write(black.tobytes())
    p.stdin.close()
    rc = p.wait()
    if rc != 0:
        raise SystemExit("ffmpeg 退出码 %d" % rc)

    total = len(variants) * (n_static + n_flick + n_black)
    print("\n写入 %s（%d 档 × %.1f s = %.1f s，%dx%d，裁切 %s 不缩放）"
          % (os.path.basename(dst), len(variants),
             (n_static + n_flick + n_black) / float(a.fps),
             total / float(a.fps), w, h, a.crop))

    # 编码保真自检：抽「每档静止帧」与「每档闪烁的 B 相位首帧」逐像素比对。
    # 标定片里最低档只有 1.2 灰阶，编码误差必须为 0，否则测的是编码器不是算法。
    seg_len = n_static + n_flick + n_black
    want = {}
    for s, bp in enumerate(variants):
        want[s * seg_len] = A
        want[s * seg_len + n_static + a.switch] = bp
    cap = cv2.VideoCapture(dst)
    idx = n = 0
    worst = 0.0
    while True:
        ok, f = cap.read()
        if not ok:
            break
        if idx in want:
            worst = max(worst, float(np.abs(f.astype(np.int16) -
                                            want[idx].astype(np.int16)).max()))
            n += 1
        idx += 1
        if n == len(want):
            break
    cap.release()
    flag = "无损 ✓" if worst <= 0.0 else ("有 %d 灰阶取整，最低档不可信" % int(worst)
                                          if worst > 1.0 else "1 灰阶取整，可接受")
    print("编码保真：抽 %d 对帧，最大绝对差 %.0f 灰阶 → %s" % (n, worst, flag))

    print("判读方法：报「从第几秒开始看见闪烁」，再用上表的「闪烁起点」对档位。")
    print("第 0 档（α=0）必须看不见 —— 它是阴性对照，能看见就说明这套呈现方式在造闪烁。")


if __name__ == "__main__":
    main()
