# -*- coding: utf-8 -*-
"""方案2：把「判动」按扰动来源拆开 —— 噪声 / 手抖 / 自动曝光漂移各占多少。

为什么要有它
------------
2.11.1 在真实素材（`assets/cam_real_1080x1920.mp4`，固定机位、单人正脸说话）上
量到 **判动 42.8%、nopix 33.5%**，而合成素材（`drop_k1`）是 3.4% / 76%。差一个
数量级。这个差必须拆开，否则不知道「是噪声把闸门顶穿了」还是「人真的在动」。

本脚本从**同一帧真实画面**出发，逐个叠加单一扰动，各自跑一遍判定：

    冻结帧            → 噪声/抖动/AGC 全为 0 时的下限（判动应为 0）
    + 传感器噪声 σ     → 只考噪声
    + 手抖 1/2 px      → 只考几何微动
    + 自动曝光/白平衡   → 只考整帧增益漂移
    三者叠加           → 模拟一台差摄像头

判据仍用 `motion_model.py`（与引擎同一套数学：640 letterbox → 64×64 → 16 格 →
4×4 块均值取最大 → 与 6 比）。输出「判动%」就是 OBS 里 `判动 hit/calls` 的预测值。

不写盘也能出结论（默认），给 OBS 端到端用才加 `--write`。

用法
----
    py -3 05_验证/make_camera_assets.py                  # 只算，不写盘
    py -3 05_验证/make_camera_assets.py --write          # 同时写 assets/cam_*.avi
    py -3 05_验证/make_camera_assets.py --base 某帧.png
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import motion_model as M   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
REAL_VIDEO = os.path.join(ASSETS, "cam_real_1080x1920.mp4")
FACE_PNG = os.path.join(ASSETS, "face_front_720p.png")
FRAMES = 300
FPS = 30

# 档位：(tag, σ噪声, 手抖 px, AGC 随机游走每帧σ, AGC 跳变幅度, 跳变周期, 说明)
# 取值依据是「2.11.1 对三种扰动的离线量化」（见 PROGRESS §9.32 的表）：
#   σ=16 已是极端传感器噪声；手抖 2 px 是临界；AGC 5% 是临界。
SPECS = [
    ("freeze",   0.0, 0.0, 0.0,  0.0,  0, "冻结帧（三条扰动全关）"),
    ("noise4",   4.0, 0.0, 0.0,  0.0,  0, "只有传感器噪声 σ=4"),
    ("noise8",   8.0, 0.0, 0.0,  0.0,  0, "只有传感器噪声 σ=8"),
    ("noise16", 16.0, 0.0, 0.0,  0.0,  0, "只有传感器噪声 σ=16（极端）"),
    ("shake1",   0.0, 1.0, 0.0,  0.0,  0, "只有手抖（稳态 ≈1 px）"),
    ("shake2",   0.0, 2.0, 0.0,  0.0,  0, "只有手抖（稳态 ≈2 px，临界）"),
    ("agc2",     0.0, 0.0, 0.001, 0.02, 60, "只有曝光/白平衡漂移（±2% 台阶，每 2 s）"),
    ("agc5",     0.0, 0.0, 0.002, 0.05, 60, "只有曝光/白平衡漂移（±5% 台阶，每 2 s，临界）"),
    ("laptop",   8.0, 1.0, 0.001, 0.02, 60, "差笔记本摄像头典型（三者叠加）"),
    ("worst",   16.0, 2.0, 0.002, 0.05, 60, "压力上限（三者都取临界以上）"),
]


def load_base(path):
    """读一张底图并缩到 720p（竖屏素材缩到 720 高）。"""
    if path.lower().endswith((".mp4", ".avi", ".mov", ".mkv")):
        cap = cv2.VideoCapture(path)
        if not cap.isOpened():
            raise SystemExit("打不开：%s" % path)
        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        cap.set(cv2.CAP_PROP_POS_FRAMES, max(0, total // 10))   # 跳过片头
        ok, frame = cap.read()
        cap.release()
        if not ok:
            raise SystemExit("读不出帧：%s" % path)
        return frame
    with open(path, "rb") as fh:
        buf = fh.read()
    img = cv2.imdecode(np.frombuffer(buf, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit("读不出图片：%s" % path)
    return img


def fit_720(base):
    """统一到长边 1280，保持比例 —— 与 720p 基线可比。"""
    h, w = base.shape[:2]
    s = 1280.0 / max(h, w)
    if s >= 1.0:
        return base
    return cv2.resize(base, (int(w * s + 0.5), int(h * s + 0.5)),
                      interpolation=cv2.INTER_AREA)


def gen_sequence(base, frames=FRAMES, sigma=0.0, shake=0.0, drift=0.0,
                 jump=0.0, every=0, seed=7):
    """逐帧施加扰动。三种扰动的模型都与真实成因对齐：

    · 噪声：逐帧独立高斯，三通道独立（传感器读出噪声 + 压缩残差）；
    · 手抖：一阶自回归（AR(1)），围绕中心小幅游走 —— 比纯随机更接近手持；
    · 曝光/白平衡：增益做随机游走（缓慢漂），另加周期性跳变（AGC 收敛时的台阶）。
    """
    rng = np.random.default_rng(seed)
    h, w = base.shape[:2]
    gain = 1.0
    dx = dy = 0.0
    for i in range(frames):
        if drift:
            gain *= 1.0 + rng.normal(0.0, drift)
        if jump and every and i > 0 and i % every == 0:
            gain *= 1.0 + (jump if rng.random() < 0.5 else -jump)
        gain = float(np.clip(gain, 0.75, 1.30))

        f = base
        if shake:
            dx = dx * 0.85 + rng.normal(0.0, shake * 0.5)
            dy = dy * 0.85 + rng.normal(0.0, shake * 0.5)
            dx = float(np.clip(dx, -2.0 * shake, 2.0 * shake))
            dy = float(np.clip(dy, -2.0 * shake, 2.0 * shake))
            Mt = np.float32([[1, 0, dx], [0, 1, dy]])
            f = cv2.warpAffine(f, Mt, (w, h), flags=cv2.INTER_LINEAR,
                               borderMode=cv2.BORDER_REPLICATE)
        if gain != 1.0 or sigma:
            ff = f.astype(np.float32)
            if gain != 1.0:
                ff *= gain
            if sigma:
                ff += rng.normal(0.0, sigma, ff.shape)
            f = np.clip(ff, 0, 255).astype(np.uint8)
        yield f


def write_avi(path, frames, size):
    """mjpg 逐帧独立压缩 —— 不做时域预测，噪声不会被编码器吃掉（保守侧）。"""
    fourcc = cv2.VideoWriter_fourcc(*"MJPG")
    vw = cv2.VideoWriter(path, fourcc, FPS, size)
    if not vw.isOpened():
        raise SystemExit("写不了：%s" % path)
    n = 0
    for f in frames:
        vw.write(f)
        n += 1
    vw.release()
    return n


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=REAL_VIDEO,
                    help="底图：视频（取 1/10 处那一帧）或图片")
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--write", action="store_true", help="同时写 assets/cam_*.avi")
    args = ap.parse_args(argv)

    base = load_base(args.base if args.base else (REAL_VIDEO if os.path.isfile(REAL_VIDEO) else FACE_PNG))
    base = fit_720(base)
    print("底图：%s  → %dx%d" % (os.path.basename(args.base), base.shape[1], base.shape[0]))
    print("每档 %d 帧（%.1f s @%d fps）；判据与引擎同一套（阈值 6，linear 缩放 = 保守侧）"
          % (args.frames, args.frames / float(FPS), FPS))
    print()
    print("%-9s %6s %6s %8s %12s %12s  %s"
          % ("档位", "σ", "手抖", "AGC", "判动%（linear）", "判动%（area）", "说明"))
    print("-" * 108)

    out = []
    for tag, sigma, shake, drift, jump, every, desc in SPECS:
        res = {}
        for interp in ("linear", "area"):
            seq = gen_sequence(base, args.frames, sigma, shake, drift, jump, every)
            vals = M.predict_frames(seq, interp=interp)
            a = np.asarray(vals)
            res[interp] = {
                "hot": int((a > M.DEFAULT_THR).sum()),
                "calls": len(a),
                "ratio": float((a > M.DEFAULT_THR).mean()),
                "p50": float(np.percentile(a, 50)),
                "p90": float(np.percentile(a, 90)),
                "mx": float(a.max()),
            }
        rec = {
            "tag": tag, "sigma": sigma, "shake": shake,
            "drift": drift, "jump": jump, "desc": desc,
            "frames": args.frames,
            "linear": res["linear"], "area": res["area"],
        }
        out.append(rec)
        agc = ("%.1f%%/帧" % (drift * 100)) if drift else "-"
        if jump:
            agc += "+%.0f%%跳" % (jump * 100)
        print("%-9s %6.0f %6.1f %8s   %5.1f%%  p90 %5.2f  %5.1f%%  %s"
              % (tag, sigma, shake, agc,
                 res["linear"]["ratio"] * 100, res["linear"]["p90"],
                 res["area"]["ratio"] * 100, desc))

    print()
    print("读法：`freeze` 必须是 0.0%（否则脚本或模型有问题）；单档的判动% 就是")
    print("      该类扰动单独贡献的「会误判成动」的帧比例。叠加档看它们是否可加。")

    if args.write:
        os.makedirs(ASSETS, exist_ok=True)
        print()
        for (tag, sigma, shake, drift, jump, every, _), rec in zip(SPECS, out):
            p = os.path.join(ASSETS, "cam_%s_720p.avi" % tag)
            seq = gen_sequence(base, args.frames, sigma, shake, drift, jump, every)
            n = write_avi(p, seq, (base.shape[1], base.shape[0]))
            rec["file"] = os.path.basename(p)
            print("写出 %-24s %d 帧  %5.1f MB" % (os.path.basename(p), n,
                                                 os.path.getsize(p) / 1048576.0))

    jp = os.path.join(HERE, "out", "cam_assets.json")
    os.makedirs(os.path.dirname(jp), exist_ok=True)
    with open(jp, "w", encoding="utf-8") as fh:
        json.dump({"base": os.path.basename(args.base), "spec": out}, fh,
                  ensure_ascii=False, indent=2)
    print("\nJSON: %s" % jp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
