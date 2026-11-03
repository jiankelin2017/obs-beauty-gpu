#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「正面正脸」素材的静态场景集合（复用 make_static_collection 的骨架）。

产出两份，只新建、不修改既有集合：
    beauty_front4k  : 画布 3840x2160，源 assets/face_front_4k.png
    beauty_front720 : 画布 1280x720 ，源 assets/face_front_720p.png

为什么要两份：帧环只在单脸时启用（`mg.count == 1`），而正脸素材本身是单脸，
所以它既能测 720p 也能测 4K 的完整链路；与既有的侧脸素材形成对照。

用法（OBS 必须关闭，否则退出时会把集合文件覆盖回去）：
    py -3 05_验证/make_front_collection.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_static_collection as M   # noqa: E402


def main():
    print("场景集合目录: %s" % M.scenes_dir())
    M.build("beauty_front4k", "face_front_4k.png", (3840, 2160))
    M.build("beauty_front720", "face_front_720p.png", (1280, 720))
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
