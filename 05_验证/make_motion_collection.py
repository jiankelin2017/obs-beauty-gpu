#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「运动」场景集合：把 assets/motion_720p.mp4 循环喂进媒体源。

为什么必须要它：S7 的验收口径是「运动滞后 ≤ +1 帧」，而静态素材根本测不出这一条 ——
静止画面下自适应会把周期拉到 8 帧，但**正因为画面不动，任何滞后都看不出来**。
只有让人脸在画面里持续移动，`dper=` 才能证明「一有运动就回到每帧」。

产出（只新建，不改既有集合）：
    beauty_motion720 : 画布 1280x720，源 motion_720p.mp4（循环）

用法（OBS 必须关闭，否则退出时会把集合文件覆盖回去）：
    py -3 05_验证/make_motion_collection.py
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_static_collection as M   # noqa: E402


def build(name, mp4, canvas):
    p = os.path.join(M.ASSETS, mp4)
    if not os.path.isfile(p):
        sys.exit("缺素材: %s" % p)

    # 媒体源的 settings 只认 local_file / looping 两个必需项；其余交给 OBS 补默认。
    # close_when_inactive=False：否则切走场景会把播放器关掉，长跑中途复用时
    # 首帧要重新 seek，前几秒的 dper 会被误读成「静止」。
    st = {
        "local_file": p.replace("\\", "/"),
        "looping": True,
        "close_when_inactive": False,
        "restart_on_activate": True,
        "clear_on_media_end": False,
        "speed_percent": 100,
    }

    fset = {k: 0.0 for k in M.PANEL_KEYS}
    fset["enabled"] = True

    src_uuid = M.fix_guid(name + "/" + M.SRC_NAME)
    filt = {
        "prev_ver": 536936449,
        "name": M.FILTER_NAME,
        "uuid": M.fix_guid(name + "/filter"),
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

    src = {
        "prev_ver": 536936449,
        "name": M.SRC_NAME,
        "uuid": src_uuid,
        "id": "ffmpeg_source",
        "versioned_id": "ffmpeg_source",
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

    scene = M.scene_template()
    if scene is None:
        sys.exit("找不到可借用的 scene 骨架")
    # 与静态集合同一条教训：骨架里可能带着场景级滤镜，照抄 = 串联处理两次
    scene["filters"] = []
    scene["name"] = M.SCENE_NAME
    scene["uuid"] = M.fix_guid(name + "/scene")
    scene["settings"]["custom_size"] = True
    scene["settings"]["cx"] = canvas[0]
    scene["settings"]["cy"] = canvas[1]
    scene["settings"]["items"] = [
        M.make_item(src_uuid, M.SRC_NAME, canvas)]
    scene["settings"]["id_counter"] = 2
    scene["hotkeys"] = {}

    doc = {
        "name": name,
        "sources": [src, scene],
        "groups": [],
        "scene_order": [{"name": M.SCENE_NAME}],
        "current_scene": M.SCENE_NAME,
        "current_program_scene": M.SCENE_NAME,
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

    dst = os.path.join(M.scenes_dir(), name + ".json")
    if os.path.isfile(dst):
        shutil.copy2(dst, dst + ".bak")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=4)
    print("写入: %s  (%dx%d, src=%s)" % (dst, canvas[0], canvas[1], mp4))


def main():
    print("场景集合目录: %s" % M.scenes_dir())
    build("beauty_motion720", "motion_720p.mp4", (1280, 720))
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
