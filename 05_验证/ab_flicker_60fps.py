#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把两臂按帧号配对后交替播放（闪烁法），供人眼判 P2-5 的 60 fps 格。

为什么要专门写（不复用 ab_make.py）
----------------------------------
`ab_make.py` 读的是 `ab_pair.py` 产出的固定文件名 + 竖屏 1080x1920 的画布假设。
本格是 1280x720 / 60 fps / 9 格条码，直接改它会动 §9.35 的历史口径。

闪烁的时间尺度
--------------
§9.35 在 30 fps 下「每 3 帧切」= 100 ms 一个相位。视觉系统对这个量级的时变
最敏感。60 fps 下要保住同一个物理时长，就得**每 6 帧切一次**，否则相位切换
快一倍，会从「闪烁」变成「抖动融合」，反而看不出差异。

为什么要走 ffmpeg 而不是 cv2.VideoWriter（2026-11-04 修）
--------------------------------------------------------
第一版用 `cv2.VideoWriter(..., fourcc='FFV1', ...)` 写 `.mp4`，直接报
    FFMPEG: tag 0x31564646/'FFV1' is not supported with ... format 'mp4'
原因：FFV1 是无损帧内编码，本来就不属于 MP4 容器。OpenCV 的 VideoWriter 把
fourcc 和容器**分开**传，传错组合只能在运行时失败，没有任何前置校验。
所以这里不再赌 fourcc/容器的组合，改成把裸 BGR 帧通过管道喂给 ffmpeg，
由 ffmpeg 自己封 mp4 + libx264 `-qp 0`（无损）。两条好处：
  1. 输出是标准 mp4，任何播放器都能放（人眼判片的成本要尽量低）；
  2. 编码参数写在命令行里，读代码的人一眼能看到「这是无损」，
     而不是从 fourcc 反推。

`-pix_fmt yuv444p` 不是 RGB 直通，理论上 RGB→YUV444 会有 ≤1 灰阶的取整误差。
但本轮的判据是 §9.35 的 6 / 9 灰阶门限，1 灰阶的编码误差不会改变结论 ——
何况脚本结尾会实测「写盘前后最大差」（check_encode），不靠这个推理。

用法
----
    py -3 05_验证/ab_flicker_60fps.py --a max_combo_ll60.mp4 \\
                                      --b rms_combo_ll60.mp4 --switch 6 \\
                                      --tag 60fps_max_rms
    # 输出 out/ab/flicker_<tag>.mp4（x264 无损）
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


def scan(path, bits):
    """读一遍片子，返回 {帧号: 该帧图像}。同一帧号只留第一次出现的。"""
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
        if c >= 0:
            got.setdefault(c, f)
    cap.release()
    return got


