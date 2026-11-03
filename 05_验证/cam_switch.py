#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 OBS 里的采集源切到指定设备，并读出它真实的分辨率/格式能力。

为什么要有这一支：`beauty_s1` 集合原来绑的是内置头（`Integrated Camera`），换采集卡后
源仍在用旧设备。手改集合 JSON 要拼 `video_device_id`（形如 `USB Video:\\?\\usb#vid_...#{65e8773d-...}\\global`），
拼错就是「找不到设备」黑屏。这里走 OBS 自己的设备枚举，取到的值一定是合法的。

用法：
    py -3 05_验证/cam_switch.py                       # 只列出所有采集设备，不改
    py -3 05_验证/cam_switch.py --match "USB Video"    # 切到名字含该串的设备
    py -3 05_验证/cam_switch.py --match "USB Video" --resolution 1920x1080 --fps 60
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
    ap.add_argument("--collection", default="beauty_s1")
    ap.add_argument("--source", default="视频采集设备")
    ap.add_argument("--match", default=None, help="设备名包含该串即选中")
    ap.add_argument("--resolution", default=None, help="如 1920x1080")
    ap.add_argument("--fps", default=None, help="如 60")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    # OBS 可能已经被人打开在看画面。那种情况直接接管，不重启、也不关它
    # ——重启会丢掉用户正在看的预览，而且每次起停都要重载 shader。
    attached = V.obs_running()
    snap = V.log_dir_snapshot()
    proc = None
    if attached:
        print("OBS 已在运行，接管当前会话（结束时不会关闭它）")
    else:
        proc, snap = V.start_obs(args.collection, extra_env={"BEAUTY_PROFILE": "1"})
    try:
        c = obsctl.wait_ready(wait=90)
        try:
            colset = c.req("GetSceneCollectionList") or {}
            print("当前场景集合：%s" % colset.get("currentSceneCollectionName"))
        except Exception:                # noqa: BLE001
            pass
        print("=== 采集设备列表 ===")
        items = c.input_prop_items(args.source, "video_device_id")
        for i, it in enumerate(items):
            print("  [%d] %s" % (i, it.get("itemName")))
        if not items:
            print("  !! 源 %r 不是采集设备，或没有 video_device_id 属性"
                  % args.source)
            return 2

        print("\n=== 当前设备 ===")
        cur = c.input_settings(args.source)
        print("  %s" % (cur.get("video_device_id") or "(空)"))

        if args.match:
            hit = [it for it in items
                   if args.match.lower() in str(it.get("itemName", "")).lower()]
            if not hit:
                print("\n!! 没有名字含 %r 的设备" % args.match)
                return 2
            val = hit[0]["itemValue"]
            st = {"video_device_id": val, "last_video_device_id": val}
            if args.resolution:
                st["res_type"] = 1
                st["resolution"] = args.resolution
            if args.fps:
                st["fps_type"] = 1
                st["fps_custom"] = int(args.fps)
            print("\n=== 切到 %s ===" % hit[0]["itemName"])
            c.set_input_settings(args.source, st)
            time.sleep(2.5)
            back = c.input_settings(args.source)
            ok = (back.get("video_device_id") == val)
            print("  读回一致：%s" % ("是" if ok else "否（%s）"
                                     % back.get("video_device_id")))

        # 设备能力：分辨率 / 格式 / 帧率候选
        for prop in ("resolution", "video_format", "fps"):
            try:
                opts = c.input_prop_items(args.source, prop)
            except Exception as e:                      # noqa: BLE001
                print("  %s 枚举失败：%s" % (prop, e))
                continue
            names = [str(o.get("itemName")) for o in opts]
            print("  %-14s %d 项：%s" % (prop, len(names),
                                        ", ".join(names[:12])
                                        + (" ..." if len(names) > 12 else "")))

        path = os.path.join(OUT, "_probe.png")
        img = c.shot(args.source, path)
        print("\n=== 实际出图 ===\n  %dx%d  -> %s" % (img.size[0], img.size[1], path))
        c.close()
    finally:
        if proc is not None:
            V.close_obs()
    print("\n日志：%s" % V.newest_log(snap))
    return 0


if __name__ == "__main__":
    sys.exit(main())
