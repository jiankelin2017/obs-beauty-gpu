#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「画面旋转」搬到美颜滤镜**之前**。

问题（2026-11-03 实测）：用户把采集源在场景里的旋转设成 -90°，预览正立，但
OBS 的滤镜挂在源上，场景项变换发生在滤镜之后 —— 美颜算法拿到的仍是原始
1920x1080、人脸横躺的画面。YaNet/YuNet 与五官椭圆都是按「眼睛在上」的假设做
几何，横躺时必然错位。

解法：插一层子场景。子场景内部的旋转发生在「子场景渲染」时，而父场景把这个
子场景当普通源来用，滤镜挂在子场景源上 —— 于是滤镜吃到的就是已旋转的画面。

   采集源 --(子场景内 rot=-90)--> 子场景 --(OBS美颜滤镜)--> 父场景

可回滚：`--revert` 关掉子场景项、恢复原项与滤镜，并删除子场景。

用法：
  py -3 05_验证/cam_rot.py --apply                     # 默认集合「视频」
  py -3 05_验证/cam_rot.py --apply --probe             # 应用后立刻截图验证方向
  py -3 05_验证/cam_rot.py --revert
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import verify_s1_correct as V      # noqa: E402

OUT = os.path.join(HERE, "out", "cam")
ROT_SCENE = "CamRot"       # 子场景名
FILT = "OBS美颜"


def find_item(c, scene, source):
    for it in c.scene_items(scene):
        if it.get("sourceName") == source:
            return it
    raise KeyError("场景 %r 里没有源 %r" % (scene, source))


def apply_rot(c, parent, src):
    scenes = [s.get("sceneName") for s in c.scenes()]
    if ROT_SCENE not in scenes:
        c.req("CreateScene", {"sceneName": ROT_SCENE})
        print("  建子场景 %s" % ROT_SCENE)
    else:
        print("  子场景 %s 已存在，复用" % ROT_SCENE)

    # 1) 子场景里放采集源，并把父场景原项的变换**原样搬过来**
    #    画布是全局的，所以同一个 transform 在子场景里语义完全相同。
    pit = find_item(c, parent, src)
    ptrans = c.req("GetSceneItemTransform",
                   {"sceneName": parent, "sceneItemId": pit["sceneItemId"]}
                   ).get("sceneItemTransform") or {}
    print("  原项变换：pos=(%.1f,%.1f) rot=%.1f scale=(%.3f,%.3f) boundsType=%s"
          % (ptrans.get("positionX", 0), ptrans.get("positionY", 0),
             ptrans.get("rotation", 0), ptrans.get("scaleX", 1),
             ptrans.get("scaleY", 1), ptrans.get("boundsType")))

    try:
        rit = find_item(c, ROT_SCENE, src)
        print("  子场景里已有采集源项 id=%s" % rit["sceneItemId"])
    except KeyError:
        rid = c.req("CreateSceneItem", {"sceneName": ROT_SCENE,
                                        "sourceName": src,
                                        "sceneItemEnabled": True})["sceneItemId"]
        print("  子场景里新建采集源项 id=%s" % rid)
        rit = {"sceneItemId": rid}
    c.req("SetSceneItemTransform", {"sceneName": ROT_SCENE,
                                    "sceneItemId": rit["sceneItemId"],
                                    "sceneItemTransform": ptrans})

    # 2) 父场景：禁用原采集项，放入子场景项（不缩放、不旋转，铺满画布）
    c.req("SetSceneItemEnabled", {"sceneName": parent,
                                  "sceneItemId": pit["sceneItemId"],
                                  "sceneItemEnabled": False})
    print("  父场景原采集项已禁用（保留，便于回滚）")
    try:
        cit = find_item(c, parent, ROT_SCENE)
    except KeyError:
        cid = c.req("CreateSceneItem", {"sceneName": parent,
                                        "sourceName": ROT_SCENE,
                                        "sceneItemEnabled": True})["sceneItemId"]
        print("  父场景新建子场景项 id=%s" % cid)
        cit = {"sceneItemId": cid}
    # 子场景项不缩放不旋转，按画布原尺寸铺满。只描画必要字段：
    # boundsWidth/boundsHeight 传 0 会被 OBS 判为非法（下限 1.0）。
    c.req("SetSceneItemTransform", {
        "sceneName": parent, "sceneItemId": cit["sceneItemId"],
        "sceneItemTransform": {"positionX": 0, "positionY": 0,
                               "rotation": 0, "scaleX": 1, "scaleY": 1}})

    # 3) 把滤镜挪到子场景上：设置原样复制，然后关掉原源上的那份
    fsrc = c.req("GetSourceFilter", {"sourceName": src, "filterName": FILT})
    fset = fsrc.get("filterSettings") or {}
    kind = fsrc.get("filterKind") or "obs_beauty_filter"
    have = [f.get("filterName") for f in c.filters(ROT_SCENE)]
    if FILT in have:
        c.req("SetSourceFilterSettings", {"sourceName": ROT_SCENE,
                                          "filterName": FILT,
                                          "filterSettings": fset,
                                          "overlay": True})
        print("  子场景滤镜已存在，设置同步")
    else:
        c.req("CreateSourceFilter", {"sourceName": ROT_SCENE,
                                     "filterName": FILT, "filterKind": kind,
                                     "filterSettings": fset})
        print("  在子场景上新建滤镜 %s（kind=%s，设置复制 %d 项）"
              % (FILT, kind, len(fset)))
    c.req("SetSourceFilterEnabled", {"sourceName": ROT_SCENE,
                                     "filterName": FILT, "filterEnabled": True})
    c.req("SetSourceFilterEnabled", {"sourceName": src,
                                     "filterName": FILT, "filterEnabled": False})
    print("  原源上的滤镜已禁用")
    return True


