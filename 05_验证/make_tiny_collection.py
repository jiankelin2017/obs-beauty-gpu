#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 beauty_front720 模板造「超小脸」集合（§9.37.6 未验项 1）。

背景：§9.37.6 第 1 条写「超小脸（占画面 <1%）椭圆半径可能 <1 px，CPU 有
rx<0.5 短路、shader 没有」——这条**推断本身是错的**，它把归一化半径当成了
像素半径。系数实测最小 rx=0.050（鼻梁）、ry=0.055（亮眼），换算像素要
`rx*faceW`，所以触发 0.5 px 短路需要**脸宽 <10 px 或脸高 <9 px**，而 YuNet
在 1080p 上检不出这个量级的脸。本脚本用来做实测，而不是继续用推断。

素材：把 assets/face_front_4k.png 等比缩到 width=W，再 pad 到 1920×1080 黑底
居中。缩放后脸宽 ≈ 原脸宽 * W/3840，占画布面积按 W² 衰减。

用法：
    py -3 05_验证/make_tiny_collection.py                # 生成 480/320/240/160 四档
    py -3 05_验证/make_tiny_collection.py --width 320    # 只生成一档
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
SCENES = os.path.join(os.environ["APPDATA"], "obs-studio", "basic", "scenes")
TEMPLATE = os.path.join(SCENES, "beauty_front720.json")
FFMPEG = os.path.join(HERE, "tools", "ffmpeg.exe")
SRC = os.path.join(ASSETS, "face_front_4k.png")

CANVAS = (1920, 1080)


def make_png(w):
    out = os.path.join(ASSETS, "tiny_%dw_1080pad.png" % w)
    if os.path.exists(out):
        print("  素材已存在：%s" % os.path.basename(out))
        return out
    if not os.path.exists(FFMPEG):
        raise SystemExit("找不到 ffmpeg：%s" % FFMPEG)
    vf = ("scale=%d:-1:flags=lanczos,"
          "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black" % (w, CANVAS[0], CANVAS[1]))
    subprocess.run([FFMPEG, "-hide_banner", "-v", "error", "-y", "-i", SRC,
                    "-vf", vf, out], check=True)
    print("  生成素材：%s" % os.path.basename(out))
    return out


def make_collection(w, png):
    name = "beauty_tinyface%04d" % w
    d = json.loads(open(TEMPLATE, encoding="utf-8").read())

    d["name"] = name
    d["resolution"] = {"x": CANVAS[0], "y": CANVAS[1]}

    src = [s for s in d["sources"] if s["id"] == "image_source"][0]
    src["settings"]["file"] = png.replace("\\", "/")

    scene = [s for s in d["sources"] if s["id"] == "scene"][0]
    st = scene["settings"]
    # 模板的 custom_size 是 1280x720（720p 集合），画布改 1080p 后
    # scale_ref 与 cx/cy 都要跟着改，否则 OBS 会按旧画布缩放 —— §9.40.4 踩过。
    st["custom_size"] = True
    st["cx"] = float(CANVAS[0])
    st["cy"] = float(CANVAS[1])
    for it in st["items"]:
        it["scale_ref"] = {"x": float(CANVAS[0]), "y": float(CANVAS[1])}
        it["scale"] = {"x": 1.0, "y": 1.0}
        it["scale_rel"] = {"x": 1.0, "y": 1.0}
        it["pos"] = {"x": 0.0, "y": 0.0}
        it["pos_rel"] = {"x": -1.0, "y": -1.0}
        it["align"] = 5          # 左上
        it["bounds_type"] = 0

    path = os.path.join(SCENES, name + ".json")
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(d, fp, ensure_ascii=False, indent=4)
    print("  已写集合：%s" % path)
    return name


def main():
    ap = argparse.ArgumentParser(
        description="从 beauty_front720 模板造超小脸集合（1920x1080）")
    ap.add_argument("--width", type=int, action="append",
                    help="素材缩放宽度（可重复）；默认 480/320/240/160")
    args = ap.parse_args()

    widths = args.width or [480, 320, 240, 160]
    if not os.path.exists(TEMPLATE):
        raise SystemExit("找不到模板集合：%s" % TEMPLATE)
    if not os.path.exists(SRC):
        raise SystemExit("找不到源图：%s" % SRC)

    for w in widths:
        print("[width=%d]" % w)
        png = make_png(w)
        make_collection(w, png)
    print("\n完成。跑测：py -3 05_验证/verify_region_samemask.py run "
          "--collection beauty_tinyface0320 --only threed")


if __name__ == "__main__":
    sys.exit(main())
