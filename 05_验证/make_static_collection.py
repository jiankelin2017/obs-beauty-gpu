#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
S1 正确性验收用「静态图集合」生成器（2026-11-02）。

为什么需要静态图：
  第 1 项要判 maxdiff=0（2.3.0 vs 2.2.1、直通 vs 滤镜禁用）。笔记本内置摄像头
  是**实时画面**，两次运行输入本来就不同，像素差再小也归不到代码头上。
  把一张固定 PNG 喂给 image_source，输入就完全确定，diff 才有意义。

生成两份（与 camera 集合完全隔离，只新建不修改）：
  beauty_static.json    : 画布 1280x720，源 assets/face_720p.png
  beauty_static_4k.json : 画布 3840x2160，源 assets/face_4k.png（第 3 项目视）

两条约定：
  * 源名 "still"、滤镜名 "Beauty" 全用 ASCII —— websocket 请求里直接用，
    不碰中文编码。
  * 初始 14 项参数全 0 且 enabled=true（S1 直通的前提条件）；具体组合由
    verify_s1_correct.py 在运行时通过 websocket 改，不写死在这里。

用法（OBS 必须关闭，否则退出时会把文件覆盖回去）：
    python 05_验证/make_static_collection.py
"""
import glob
import json
import os
import shutil
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

PANEL_KEYS = [
    "blur_level", "clarity", "color_level", "red_level", "sharpen",
    "global_sharpen", "face_threed", "eye_bright", "remove_pouch_strength",
    "remove_nasolabial_folds_strength", "delspot_level", "thin_face",
    "big_eye", "chin",
]

SRC_NAME = "still"
FILTER_NAME = "Beauty"
SCENE_NAME = "Main"


def scenes_dir():
    appdata = os.environ.get("APPDATA")
    if not appdata:
        sys.exit("找不到 %APPDATA%")
    d = os.path.join(appdata, "obs-studio", "basic", "scenes")
    if not os.path.isdir(d):
        sys.exit("场景集合目录不存在: %s" % d)
    return d


def fix_guid(tag):
    """由字符串生成稳定的 GUID（多次运行 uuid 不变，OBS 不会认为换了源）。"""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "obs-beauty/" + tag))


def image_source_template():
    """从用户现有集合里借一个 image_source 的 settings 结构，保证键名齐全。"""
    for f in glob.glob(os.path.join(scenes_dir(), "*.json")):
        try:
            with open(f, encoding="utf-8") as fh:
                doc = json.load(fh)
        except Exception:
            continue
        for s in doc.get("sources", []):
            if s.get("id") == "image_source":
                st = dict(s.get("settings") or {})
                print("image_source 模板取自: %s" % os.path.basename(f))
                return st
    return {"file": "", "unload": False, "linear_alpha": False}


def scene_template():
    """借一个 scene 源的字段骨架。"""
    for f in glob.glob(os.path.join(scenes_dir(), "*.json")):
        try:
            with open(f, encoding="utf-8") as fh:
                doc = json.load(fh)
        except Exception:
            continue
        for s in doc.get("sources", []):
            if s.get("id") == "scene":
                return json.loads(json.dumps(s))  # deep copy
    return None


def make_item(src_uuid, name, canvas):
    cx, cy = canvas
    return {
        "name": name,
        "source_uuid": src_uuid,
        "visible": True,
        "locked": False,
        "rot": 0.0,
        "scale_ref": {"x": float(cx), "y": float(cy)},
        "align": 5,
        # bounds_type=0（不设边界）+ 素材尺寸==画布尺寸：1:1 贴满，不缩放、
        # 不插值。采集时 verify 会把 profile 画布同步成同一尺寸，所以 pos 就是
        # (0,0)，OBS 也不会像画布不一致时那样把它改写成 280。
        # 教训：bounds_type=2 时 OBS 会把 bounds 归一化成离谱的 360x360（图缩成
        # 1/4）；bounds_type=0 + 画布不一致时它会把 pos 改掉。两个坑都会让两版
        # 的「滤镜禁用」基准图对不上（实测差 44% 像素），对拍全废。
        "bounds_type": 0,
        "bounds_align": 0,
        "bounds_crop": False,
        "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0,
        "id": 1,
        "group_item_backup": False,
        "pos": {"x": 0.0, "y": 0.0},
        "pos_rel": {"x": -1.0, "y": -1.0},
        "scale": {"x": 1.0, "y": 1.0},
        "scale_rel": {"x": 1.0, "y": 1.0},
        "bounds": {"x": 0.0, "y": 0.0},
        "bounds_rel": {"x": 0.0, "y": 0.0},
        "scale_filter": "disable",
        "blend_method": "default",
        "blend_type": "normal",
        "show_transition": {"duration": 300},
        "hide_transition": {"duration": 300},
        "private_settings": {},
    }


def build(name, png, canvas):
    png_abs = os.path.join(ASSETS, png)
    if not os.path.isfile(png_abs):
        sys.exit("缺素材: %s（先跑 assets 生成步骤）" % png_abs)
    st = image_source_template()
    st["file"] = png_abs.replace("\\", "/")
    st["unload"] = False

    fset = {k: 0.0 for k in PANEL_KEYS}
    fset["enabled"] = True

    src_uuid = fix_guid(name + "/" + SRC_NAME)
    filt = {
        "prev_ver": 536936449,
        "name": FILTER_NAME,
        "uuid": fix_guid(name + "/filter"),
        "id": "obs_beauty_filter",
        "versioned_id": "obs_beauty_filter",
        "settings": fset,
        "mixers": 0, "sync": 0, "flags": 0,
        "volume": 1.0, "balance": 0.5,
        "enabled": True,
        "muted": False,
        "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0,
        "hotkeys": {},
        "deinterlace_mode": 0, "deinterlace_field_order": 0,
        "monitoring_type": 0,
        "private_settings": {},
    }

    img = {
        "prev_ver": 536936449,
        "name": SRC_NAME,
        "uuid": src_uuid,
        "id": "image_source",
        "versioned_id": "image_source",
        "settings": st,
        "filters": [filt],
        "mixers": 0, "sync": 0, "flags": 0,
        "volume": 1.0, "balance": 0.5,
        "enabled": True,
        "muted": False,
        "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0,
        "hotkeys": {},
        "deinterlace_mode": 0, "deinterlace_field_order": 0,
        "monitoring_type": 0,
        "private_settings": {},
    }

    scene = scene_template()
    if scene is None:
        sys.exit("找不到可借用的 scene 骨架")
    # 骨架是从已有集合里借的，而 beauty_4k 那类集合把美颜滤镜挪到了场景上，
    # 照抄就会**多带一个滤镜**：场景一个 + 源一个 → 串联处理两次，每次采集的
    # 基准整体漂移（同版本两次对拍 diff 到 16~26，本该 0）。必须清空。
    scene["filters"] = []
    scene["name"] = SCENE_NAME
    scene["uuid"] = fix_guid(name + "/scene")
    scene["settings"]["custom_size"] = True
    scene["settings"]["cx"] = canvas[0]
    scene["settings"]["cy"] = canvas[1]
    scene["settings"]["items"] = [make_item(src_uuid, SRC_NAME, canvas)]
    scene["settings"]["id_counter"] = 2
    scene["hotkeys"] = {}

    doc = {
        "name": name,
        "sources": [img, scene],
        "groups": [],
        "scene_order": [{"name": SCENE_NAME}],
        "current_scene": SCENE_NAME,
        "current_program_scene": SCENE_NAME,
        "canvases": [],
        "current_transition": "淡入淡出",
        "transition_duration": 300,
        "transitions": [],
        "quick_transitions": [],
        "saved_projectors": [],
        "preview_locked": False,
        "scaling_enabled": False,
        "scaling_level": -6,
        "scaling_off_x": 0.0, "scaling_off_y": 0.0,
        "virtual-camera": {"type2": 3},
        "modules": {"scripts-tool": []},
        "resolution": {"x": canvas[0], "y": canvas[1]},
        "version": 2,
    }

    dst = os.path.join(scenes_dir(), name + ".json")
    if os.path.isfile(dst):
        shutil.copy2(dst, dst + ".bak")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=4)
    print("写入: %s  (%dx%d, src=%s)" % (dst, canvas[0], canvas[1], os.path.basename(png)))


def main():
    print("场景集合目录: %s" % scenes_dir())
    build("beauty_static", "face_720p.png", (1280, 720))
    build("beauty_static_4k", "face_4k.png", (3840, 2160))
    # S2 帧环长跑专用：单脸 4K（帧环只在 mg.count==1 时启用，多脸素材等于空跑）
    build("beauty_soak4k", "face_4k_single.png", (3840, 2160))
    # 720p 单脸：测 720p 的帧环收益（beauty_static 是多脸，帧环不启用）
    build("beauty_soak720", "face_720p_single.png", (1280, 720))
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
