#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用插件同款 YuNet 模型筛选「正脸、单脸、近景」的候选照片。

为什么要有这支：验收素材必须能代表人脸朝向下的真实负载，而侧脸会让掩码覆盖率
与五官椭圆几何两项指标不可外推。肉眼判断朝向不可复现，这里直接用与插件运行时
相同的检测器打分。

用法：
    py -3 05_验证/scan_frontal_faces.py                 # 扫默认目录
    py -3 05_验证/scan_frontal_faces.py <目录> [目录...]
    py -3 05_验证/scan_frontal_faces.py --top 10 <目录>

判据（三档，全部来自模型输出的 5 个关键点，不含任何人工调参）：
    yaw   = |鼻尖x - 双眼中心x| / 瞳距      越小越正
    roll  = 双眼连线与水平线的夹角(度)        越小越平
    pitch = |嘴中心y - 双眼中心y| / 瞳距      落在 0.55~1.05 视为平视
    face  = 主脸框高 / 图高                  越大越接近近景

只读扫描：不复制、不改名、不写回源目录。结果打印到 stdout，可按需重定向。
"""
import argparse
import math
import os
import sys

import cv2

DEFAULT_DIRS = [
    r"C:/Users/Administrator/Pictures",
    r"C:/Users/Administrator/Desktop",
    r"C:/Users/Administrator/Documents",
    r"C:/Users/Administrator/Downloads",
]
MODEL_CANDIDATES = [
    r"D:/插件/OBS美颜GPU版/06_离线验证/models/yunet.onnx",
    r"C:/ProgramData/obs-studio/plugins/obs-beauty/bin/64bit/models/yunet.onnx",
]
EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".webp")
DET_SIDE = 640          # 与插件 640x640 的输入一致
SCORE_MIN = 0.75


def find_model():
    """返回可被 cv2.dnn 加载的模型路径。

    OpenCV 的 ONNX 读取器在 Windows 上按 ANSI 打开文件，路径含中文会直接报
    `Can't read ONNX file`（本项目路径 `D:\\插件\\…` 正好命中）。这里把模型复制到
    ASCII 临时目录再加载；文件本体不动。
    """
    import shutil
    import tempfile

    for p in MODEL_CANDIDATES:
        if not os.path.isfile(p):
            continue
        try:
            p.encode("ascii")
            return p
        except UnicodeEncodeError:
            dst = os.path.join(tempfile.gettempdir(), "beauty_scan", "yunet.onnx")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not os.path.isfile(dst) or os.path.getsize(dst) != os.path.getsize(p):
                shutil.copyfile(p, dst)
            return dst
    sys.exit("找不到 yunet.onnx，请检查模型路径")


def imread_any(path):
    """读图并按 EXIF 摆正（手机竖拍的照片像素是横的，EXIF 里带旋转标记）。"""
    try:
        from PIL import Image, ImageOps
        import numpy as np
        with Image.open(path) as im:
            im = ImageOps.exif_transpose(im).convert("RGB")
            return cv2.cvtColor(np.array(im), cv2.COLOR_RGB2BGR)
    except Exception:
        return cv2.imread(path)


def frontal_metrics(face, img_w, img_h):
    """face = [x,y,w,h, rex,rey, lex,ley, nx,ny, mrx,mry, mlx,mly, score]"""
    x, y, w, h = face[0:4]
    rex, rey = face[4], face[5]          # 图像左眼（被检者右眼）
    lex, ley = face[6], face[7]
    nx, ny = face[8], face[9]
    mrx, mry = face[10], face[11]
    mlx, mly = face[12], face[13]
    score = float(face[14])

    iod = math.hypot(lex - rex, ley - rey)
    if iod < 1e-3:
        return None
    ex, ey = (rex + lex) / 2.0, (rey + ley) / 2.0
    mx, my = (mrx + mlx) / 2.0, (mry + mly) / 2.0

    yaw = float(abs(nx - ex) / iod)
    roll = float(abs(math.degrees(math.atan2(ley - rey, lex - rex))))
    pitch = float(abs(my - ey) / iod)
    # 左右对称度：鼻尖到两眼的距离差，侧脸时会明显偏
    asym = float(abs(math.hypot(nx - rex, ny - rey) - math.hypot(nx - lex, ny - ley)) / iod)
    return {
        "yaw": yaw, "roll": roll, "pitch": pitch, "asym": asym,
        "iod": iod, "score": score,
        "face_frac": float(h) / float(img_h),
        "box": (int(x), int(y), int(w), int(h)),
    }


def scan(dirs, top, min_frac):
    model = find_model()
    det = cv2.FaceDetectorYN.create(model, "", (DET_SIDE, DET_SIDE),
                                    SCORE_MIN, 0.3, 5000)
    rows = []
    seen = 0
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for fn in files:
                if not fn.lower().endswith(EXTS):
                    continue
                p = os.path.join(root, fn)
                img = imread_any(p)
                if img is None or img.size == 0:
                    continue
                seen += 1
                h, w = img.shape[:2]
                scale = DET_SIDE / float(max(h, w))
                small = cv2.resize(img, (max(1, int(w * scale)), max(1, int(h * scale))),
                                   interpolation=cv2.INTER_AREA)
                sh, sw = small.shape[:2]
                det.setInputSize((sw, sh))
                _ret, faces = det.detect(small)
                if faces is None or len(faces) == 0:
                    continue
                # 只保留主脸（框最大）；多脸素材无法用于帧环测试，另行标注
                faces = sorted(faces, key=lambda f: f[2] * f[3], reverse=True)
                main = faces[0].copy()
                main[0:4] /= scale           # 回到原图尺度，便于按原图裁切
                m = frontal_metrics(main, w, h)
                if m is None:
                    continue
                rows.append({
                    "path": p, "n_faces": len(faces), "img": (w, h),
                    "yaw": m["yaw"], "roll": m["roll"], "pitch": m["pitch"],
                    "asym": m["asym"], "score": m["score"],
                    "face_frac": m["face_frac"], "box": m["box"],
                })

    # 排序：先卡硬条件，再按 yaw 为主、roll 与 frac 为辅
    ok = [r for r in rows if r["yaw"] <= 0.12 and r["roll"] <= 6.0
          and 0.50 <= r["pitch"] <= 1.10 and r["face_frac"] >= min_frac]
    ok.sort(key=lambda r: (r["yaw"] + 0.01 * r["roll"], -r["face_frac"]))
    print("# 扫描 %d 张，检出人脸 %d 张，通过正脸/近景门槛 %d 张"
          % (seen, len(rows), len(ok)))
    print("# %-4s %-6s %-6s %-6s %-5s %-6s %-6s %s"
          % ("yaw", "roll", "pitch", "asym", "faces", "face%", "score", "path"))
    for r in ok[:top]:
        print("  %-6.3f %-6.2f %-6.3f %-6.3f %-5d %-6.1f %-6.2f %s"
              % (r["yaw"], r["roll"], r["pitch"], r["asym"], r["n_faces"],
                 r["face_frac"] * 100, r["score"], r["path"]))
    if len(ok) > top:
        print("# …… 其余 %d 张略" % (len(ok) - top))
    return ok


def main(argv):
    ap = argparse.ArgumentParser(description="用 YuNet 筛选正脸近景照片")
    ap.add_argument("dirs", nargs="*", default=None)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--min-frac", type=float, default=0.25,
                    help="主脸框高占图高的最小比例，默认 0.25")
    a = ap.parse_args(argv[1:])
    scan(a.dirs or DEFAULT_DIRS, a.top, a.min_frac)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
