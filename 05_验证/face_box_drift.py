# -*- coding: utf-8 -*-
"""量「人脸框在相邻检测之间移动了多少」—— P1-1c 的代价必须用这个数结账。

为什么需要它
------------
2.12.0 的 RMS 聚合把真实说话素材的判动率从 42.8% 压到 7%，代价是掩码对齐率
`align%` 从 **68.5% 掉到 23.5%**（同一素材、同一会话、两臂只差一个环境变量）。
`align%` 的含义是「本帧用的是与本帧同源的那份掩码」，它掉下来就等于
「更多帧在用更旧的掩码」，最坏 `maskAge_ = 7`，即 8 帧 @30fps ≈ 267 ms。

问题：267 ms 的旧掩码，到底会让掩码错多少像素？

这个脚本不测画面，只测**输入侧**：把 YuNet 按插件的口径跑一遍真实素材，
统计「间隔 k 帧的两份检测框」之间的中心位移 / 尺寸变化 / IoU。
掩码是从检测框长出来的，框不动，掩码就不动 —— 于是
「旧掩码」与「新掩码」的差就被这个数卡死了，不必再争论。

判据（与插件对齐，不是我自己定的）
----------------------------------
    · 输入 640×640 等比 letterbox（`beauty_face.cpp:42`）
    · score = sqrt(cls*obj)，阈值 0.6、NMS 0.3（`beauty_face.h:65`）
    · 掩码羽化宽度 = 0.12 × 脸宽（`BEAUTY_MASK_FEATHER` 默认自适应）

结论用法：**框位移 << 羽化宽度** 时，align% 下降不构成可见拖影 ——
羽化带本来就把边界抹成了渐变，几十像素的羽化吃掉 1–2 px 的偏移绰绰有余。

用法
----
    py -3 face_box_drift.py                       # 默认 cam_real
    py -3 face_box_drift.py --stride 1 --gaps 1 2 4 8 16
    py -3 face_box_drift.py assets/motion_720p.mp4
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL_CANDIDATES = [
    os.path.join(HERE, "..", "06_离线验证", "models", "yunet.onnx"),
    r"C:/ProgramData/obs-studio/plugins/obs-beauty/bin/64bit/models/yunet.onnx",
]
DET_SIDE = 640
CONF = 0.6
NMS = 0.3
FEATHER_K = 0.12     # 掩码羽化 = 0.12 × 脸宽


def find_model():
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
    raise SystemExit("找不到 yunet.onnx")


def detect_boxes(det, frame):
    """整帧 → 源坐标系的框列表 [(cx, cy, w, h)]。只保留最大的一张脸。"""
    h, w = frame.shape[:2]
    s = min(float(DET_SIDE) / w, float(DET_SIDE) / h)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    small = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_AREA)
    canvas = cv2.copyMakeBorder(small, 0, DET_SIDE - nh, 0, DET_SIDE - nw,
                                cv2.BORDER_CONSTANT, value=(0, 0, 0))
    det.setInputSize((DET_SIDE, DET_SIDE))
    _, faces = det.detect(canvas)
    if faces is None or not len(faces):
        return None
    f = max(faces, key=lambda r: r[2] * r[3])   # 最大的一张
    # faces 的列：x, y, w, h, 5 个关键点, score  —— 都在 canvas 坐标里
    bx, by, bw, bh = f[0] / s, f[1] / s, f[2] / s, f[3] / s
    return (bx + bw / 2.0, by + bh / 2.0, bw, bh)


def scan(path, stride, gaps):
    model = find_model()
    det = cv2.FaceDetectorYN.create(model, "", (DET_SIDE, DET_SIDE), CONF, NMS, 5000)
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit("打不开：%s" % path)
    boxes, i = [], 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if i % stride == 0:
            boxes.append(detect_boxes(det, frame))
        i += 1
    cap.release()
    res = {}
    for g in gaps:
        dxy, dw, iou = [], [], []
        for a in range(len(boxes) - g):
            p, q = boxes[a], boxes[a + g]
            if p is None or q is None:
                continue
            dxy.append(((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5)
            dw.append(abs(p[2] - q[2]) / max(1.0, p[2]))
            iou.append(_iou(p, q))
        res[g] = {"dxy": dxy, "dw": dw, "iou": iou}
    widths = [b[2] for b in boxes if b is not None]
    fw = float(np.median(widths)) if widths else float("nan")
    return len(boxes), sum(1 for b in boxes if b is None), res, fw


def _iou(p, q):
    ax0, ay0 = p[0] - p[2] / 2, p[1] - p[3] / 2
    bx0, by0 = q[0] - q[2] / 2, q[1] - q[3] / 2
    ix = max(0.0, min(ax0 + p[2], bx0 + q[2]) - max(ax0, bx0))
    iy = max(0.0, min(ay0 + p[3], by0 + q[3]) - max(ay0, by0))
    inter = ix * iy
    union = p[2] * p[3] + q[2] * q[3] - inter
    return inter / union if union > 0 else 0.0


def pct(v, q):
    return float(np.percentile(v, q)) if len(v) else float("nan")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*",
                    default=[os.path.join(HERE, "assets", "cam_real_1080x1920.mp4")])
    ap.add_argument("--stride", type=int, default=2,
                    help="每多少帧检测一次（默认 2；stride=1 时 gap 单位就是帧）")
    ap.add_argument("--gaps", type=int, nargs="*", default=[1, 2, 4, 8])
    args = ap.parse_args(argv)

    for path in args.files:
        n, miss, res, fw = scan(path, args.stride, args.gaps)
        feather = FEATHER_K * fw
        print("== %s" % os.path.basename(path))
        print("   检测 %d 帧，未检出 %d 帧（%.1f%%），脸宽中位 %.0f px，"
              "羽化宽度 %.1f px（%.2f × 脸宽）"
              % (n, miss, 100.0 * miss / max(1, n), fw, feather, FEATHER_K))
        print("   实际间隔 = gap × %d 帧" % args.stride)
        print("   %-6s %-9s %-9s %-9s %-11s %-9s" % (
            "gap", "位移p50", "p90", "max", "尺寸变化p50", "IoU中位"))
        for g in args.gaps:
            r = res[g]
            if not r["dxy"]:
                print("   %-6d （无数据）" % g)
                continue
            print("   %-6d %8.2f %9.2f %9.2f %10.3f %9.3f"
                  % (g, pct(r["dxy"], 50), pct(r["dxy"], 90), max(r["dxy"]),
                     pct(r["dw"], 50), pct(r["iou"], 50)))
        print()
    print("读法：位移与「0.12 × 脸宽」的羽化宽度比。羽化带把掩码边界抹成渐变，")
    print("      框位移远小于羽化宽度时，用 8 帧前的掩码与用本帧掩码在画面上")
    print("      不可区分 —— align%% 掉下来才不等于拖影。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
