#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给一段素材出「逐秒人脸时间线」——用来定位能剪成测试片段的区间。

为什么要有这支
--------------
`count_faces.py` 只回答「多脸占多少」，不记时间和框。而实际取材时真正要的是：
**哪几秒是干净正脸、哪几秒脸被挡住、哪几秒脸小到像素级边缘**。有了时间线，
T10-b 的遮挡补测、P2-5 的 60 fps 覆盖都能直接按秒剪，不用再回头找素材。

判据与插件完全对齐（不自定义阈值）
----------------------------------
· 640×640 等比 letterbox —— `beauty_face.cpp` 的 letterboxRgb
· score = sqrt(cls*obj)，阈值 0.6，NMS IoU 0.3 —— `beauty_face.h` 默认参数
· 模型 YuNet 2023mar，与插件同一个 onnx

分类只看「最大人脸的宽度占画面宽度的比例」（不是面积：面积对距离更敏感，
宽度比更接近「脸有多少像素」这一实际约束）：
    ≥10%  大脸        5–10% 中脸        2–5% 小脸        <2% 超小脸
    0 张  无脸（遮挡或检不出）

只读素材，不复制、不改名。
"""

import argparse
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from count_faces import find_model, DET_SIDE, CONF, NMS   # 复用同一套口径


def detect(det, img):
    """返回 [(x,y,w,h,score), ...]，坐标为原图比例无关的绝对像素。"""
    h, w = img.shape[:2]
    s = min(float(DET_SIDE) / w, float(DET_SIDE) / h)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    small = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_AREA)
    canvas = cv2.copyMakeBorder(small, 0, DET_SIDE - nh, 0, DET_SIDE - nw,
                                cv2.BORDER_CONSTANT, value=(0, 0, 0))
    det.setInputSize((DET_SIDE, DET_SIDE))
    _, faces = det.detect(canvas)
    if faces is None:
        return []
    out = []
    for f in faces:
        x, y, bw, bh = [float(v) / s for v in f[:4]]
        out.append((x, y, bw, bh, float(f[-1])))
    return out


def classify(n, ratio):
    if n == 0:
        return "遮挡/无脸"
    if n >= 2:
        return "多脸"
    if ratio >= 0.10:
        return "大脸"
    if ratio >= 0.05:
        return "中脸"
    if ratio >= 0.02:
        return "小脸"
    return "超小脸"


def run(path, step):
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        sys.exit("打不开视频: %s" % path)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    W = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    H = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    det = cv2.FaceDetectorYN.create(find_model(), "", (DET_SIDE, DET_SIDE),
                                    CONF, NMS, 5000)
    every = max(1, int(round(fps * step)))
    rows = []
    i = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if i % every == 0:
            fs = detect(det, frame)
            n = len(fs)
            ratio = (max(f[2] for f in fs) / float(W)) if fs else 0.0
            rows.append({"t": round(i / fps, 2), "n": n,
                         "ratio": round(ratio, 4),
                         "cls": classify(n, ratio)})
        i += 1
    cap.release()
    return {"path": path, "fps": fps, "frames": total, "w": W, "h": H,
            "step": step, "rows": rows}


def segments(rows):
    """把连续同类的秒并成区间，方便直接拿去剪片。"""
    out = []
    for r in rows:
        if out and out[-1]["cls"] == r["cls"]:
            out[-1]["end"] = r["t"]
            out[-1]["n"] += 1
        else:
            out.append({"cls": r["cls"], "start": r["t"], "end": r["t"], "n": 1})
    return [s for s in out if s["n"] >= 2]     # 只留 ≥2 秒的成段区间


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("videos", nargs="+")
    ap.add_argument("--step", type=float, default=1.0, help="采样间隔秒（默认 1）")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    logs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "logs")
    os.makedirs(logs, exist_ok=True)
    all_res = []
    for p in args.videos:
        r = run(p, args.step)
        all_res.append(r)
        name = os.path.basename(p)
        print("\n=== %s  %dx%d @ %.0ffps  %.1fs ===" %
              (name, r["w"], r["h"], r["fps"],
               (r["frames"] / r["fps"]) if r["fps"] else 0))
        cnt = {}
        for row in r["rows"]:
            cnt[row["cls"]] = cnt.get(row["cls"], 0) + 1
        tot = sum(cnt.values())
        for k in ("大脸", "中脸", "小脸", "超小脸", "多脸", "遮挡/无脸"):
            if k in cnt:
                print("  %-8s %3d 秒  %5.1f%%" % (k, cnt[k], 100.0 * cnt[k] / tot))
        segs = segments(r["rows"])
        print("  成段区间（≥2 秒）：")
        for s in segs:
            print("    %6.1f–%6.1fs  %s" % (s["start"], s["end"], s["cls"]))

    if args.json:
        json.dump([{k: v for k, v in r.items() if k != "rows"} |
                   {"rows": r["rows"][::max(1, len(r["rows"]) // 60)],
                    "segments": segments(r["rows"])} for r in all_res],
                  open(args.json, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1)
        print("\nJSON: %s" % args.json)


if __name__ == "__main__":
    main()
