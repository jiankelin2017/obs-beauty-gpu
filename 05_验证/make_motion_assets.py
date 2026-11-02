# -*- coding: utf-8 -*-
"""S3b 拖影专项素材：逐帧运动的人脸 mp4。

为什么要文件而不是 websocket 挪源：
    obs-websocket 一个来回 2~5 ms，30 fps 下做不到「每帧固定一步」；
    而每 0.5 s 跳一次，跳完掩码早追平了，量不出「一帧滞后」。
    只有让画面**每帧匀速走固定像素**，滞后一帧才会表现为固定的位移差。

素材构造：
    底图 = face_*_single.png 的整幅（主脸清晰、背景虚化）
    贴片 = 底图中心那块（含整张脸），每帧平移到 x(t)，三角波往复
    左上角烧帧号（便于人眼核对），贴片外区域保持静止

真值随素材一起写出（motion_*.json），分析时按 x_src(t) 与录制的 x_out(t)
做互相关即可得到滞后帧数 —— 不需要在运行时同步任何东西。

用法：
    py -3 make_motion_assets.py            # 生成 720p + 4K
    py -3 make_motion_assets.py --only 720p
"""
import argparse
import json
import os
import shutil
import tempfile

import cv2
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

FPS = 30
FRAMES = 300          # 10 s
HALF_PERIOD = 30      # 30 帧走半程（1 s 一个来回 = 0.5 Hz）
FADE = 24             # 贴片羽化边宽（px，按 720p 基准，4K 按比例放大）


def tri(t, amp, half):
    """三角波：t 帧 → 位移（px）。周期 2*half。"""
    p = t % (2 * half)
    return amp * (p / half) if p < half else amp * (2 - p / half)


def imread_uni(path):
    """cv2.imread 不认中文路径（Windows 下按 ANSI 打开）—— 走 PIL。"""
    return np.array(Image.open(path).convert("RGB"))[:, :, ::-1].copy()


def build(tag, src_name, out_wh, step, amp, face_box):
    sw, sh = out_wh
    src = imread_uni(os.path.join(ASSETS, src_name))
    if src is None:
        raise SystemExit("缺素材：%s" % src_name)
    sx, sy = src.shape[1] / float(sw), src.shape[0] / float(sh)

    # 贴片：按输出坐标给出，再映射回源图裁剪
    fx0, fy0, fx1, fy1 = face_box
    px0, py0 = int(fx0 * sx), int(fy0 * sy)
    px1, py1 = int(fx1 * sx), int(fy1 * sy)
    patch = src[py0:py1, px0:px1].copy()
    pw, ph = fx1 - fx0, fy1 - fy0
    patch = cv2.resize(patch, (pw, ph), interpolation=cv2.INTER_AREA)

    # 贴片羽化 alpha（矩形软边，避免接缝被当成边缘细节影响磨皮对比）
    fe = max(2, int(FADE * (pw / 480.0)))
    a = np.ones((ph, pw), np.float32)
    ramp = np.linspace(0.0, 1.0, fe, dtype=np.float32)
    a[:, :fe] *= ramp
    a[:, -fe:] *= ramp[::-1]
    a[:fe, :] *= ramp[:, None]
    a[-fe:, :] *= ramp[::-1][:, None]
    a3 = a[:, :, None]

    # 底图：源图整幅**重度虚化**后作为静止背景。
    # ⚠ 不能直接用源图：源图中央就有那张清晰的脸，贴片移开后它会露出来，
    # 于是画面里同时存在「静止的脸 + 运动的脸」→ 检测器多脸（帧环整段绕过），
    # 模板匹配也会命中静止那张（2026-11-02 自检误差 102/180 px 就是这么来的）。
    blur = int(sh / 12) | 1
    base_src = cv2.GaussianBlur(src, (blur, blur), 0)
    base_src = (base_src.astype(np.float32) * 0.82).astype(np.uint8)
    base = cv2.resize(base_src, (sw, sh), interpolation=cv2.INTER_AREA)

    out = os.path.join(ASSETS, "motion_%s.mp4" % tag)
    # VideoWriter 同样不认中文路径：先写到系统临时目录（ASCII），再搬回来
    tmp = os.path.join(tempfile.gettempdir(), "motion_%s.mp4" % tag)
    vw = cv2.VideoWriter(tmp, cv2.VideoWriter_fourcc(*"mp4v"), FPS, (sw, sh))
    if not vw.isOpened():
        raise SystemExit("VideoWriter 打不开（缺 mp4v 编码器）")

    fh_scale = sh / 720.0
    xs = []
    for t in range(FRAMES):
        canvas = base.copy()
        dx = int(round(tri(t, amp, HALF_PERIOD)))
        x = (sw - pw) // 2 + dx
        y = (sh - ph) // 2
        roi = canvas[y:y + ph, x:x + pw].astype(np.float32)
        canvas[y:y + ph, x:x + pw] = (patch.astype(np.float32) * a3 +
                                      roi * (1.0 - a3)).astype(np.uint8)
        # 帧号烧录（左上角，远离人脸）
        txt = "%03d" % t
        cv2.putText(canvas, txt, (int(24 * fh_scale), int(56 * fh_scale)),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.4 * fh_scale, (255, 255, 255),
                    max(2, int(3 * fh_scale)), cv2.LINE_AA)
        vw.write(canvas)
        xs.append(x)
    vw.release()
    # 跨盘（TEMP 在 C:）走 copy2 + remove；shutil.move 在这台机器上会触发
    # safe-delete 报 "Some operations were aborted"（2026-11-02 踩过）。
    try:
        os.replace(tmp, out)
    except OSError:
        shutil.copy2(tmp, out)
        try:
            os.remove(tmp)
        except OSError:
            pass

    meta = {
        "tag": tag, "file": os.path.basename(out), "wh": [sw, sh], "fps": FPS,
        "frames": FRAMES, "half_period": HALF_PERIOD, "amp": amp, "step": step,
        "face_box": face_box, "patch_wh": [pw, ph],
        "x0": xs[0], "x": xs,          # 每帧贴片左边界真值
    }
    with open(os.path.join(ASSETS, "motion_%s.json" % tag), "w",
              encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)

    print("写出 %s  %dx%d  %d 帧  %.1f KB"
          % (os.path.basename(out), sw, sh, FRAMES,
             os.path.getsize(out) / 1024.0))
    return out, meta


