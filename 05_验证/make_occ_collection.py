#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""造「遮挡格」集合：`beauty_real_cam_occ1080`（1920×1080）。

为什么要单独一个集合
--------------------
`verify_region_equiv.py` 的视频格靠集合里的 ffmpeg 源循环播放；原来那一格用的是
竖屏摄像头素材（`cam_real_1080x1920.mp4`，1080×1920）。2026-11-04 录到的真实采集
素材是**横屏 1920×1080**，直接塞进竖屏集合会被缩放/裁切，几何就不是原始的了。

所以从 `beauty_rot_live`（同为 1920×1080、源尺寸 = 画布尺寸、已挂 Beauty 滤镜、
`pos_rel=(-1,-1)`）复制一份，只换源：
  image_source  →  ffmpeg_source（循环播放剪好的遮挡片段）

几何为什么不重算
----------------
模板的 `scale_ref` 是 1920×1080、`align=5`（左上）、`pos=(0,0)`、`scale=1.0`，
`pos_rel=(-1,-1)` 正是「项左上角贴在画布左上角」。新素材同为 1920×1080，
所以这一组值原样有效 —— 只要**同时把 scene 的 custom_size `cx/cy` 改成 1920/1080**
（模板里残留的是 1080/1920，那是它被 `V.start_obs(canvas=...)` 改过画布后留下的
不一致值，照抄会把画面按竖屏裁切）。

用法
----
    py -3 05_验证/make_occ_collection.py
    py -3 05_验证/make_occ_collection.py --video assets/capture/xxx.mp4 --name beauty_real_cam_xxx
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "basic", "scenes")

TEMPLATE = "beauty_rot_live"
TARGET = "beauty_real_cam_occ1080"
VIDEO = os.path.join(HERE, "assets", "capture", "occ_1080p_86-120s.mp4")
W, H = 1920, 1080


def main():
    ap = argparse.ArgumentParser(description="从 beauty_rot_live 模板造遮挡格集合")
    ap.add_argument("--template", default=TEMPLATE)
    ap.add_argument("--name", default=TARGET)
    ap.add_argument("--video", default=VIDEO)
    ap.add_argument("--size", default="%dx%d" % (W, H))
    args = ap.parse_args()

    w, h = [int(x) for x in args.size.lower().split("x")]
    src = os.path.join(SCENES, args.template + ".json")
    dst = os.path.join(SCENES, args.name + ".json")
    if not os.path.isfile(src):
        sys.exit("找不到模板集合：%s" % src)
    if not os.path.isfile(args.video):
        sys.exit("找不到视频：%s" % args.video)
    if os.path.exists(dst):
        print("注意：目标已存在，将被覆盖 —— %s" % dst)

    with open(src, "r", encoding="utf-8") as f:
        d = json.load(f)

    d["name"] = args.name
    d["resolution"] = {"x": w, "y": h}

    # 源：image_source → ffmpeg_source（循环播放，与 real_cam 那一格同参数）
    for s in d.get("sources", []):
        if s.get("id") == "image_source":
            s["id"] = "ffmpeg_source"
            s["versioned_id"] = "ffmpeg_source"
            s["settings"] = {
                "local_file": args.video.replace("\\", "/"),
                "close_when_inactive": False,
                "looping": True,
                "clear_on_media_end": False,
                "restart_on_activate": True,
                "speed_percent": 100,
            }
            print("源 → ffmpeg_source：%s" % s["settings"]["local_file"])
        elif s.get("id") == "scene":
            # 画布与项几何：custom_size + cx/cy 必须与本集合一致
            st = s.setdefault("settings", {})
            st["custom_size"] = True
            st["cx"] = w
            st["cy"] = h
            for it in st.get("items", []):
                it["scale_ref"] = {"x": float(w), "y": float(h)}
                it["pos"] = {"x": 0.0, "y": 0.0}
                it["pos_rel"] = {"x": -1.0, "y": -1.0}
                it["scale"] = {"x": 1.0, "y": 1.0}
                it["scale_rel"] = {"x": 1.0, "y": 1.0}
                it["align"] = 5
                it["rot"] = 0.0
            print("场景几何 → %dx%d，pos_rel=(-1,-1)" % (w, h))

    # 滤镜必须还在，否则 patch_collection() 会直接退出
    nf = 0
    for s in d.get("sources", []):
        for flt in s.get("filters", []):
            if "delspot_level" in flt.get("settings", {}):
                nf += 1
    if not nf:
        sys.exit("模板里没有 Beauty 滤镜设置 —— patch_collection() 会失败")

    with open(dst, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=4)
    print("已写出：%s（%d 处滤镜）" % (dst, nf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
