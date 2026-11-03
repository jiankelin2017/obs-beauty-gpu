#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""看「用户实际所见」与「算法实际所见」的差别。

背景：OBS 的滤镜挂在源上，场景项的旋转发生在滤镜**之后**。用户把画面转了 -90°
让预览正立，但美颜算法拿到的仍是旋转前的原始画面 —— 脸是躺着的，YuNet 与五官
几何都会按「眼睛在上、法令纹在下」的假设去套一个躺着的脸。

这个脚本一次截两张图给结论提供依据：
  1. 场景截图（SaveSourceScreenshot 指定场景名）= 用户看到的画面
  2. 源截图（指定采集源名）= 滤镜实际输入的画面
两只图的方向差，就是「旋转发生在滤镜之后」的直接证据。

用法：
  py -3 05_验证/probe_scene.py --collection 视频 --scene 场景 --source 视频采集设备
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import verify_s1_correct as V      # noqa: E402

OUT = os.path.join(HERE, "out", "cam")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default="视频")
    ap.add_argument("--scene", default="场景")
    ap.add_argument("--source", default="视频采集设备")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    attached = V.obs_running()
    if attached:
        # 人可能正开着 OBS 对着预览摆机位，重启会打断他。接管即可。
        print("OBS 已在运行，接管当前会话（结束时不会关闭它）")
        p = None
    else:
        p, snap = V.start_obs(args.collection, extra_env={"BEAUTY_PROFILE": "1"})
    try:
        c = obsctl.wait_ready(wait=90)
        print("OBS 已连接")
        # 连上后等相机出帧（dshow 首帧常要 1~2 s）
        time.sleep(3.0)

        items = c.scene_items(args.scene)
        print("场景「%s」的项：" % args.scene)
        for it in items:
            print("   %s (id=%s)" % (it.get("sourceName"), it.get("sceneItemId")))

        for s in (args.scene, args.source):
            path = os.path.join(OUT, "probe_%s.png" % ("scene" if s == args.scene
                                                       else "source"))
            img = c.shot(s, path)
            print("  截图 %-8s %dx%d -> %s" % (s, img.size[0], img.size[1], path))

        # 滤镜挂在哪个源上：这是「旋转在滤镜之后」能否成立的另一半证据。
        for s in (args.scene, args.source):
            try:
                fl = c.filters(s)
            except Exception as e:   # noqa: BLE001
                print("  %s 取滤镜失败：%s" % (s, e))
                continue
            print("  源「%s」的滤镜：%s" % (s, [(f.get("name"), f.get("enabled"))
                                              for f in fl]))
        c.close()
    finally:
        if p is not None:
            V.close_obs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
