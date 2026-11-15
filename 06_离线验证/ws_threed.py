#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""在真实 GPU 链上单独跑「五官立体」，并与 CPU 离线台逐像素对拍。

三个坑（都是这轮踩出来的，写在这里免得下次重踩）：
  1. 源在 CamRot 里被用户隐藏过 → 隐藏时 GetSourceScreenshot(源) 返回的是
     整个场景（摄像头画面）。所以临时建一个只放这张图的场景、切过去拍。
  2. **直接截源不会过滤镜**（拿到的是原图，0 像素变化）；必须截**场景**。
     场景截图是画布分辨率 1920x1080，所以素材也要用 1920x1080，
     这样图源在画布里是 1:1，不经过任何重采样，能和 CPU 逐像素对拍。
  3. 静态图源是**按需渲染**：改完参数不主动要帧就一直是旧画面，
     而且检测/几何是异步的 → 先空拍几帧推到位，最后一帧才作数。

用法（OBS 已启动并开着 websocket）：
    python ws_threed.py
产出：_gpu_td_off.png / _gpu_td30.png / _gpu_td.png（1920x1080）
"""
import os
import sys
import time

sys.argv = ["x"]
import ws_three as W  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SCENE = "ws_threed_scene"
IMG = os.path.join(HERE, "_wssrc_face_1080.png")


def run(o, st, out, wait=1.1, warm=6):
    o.call("SetSourceFilterSettings", sourceName=W.SRC, filterName=W.FILT,
           filterSettings=st, overlay=True)
    o.call("SetSourceFilterEnabled", sourceName=W.SRC, filterName=W.FILT,
           filterEnabled=True)
    for _ in range(warm):
        o.shot(SCENE, os.path.join(HERE, "_tmp_shot.png"))
        time.sleep(wait)
    o.shot(SCENE, out)
    print(out, flush=True)


def main():
    o = W.OBS()
    prev_scene = o.call("GetSceneList")["currentProgramSceneName"]

    # 场景必须先生成：CreateInput 的 sceneName 指向不存在的场景会静默失败。
    try:
        o.call("CreateScene", sceneName=SCENE)
    except RuntimeError as e:
        print("scene exists:", e)
    # 源可能已存在（上一次测试建的）：CreateInput 同名会报 601 且不生效，
    # 所以再用 SetInputSettings 把文件换成 1080p 那张。
    try:
        o.call("CreateInput", inputName=W.SRC, inputKind="image_source",
               sceneName=SCENE, inputSettings={"file": IMG},
               sceneItemEnabled=True)
    except RuntimeError as e:
        print("input exists:", e)
        o.call("SetInputSettings", inputName=W.SRC,
               inputSettings={"file": IMG}, overlay=True)
        # 源已存在但**不等于已经挂在这个新场景里** —— 不补这一句，场景是空的，
        # 截出来就是一张纯黑（这轮就黑了三次）。
        try:
            o.call("CreateSceneItem", sceneName=SCENE, sourceName=W.SRC,
                   sceneItemEnabled=True)
        except RuntimeError as e2:
            print("scene item exists:", e2)
    try:
        o.call("CreateSourceFilter", sourceName=W.SRC, filterName=W.FILT,
               filterKind="obs_beauty_filter", filterSettings={})
    except RuntimeError as e:
        print("filter exists:", e)
    o.call("SetCurrentProgramScene", sceneName=SCENE)
    time.sleep(1.5)

    try:
        b = dict(W.BASE)
        run(o, dict(b, face_threed=0.0), os.path.join(HERE, "_gpu_td_off.png"))
        run(o, dict(b, face_threed=30.0), os.path.join(HERE, "_gpu_td30.png"))
        run(o, dict(b, face_threed=100.0), os.path.join(HERE, "_gpu_td.png"))
    finally:
        o.call("SetCurrentProgramScene", sceneName=prev_scene)
        try:
            o.call("RemoveScene", sceneName=SCENE)
        except Exception as e:
            print("remove scene:", e)
        o.ws.close()
    print("done", flush=True)


if __name__ == "__main__":
    main()
