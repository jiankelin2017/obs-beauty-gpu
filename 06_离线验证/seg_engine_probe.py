#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
真实引擎链路上的分割探针 —— 不启动 OBS，跑 CoreEngine 的**同一条** updateMask。

为什么需要它：`segtest` 是自己重写了一遍「裁脸 → 推理 → 写掩码」的坐标数学，
只能证明模型本身可用；而插件里真正跑的是 buildMaskInto → 取最优脸 →
skinsegRun → 发布 → 宿主拷贝。这条链以前只在 async worker 里走，离线台强制
同步检测，所以一次都没在桌面上验过。本脚本补上这一段。

用法：
    python seg_engine_probe.py --frame goldens/frame_1280x720.raw --w 1280 --h 720 \
        --model models/selfie_multiclass_256.onnx --profile selfie6 --out _seg_engine

输出：
    <out>.rgba  512×512×4（R 皮肤 / G 五官 / B 非肤 / A 皮肤）
    <out>.png   四联图（原图裁切 | R 热图 | B 热图 | R×(1−B) 合成）
    控制台：crop/uv/ms + 脸区/背景/头发各通道均值
"""
import argparse
import ctypes
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "core_probe.dll")


def find_yunet():
    env = os.environ.get("BEAUTY_FACE_MODEL")
    if env and os.path.isfile(env):
        return env
    for p in (os.path.join(HERE, "models", "yunet.onnx"),):
        if os.path.isfile(p):
            return p
    raise SystemExit("找不到 yunet.onnx，请用 BEAUTY_FACE_MODEL 指定")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", default=os.path.join(HERE, "goldens", "frame_1280x720.raw"))
    ap.add_argument("--w", type=int, default=1280)
    ap.add_argument("--h", type=int, default=720)
    ap.add_argument("--model", required=True)
    ap.add_argument("--profile", default="selfie6", help="selfie6 或留空 = bisenet19")
    ap.add_argument("--every", type=int, default=1, help="BEAUTY_SEG_EVERY，验链路用 1")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--out", default=os.path.join(HERE, "_seg_engine"))
    args = ap.parse_args()

    os.environ.setdefault("BEAUTY_FACE_MODEL", find_yunet())
    os.environ["BEAUTY_SEG"] = "1"
    os.environ["BEAUTY_SEG_MODEL"] = os.path.abspath(args.model)
    os.environ["BEAUTY_SEG_EVERY"] = str(args.every)
    os.environ["BEAUTY_SEG_THREADS"] = str(args.threads)
    os.environ["BEAUTY_SEG_PROFILE"] = args.profile
    os.environ["BEAUTY_ENGINE_IMPL"] = "core"

    # 先把本目录的 onnxruntime.dll 顶上去。
    # beauty_onnx.cpp 用 LoadLibraryA("onnxruntime.dll") 按名字加载，Windows 的
    # 搜索顺序里 **python.exe 所在目录** 排在 cwd 之前 —— 机器上装了 pip 版
    # onnxruntime 时会被抢先命中（1.17.1 / API 17），于是报
    # 「requested API version [20] is not available」。按全路径先加载一份，
    # 后面按名字加载就会复用这个已加载模块。
    ort = os.path.join(HERE, "onnxruntime.dll")
    if os.path.isfile(ort):
        ctypes.WinDLL(ort)

    dll = ctypes.WinDLL(DLL)
    msg = ctypes.create_string_buffer(1024)
    # 1) 是否真的启动（模型加载成功）
    ok = dll.BeautyCoreSegInit()
    if not ok:
        # 三种失败原因（路径错 / 尺寸不符 / ORT 版本）以前都只显示一句
        # 「分割未启动」，现在把引擎侧的原话打出来。
        try:
            dll.BeautyCoreSegError.restype = ctypes.c_char_p
            why = dll.BeautyCoreSegError()
            why = why.decode("utf-8", "replace") if why else ""
        except Exception:
            why = ""
        raise SystemExit("分割未启动：" + (why or "（引擎未给出原因）") +
                         "；检查 BEAUTY_SEG_MODEL / BEAUTY_SEG_PROFILE")
    print("分割已启动")

    # 2) 跑一次掩码（内部走同步 updateMask → runSegIfDue）
    mask = ctypes.create_string_buffer(args.w * args.h)
    rc = dll.BeautyCoreMask(args.frame.encode("utf-8"), args.w, args.h,
                            (args.out + "_mask.bin").encode("utf-8"), msg, 1024)
    print(f"BeautyCoreMask rc={rc} {msg.value.decode('utf-8', 'replace')}")
    if rc != 0:
        sys.exit(1)

    # 3) 割出来的语义纹理
    rc = dll.BeautyCoreSegDump((args.out + ".rgba").encode("utf-8"), msg, 1024)
    print(f"BeautyCoreSegDump rc={rc} {msg.value.decode('utf-8', 'replace')}")
    if rc != 0:
        sys.exit(1)

    raw = np.fromfile(args.out + ".rgba", np.uint8)
    W = H = 512
    hdr = np.frombuffer(raw[:16].tobytes(), np.int32)      # w,h,cropped,ms
    uv = np.frombuffer(raw[16:32].tobytes(), np.float32)   # 裁切框（帧归一化）
    seg = raw[32:].reshape(H, W, 4)
    print(f"纹理 {W}x{H} crop={hdr[2]} ms={hdr[3]} uv={tuple(round(float(v), 4) for v in uv)}")

    # 4) 回贴到原图，量各区域均值
    frame = np.fromfile(args.frame, np.uint8).reshape(args.h, args.w, 4)[:, :, :3]
    R, G, Bch, A = (seg[:, :, i].astype(np.float32) / 255.0 for i in range(4))
    comp = R * (1.0 - Bch)

    def to_frame(img512):
        """512 语义图 → 全帧（裁切框外填 0）。"""
        full = np.zeros((args.h, args.w), np.float32)
        x0 = int(round(uv[0] * args.w))
        y0 = int(round(uv[1] * args.h))
        wd = int(round(uv[2] * args.w))
        ht = int(round(uv[3] * args.h))
        wd = max(1, min(wd, args.w - x0))
        ht = max(1, min(ht, args.h - y0))
        import cv2
        full[y0:y0 + ht, x0:x0 + wd] = cv2.resize(img512, (wd, ht), interpolation=cv2.INTER_LINEAR)
        return full

    print("\n区域均值（R 皮肤 / B 非肤 / R×(1−B) 实际权重）：")
    regions = {
        "整帧": np.ones((args.h, args.w), bool),
        "裁切框": None,   # 下面单独填
    }
    box = np.zeros((args.h, args.w), bool)
    bx0, by0 = int(round(uv[0] * args.w)), int(round(uv[1] * args.h))
    bw, bh = int(round(uv[2] * args.w)), int(round(uv[3] * args.h))
    box[by0:by0 + bh, bx0:bx0 + bw] = True
    regions["裁切框"] = box
    # 脸区：用掩码中心 1/3 方块近似
    fx, fy = bx0 + bw // 3, by0 + bh // 3
    face = np.zeros((args.h, args.w), bool)
    face[fy:fy + bh // 3, fx:fx + bw // 3] = True
    regions["脸中心"] = face

    for name, sel in regions.items():
        if sel is None:
            continue
        for tag, img in (("R", R), ("B", Bch), ("R(1-B)", comp)):
            m = to_frame(img)[sel].mean()
            print(f"  {name:6s} {tag:6s} {m:.3f}")

    # 5) 四联图
    try:
        import cv2
        crop = frame[by0:by0 + bh, bx0:bx0 + bw]
        if crop.size == 0:
            crop = frame
        crop = cv2.resize(crop, (512, 512))
        heat = lambda a: cv2.applyColorMap((np.clip(a, 0, 1) * 255).astype(np.uint8), cv2.COLORMAP_JET)
        blend = (crop.astype(np.float32) * 0.35 + 40).astype(np.uint8)
        keep = comp > 0.5
        blend[keep] = (crop[keep] * 0.85).astype(np.uint8)
        out = np.hstack([crop, heat(R), heat(Bch), blend])
        cv2.imencode(".png", out)[1].tofile(args.out + ".png")
        print(f"\n看图：{args.out}.png  （左=输入裁切 2=R 3=B 4=R×(1−B) 提亮处）")
    except ImportError:
        print("（没装 opencv，跳过四联图）")


if __name__ == "__main__":
    main()
