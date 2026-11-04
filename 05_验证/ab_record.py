#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""人眼 A/B：用真机录屏，把 `max` 与 `rms` 两臂的动态画面拿下来。

为什么不能靠静态对拍
--------------------
`verify_s1_correct.py` 的六配置在方案C 开启下 `maxdiff=0` —— 那是因为静态图
让掩码有机会收敛到当前帧。**mask 的滞后只在播放中才存在**，一暂停就消失。
所以「rms 的代价看不看得见」这件事，必须在播放状态下录。

为什么用标记而不是内容匹配
--------------------------
两次录制的起始时刻不同。素材开头 4 帧是白/黑/红/绿同步标记（`make_ab_clip.py`），
素材 8.13 s 循环，录 13 s 必然覆盖一个完整循环 → 一定能定位相位差。

臂与参数
--------
`off`（不开方案C）作为「画面上限」参照：人眼 A/B 真正要比的是 `max` vs `rms`，
`off` 只用来确认「这段素材美颜确实在起作用」，否则两臂一样是空对空。

用法
----
    py -3 ab_record.py --cfg combo
    py -3 ab_record.py --cfg combo --arms max rms rms4 --seconds 13
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                    # noqa: E402
import verify_s1_correct as V    # noqa: E402
from mask_off_probe import CFGS  # noqa: E402

COLL = "beauty_real_ab"
CANVAS = (1080, 1920)
FPS = None          # None = 沿用 basic.ini（30）；P2-5 的 60 fps 格要显式给
REC_ENCODER = None  # None = 沿用 basic.ini（nvenc）；"obs_x264" = 真无损
OUT = os.path.join(HERE, "out", "ab")

ARMS = {
    "off":  {},
    "max":  {"BEAUTY_MOTION_GPU": "1"},
    "rms":  {"BEAUTY_MOTION_GPU": "1", "BEAUTY_DET_ADAPT_AGG": "rms"},
    "rms4": {"BEAUTY_MOTION_GPU": "1", "BEAUTY_DET_ADAPT_AGG": "rms",
             "BEAUTY_DET_ADAPT_STILL": "4"},
    # ── 以下是 §9.35.7 的节拍归因臂，不是拿来做人眼对比的 ──
    # `BEAUTY_MASK_EVERY=N` 会**自动关掉自适应**（源码 beauty_core.cpp:1305 的注释），
    # 于是提交节奏被钉死在「每 N 帧一次」，与画面动不动、用哪种聚合都无关。
    # 用它把「提交间隔」这个变量单独拎出来：
    #   fix4  应该复现 rms4（period 3 = 每 4 帧）的抖动率
    #   fix8  应该复现 rms / strong（period 7 = 每 8 帧）的抖动率
    #   rms30 把间隔拉到每 30 帧，抖动量应随间隔线性上升
    "fix4":  {"BEAUTY_MOTION_GPU": "1", "BEAUTY_MASK_EVERY": "4"},
    "fix8":  {"BEAUTY_MOTION_GPU": "1", "BEAUTY_MASK_EVERY": "8"},
    "rms30": {"BEAUTY_MOTION_GPU": "1", "BEAUTY_DET_ADAPT_AGG": "rms",
              "BEAUTY_DET_ADAPT_STILL": "30"},
    # 把「动不动」这个变量也钉死：THR 决定判动阈值（motDiff > THR 即判动）。
    #   rms1 = rms 聚合 + 阈值 1 → 恒判在动 → period 0 → 每帧提交
    #   max64 = max 聚合 + 阈值 64 → 恒判静止 → period 7 → 每 8 帧提交
    # 两臂的聚合方式都与上面对调，用来证明「抖动跟 agg 无关、跟提交间隔有关」。
    # ⚠ THR 的合法区间是 1..64（beauty_core.cpp:1319），给 0 或 255 会被**静默丢弃**、
    # 退回默认 6 —— 我第一次就踩了这个坑，两臂都变成了「再测一遍 rms/max」。
    "rms1":  {"BEAUTY_MOTION_GPU": "1", "BEAUTY_DET_ADAPT_AGG": "rms",
               "BEAUTY_DET_ADAPT_THR": "1"},
    "max64": {"BEAUTY_MOTION_GPU": "1", "BEAUTY_DET_ADAPT_THR": "64"},
}


def fit_item(c, canvas):
    """把播放源强制铺满画布（不能依赖集合文件里的几何）。

    为什么（2026-11-04，P2-5 换 1280x720 时踩到）：集合是从 1080x1920 竖屏模板
    复制来的，OBS 会按自己的基准尺寸把 `pos_rel` / `scale_ref` 换算成绝对
    `pos` / `scale`。实测 1280x720 下变成「素材放大 2 倍、只显示左上 1/4、
    落在画布右下」（pos=640,360 / scale=2），而**录制本身完全正常** ——
    帧率、时长、码率都对，只有对着画面看才发现，日志里一个字都没有。

    这里用 SCALE_STRETCH 直接钉到画布：16:9 素材进 16:9 画布不会变形，
    也不依赖 OBS 对源尺寸的判断。
    """
    for it in c.scene_items("Main"):
        if it.get("sourceName") != "still":
            continue
        c.set_item_transform("Main", it["sceneItemId"], {
            "alignment": 5,
            "boundsType": "OBS_BOUNDS_STRETCH",
            "boundsAlignment": 5,
            "boundsWidth": float(canvas[0]),
            "boundsHeight": float(canvas[1]),
            "positionX": 0.0,
            "positionY": 0.0,
            "rotation": 0.0,
            "cropLeft": 0, "cropTop": 0, "cropRight": 0, "cropBottom": 0,
        })
        print("  播放源已铺满画布 %dx%d" % canvas)


