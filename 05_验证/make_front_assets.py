#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一张正脸人像合成成 16:9 的 4K / 720p 验收素材，并用插件同款 YuNet 复检。

为什么需要这支：现有 `face_4k*.png` 出自一组活动抓拍，主人物是**四分之三侧脸**
（实测 yaw≈0.21、可见单侧耳），掩码覆盖率与五官椭圆几何两项指标不能外推到正面
主持人。本机图库里没有可用的正面近景（相机原片主脸仅占画面 3%~25%，其余候选是
截图 / 聊天图 / 带水印的低分辨率合成图），因此改为合成。

构图做法（与 make_assets.py 的「几何不动、只糊背景」同源）：
    1. 原图按人脸框等比放大到「脸高 ≈ 画布高 × FACE_FRAC」；
    2. 画布背景 = 同一张图铺满 + 高斯模糊（模拟近景散景），保证色调一致；
    3. 清晰层贴到画面中心偏上，边缘羽化过渡到背景，避免出现矩形硬边；
    4. 4K 出图后再降采样出 720p，两档构图一致，只有分辨率不同。

用法：
    py -3 05_验证/make_front_assets.py <人像路径> [--frac 0.55]

产出（05_验证/assets/，已 gitignore）：
    face_front_4k.png    3840x2160
    face_front_720p.png  1280x720
    face_front_meta.json 来源、哈希、检测指标（便于复现与审计）
