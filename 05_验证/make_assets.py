#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成正确性验收用的静态人脸素材（2026-11-02）。

为什么要单独一支：素材是**从一张本机照片裁出来的**，不适合提交进仓库（隐私 +
体积 7 MB）。这里把裁剪参数固化下来，换机器时用同一张/同类照片即可复现：

    python 05_验证/make_assets.py <照片路径>

不带参数时用默认那张（本机存在才可用）：
    C:/Users/Administrator/Pictures/A7300538.jpg

产出（05_验证/assets/，已 gitignore）：
    face_720p.png  1280x720   720p 对拍素材
    face_4k.png    3840x2160  4K 目视素材

取景约定：裁 (0,340)-(2624,1816) 这块 16:9 区域 —— 主人物整头完整、正脸、占画面
高度约 6 成，接近摄像头近景。背景群众脸不影响生产路径（生产按「人物个数」取前
N 张脸；多脸只在 GPU 掩码对拍那条诊断路径上会跳过）。
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

DEFAULT_SRC = r"C:/Users/Administrator/Pictures/A7300538.jpg"
# 原图 2624x3936 上的取景框（16:9）
BOX = (0, 340, 2624, 1816)


def main(argv):
    src_path = argv[1] if len(argv) > 1 else DEFAULT_SRC
    if not os.path.isfile(src_path):
        sys.exit("找不到照片: %s\n用法: python make_assets.py <照片路径>" % src_path)
    os.makedirs(ASSETS, exist_ok=True)

    src = Image.open(src_path)
    crop = src.crop(BOX)

    a = crop.resize((1280, 720), Image.LANCZOS)
    a.save(os.path.join(ASSETS, "face_720p.png"))
    print("face_720p.png  %s  %d B" % (a.size, os.path.getsize(os.path.join(ASSETS, "face_720p.png"))))

    b = crop.resize((3840, 2160), Image.LANCZOS)
    b.save(os.path.join(ASSETS, "face_4k.png"))
    print("face_4k.png    %s  %d B" % (b.size, os.path.getsize(os.path.join(ASSETS, "face_4k.png"))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
