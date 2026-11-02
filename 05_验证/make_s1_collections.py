#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
S1 验收用场景集合生成器（2026-11-02）。

从用户现有 "视频" 集合派生两份**独立**集合，只改副本，绝不碰用户集合：

  beauty_s1.json : 720p（相机原生档），美颜滤镜**全 0 参数 + enabled=true**
                   → 用于验证 S1「全 0 直通」：走 passthrough，往返应≈0。
  beauty_4k.json : scene 模式、画布 3840x2160，美颜滤镜**全 0 参数 + enabled=true**
                   → 验证高分辨率下 passthrough 同样≈0。

为什么必须显式全 0：
  BeautySettings 默认构造把 panel[0]（磨皮）设为 20，allZero() 要求 14 项全 0，
  缺键的项会回落到默认 20 → 不会触发直通。所以 14 个键一个都不能少。

滤镜 enabled 必须 true：
  obs-beauty.cpp 渲染入口 `if (!f->enabled ...) return;`，enabled=false 时不渲染、
  也就不会有「性能」行。

用法:
    python 05_验证/make_s1_collections.py
（OBS 必须先关闭，否则写入的集合不会在本次会话出现，且退出时可能被覆盖。）
"""
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import phase0_prep as P

SCENES = P.scenes_dir()
BASE_NAME = "视频"          # 用户现有集合（只读，不修改）
BASE = os.path.join(SCENES, BASE_NAME + ".json")

# g_params[] 的 14 个键（顺序无关，键名必须齐全）
PANEL_KEYS = [
    "blur_level", "clarity", "color_level", "red_level", "sharpen",
    "global_sharpen", "face_threed", "eye_bright", "remove_pouch_strength",
    "remove_nasolabial_folds_strength", "delspot_level", "thin_face",
    "big_eye", "chin",
]


def load_base():
    if not os.path.isfile(BASE):
        sys.exit("找不到基准集合: %s" % BASE)
    with open(BASE, encoding="utf-8") as fh:
        return json.load(fh)


def find_src(doc, sid):
    for s in doc.get("sources", []):
        if s.get("id") == sid:
            return s
    return None


def find_beauty(src):
    for f in src.get("filters", []):
        if f.get("id") == "obs_beauty_filter":
            return f
    return None


def zero_filter(bf):
    """14 项面板参数全清 0；enabled 置 true（否则不渲染）。"""
    st = bf.setdefault("settings", {})
    for k in PANEL_KEYS:
        st[k] = 0.0
    st["enabled"] = True


def dump(doc, name):
    dst = os.path.join(SCENES, name + ".json")
    if os.path.isfile(dst):
        shutil.copy2(dst, dst + ".bak")
    doc["name"] = name
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=4)
    print("写入: %s" % dst)


def make_camera():
    """beauty_s1：相机档（沿用设备默认分辨率，本机 1280x720），滤镜全 0。"""
    doc = load_base()
    cam = find_src(doc, "dshow_input")
    bf = find_beauty(cam)
    if bf is None:
        sys.exit("基准集合的相机源上没有 obs_beauty_filter")
    zero_filter(bf)
    dump(doc, "beauty_s1")


def make_4k():
    """beauty_4k：scene 模式，画布 3840x2160，滤镜挪到场景源，全 0。"""
    doc = load_base()
    cam = find_src(doc, "dshow_input")
    scene = find_src(doc, "scene")
    bf = find_beauty(cam)
    if bf is None:
        sys.exit("基准集合的相机源上没有 obs_beauty_filter")
    cw, ch = 3840, 2160
    # 相机钉在原生分辨率，保证同构图（脸的大小不随画布变）
    src = P.camera_native_res(cam)
    P.set_camera_resolution(cam, "%dx%d" % src)
    scene["settings"]["custom_size"] = True
    scene["settings"]["cx"] = cw
    scene["settings"]["cy"] = ch
    scene["settings"]["items"] = [P.build_item(cam.get("uuid"), (cw, ch), src)]
    # 滤镜从相机挪到场景源：滤镜分辨率 = 画布
    cam["filters"] = [f for f in cam["filters"] if f.get("id") != "obs_beauty_filter"]
    zero_filter(bf)
    scene.setdefault("filters", []).append(bf)
    doc["current_scene"] = scene.get("name")
    doc["current_program_scene"] = scene.get("name")
    dump(doc, "beauty_4k")


def main():
    print("基准集合: %s" % BASE)
    make_camera()
    make_4k()
    print("完成。OBS 关闭状态下生成，重启 OBS 后可在「场景集合」里看到 beauty_s1 / beauty_4k。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