"""
import argparse
import hashlib
import json
import math
import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
MODEL_CANDIDATES = [
    os.path.join(os.path.dirname(HERE), "06_离线验证", "models", "yunet.onnx"),
    r"C:/ProgramData/obs-studio/plugins/obs-beauty/bin/64bit/models/yunet.onnx",
]
CANVAS = (3840, 2160)
OUT_4K = "face_front_4k.png"
OUT_720 = "face_front_720p.png"
OUT_META = "face_front_meta.json"

FACE_CX = 0.500      # 人脸中心在画布上的横向位置（比例）
FACE_CY = 0.500      # 纵向位置：脸中心对齐中线，头顶留出余量
FEATHER = 0.26       # 清晰层边缘羽化宽度（占清晰层短边比例）
BG_BLUR = 110        # 背景虚化半径（4K 像素）


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def model_path():
    for p in MODEL_CANDIDATES:
        if os.path.isfile(p):
            try:
                p.encode("ascii")
                return p
            except UnicodeEncodeError:
                import shutil
                import tempfile
                dst = os.path.join(tempfile.gettempdir(), "beauty_scan", "yunet.onnx")
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not os.path.isfile(dst) or os.path.getsize(dst) != os.path.getsize(p):
                    shutil.copyfile(p, dst)
                return dst
    sys.exit("找不到 yunet.onnx")


def detect(img_bgr, score_min=0.5):
    """返回 (faces, 主脸指标 dict 或 None)。输入 BGR。"""
    det = cv2.FaceDetectorYN.create(model_path(), "", (640, 640), score_min, 0.3, 5000)
    h, w = img_bgr.shape[:2]
    small = img_bgr
    sc = 1.0
    if max(h, w) > 1600:                     # 4K 直接送 640 会漏检小脸，先缩
        sc = 1600.0 / max(h, w)
        small = cv2.resize(img_bgr, (int(w * sc), int(h * sc)), interpolation=cv2.INTER_AREA)
    det.setInputSize((small.shape[1], small.shape[0]))
    _ret, faces = det.detect(small)
    if faces is None or len(faces) == 0:
        return 0, None
    f = sorted(faces, key=lambda x: x[2] * x[3], reverse=True)[0].copy()
    f[0:4] /= sc
    rex, rey, lex, ley = f[4], f[5], f[6], f[7]
    nx, ny = f[8], f[9]
    mrx, mry, mlx, mly = f[10], f[11], f[12], f[13]
    iod = float(np.hypot(lex - rex, ley - rey))
    ex, ey = (rex + lex) / 2.0, (rey + ley) / 2.0
    my = (mry + mly) / 2.0
    return len(faces), {
        "yaw": float(abs(nx - ex) / iod),
        "roll": float(abs(np.degrees(np.arctan2(ley - rey, lex - rex)))),
        "pitch": float(abs(my - ey) / iod),
        "face_frac": float(f[3]) / h,
        "score": float(f[14]),
        "box": [int(v) for v in f[0:4]],
    }


def warm_to_skin(src, box, cb_t, cr_t, target_r2=0.20):
    """把肤色色度移进插件肤色椭圆，但不推到正中心。

    为什么必须做：插件用 YCbCr 椭圆判肤色（beauty_core.cpp 的 skinWeight，中心
    cb=-0.065 / cr=+0.095，半径 0.075/0.070）。AI 生成的人像偏灰、几乎无色度，
    实测主脸区 cb=-0.012 / cr=+0.015 → r²=1.80 落在椭圆外，于是掩码恒为 0，
    日志里 `faces` 也变 0（真机 4K / 720p 均复现）。

    为什么只推到 target_r2 而不是中心：推到中心 = 把肤色拉成标准肤色，整张图会
    明显偏暖（像钨丝灯）。1024 张对照里真实素材的 r² 是 0.11，这里取 0.20 ——
    刚进椭圆、留出余量，观感仍是自然室内光。
    """
    a = np.asarray(src).astype(np.float32) / 255.0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b
    x, y, w, h = box
    cb0 = float(np.median(cb[y:y + h, x:x + w]))
    cr0 = float(np.median(cr[y:y + h, x:x + w]))
    du, dv = (cb0 - cb_t) / 0.075, (cr0 - cr_t) / 0.070
    r2 = du * du + dv * dv
    t = 1.0 - math.sqrt(min(1.0, target_r2 / r2)) if r2 > 0 else 0.0
    dcb, dcr = t * (cb_t - cb0), t * (cr_t - cr0)
    yv = 0.299 * r + 0.587 * g + 0.114 * b
    cb2, cr2 = cb + dcb, cr + dcr
    r2o = yv + 1.402 * cr2
    g2 = yv - 0.344136 * cb2 - 0.714136 * cr2
    b2 = yv + 1.772 * cb2
    out = np.clip(np.stack([r2o, g2, b2], axis=-1) * 255.0, 0, 255).astype(np.uint8)
    print("肤色色度校正: Δcb=%+.3f Δcr=%+.3f（推到椭圆内 r²≈%.2f）"
          % (dcb, dcr, target_r2))
    return Image.fromarray(out)


def feathered_mask(size, feather):
    """中心清晰、四周线性淡出的蒙版（羽化宽度按短边比例）。"""
    w, h = size
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    fw = int(min(w, h) * feather)
    steps = max(8, fw // 8)
    # 由外向内叠加矩形，越内越亮 —— 等价于四边羽化
    for i in range(steps, 0, -1):
        inset = int(fw * i / float(steps))
        v = int(255 * (1.0 - (i - 1) / float(steps)))
        d.rectangle([inset, inset, w - 1 - inset, h - 1 - inset], fill=v)
    return m


def skin_r2(src, box, cb_t=-0.065, cr_t=0.095, ru=0.075, rv=0.070):
    """主脸区中位色度到插件肤色椭圆中心的归一化距离平方（<1 即在椭圆内）。"""
    a = np.asarray(src).astype(np.float32) / 255.0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b
    x, y, w, h = box
    du = (float(np.median(cb[y:y + h, x:x + w])) - cb_t) / ru
    dv = (float(np.median(cr[y:y + h, x:x + w])) - cr_t) / rv
    return du * du + dv * dv


def build(src_path, frac, crop_bottom=0, skin_target=True):
    src = ImageOps.exif_transpose(Image.open(src_path)).convert("RGB")
    if crop_bottom > 0:
        # 生成图右下角常带「AI生成」水印，先裁掉再构图，否则水印会落在画面里
        src = src.crop((0, 0, src.width, src.height - int(crop_bottom)))
    arr = cv2.cvtColor(np.array(src), cv2.COLOR_RGB2BGR)
    n, m = detect(arr)
    if n == 0:
        sys.exit("这张图里没检出人脸，无法定位构图")
    if n > 1:
        print("⚠ 检出 %d 张脸，取最大的一张；请确认合成后仍为单脸" % n)

    if skin_target:
        r2_before = skin_r2(src, m["box"])
        src = warm_to_skin(src, m["box"], -0.065, 0.095)
        arr = cv2.cvtColor(np.array(src), cv2.COLOR_RGB2BGR)
        n2, m2 = detect(arr)
        if n2 == 0:
            sys.exit("色度校正后反而检不出人脸，请检查原图")
        m = m2
        print("肤色椭圆距离 r²: %.2f → %.2f（<1 即在椭圆内）"
              % (r2_before, skin_r2(src, m["box"])))
    x, y, bw, bh = m["box"]
    cx, cy = x + bw / 2.0, y + bh / 2.0

    W, H = CANVAS
    # 1) 清晰层：把脸放大到占画布高度 frac
    k = (H * frac) / bh
    fg = src.resize((int(round(src.width * k)), int(round(src.height * k))), Image.LANCZOS)
    fg = fg.filter(ImageFilter.UnsharpMask(radius=2, percent=60, threshold=3))
    fcx, fcy = cx * k, cy * k

    # 2) 背景：把清晰层的**最外一列**横向铺开再重度模糊。
    #    为什么不镜像整图：镜像会把半张脸复制到两侧，模糊后成为「幽灵脸」。
    #    为什么不用平涂同色：原图背景带横向明暗渐变，平涂会在接缝上下出现色差台阶。
    #    只复制边界列可保证纵向渐变连续、横向恒定，模糊后即为干净背景。
    # 2) 背景：同一张图 cover 铺满画布后重度模糊（散景），与前景同色温。
    #    不用边缘镜像：镜像会把半张脸复制到两侧，模糊后成为「幽灵脸」。
    #    不用平涂同色：原图背景带横向明暗渐变，平涂会在接缝处出现色差台阶。
    s_bg = max(W / float(src.width), H / float(src.height))
    bg = src.resize((int(round(src.width * s_bg)) + 2, int(round(src.height * s_bg)) + 2),
                    Image.LANCZOS)
    bg = bg.crop(((bg.width - W) // 2, (bg.height - H) // 2,
                  (bg.width - W) // 2 + W, (bg.height - H) // 2 + H))
    bg = bg.filter(ImageFilter.GaussianBlur(BG_BLUR))

    # 3) 贴图：脸中心落在 (FACE_CX, FACE_CY)，且不允许裁掉头顶。
    #    画布尺寸固定为 CANVAS；不能跟着清晰层实际尺寸走，否则 4K 会变成
    #    3840x2390，宿主按 16:9 letterbox 后构图与检测都偏。
    px = int(round(W * FACE_CX - fcx))
    py = int(round(H * FACE_CY - fcy))
    py = max(py, 40)                 # 头顶至少留 40px，避免整头被切
    canvas = bg.copy()
    mask = feathered_mask(fg.size, FEATHER)
    canvas.paste(fg, (px, py), mask)
    assert canvas.size == (W, H), "画布尺寸异常: %s" % (canvas.size,)
    if py != int(round(H * FACE_CY - fcy)):
        print("注意：为保证头顶完整，纵向位置由 %d 下移到 %d"
              % (int(round(H * FACE_CY - fcy)), py))

    # 4) 出图
    os.makedirs(ASSETS, exist_ok=True)
    p4k = os.path.join(ASSETS, OUT_4K)
    canvas.save(p4k)
    p720 = os.path.join(ASSETS, OUT_720)
    canvas.resize((1280, 720), Image.LANCZOS).save(p720)

    # 5) 复检：4K 与 720p 都必须 faces=1
    res = {}
    for name, path in ((OUT_4K, p4k), (OUT_720, p720)):
        a = cv2.cvtColor(np.array(Image.open(path).convert("RGB")), cv2.COLOR_RGB2BGR)
        nn, mm = detect(a)
        res[name] = {"faces": nn, "metrics": mm,
                     "size": list(Image.open(path).size),
                     "bytes": os.path.getsize(path)}
        print("%-20s faces=%d %s" % (name, nn, "" if not mm else
              "yaw=%.3f roll=%.1f pitch=%.2f face=%.1f%% score=%.2f"
              % (mm["yaw"], mm["roll"], mm["pitch"], mm["face_frac"] * 100, mm["score"])))

    meta = {
        "generated_by": "05_验证/make_front_assets.py",
        "source_path": src_path,
        "source_md5": md5_of(src_path),
        "source_size": [src.width, src.height],
        "face_frac_target": frac,
        "outputs": res,
        "note": "合成正脸素材。来源为 AI 生成人像（非真实人物照片），仅用于内部验收；"
                "assets/ 已 gitignore，不入库。",
    }
    with open(os.path.join(ASSETS, OUT_META), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    bad = [k for k, v in res.items() if v["faces"] != 1]
    if bad:
        print("⚠ 以下产物不是单脸，长跑会绕过帧环：%s" % ", ".join(bad))
        return 2
    print("OK: 两张都是单脸，可直接用于长跑与对拍")
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--frac", type=float, default=0.50, help="脸高占画布高的比例")
    ap.add_argument("--crop-bottom", type=int, default=0,
                    help="先裁掉源图底部若干像素（去水印用）")
    ap.add_argument("--no-skin-fix", action="store_true",
                    help="跳过肤色色度校正（仅用于对照实验）")
    a = ap.parse_args(argv[1:])
    return build(a.src, a.frac, a.crop_bottom, not a.no_skin_fix)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
