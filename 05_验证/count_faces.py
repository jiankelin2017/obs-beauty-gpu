#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统计素材里「检出 2 张及以上人脸」的帧占比 —— P1-4 多脸量化的量尺。

为什么要有这支：插件目前的五官/掩码几何按**单脸**设计（`regionGeom.faceCount != 1`
时五官链直接放弃），多脸场景的实际代价没量过。这个脚本不改任何代码，只回答一个
问题：我们手里的素材里，多脸到底占多少。占比太低就不值得为它立项。

判据与插件对齐（不是我自己定的）：
    · 输入 640×640 等比 letterbox —— 见 `beauty_face.cpp:42`
    · score = sqrt(cls*obj)，阈值 **0.6** —— 见 `beauty_face.h:65` 的默认参数
    · NMS IoU **0.3** —— 同上
    · 模型 YuNet 2023mar，与插件同一个 onnx

用法：
    py -3 05_验证/count_faces.py                       # 扫默认素材目录
    py -3 05_验证/count_faces.py <文件或目录> [...]
    py -3 05_验证/count_faces.py --stride 5 assets/     # 视频每 5 帧取 1 帧
    py -3 05_验证/count_faces.py --json out.json 素材/

只读：不复制、不改名、不写回源目录。
"""
import argparse
import json
import os
import sys

import cv2
import numpy as np

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_DIRS = [os.path.join(_ROOT, "05_验证", "assets")]
MODEL_CANDIDATES = [
    os.path.join(_ROOT, "06_离线验证", "models", "yunet.onnx"),
    r"C:/ProgramData/obs-studio/plugins/obs-beauty/bin/64bit/models/yunet.onnx",
]
IMG_EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".webp")
VID_EXTS = (".mp4", ".mkv", ".mov", ".avi")

DET_SIDE = 640      # 与 beauty_face.cpp 的 kDefaultInputSize 一致
CONF = 0.6          # 与 beauty_face.h:65 默认参数一致
NMS = 0.3


def find_model():
    """返回可加载的模型路径。含中文的路径 OpenCV 读不了，复制到 ASCII 临时目录。"""
    import shutil
    import tempfile

    for p in MODEL_CANDIDATES:
        if not os.path.isfile(p):
            continue
        try:
            p.encode("ascii")
            return p
        except UnicodeEncodeError:
            pass
        tmp = os.path.join(tempfile.gettempdir(), "yunet_ascii.onnx")
        if not os.path.isfile(tmp) or os.path.getsize(tmp) != os.path.getsize(p):
            shutil.copyfile(p, tmp)
        return tmp
    raise SystemExit("找不到 yunet.onnx，试过：\n  " + "\n  ".join(MODEL_CANDIDATES))


def make_detector(model):
    det = cv2.FaceDetectorYN.create(model, "", (DET_SIDE, DET_SIDE),
                                    CONF, NMS, 5000)
    return det


def count_one(det, img):
    """返回该图检出的人脸数。输入按等比 letterbox 缩到 640 宽高内，与插件一致。"""
    h, w = img.shape[:2]
    if h <= 0 or w <= 0:
        return 0
    s = min(float(DET_SIDE) / w, float(DET_SIDE) / h)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    small = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_AREA)
    canvas = cv2.copyMakeBorder(small, 0, DET_SIDE - nh, 0, DET_SIDE - nw,
                                cv2.BORDER_CONSTANT, value=(0, 0, 0))
    det.setInputSize((DET_SIDE, DET_SIDE))
    _, faces = det.detect(canvas)
    return 0 if faces is None else int(len(faces))


def scan_image(det, path):
    """读图。必须走 imdecode —— cv2.imread 在 Windows 上按 ANSI 打开路径，
    本项目路径含「插件」两个字会直接读失败（与模型加载同一个坑）。"""
    try:
        with open(path, "rb") as fh:
            buf = fh.read()
    except OSError:
        return None
    arr = cv2.imdecode(np.frombuffer(buf, dtype=np.uint8), cv2.IMREAD_COLOR)
    if arr is None:
        return None
    return [count_one(det, arr)]


def scan_video(det, path, stride):
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        return None
    counts = []
    i = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if i % stride == 0:
            counts.append(count_one(det, frame))
        i += 1
    cap.release()
    return counts


def collect(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                full = os.path.join(p, name)
                if os.path.isfile(full) and name.lower().endswith(IMG_EXTS + VID_EXTS):
                    out.append(full)
        elif os.path.isfile(p):
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(description="统计素材的多脸帧占比")
    ap.add_argument("paths", nargs="*", default=None,
                    help="文件或目录；缺省扫 05_验证/assets")
    ap.add_argument("--stride", type=int, default=10,
                    help="视频抽帧间隔（默认 10 帧取 1）")
    ap.add_argument("--json", default=None, help="把结果另存为 JSON")
    args = ap.parse_args()

    files = collect(args.paths or DEFAULT_DIRS)
    if not files:
        raise SystemExit("没有找到可扫的素材")

    det = make_detector(find_model())
    rows = []
    total_n = 0
    total_multi = 0

    print("%-26s %7s %6s %6s %6s %6s  %s" %
          ("素材", "样本", "face0", "face1", "face2+", "多脸%", "faces 取值"))
    print("-" * 92)
    for f in files:
        name = os.path.basename(f)
        low = name.lower()
        if low.endswith(IMG_EXTS):
            counts = scan_image(det, f)
        elif low.endswith(VID_EXTS):
            counts = scan_video(det, f, max(1, args.stride))
        else:
            continue
        if counts is None:
            print("%-26s  读取失败，跳过" % name)
            continue
        n = len(counts)
        c0 = sum(1 for c in counts if c == 0)
        c1 = sum(1 for c in counts if c == 1)
        cm = sum(1 for c in counts if c >= 2)
        vals = sorted(set(counts))
        pct = (100.0 * cm / n) if n else 0.0
        print("%-26s %7d %6d %6d %6d %5.1f%%  %s" %
              (name, n, c0, c1, cm, pct, vals))
        rows.append({"file": name, "samples": n, "face0": c0, "face1": c1,
                     "face2plus": cm, "multi_pct": round(pct, 2),
                     "values": vals})
        total_n += n
        total_multi += cm

    print("-" * 92)
    allpct = (100.0 * total_multi / total_n) if total_n else 0.0
    print("合计：%d 个样本，多脸 %d 个，占比 **%.2f%%**" %
          (total_n, total_multi, allpct))
    print()
    print("判据（既定）：占比 ≥5%% 才为多脸立项；本次 %.2f%% → %s" %
          (allpct, "达标，值得立项" if allpct >= 5.0 else "未达标，不立项"))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"stride": args.stride, "conf": CONF, "nms": NMS,
                       "det_side": DET_SIDE, "rows": rows,
                       "total_samples": total_n,
                       "total_multi": total_multi,
                       "overall_multi_pct": round(allpct, 2)}, fh,
                      ensure_ascii=False, indent=2)
        print("JSON 已写入 %s" % args.json)


if __name__ == "__main__":
    main()