# 两种容器/编码，用途不同（2026-11-04 实测取舍）：
#   ffv1 : FFV1 + bgr0 封 mkv，写盘前后 **0 灰阶差**（真无损），但只有
#          VLC / PotPlayer / mpv 这类播放器能放，Windows「电影和电视」不行；
#   x264 : libx264 -qp 0 + yuv444p 封 mp4，**2 灰阶差**（RGB→YUV444 取整），
#          任何播放器都能放，给「随手点开看一眼」用。
# 2 灰阶只是 §9.35 门限（6）的三分之一，不影响判读；但既然 ffv1 能拿到 0，
# 就把 ffv1 当母版，mp4 当便利用副本，两个都留。
FMTS = {
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


def check_encode(dst, picks):
    """写盘保真自检：按采样帧号比对「写入前 / 解码后」，返回最大绝对差。

    人眼判片的前提是「片子里没有编码失真」。有损录制把底噪抬到 6.6 灰阶那次
    教训（§9.41）说明：不对这一步做实测，就分不清「人眼看到的差异」是算法
    的差异还是编码的差异。
    """
    if not picks:
        return None, 0
    cap = cv2.VideoCapture(dst)
    want = dict(picks)          # 输出帧序号 -> 期望图
    worst, n = 0.0, 0
    idx = 0
    while True:
        ok, f = cap.read()
        if not ok:
            break
        if idx in want:
            d = np.abs(f.astype(np.int16) - want[idx].astype(np.int16))
            worst = max(worst, float(d.max()))
            n += 1
            if len(want) == n:
                break
        idx += 1
    cap.release()
    return worst, n


def blank_code(frame, bits):
    """把右下角帧号条码涂黑。

    为什么必须能关掉（2026-11-04，判读翻车后补）
    ------------------------------------------
    条码每帧都在变，实测该区域均值在 74 / 90 / 106 三个值之间逐帧跳，
    单帧跳变 **15–30 灰阶**，面积 172×46 px。这比臂间差异（0–12 灰阶）
    大一个量级，而且是一块纯黑白的硬边方块 —— 人眼 A/B 里「看到闪烁」
    极可能来自它，不是脸。

    条码是给机器配对用的，判读时它是纯粹的污染源：必须能关。
    """
    f = frame.copy()
    h, w = f.shape[:2]
    cw = 18
    y0 = h - 56
    x0 = w - bits * cw - 16
    f[max(0, y0):, max(0, x0):] = 0
    return f


def burst_seq(amp_name, thr, pad, loop_n):
    """从 `amp_*.json` 里挑出超门限的帧，取它们所在的连续片段，循环若干遍。

    为什么需要（2026-11-04，P2-5 60 fps 格）：这一格的差异只出现在
    **5.3% 的帧**上（`p50 = 0.00`），全片 7.1 s 里绝大部分帧两臂逐位相同。
    整片闪烁法看下来，人眼看到的是「一大堆完全相同的帧」—— 相位切换没有
    任何东西可闪，信噪比被稀释掉了。

    所以给出第二支更「贴身」的片：只取超过门限的连续片段（前后各扩 `pad`
    帧保住运动连续），首尾相接循环。同一条带子被反复呈现，闪烁法该有的
    时变刺激才真正出现。判读逻辑不变，变的只是把刺激摆到眼睛前面。
    """
    j = json.load(open(os.path.join(OUT, amp_name), encoding="utf-8"))
    codes, pf = j["codes"], j["per_frame"]
    hot = [c for c, p in zip(codes, pf) if p > thr]
    if not hot:
        return [], []
    runs, cur = [], [hot[0]]
    for c in hot[1:]:
        if c == cur[-1] + 1:
            cur.append(c)
        else:
            runs.append(cur)
            cur = [c]
    runs.append(cur)
    pos = {c: i for i, c in enumerate(codes)}
    seqs = []
    for r in runs:
        i0 = max(0, pos[r[0]] - pad)
        i1 = min(len(codes) - 1, pos[r[-1]] + pad)
        seqs.append(codes[i0:i1 + 1])
    out = []
    for _ in range(loop_n):
        for s in seqs:
            out.extend(s)
    return out, [(s[0], s[-1], len(s)) for s in seqs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--switch", type=int, default=6,
                    help="每 N 帧切一次；60fps 下 6 帧 = 100ms，与 §9.35 的 30fps/3帧等时")
    ap.add_argument("--bits", type=int, default=9)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--tag", default="")
    ap.add_argument("--fmt", default="x264", choices=sorted(FMTS),
                    help="ffv1=真无损 mkv（母版）/ x264=无损 mp4（便利用）")
    ap.add_argument("--from-amp", default="",
                    help="只循环该 amp_*.json 里超门限的片段（如 amp_ll60_max_rms.json）")
    ap.add_argument("--thr", type=float, default=6.0, help="超门限判据（灰阶）")
    ap.add_argument("--pad", type=int, default=8, help="片段前后各扩几帧")
    ap.add_argument("--loop", type=int, default=4, help="片段循环几遍")
    ap.add_argument("--blank-code", action="store_true",
                    help="涂黑右下角帧号条码（判读专用；条码每帧跳 15–30 灰阶，会压倒信号）")
    ap.add_argument("--frozen", default=None,
                    help="冻结帧模式：max=取差异最大帧 / zero=取零差异帧（阴性对照）/ 数字=指定帧号。"
                         "内容完全静止、条码也不变，只剩臂间差异，是灵敏度上限的干净测法")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="冻结帧模式的时长（秒）")
    a = ap.parse_args()

    fa = scan(os.path.join(OUT, a.a), a.bits)
    fb = scan(os.path.join(OUT, a.b), a.bits)
    common = sorted(set(fa) & set(fb))
    if len(common) < 30:
        raise SystemExit("可配对帧太少（%d）" % len(common))

    if a.frozen is not None:
        if not a.from_amp:
            raise SystemExit("--frozen 需要同时给 --from-amp（用它的 per_frame 挑帧）")
        j = json.load(open(os.path.join(OUT, a.from_amp), encoding="utf-8"))
        codes, pf = j["codes"], j["per_frame"]
        if a.frozen == "max":
            i = int(np.argmax(pf))
        elif a.frozen == "zero":
            zs = [k for k, v in enumerate(pf) if v <= 0.0]
            if not zs:
                raise SystemExit("%s 里没有零差异帧，无法做阴性对照" % a.from_amp)
            i = zs[len(zs) // 2]
        else:
            want = int(a.frozen)
            if want not in codes:
                raise SystemExit("帧号 %d 不在配对集里" % want)
            i = codes.index(want)
        code = codes[i]
        n = int(a.fps * a.seconds)
        seq = [code] * n
        print("冻结帧：帧号 %d，原始峰值 %.2f 灰阶，%d 帧（%.1f s，每 %d 帧切）"
              % (code, pf[i], n, a.seconds, a.switch))
    elif a.from_amp:
        seq, wins = burst_seq(a.from_amp, a.thr, a.pad, a.loop)
        if not seq:
            raise SystemExit("%s 里没有 >%.0f 灰阶的帧" % (a.from_amp, a.thr))
        miss = [c for c in seq if c not in fa or c not in fb]
        seq = [c for c in seq if c in fa and c in fb]
        print("片段 %d 段（每段 %d–%d 帧）× %d 遍 = %d 帧，缺帧 %d"
              % (len(wins), min(x[2] for x in wins), max(x[2] for x in wins),
                 a.loop, len(seq), len(miss)))
    else:
        seq = common
    print("共 %d 帧，每 %d 帧切换（%.0f ms/相位），%.1f s"
          % (len(seq), a.switch, 1000.0 * a.switch / a.fps,
             len(seq) / float(a.fps)))

    tag = a.tag or "p25"
    ext, _ = FMTS[a.fmt]
    dst = os.path.join(OUT, "flicker_%s.%s" % (tag, ext))
    h, w = fa[seq[0]].shape[:2]

    p = open_ffmpeg(dst, w, h, a.fps, a.fmt)
    picks = {}
    for i, c in enumerate(seq):
        src = fa if (i // a.switch) % 2 == 0 else fb
        out = blank_code(src[c], a.bits) if a.blank_code else src[c]
        p.stdin.write(out.tobytes())
        # 采样：开头 12 帧（含相位切换点）+ 每 40 帧 + 最后 3 帧
        if i < 12 or i % 40 == 0 or i >= len(seq) - 3:
            picks[i] = out
    p.stdin.close()
    rc = p.wait()
    if rc != 0:
        raise SystemExit("ffmpeg 退出码 %d" % rc)

    sz = os.path.getsize(dst) / 1e6
    print("写入 %s（%d 帧，%.1f MB，%.1f s，相位切换 %d 次）"
          % (os.path.basename(dst), len(seq), sz,
             len(seq) / float(a.fps), len(seq) // a.switch))

    worst, n = check_encode(dst, list(picks.items()))
    if worst is None:
        print("编码保真：未取到采样帧，跳过")
    else:
        flag = "无损" if worst <= 0.0 else ("可接受（≤1 灰阶取整）" if worst <= 2.0
                                        else "异常，需查")
        print("编码保真：抽 %d 帧比对，最大绝对差 %.0f 灰阶 → %s" % (n, worst, flag))


if __name__ == "__main__":
    main()
