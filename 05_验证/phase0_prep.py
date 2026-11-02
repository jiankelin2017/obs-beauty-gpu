#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase 0 基线测量 —— 场景集合生成器。

从用户现有的 beauty_test 场景集合派生一份**独立**的 phase0_baseline 集合，
只改这个副本，绝不碰用户自己的集合。用途：把滤镜参数设成规程规定的中高负载，
并按需改「滤镜作用的分辨率」。

为什么要派生而不是直接改 beauty_test：
  1) 规程要求的参数（磨皮 60 / 美白 40 / 红润 20 / 清晰 30 / 大眼 10 / 瘦脸 10 / 下巴 10）
     会改变画面，用户自己的场景不该被悄悄改掉；
  2) 每次测量要换分辨率，需要反复重写，写副本可以随便覆盖。

三种分辨率来源（--mode）：
  camera  滤镜挂在「视频采集设备」上，分辨率由摄像头原生输出决定（--res 设 dshow 模式）。
          → 最真实，但受硬件上限约束（Integrated Camera 大概率只有 720p）。
  scene   滤镜挂到「美颜测试」这个场景源上，分辨率 = 集合画布（--canvas 设）。
          摄像头画面被缩放填满画布，滤镜就在画布尺寸上跑。
          → 能拿到 1080p / 4K 的**像素量**负载，但画面内容是放大过的。
          用于 camera 模式拿不到的分辨率；必须与 camera 模式在重叠分辨率上交叉校验。

用法:
    python phase0_prep.py --mode camera --res 1280x720
    python phase0_prep.py --mode camera --res 1920x1080
    python phase0_prep.py --mode scene  --canvas 3840x2160
    python phase0_prep.py --list-camera      # 只列出摄像头支持的采集模式（需 pydshow，通常不可用）
