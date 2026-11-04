#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""人眼 A/B 的分析与产物生成。

输入：`ab_record.py` 录下的若干臂（每臂一个 mp4，素材循环播放，开头有
白/黑/红/绿同步标记）。
输出：
  1. 客观指标（逐帧差异的面积/幅度/位置）+ 噪声底
  2. 给人看的产物：并排、脸区放大并排、闪烁法交替、差异放大

为什么噪声底必须是 0
--------------------
A/B 的结论完全取决于「差异图里的东西是不是真的」。同一臂录两次，如果两次
逐字节不同，那两臂之间的差异就没有意义。实测（2.13.0 这轮）噪声底是
**0.000%** —— OBS 的 nvenc 在相同输入下决策稳定。这条成立，后面所有非零
差异才可以归因。

为什么用「闪烁法」
------------------
同一位置交替显示 A / B，人眼对**时变**的差异远比并排敏感（并排要来回扫视，
闪烁只要盯着看）。这是视觉心理物理里的标准做法。闪烁法都看不出，就是真看不出。
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

FACE_CROP = (120, 660, 80, 720)     # x0, x1, y0, y1（1080×1920 尺度，含脸+脖子）
SIDE_W, SIDE_H = 540, 640           # 并排时单格尺寸


def gray(frame):
    return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)


def find_mark(path):
    """返回同步标记里**黑帧**的索引。

    素材开头是 白/黑/红/绿 四帧。白帧靠不住：开着美颜时整幅纯白会被当成
    肤色做处理（实测 max 臂里白的均值从 255 掉到画面内容级，四帧序列只剩三帧）。
    黑/红/绿 三帧在所有臂里都原样通过（实测 (0,0,0) / (3,0,232) / (8,255,19)），
    所以锚用「黑 → 红 → 绿」这个顺序三连，比找单帧白稳得多。
    """
    cap = cv2.VideoCapture(path)
    sig = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        sig.append([float(x) for x in cv2.mean(f)[:3]])
    cap.release()
    s = np.array(sig)
    if not len(s):
        return -1
    isb = s.max(axis=1) < 25
    isr = (s[:, 2] > 210) & (s[:, 0] < 40) & (s[:, 1] < 40)
    isg = (s[:, 1] > 210) & (s[:, 2] < 40) & (s[:, 0] < 40)
    for i in range(len(s) - 2):
        if isb[i] and isr[i + 1] and isg[i + 2]:
            return i
    return -1


def read_frames(path, start, count):
    """从 start 帧起读 count 帧。"""
    cap = cv2.VideoCapture(path)
    cap.set(cv2.CAP_PROP_POS_FRAMES, start)
    out = []
    for _ in range(count):
        ok, f = cap.read()
        if not ok:
            break
        out.append(f)
    cap.release()
    return out


def usable(path):
    """一臂可用的内容帧数（标记之后、下一个标记之前）。"""
    m = find_mark(path)
    if m < 0:
        return -1, 0
    cap = cv2.VideoCapture(path)
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    cap.release()
    # 素材一个循环 244 帧；标记后内容 240 帧。再留 4 帧余量给循环接缝。
    return m, min(n - (m + 3), 236)


def stats_pair(ga, gb, thr_list=(1, 2, 4, 8)):
    """两张灰度图 → 差异统计。"""
    d = np.abs(ga.astype(np.int16) - gb.astype(np.int16))
    out = {"mean": float(d.mean()), "max": int(d.max()),
           "p99": float(np.percentile(d, 99))}
    for t in thr_list:
        out["a%d" % t] = 100.0 * float((d >= t).mean())
    return out, d


def centroid_face(d, box):
    """差异的加权质心，判断它是不是落在脸上。"""
    x0, x1, y0, y1 = box
    sub = d[y0:y1, x0:x1].astype(np.float64)
    w = sub.sum()
    if w <= 0:
        return None, 0.0
    ys, xs = np.mgrid[y0:y1, x0:x1]
    return (float((xs * sub).sum() / w), float((ys * sub).sum() / w)), float(
        100.0 * sub.sum() / max(1.0, d.astype(np.float64).sum()))


def put_label(img, text, color=(255, 255, 255)):
    cv2.putText(img, text, (14, 42), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (0, 0, 0), 5,
                cv2.LINE_AA)
    cv2.putText(img, text, (14, 42), cv2.FONT_HERSHEY_SIMPLEX, 1.2, color, 2,
                cv2.LINE_AA)
    return img