def record_once(arm, cfg_name, seconds, tag):
    """起一轮 OBS、下参数、录一段、关掉，返回录制文件路径与日志路径。"""
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")

    env = dict(ARMS[arm])
    env["BEAUTY_PROFILE"] = "1"
    proc, snap = V.start_obs(COLL, extra_env=env, canvas=CANVAS,
                             fps=FPS, rec_encoder=REC_ENCODER)
    rec_path = None
    try:
        c = obsctl.wait_ready(wait=120)
        if c is None:
            sys.exit("OBS 的 websocket 没起来")
        # websocket 先起来、场景还在加载，这时下参数会拿到 code=207
        # "OBS is not ready to perform the request"。重试到成功为止。
        time.sleep(3.0)
        for attempt in range(12):
            try:
                V.set_config(c, CFGS[cfg_name][1])
                break
            except RuntimeError as e:
                if "207" not in str(e) and "not ready" not in str(e):
                    raise
                if attempt == 11:
                    raise
                time.sleep(1.0)

        try:
            fit_item(c, CANVAS)
        except Exception as e:
            print("  !! 铺满画布失败（画面可能被裁）：%s" % e)

        # 录制目录直接指到 out/ab，省得去猜默认路径
        os.makedirs(OUT, exist_ok=True)
        try:
            c.req("SetRecordDirectory", {"recordDirectory": OUT.replace("\\", "/")})
        except Exception as e:
            print("  SetRecordDirectory 失败（用默认目录）：%s" % e)

        time.sleep(2.0)          # 让素材起播、滤镜稳态
        c.req("StartRecord")
        t0 = time.time()
        while time.time() - t0 < seconds:
            time.sleep(0.5)
            st = c.req("GetRecordStatus")
            if not st.get("outputActive"):
                sys.exit("录制提前停了（%s）" % arm)
        c.req("StopRecord")
        time.sleep(1.5)
        try:
            rec_path = c.req("GetRecordStatus").get("outputPath")
        except Exception:
            rec_path = None
        c.close()
    finally:
        log = V.close_obs()

    return rec_path, log, snap


def relocate(rec_path, dst):
    """把录制文件搬到规范名字上（OBS 生成的是带时间戳的名字）。"""
    if not rec_path or not os.path.isfile(rec_path):
        # 兜底：在录制目录里找最新改动的视频
        cands = []
        for f in os.listdir(OUT):
            if f.lower().endswith((".mp4", ".mkv", ".hybrid_mp4")):
                cands.append(os.path.join(OUT, f))
        if not cands:
            return None
        rec_path = max(cands, key=os.path.getmtime)
    if os.path.abspath(rec_path) != os.path.abspath(dst):
        if os.path.exists(dst):
            try:
                os.remove(dst + ".old")
            except OSError:
                pass
            os.replace(dst, dst + ".old")
        shutil.move(rec_path, dst)
    return dst


def main(argv=None):
    global COLL, CANVAS, FPS, REC_ENCODER
    ap = argparse.ArgumentParser()
    ap.add_argument("--cfg", default="combo", choices=tuple(CFGS))
    ap.add_argument("--arms", nargs="*", default=["max", "rms", "rms4"],
                    choices=tuple(ARMS))
    ap.add_argument("--seconds", type=float, default=13.0,
                    help="每臂录制时长；素材 8.13 s 循环，>9 s 才保证覆盖一整圈")
    ap.add_argument("--tag", default="", help="输出文件名后缀")
    ap.add_argument("--coll", default=COLL, help="场景集合名")
    ap.add_argument("--canvas", default="",
                    help="画布 WxH（默认沿用 1080x1920）")
    ap.add_argument("--fps", type=int, default=0,
                    help="输出帧率；0 = 沿用 basic.ini。必须与素材帧率一致，"
                         "否则会抽帧/重帧，闪烁法的时变信号直接失真")
    ap.add_argument("--lossless", action="store_true",
                    help="改用 x264 无损录制（NVENC 没有无损模式，"
                         "有损下噪声底会到 p50 6.6 灰阶，与信号同量级）")
    a = ap.parse_args(argv)

    COLL = a.coll
    FPS = a.fps or None
    REC_ENCODER = "obs_x264" if a.lossless else None
    if a.canvas:
        cw, ch = [int(x) for x in a.canvas.lower().split("x")]
        CANVAS = (cw, ch)

    os.makedirs(OUT, exist_ok=True)
    meta = []
    for arm in a.arms:
        print("\n=== %s（%s，%.0f s）===" % (arm, CFGS[a.cfg][0], a.seconds))
        name = "%s_%s%s" % (arm, a.cfg, ("_" + a.tag) if a.tag else "")
        raw_path, log, _ = record_once(arm, a.cfg, a.seconds, name)
        dst = os.path.join(OUT, name + ".mp4")
        got = relocate(raw_path, dst)
        if not got:
            print("  录制失败：没找到输出文件")
            continue
        sz = os.path.getsize(got) / 1e6
        print("  -> %s  %.1f MB" % (os.path.basename(got), sz))
        meta.append({"arm": arm, "cfg": a.cfg, "file": got, "log": log,
                     "seconds": a.seconds})

    mf = os.path.join(OUT, "meta_%s%s.json" % (a.cfg, ("_" + a.tag) if a.tag else ""))
    with open(mf, "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
    print("\n元信息写入 %s" % mf)
    return 0


if __name__ == "__main__":
    sys.exit(main())
