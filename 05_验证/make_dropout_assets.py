# -*- coding: utf-8 -*-
"""造「脸持续在场、每 N 帧短暂消失 k 帧」的素材（2.11.1 边界压测）。

为什么不用 flash_k* 那批
------------------------
flash_k* 是「空场里闪出人脸」。实测那批**压不到方案C**：`want_thumb` 要求
「当前掩码里正好一张脸（mkfaces0==1）且上一帧全认领（prev_full==1）」，
空场时两条都不成立 → 缩略图两段式分支整段不进（BEAUTY_MOT_DUMP 在这批上
只存到 0~7 张）。所以那批测出来「两臂一致」是真的，但**不是因为晚一帧
没影响，而是因为方案C 压根没参与**（PROGRESS §9.31）。

于是把场景翻过来：**脸一直在**（保证分支开启），每隔 CYCLE 帧抽掉 k 帧
换成空场（模拟低头、走过灯柱、手挡一下）。这个才是方案C 真正要担心的形态：
检测节流着、画面忽然「没脸了」，两臂对「哪一帧被提交」差一帧。

产出
----
    assets/drop_k1_720p.avi / drop_k2 / drop_k4   （30 fps，600 帧）
    assets/drop_k*_720p.json                      （每段真实消失帧数，供核对）

判据与 flash 那批一致：空场用覆盖矩形平均绝对差找出真实消失段，逐帧期望值
必须精确等于 k。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

FPS = 30
FRAMES = 600
CYCLE = 60        # 每 60 帧（2 s）消失一次
OFF = 20          # 从第 20 帧起
MEAS = (250, 0, 830, 720)   # 人像竖条：判「这一帧是不是空场」只看这块


def imread_uni(path):
    with open(path, "rb") as fh:
        buf = fh.read()
    arr = cv2.imdecode(np.frombuffer(buf, dtype=np.uint8), cv2.IMREAD_COLOR)
    if arr is None:
        raise SystemExit("读不出素材：%s" % path)
    return arr


def build_plate(base):
    """空场：把人物竖条用左侧纯背景横向拉过去盖掉（整块与脸无关）。"""
    x0, y0, x1, y1 = MEAS
    cols = base[:, 20:120, :]
    fill = cv2.resize(cols, (x1 - x0, y1 - y0), interpolation=cv2.INTER_LINEAR)
    plate = base.copy()
    plate[y0:y1, x0:x1, :] = fill
    return plate


def write_video(path, frames_iter, wh, codec):
    fourcc = cv2.VideoWriter_fourcc(*("MJPG" if codec == "mjpg" else "mp4v"))
    out = cv2.VideoWriter(path, fourcc, FPS, wh)
    if not out.isOpened():
        raise SystemExit("VideoWriter 打不开：%s" % path)
    n = 0
    for f in frames_iter:
        out.write(f)
        n += 1
    out.release()
    return n


def measure(path, plate):
    """逐帧判「这一帧是不是空场」（这块与空场模板的平均绝对差 ≤ 3 灰阶 → 空场）。"""
    x0, y0, x1, y1 = MEAS
    cap = cv2.VideoCapture(path)
    roi0 = plate[y0:y1, x0:x1, :].astype(np.int16)
    is_face, n = [], 0
    while True:
        ok, fr = cap.read()
        if not ok:
            break
        n += 1
        roi = fr[y0:y1, x0:x1, :].astype(np.int16)
        d = np.abs(roi - roi0).mean()
        is_face.append(d > 3.0)      # True = 脸还在（与空场模板不同）
    cap.release()
    runs, cur = [], 0
    for p in is_face:
        if not p:                    # 连续「空场」段 = 一次消失
            cur += 1
        elif cur:
            runs.append(cur)
            cur = 0
    if cur:
        runs.append(cur)
    return n, is_face, runs


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ks", type=int, nargs="*", default=[1, 2, 4])
    ap.add_argument("--codec", default="mjpg", choices=("mjpg", "mp4v"))
    args = ap.parse_args(argv)

    base_path = os.path.join(ASSETS, "face_front_720p.png")
    if not os.path.exists(base_path):
        sys.exit("缺底图：%s" % base_path)
    src = imread_uni(base_path)
    sh, sw = src.shape[:2]
    plate = build_plate(src)

    for k in args.ks:
        name = "drop_k%d_720p" % k
        ext = ".avi" if args.codec == "mjpg" else ".mp4"
        path = os.path.join(ASSETS, name + ext)

        def gen(k=k):
            for t in range(FRAMES):
                ph = t % CYCLE
                yield plate if OFF <= ph < OFF + k else src

        write_video(path, gen(), (sw, sh), args.codec)
        n, is_face, runs = measure(path, plate)
        exp = len(range(0, FRAMES, CYCLE)) * k
        meta = {
            "tag": name, "file": os.path.basename(path), "wh": [sw, sh],
            "fps": FPS, "frames": n, "cycle": CYCLE, "k": k, "off": OFF,
            "meas": list(MEAS), "codec": args.codec,
            "expect_gone_frames": exp,
            "measured_gone_frames": int(sum(1 for p in is_face if not p)),
            "measured_runs": runs,
            "note": "脸持续在场，每 60 帧消失 k 帧。用来在方案C 真正生效的"
                    "状态（单脸 + 全认领）下量「晚一帧」的后果。",
        }
        with open(os.path.join(ASSETS, name + ".json"), "w",
                  encoding="utf-8") as fh:
            json.dump(meta, fh, ensure_ascii=False, indent=1)
        bad = "" if meta["measured_gone_frames"] == exp else "  ← 与预期不符！"
        print("写出 %-22s %d 帧 %6.1f KB 消失 %d 帧/段 实测段长 %s（预期 %d）%s"
              % (os.path.basename(path), n, os.path.getsize(path) / 1024.0,
                 len(runs), sorted(set(runs)), k, bad))
    print("完成。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