def crop_face(f):
    x0, x1, y0, y1 = FACE_CROP
    return f[y0:y1, x0:x1]


def write_video(path, frames, fps=30):
    """frames 是 RGB/BGR 帧列表（同尺寸）。"""
    if not frames:
        return None
    h, w = frames[0].shape[:2]
    wr = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
    if not wr.isOpened():
        return None
    for f in frames:
        wr.write(f)
    wr.release()
    return path


def build_outputs(arm_files, marks, arms, n, face_crop=True):
    """生成四种产物。arm_files: {arm: path}，marks: {arm: 标记帧索引}。"""
    os.makedirs(OUT, exist_ok=True)
    made = {}
    a, b = arms[0], arms[1]
    # 每臂用自己的标记帧位置 —— 两次录制的起始时刻不同，标记帧索引也不同
    fa = read_frames(arm_files[a], marks[a] + 3, n)
    fb = read_frames(arm_files[b], marks[b] + 3, n)
    m = min(len(fa), len(fb))
    fa, fb = fa[:m], fb[:m]

    # 1) 并排（全画面，各缩一半）
    side = []
    for i in range(m):
        la = cv2.resize(fa[i], (SIDE_W, SIDE_H), interpolation=cv2.INTER_AREA)
        lb = cv2.resize(fb[i], (SIDE_W, SIDE_H), interpolation=cv2.INTER_AREA)
        row = np.hstack([put_label(la.copy(), "A"), put_label(lb.copy(), "B")])
        cv2.line(row, (SIDE_W, 0), (SIDE_W, SIDE_H), (255, 255, 255), 2)
        side.append(row)
    made["side"] = write_video(os.path.join(OUT, "ab_side.mp4"), side)

    # 2) 脸区放大并排
    if face_crop:
        fs = []
        for i in range(m):
            ca = crop_face(fa[i]); cb = crop_face(fb[i])
            ca = cv2.resize(ca, (SIDE_W, SIDE_H), interpolation=cv2.INTER_CUBIC)
            cb = cv2.resize(cb, (SIDE_W, SIDE_H), interpolation=cv2.INTER_CUBIC)
            row = np.hstack([put_label(ca.copy(), "A"), put_label(cb.copy(), "B")])
            cv2.line(row, (SIDE_W, 0), (SIDE_W, SIDE_H), (255, 255, 255), 2)
            fs.append(row)
        made["face"] = write_video(os.path.join(OUT, "ab_face.mp4"), fs)

    # 3) 闪烁法：同一位置交替 A/B，每 2 帧切一次（15 Hz，肉眼看到的是融合像）
    fl = []
    for i in range(m):
        src = fa[i] if (i // 2) % 2 == 0 else fb[i]
        c = crop_face(src)
        c = cv2.resize(c, (SIDE_W, SIDE_H), interpolation=cv2.INTER_CUBIC)
        tag = "A" if (i // 2) % 2 == 0 else "B"
        fl.append(put_label(c.copy(), tag))
    made["flicker"] = write_video(os.path.join(OUT, "ab_flicker.mp4"), fl)

    # 4) 差异放大 ×20（只看物理量，不看美观）
    dm = []
    for i in range(m):
        d = np.abs(gray(fa[i]).astype(np.int16) - gray(fb[i]).astype(np.int16))
        v = np.clip(d.astype(np.float32) * 20.0, 0, 255).astype(np.uint8)
        v = cv2.applyColorMap(v, cv2.COLORMAP_INFERNO)
        v = cv2.resize(v, (SIDE_W, SIDE_H), interpolation=cv2.INTER_AREA)
        dm.append(put_label(v, "|A-B| x20"))
    made["diff"] = write_video(os.path.join(OUT, "ab_diff_x20.mp4"), dm)

    return made, m


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", nargs="*", default=["max", "rms4"],
                    help="两臂：第一个当 A，第二个当 B")
    ap.add_argument("--cfg", default="combo")
    ap.add_argument("--tag", default="")
    ap.add_argument("--noise", nargs="*", default=[],
                    help="噪声底对照：同臂的两份录制文件名（不含扩展名）")
    ap.add_argument("--limit", type=int, default=0, help="只分析前 N 帧")
    a = ap.parse_args(argv)

    def path_of(arm):
        return os.path.join(OUT, "%s_%s%s.mp4"
                            % (arm, a.cfg, ("_" + a.tag) if a.tag else ""))

    files = {arm: path_of(arm) for arm in a.arms}
    for arm, p in files.items():
        if not os.path.isfile(p):
            sys.exit("缺文件：%s" % p)

    marks = {arm: find_mark(p) for arm, p in files.items()}
    print("同步标记帧：%s" % marks)
    if any(v < 0 for v in marks.values()):
        sys.exit("有臂没找到同步标记，无法对齐")

    # 以标记帧为原点对齐（各臂的标记帧索引不同，但标记之后是同一段内容）
    counts = [usable(p)[1] for p in files.values()]
    n = min(counts)
    if a.limit:
        n = min(n, a.limit)
    print("可用内容帧 %d（各臂 %s），即 %.2f s @30fps" % (n, counts, n / 30.0))

    # 噪声底：同一臂的两份录制，逐帧差异
    if a.noise:
        p1 = os.path.join(OUT, a.noise[0] + ".mp4")
        p2 = os.path.join(OUT, a.noise[1] + ".mp4")
        m1, m2 = find_mark(p1), find_mark(p2)
        f1 = read_frames(p1, m1 + 3, n)
        f2 = read_frames(p2, m2 + 3, n)
        k = min(len(f1), len(f2))
        acc = []
        for i in range(k):
            s, _ = stats_pair(gray(f1[i]), gray(f2[i]))
            acc.append(s)
        arr = {kk: float(np.median([x[kk] for x in acc])) for kk in acc[0]}
        print("\n== 噪声底（同臂两次录制，%d 帧）" % k)
        print("   均值 %.4f  max %d  p99 %.1f  ≥4 灰阶占比 %.4f%%"
              % (arr["mean"], arr["max"], arr["p99"], arr["a4"]))

    # 两臂逐帧
    x0, x1, y0, y1 = FACE_CROP
    fa = read_frames(files[a.arms[0]], marks[a.arms[0]] + 3, n)
    fb = read_frames(files[a.arms[1]], marks[a.arms[1]] + 3, n)
    k = min(len(fa), len(fb))
    rows = []
    for i in range(k):
        s, d = stats_pair(gray(fa[i]), gray(fb[i]))
        c, share = centroid_face(d, (x0, x1, y0, y1))
        s["face_share"] = share
        s["cx"], s["cy"] = c if c else (float("nan"), float("nan"))
        rows.append(s)
    keys = ("mean", "max", "p99", "a1", "a2", "a4", "a8", "face_share")
    med = {kk: float(np.median([r[kk] for r in rows])) for kk in keys}
    p90 = {kk: float(np.percentile([r[kk] for r in rows], 90)) for kk in keys}
    worst = max(rows, key=lambda r: r["a4"])
    nz = [r for r in rows if r["a4"] > 0.0005]

    print("\n== %s vs %s（%d 帧 = %.1f s）" % (a.arms[0], a.arms[1], k, k / 30.0))
    print("   %-10s %10s %10s" % ("指标", "中位", "p90"))
    for kk in ("mean", "max", "p99", "a1", "a2", "a4", "a8", "face_share"):
        print("   %-10s %10.4f %10.4f" % (kk, med[kk], p90[kk]))
    print("   有可见差异（≥4 灰阶覆盖 >0.05%% 画面）的帧：%d/%d = %.1f%%"
          % (len(nz), k, 100.0 * len(nz) / max(1, k)))
    print("   最差帧：≥4 灰阶占 %.3f%% 画面、max %d 灰阶、差异质心 (%.0f, %.0f)、"
          "落在脸区 %.1f%%"
          % (worst["a4"], worst["max"], worst["cx"], worst["cy"],
             worst["face_share"]))

    print("\n== 生成人眼产物")
    arms2 = list(a.arms[:2])
    made, cnt = build_outputs(files, marks, arms2, n)
    for kk, v in made.items():
        print("   %-8s %s" % (kk, v if v else "生成失败"))

    rep = os.path.join(OUT, "report_%s_%s.json" % (a.cfg, "_".join(arms2)))
    with open(rep, "w", encoding="utf-8") as fh:
        json.dump({"arms": arms2, "frames": k, "marks": marks,
                   "median": med, "p90": p90, "worst": worst,
                   "frames_with_visible": len(nz),
                   "outputs": made}, fh, ensure_ascii=False, indent=2)
    print("\n报告写入 %s" % rep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
