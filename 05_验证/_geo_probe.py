#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性实验：找出 1280x720 画布下场景项的正确几何参数。

症状：素材 1280x720 与画布同尺寸，但录制出来左侧 280 px 黑边、右侧被裁，
可见内容 1000x720。集合里 `pos` 被 OBS 改写成 280，说明 `pos_rel=-1` 的
换算结果不是「左对齐」，而是一个依赖 item 尺寸的值。

本脚本对同一集合试三组参数，各起一次 OBS 截一帧，量「非黑区域」的边界。
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import cv2
import numpy as np
import obsctl
import verify_s1_correct as V

SCENES = os.path.join(os.environ["APPDATA"], "obs-studio", "basic", "scenes")
COLL = "beauty_real_ab720p60"
CW, CH = 1280, 720

VARIANTS = {
    "A_posrel0": dict(align=5, bounds_type=0, pos=(0.0, 0.0), pos_rel=(0.0, 0.0)),
    "B_bounds": dict(align=5, bounds_type=2, pos=(0.0, 0.0), pos_rel=(0.0, 0.0),
                     bounds=(float(CW), float(CH))),
    "C_posrel1": dict(align=5, bounds_type=0, pos=(0.0, 0.0), pos_rel=(1.0, 1.0)),
}


def patch(name, spec, dst):
    d = json.load(open(os.path.join(SCENES, name + ".json"), encoding="utf-8"))
    sc = [s for s in d["sources"] if s["id"] == "scene"][0]["settings"]
    sc["cx"], sc["cy"] = float(CW), float(CH)
    it = sc["items"][0]
    it["align"] = spec["align"]
    it["bounds_type"] = spec["bounds_type"]
    it["pos"] = {"x": spec["pos"][0], "y": spec["pos"][1]}
    it["pos_rel"] = {"x": spec["pos_rel"][0], "y": spec["pos_rel"][1]}
    it["scale"] = {"x": 1.0, "y": 1.0}
    it["scale_rel"] = {"x": 1.0, "y": 1.0}
    it["scale_ref"] = {"x": float(CW), "y": float(CH)}
    if "bounds" in spec:
        it["bounds"] = {"x": spec["bounds"][0], "y": spec["bounds"][1]}
    d["name"] = os.path.basename(dst)[:-5]
    for s in d["sources"]:
        if s["id"] == "ffmpeg_source":
            s["settings"]["local_file"] = s["settings"]["local_file"]
    json.dump(d, open(dst, "w", encoding="utf-8"), ensure_ascii=False, indent=4)
    return d["name"]


def measure(png):
    # cv2.imread 读不了中文路径（会静默返回 None），必须走 fromfile+imdecode
    try:
        im = cv2.imdecode(np.fromfile(png, dtype=np.uint8), cv2.IMREAD_COLOR)
    except Exception:
        im = None
    if im is None:
        return "读不到（%s）" % png
    g = im.mean(axis=2)
    col, row = g.mean(axis=0), g.mean(axis=1)
    nc, nr = np.nonzero(col > 8)[0], np.nonzero(row > 8)[0]
    if not len(nc):
        return "全黑"
    return "x %d..%d (宽 %d)  y %d..%d (高 %d)" % (
        nc.min(), nc.max(), nc.max() - nc.min() + 1,
        nr.min(), nr.max(), nr.max() - nr.min() + 1)


def main():
    src = os.path.join(SCENES, COLL + ".json")
    raw = open(src, "rb").read()
    try:
        for label, spec in VARIANTS.items():
            name = "beauty_geo_" + label
            patch(COLL, spec, os.path.join(SCENES, name + ".json"))
            if not V.close_obs():
                sys.exit("关不掉 OBS")
            V.start_obs(name, extra_env={"BEAUTY_PROFILE": "1"},
                        canvas=(CW, CH))
            try:
                c = obsctl.wait_ready(wait=90, quiet=True)
                import time
                time.sleep(5.0)
                out = os.path.join(HERE, "out", "ab", "_geo_%s.png" % label)
                c.shot("still", out)
                c.close()
            finally:
                V.close_obs()
            print("%-12s -> %s" % (label, measure(out)))
    finally:
        open(src, "wb").write(raw)


if __name__ == "__main__":
    main()
