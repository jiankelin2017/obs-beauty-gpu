#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成两族「短事件」场景集合（媒体源循环播放 assets 下的 avi）。

flash 族（空场里闪出人脸，2.11.1 实测压不到方案C，留作历史对照）：
    beauty_flash1_720 : 人脸每 60 帧出现 1 帧（33 ms）
    beauty_flash2_720 : 2 帧（67 ms）
    beauty_flash4_720 : 4 帧（133 ms）
    beauty_flash8_720 : 8 帧（267 ms，正好等于静止时的检测周期）

drop 族（脸持续在场，每 60 帧消失 k 帧 —— 方案C 真正生效的形态下量边界）：
    beauty_drop1_720 : 消失 1 帧（33 ms）
    beauty_drop2_720 : 2 帧（67 ms）
    beauty_drop4_720 : 4 帧（133 ms）

用法（OBS 必须关闭，否则退出时会把集合文件覆盖回去）：
    py -3 05_验证/make_flash_collection.py
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_static_collection as M   # noqa: E402

KS = (0, 1, 2, 4, 8)
DROP_KS = (1, 2, 4)
CANVAS = (1280, 720)


def build(name, avi):
    p = os.path.join(M.ASSETS, avi)
    if not os.path.isfile(p):
        sys.exit("缺素材: %s（先跑 make_flash_assets.py）" % p)

    # 与运动集合同一套设置：循环、切走不关播放器、回到场景从头播。
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
    scene["filters"] = []       # 骨架可能带场景级滤镜，照抄 = 串联处理两次
    scene["name"] = M.SCENE_NAME
    scene["uuid"] = M.fix_guid(name + "/scene")
    scene["settings"]["custom_size"] = True
    scene["settings"]["cx"] = CANVAS[0]
    scene["settings"]["cy"] = CANVAS[1]
    scene["settings"]["items"] = [M.make_item(src_uuid, M.SRC_NAME, CANVAS)]
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
        "resolution": {"x": CANVAS[0], "y": CANVAS[1]},
        "version": 2,
    }

    dst = os.path.join(M.scenes_dir(), name + ".json")
    if os.path.isfile(dst):
        shutil.copy2(dst, dst + ".bak")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=4)
    print("写入: %s  (%dx%d, src=%s)" % (dst, CANVAS[0], CANVAS[1], avi))


def main():
    print("场景集合目录: %s" % M.scenes_dir())
    for k in KS:
        build("beauty_flash%d_720" % k, "flash_k%d_720p.avi" % k)
    for k in DROP_KS:
        build("beauty_drop%d_720" % k, "drop_k%d_720p.avi" % k)
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