def verify(path, meta):
    """自检：贴片位置能否被模板匹配准确测回（分析脚本依赖这一点）。"""
    tmp = os.path.join(tempfile.gettempdir(), "verify_%s.mp4" % meta["tag"])
    shutil.copy2(path, tmp)
    cap = cv2.VideoCapture(tmp)
    ok, ref = cap.read()
    if not ok:
        print("  !! 读不出首帧")
        cap.release()
        return False
    pw, ph = meta["patch_wh"]
    x0 = meta["x"][0]
    y0 = (meta["wh"][1] - ph) // 2
    tpl = ref[y0 + 40:y0 + ph - 40, x0 + 40:x0 + pw - 40]
    errs = []
    for t in (0, 17, 45, 90, 143):
        cap.set(cv2.CAP_PROP_POS_FRAMES, t)
        ok, fr = cap.read()
        if not ok:
            errs.append(999)
            continue
        r = cv2.matchTemplate(cv2.cvtColor(fr, cv2.COLOR_BGR2GRAY),
                              cv2.cvtColor(tpl, cv2.COLOR_BGR2GRAY),
                              cv2.TM_CCOEFF_NORMED)
        _, _, _, loc = cv2.minMaxLoc(r)
        # 期望位置 = 该帧贴片左边界 + 模板在贴片内的偏移（40）
        errs.append(abs(loc[0] - (meta["x"][t] + 40)))
    cap.release()
    try:
        os.remove(tmp)
    except OSError:
        pass
    print("  位置可测性：误差 %s px（应全为 0）" % errs)
    return max(errs) <= 1


JOBS = [
    # tag, 源图, 输出尺寸, step, amp, 贴片框（输出坐标系）
    ("720p", "face_720p_single.png", (1280, 720), 6, 180, (400, 60, 880, 660)),
    ("4k", "face_4k_single.png", (3840, 2160), 18, 540, (1200, 180, 2640, 1980)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=("720p", "4k"), help="只生成一档")
    args = ap.parse_args()
    for tag, src, wh, step, amp, box in JOBS:
        if args.only and args.only != tag:
            continue
        p, meta = build(tag, src, wh, step, amp, box)
        verify(p, meta)


if __name__ == "__main__":
    main()
