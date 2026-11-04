# -*- coding: utf-8 -*-
"""从真实素材里取一帧正脸存成静态 PNG（2.13.0，掩码偏移代价实验的输入）。

为什么要真实帧而不是现成的 face_720p.png：
    这一轮要回答的是「掩码比脸旧 d 像素，画面错多少灰阶」。d 的单位是像素，
    而像素的**意义**取决于脸有多大 —— 羽化宽度按源码是 `0.08 × 脸宽`
    （`beauty_core.cpp:822` 的 kMaskFeatherRatio），d 与羽化的比值才是可迁移的量。
    用 720p 合成图里的脸（宽度和真实素材差一截）量出来的灰度代价，
    换算到 1080×1920 的真人画面上会偏。

选帧判据（三档，按顺序）：
    1. 恰好一张脸（多脸会被 `mkfaces0==1` 挡掉，测的不是同一条路径）；
    2. 框离四边都留 ≥ 90 px —— 这条路要能把几何平移 ±64 px 而不出画；
    3. 脸宽最大（磨皮作用面积最大，信噪比最好）。

用法：
    py -3 05_验证/make_still_real.py
    py -3 05_验证/make_still_real.py --margin 90 --skip 1200
"""
from __future__ import annotations

import argparse
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import face_box_drift as D   # noqa: E402  检测口径（唯一实现）

SRC = os.path.join(HERE, "assets", "cam_real_1080x1920.mp4")
DST = os.path.join(HERE, "assets", "real_1080x1920.png")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--dst", default=DST)
    ap.add_argument("--stride", type=int, default=5)
    ap.add_argument("--margin", type=int, default=90,
                    help="脸框到画面四边的最小距离（要给几何平移留余量）")
    a = ap.parse_args(argv)

    model = D.find_model()
    det = cv2.FaceDetectorYN.create(model, "", (D.DET_SIDE, D.DET_SIDE),
                                   D.CONF, D.NMS, 5000)
    cap = cv2.VideoCapture(a.src)
    if not cap.isOpened():
        sys.exit("打不开：%s" % a.src)
    H = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    W = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))

    best = None
    i = 0
    n_face = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if i % a.stride == 0:
            box = D.detect_boxes(det, frame)
            if box is not None:
                cx, cy, w, h = box
                x0, y0 = cx - w / 2, cy - h / 2
                x1, y1 = cx + w / 2, cy + h / 2
                inside = (x0 >= a.margin and y0 >= a.margin and
                          x1 <= W - a.margin and y1 <= H - a.margin)
                if inside:
                    n_face += 1
                    if best is None or w > best[1]:
                        best = (i, w, box, frame.copy())
        i += 1
    cap.release()

    if best is None:
        sys.exit("没有任何一帧满足「单脸 + 离边 ≥ %d px」" % a.margin)

    idx, fw, box, frame = best
    d = os.path.dirname(os.path.abspath(a.dst))
    if d and not os.path.isdir(d):
        os.makedirs(d)
    # 不能直接 cv2.imwrite：路径含中文时它走 ANSI 的 fopen，静默返回 False
    # （PROGRESS §9.22.5 同一个坑）。编码成字节再自己写盘。
    ok, buf = cv2.imencode(".png", frame)
    if not ok:
        sys.exit("PNG 编码失败")
    with open(a.dst, "wb") as fh:
        fh.write(buf.tobytes())

    fe = int(fw * 0.08 + 0.5)
    print("选中帧 %d / 脸宽 %.1f px / 中心 (%.1f, %.1f) / 框 %.1f×%.1f"
          % (idx, fw, box[0], box[1], box[2], box[3]))
    print("掩码羽化宽度 0.08×脸宽 = %d px（kMaskFeatherRatio）" % fe)
    print("候选帧数 %d（stride=%d，共 %d 帧）" % (n_face, a.stride, i))
    print("写入 %s（%d×%d，%.1f MB）"
          % (a.dst, frame.shape[1], frame.shape[0],
             os.path.getsize(a.dst) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