"""
import argparse
import json
import os
import shutil
import sys

COLLECTION = "phase0_baseline"
BASE_COLLECTION = "beauty_test"

# 规程第二节规定的代表参数（面板单位 0..100，贴中高负载的真实使用）
PARAMS = {
    "blur_level": 60.0,   # 磨皮
    "color_level": 40.0,  # 美白
    "red_level": 20.0,    # 红润
    "clarity": 30.0,      # 清晰
    "big_eye": 10.0,      # 大眼
    "thin_face": 10.0,    # 瘦脸
    "chin": 10.0,         # 下巴
}


def scenes_dir():
    appdata = os.environ.get("APPDATA") or os.path.expanduser(r"~\AppData\Roaming")
    return os.path.join(appdata, "obs-studio", "basic", "scenes")


def load_base():
    p = os.path.join(scenes_dir(), BASE_COLLECTION + ".json")
    if not os.path.isfile(p):
        sys.exit("找不到基准集合: %s" % p)
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def find_source(doc, sid):
    for s in doc.get("sources", []):
        if s.get("id") == sid:
            return s
    return None


def set_camera_resolution(src, res):
    """把 dshow 源钉到指定采集模式。res_type=1 是 ResType_Custom（见 win-dshow.cpp:97）。"""
    w, h = [int(v) for v in res.lower().split("x")]
    st = src.setdefault("settings", {})
    st["res_type"] = 1
    st["resolution"] = "%dx%d" % (w, h)
    st["frame_interval"] = 333333          # 100ns 单位 → 30 fps
    st["video_format"] = 0                 # Any


def camera_native_res(cam):
    """取相机当前采集分辨率。读不到就回退 1280x720（本机 Integrated Camera 的上限）。"""
    s = cam.get("settings") or {}
    r = s.get("resolution") or ""
    if r and "x" in r.lower():
        try:
            w, h = [int(v) for v in r.lower().split("x")]
            if w > 0 and h > 0:
                return (w, h)
        except ValueError:
            pass
    w = s.get("width") or 0
    h = s.get("height") or 0
    if w > 0 and h > 0:
        return (int(w), int(h))
    return (1280, 720)


def build_item(camera_uuid, canvas, src_size=(1280, 720)):
    """一个干净的场景项：摄像头项，左上角定位，不裁不转。

    ⚠ 实测结论（3.1.44 用掩码覆盖率量出来的，别再重复踩）：
      · 720p（画布 == 相机原生）→ 相机正好铺满，掩码覆盖 11~18%，
        引擎耗时与摄像头直挂路径一致（13.6~14.9 ms）→ 这个档位口径可信。
      · 1080p / 4K → OBS 会把项的 transforms 归一化（写 scale=1.5 存回去仍是 1.0），
        相机只按原生 1280x720 居中显示，于是「脸占画面比例」随分辨率变小
        （1080p 实测 8.5%，4K 更低）。所以高分辨率档测的是**像素规模**，
        不是「真实 1080p 人脸」，结论里必须连同掩码%一起引用。

    试过但不可用的两条路（都实测过，别再试）：
      · bounds_type=2（SCALE_INNER）→ OBS 回来把 bounds 改成 360x360，脸小 3.5 倍。
      · align=5 + 绝对 scale + 去掉 *_rel → 相机被移出画面，掩码 0%。
    """
    cw, ch = canvas
    sw, sh = src_size
    return {
        "name": "视频采集设备",
        "source_uuid": camera_uuid,
        "visible": True,
        "locked": False,
        "rot": 0.0,
        "scale_ref": {"x": float(cw), "y": float(ch)},
        "align": 0,                       # 左上角定位
        "bounds_type": 0,                 # OBS_BOUNDS_NONE：不裁，用下面的 scale
        "bounds_align": 0,
        "bounds_crop": False,
        "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0,
        "id": 1,
        "group_item_backup": False,
        "pos": {"x": 0.0, "y": 0.0},
        "pos_rel": {"x": 0.0, "y": 0.0},
        "scale": {"x": float(cw) / float(sw), "y": float(ch) / float(sh)},
        "scale_rel": {"x": 1.0, "y": 1.0},
        "bounds": {"x": float(cw), "y": float(ch)},
        "bounds_rel": {"x": 1.0, "y": 1.0},
        "scale_filter": "disable",
        "blend_method": "default",
        "blend_type": "normal",
        "show_transition": {"duration": 0},
        "private_settings": {},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["camera", "scene"], default="camera")
    ap.add_argument("--res", default="", help="camera 模式：dshow 采集分辨率，如 1920x1080")
    ap.add_argument("--canvas", default="", help="scene 模式：画布分辨率，如 3840x2160")
    ap.add_argument("--out", default="", help="输出文件名（默认 %s.json）" % COLLECTION)
    ap.add_argument("--hide-item", action="store_true",
                    help="把场景项隐藏（画面全黑），用于区分有脸/整帧兜底两种成本")
    ap.add_argument("--keep-items", action="store_true",
                    help="scene 模式保留基准集合原有的场景项（用于判断黑屏是否由项构造引起）")
    ap.add_argument("--only-blur", default="",
                    help="把所有面板参数清零、只留 blur_level=N。"
                         "用于与其它分辨率做**单变量**对比（默认代表参数会让"
                         "磨皮/色调/五官同时变化，分项耗时不可比）")
    args = ap.parse_args()

    # ⚠ 3.1.50 踩过的坑：默认 PARAMS 是「代表性负载」，磨皮 60 + 色调 40/20 +
    # 清晰 30 + 美形三项 10。拿它去和「只有 blur=100」的窗口比磨皮耗时，
    # 差的不只是分辨率，还有强度 —— 结论会错。要比线性度就必须单变量。
    if args.only_blur:
        for k in PARAMS:
            PARAMS[k] = 0.0
        PARAMS["blur_level"] = float(args.only_blur)

    doc = load_base()
    # 集合内部 name 必须与文件名一致，否则 --collection <名> 找不到（OBS 按文件名加载）
    col_name = os.path.splitext(args.out)[0] if args.out else COLLECTION
    doc["name"] = col_name

    cam = find_source(doc, "dshow_input")
    if cam is None:
        sys.exit("基准集合里没有 dshow_input 源")
    scene = find_source(doc, "scene")
    beauty_filter = None
    for f in cam.get("filters", []):
        if f.get("id") == "obs_beauty_filter":
            beauty_filter = f
            break
    if beauty_filter is None:
        sys.exit("摄像头源上没有 obs_beauty_filter")
    beauty_filter["settings"].update(PARAMS)
    beauty_filter["enabled"] = True

    if args.mode == "camera":
        if not args.res:
            sys.exit("camera 模式必须给 --res")
        set_camera_resolution(cam, args.res)
        # 滤镜留在摄像头上（默认就是这样）
    else:
        if not args.canvas:
            sys.exit("scene 模式必须给 --canvas")
        cw, ch = [int(v) for v in args.canvas.lower().split("x")]
        # ⚠ 集合级的 "resolution" 字段**不生效**：OBS 的预览/输出画布来自
        # profile 的 basic.ini（BaseCX/BaseCY），改集合 JSON 没用（实测：
        # 写了 resolution=1280x720，日志仍打 base resolution: 1080x1920）。
        # 正确做法是打开场景源的 custom_size —— 它按 obs-scene.c:1281 读
        # settings 里的 custom_size / cx / cy，是集合内的设置，不动全局。
        scene["settings"]["custom_size"] = True
        scene["settings"]["cx"] = cw
        scene["settings"]["cy"] = ch
        if not args.keep_items:
            src = camera_native_res(cam)
            # 顺手把相机的采集分辨率钉死在原生值：场景模式下相机设置若被写空，
            # dshow 会退回设备默认档，脸的大小又变了 —— 那样「同构图」就不成立了。
            set_camera_resolution(cam, "%dx%d" % src)
            scene["settings"]["items"] = [build_item(cam.get("uuid"), (cw, ch), src)]
        # 把滤镜从摄像头挪到场景上：场景源的基础尺寸 = 上面 cx/cy，
        # obs-beauty 的 cx/cy 取的就是这个（obs-beauty.cpp:911）。
        cam["filters"] = [f for f in cam["filters"] if f.get("id") != "obs_beauty_filter"]
        scene.setdefault("filters", []).append(beauty_filter)
        if args.hide_item:
            for it in scene["settings"]["items"]:
                it["visible"] = False
        doc["current_scene"] = scene.get("name")
        doc["current_program_scene"] = scene.get("name")

    name = args.out or (COLLECTION + ".json")
    dst = os.path.join(scenes_dir(), name)
    if os.path.isfile(dst):
        shutil.copy2(dst, dst + ".bak")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=4)

    print("写入: %s（name=%s）" % (dst, col_name))
    print("  模式      : %s" % args.mode)
    if args.mode == "camera":
        print("  采集模式  : %s @30fps" % args.res)
        print("  滤镜挂在  : 视频采集设备（分辨率 = 摄像头输出）")
    else:
        print("  场景尺寸  : %s（custom_size，滤镜分辨率 = 它）" % args.canvas)
        print("  滤镜挂在  : 场景源")
    print("  滤镜参数  : " + ", ".join("%s=%.0f" % (k, v) for k, v in PARAMS.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
