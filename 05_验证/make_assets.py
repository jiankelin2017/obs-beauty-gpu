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
    face_720p.png       1280x720   720p 对拍素材
    face_4k.png         3840x2160  4K 目视素材（含背景群众，faces=4）
    face_4k_single.png  3840x2160  4K **单脸**素材（S2 帧环长跑用）

取景约定：裁 (0,340)-(2624,1816) 这块 16:9 区域 —— 主人物整头完整、正脸、占画面
高度约 6 成，接近摄像头近景。背景群众脸不影响生产路径（生产按「人物个数」取前
N 张脸）。

⚠ 但 face_4k.png **不能用于帧环测试**：实测这份素材 `faces=4`，而帧环的启用条件是
`mg.count == 1 && !(mg.shape & 4)`（obs-beauty.cpp:3705），多脸时帧环被整段绕过 ——
4K 长跑会变成「跑了 30 分钟但一次都没走帧环」。因此另出一份单脸素材：
主体（主人物头肩）居中、背景用同一区域的**重度虚化**填充（模拟真实近景的散景），
虚化后背景人脸不再被检出。判据是 `faces=1` 且 seg 行 `prep≈0`（帧环命中时 4K 的
prep 会从 ~4.65 掉到 0）。
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

DEFAULT_SRC = r"C:/Users/Administrator/Pictures/A7300538.jpg"
# 原图 2624x3936 上的取景框（16:9）
BOX = (0, 340, 2624, 1816)

# 单脸素材：主脸椭圆区保持清晰、其余全部重度虚化。
# 为什么不用「裁窄 + 居中拼接」：主人物左右紧邻群众（红T恤在 x 12%~33%、白帽在
# x 33%~42%、右侧人群在 x>78%），任何窄裁都还会带进别人的脸；实测裁到 x40%~85%
# 拼到 4K 后右下角仍有一排人脸，`faces` 依旧 >1，等于长跑没跑帧环。
# 改为「几何不动、只糊背景」：主脸位置确定（x 45%~75%、y 20%~80%），把它圈出来
# 保持清晰，外面整片虚化 —— 背景脸一定检不出，且画面构图与真实近景一致。
FACE_CX, FACE_CY = 0.600, 0.500     # 主脸中心（占取景框比例）
FACE_RX, FACE_RY = 0.200, 0.380     # 椭圆半轴（同上）—— 含头发与下巴
BLUR_RADIUS = 45                    # 背景虚化半径（4K 像素）
FEATHER = 0.06                      # 椭圆边缘羽化（占半轴比例）


def make_single_face_4k(src, box, canvas=(3840, 2160)):
    """主脸保持清晰、其余重度虚化的 4K 单脸素材。

    判据（必须实测确认，不能靠肉眼）：插件日志 `faces=1`，且 seg 行 `prep≈0`
    （帧环命中时 4K 的 prep 从 ~4.65 掉到 0）。
    """
    from PIL import ImageDraw, ImageFilter

    W, H = canvas
    base = src.crop(box).resize((W, H), Image.LANCZOS)
    soft = base.filter(ImageFilter.GaussianBlur(BLUR_RADIUS))

    # 椭圆蒙版做羽化：内圈全清晰，外圈全虚化，中间线性过渡，避免硬边被当成
    # 额外的「边缘特征」干扰检测。
    m = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m)
    cx, cy = FACE_CX * W, FACE_CY * H
    rx, ry = FACE_RX * W, FACE_RY * H
    steps = 14
    for i in range(steps, 0, -1):        # 由外向内叠加，越内越亮
        k = 1.0 + FEATHER * (i / float(steps))
        v = int(255 * (1.0 - (i - 1) / float(steps)))
        d.ellipse([cx - rx * k, cy - ry * k, cx + rx * k, cy + ry * k], fill=v)
    return Image.composite(base, soft, m)


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

    c = make_single_face_4k(src, BOX)
    cp = os.path.join(ASSETS, "face_4k_single.png")
    c.save(cp)
    print("face_4k_single.png %s  %d B" % (c.size, os.path.getsize(cp)))

    # 720p 单脸版：直接由 4K 单脸缩放（构图一致，只是分辨率不同），
    # 用于 S2 测 720p 的帧环收益。
    d = c.resize((1280, 720), Image.LANCZOS)
    dp = os.path.join(ASSETS, "face_720p_single.png")
    d.save(dp)
    print("face_720p_single.png %s  %d B" % (d.size, os.path.getsize(dp)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