def revert(c, parent, src):
    try:
        cit = find_item(c, parent, ROT_SCENE)
        c.req("RemoveSceneItem", {"sceneName": parent,
                                  "sceneItemId": cit["sceneItemId"]})
        print("  父场景已移除子场景项")
    except KeyError:
        print("  父场景没有子场景项")
    try:
        pit = find_item(c, parent, src)
        c.req("SetSceneItemEnabled", {"sceneName": parent,
                                      "sceneItemId": pit["sceneItemId"],
                                      "sceneItemEnabled": True})
        print("  原采集项已恢复")
    except KeyError:
        print("  父场景没有原采集项")
    try:
        c.req("SetSourceFilterEnabled", {"sourceName": src,
                                         "filterName": FILT,
                                         "filterEnabled": True})
        print("  原源上的滤镜已启用")
    except Exception as e:   # noqa: BLE001
        print("  恢复滤镜失败：%s" % e)
    scenes = [s.get("sceneName") for s in c.scenes()]
    if ROT_SCENE in scenes:
        c.req("RemoveScene", {"sceneName": ROT_SCENE})
        print("  子场景已删除")
    return True


def probe(c, parent, src):
    os.makedirs(OUT, exist_ok=True)
    for tag, name in (("rot_scene", parent), ("rot_sub", ROT_SCENE),
                      ("rot_src", src)):
        path = os.path.join(OUT, "probe_%s.png" % tag)
        img = c.shot(name, path)
        print("  截图 %-6s %dx%d -> %s" % (tag, img.size[0], img.size[1], path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default="视频")
    ap.add_argument("--scene", default="场景")
    ap.add_argument("--source", default="视频采集设备")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--revert", action="store_true")
    ap.add_argument("--probe", action="store_true")
    args = ap.parse_args()
    if not (args.apply or args.revert):
        raise SystemExit("给 --apply 或 --revert")

    attached = V.obs_running()
    p, snap = (None, None)
    if not attached:
        p, snap = V.start_obs(args.collection, extra_env={"BEAUTY_PROFILE": "1"})
    try:
        c = obsctl.wait_ready(wait=90)
        print("OBS 已连接%s" % ("" if not attached else "（接管现有会话）"))
        time.sleep(1.5)
        ok = apply_rot(c, args.scene, args.source) if args.apply \
            else revert(c, args.scene, args.source)
        if ok and args.probe:
            time.sleep(2.5)      # 等滤镜重建 + 相机出帧
            probe(c, args.scene, args.source)
        c.close()
    finally:
        if p is not None:
            V.close_obs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
