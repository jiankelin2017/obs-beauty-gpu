#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""造「人眼 A/B」用的播放素材 + 专用 OBS 集合。

为什么需要这个
--------------
两臂（max / rms）要各录一次屏，两次录制的起始时刻不可能对齐。所以素材开头
放 4 帧**同步标记**（白/黑/红/绿），素材循环播放；录制时长覆盖一个完整循环，
对齐时找这 4 帧就能求出相位差。

为什么重编码可以接受
--------------------
两臂读的是**同一份**素材，编码损失是两臂共同的地基，不进入差异。用 FFV1
（无损）只是为了别让编码噪声污染 S7 的判定值，不是为了画面本身。

素材段的选择
------------
取 cam_real_1080x1920.mp4 的 [1421, 1661)（8.0 s @30fps）。这一段的挑选依据是
mask_lag_sim 的逐帧数据：在 rms@8 口径下，它含全片最多的「rms 落后 ≥6 帧而
max 只落后 ≤2 帧」的帧（76/240 = 31.7%，全片中位窗口约 22%），且偏移
p90 = 15.6 px、max = 51.5 px。

**用人眼 A/B 看最坏段，逻辑最硬**：如果最差段都看不出来，典型段必然看不出来。
反过来不成立 —— 这一点写进结论时必须交代。

用法
----
    py -3 make_ab_clip.py            # 造素材 + 建集合
    py -3 make_ab_clip.py --check    # 只核对已有素材
"""

import argparse
import json
import os
import shutil
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets", "cam_real_1080x1920.mp4")
DST = os.path.join(HERE, "assets", "ab_clip_1080x1920.avi")

F0, F1 = 1421, 1661          # 8.0 s @30fps，见模块注释
FPS = 30
COLL_SRC = "beauty_real_cam_1080x1920"
COLL_DST = "beauty_real_ab"
CANVAS = (1080, 1920)        # 集合画布；默认与 SRC 同尺寸，--canvas 可覆盖

# 同步标记：白 / 黑 / 纯红 / 纯绿（BGR）。四帧组合在真实画面里不可能出现，
# 且用「每帧三通道的均值与方差」就能唯一识别，不依赖阈值调参。
#
# 尺寸按画布生成（2026-11-04）：原来写死 1920×1080，换 720p 素材时靠
# build_clip 里的 resize 兜底 —— 能跑，但「标记帧就是画布尺寸」这件事
# 变得靠隐式兜底维持，读代码的人会以为标记永远是 1080p。显式生成更清楚。
def make_marks(h, w):
    return [
        np.full((h, w, 3), 255, np.uint8),                       # 白
        np.zeros((h, w, 3), np.uint8),                           # 黑
        np.dstack([np.zeros((h, w), np.uint8)] * 2 +
                  [np.full((h, w), 255, np.uint8)]),             # 红
        np.dstack([np.zeros((h, w), np.uint8),
                   np.full((h, w), 255, np.uint8),
                   np.zeros((h, w), np.uint8)]),                 # 绿
    ]


MARKS = make_marks(1920, 1080)

# 帧号条码的格数。8 格只到 255 帧 —— 原设计 240 帧刚好够，换 480 帧（8 s
# @60fps）就会溢出（实测 479 读成 223，且不报错，只是对不上）。按帧数自动选：
BITS = 8


def stamp_code(frame, n, bits=None):
    """在帧右下角贴 8 格条码，bit i 写在第 i 格（白=1 黑=0）。

    为什么需要帧号
    --------------
    两臂是两次独立录制，StartRecord 相对素材起播的延迟不可能相同 —— 实测
    相差 **2.5 帧**（全局扫描在 +2 / +3 之间摆动）。整数帧对齐永远差半帧，
    而说话时半帧就足以让嘴型差一大截。第一版正是被这个坑住：错位后统计出
    「24% 脸部面积差 ≥4 灰阶」，其中绝大部分是对齐误差。

    为什么用条码不用数字
    --------------------
    H.264 会把小字糊掉，但 8 个大色块在每个块内都是平坦的，压缩后仍能按
    「格内均值 >155 / <100」稳定判读。它还能回答「哪一帧被重复或跳过了」。

    位置选右下角：离脸（画面中上）远，不进美颜作用区，也不干扰人眼判断。
    """
    bits = BITS if bits is None else bits
    h, w = frame.shape[:2]
    cw = 18
    y0, y1 = h - 52, h - 10
    x0 = w - bits * cw - 10
    cv2.rectangle(frame, (x0 - 4, y0 - 4), (w - 2, y1 + 4), (0, 0, 0), -1)
    for i in range(bits):
        on = (n >> i) & 1
        xa = x0 + i * cw
        cv2.rectangle(frame, (xa + 1, y0), (xa + cw - 2, y1),
                      (255, 255, 255) if on else (0, 0, 0), -1)
    return frame


def read_code(frame, bits=None):
    """从录到的帧读条码，返回帧号；读不准（灰）返回 -1。"""
    bits = BITS if bits is None else bits
    h, w = frame.shape[:2]
    cw = 18
    y0, y1 = h - 52, h - 10
    x0 = w - bits * cw - 10
    if y0 < 0 or x0 < 0:
        return -1
    v = 0
    for i in range(bits):
        xa = x0 + i * cw
        cell = frame[y0 + 6:y1 - 6, xa + 4:xa + cw - 5]
        if cell.size == 0:
            return -1
        m = float(cell.mean())
        if 100.0 < m < 155.0:
            return -1
        if m >= 155.0:
            v |= (1 << i)
    return v


def scenes_dir():
    appdata = os.environ.get("APPDATA") or os.path.expanduser("~/AppData/Roaming")
    return os.path.join(appdata, "obs-studio", "basic", "scenes")


def build_clip():
    global MARKS
    if not os.path.isfile(SRC):
        sys.exit("源素材不存在：%s" % SRC)
    cap = cv2.VideoCapture(SRC)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if (w, h) != CANVAS:
        sys.exit("素材 %dx%d 与画布 %dx%d 不一致 —— 不一致会被 OBS 缩放/裁切，"
                 "对齐与像素统计都会失真。请让 --canvas 与素材同尺寸。"
                 % (w, h, CANVAS[0], CANVAS[1]))
    MARKS = make_marks(h, w)
    cap.set(cv2.CAP_PROP_POS_FRAMES, F0)

    tmp = DST + ".part.avi"
    wr = cv2.VideoWriter(tmp, cv2.VideoWriter_fourcc(*"FFV1"), FPS, (w, h))
    if not wr.isOpened():
        sys.exit("FFV1 写盘打不开")

    for m in MARKS:
        wr.write(m if m.shape[:2] == (h, w) else cv2.resize(m, (w, h)))
    got = 0
    for i in range(F1 - F0):
        ok, fr = cap.read()
        if not ok:
            sys.exit("源素材在第 %d 帧提前结束" % (F0 + i))
        # 内容帧烧帧号条码；标记帧保持纯色（否则「三连识别」会失效）
        wr.write(stamp_code(fr, got))
        got += 1
    wr.release()
    cap.release()

    if os.path.exists(DST):
        bak = DST + ".old"
        try:
            os.replace(DST, bak)
        except OSError:
            pass
    os.replace(tmp, DST)

    # 写盘后必须回读核对：帧数、尺寸、标记帧是否就在开头、条码是否读得回来
    cap = cv2.VideoCapture(DST)
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    marks = []
    for i in range(len(MARKS)):
        ok, fr = cap.read()
        if not ok:
            sys.exit("回读失败：第 %d 帧" % i)
        marks.append(tuple(int(x) for x in np.array(cv2.mean(fr)[:3]).round()))
    # 挑几个内容帧验条码（0 / 1 / 100 / 239）
    codes = {}
    for n_content in (0, 1, 100, got - 1):
        cap.set(cv2.CAP_PROP_POS_FRAMES, len(MARKS) + n_content)
        ok, fr = cap.read()
        if ok:
            codes[n_content] = read_code(fr)
    cap.release()

    sz = os.path.getsize(DST) / 1e6
    print("素材：%s" % DST)
    print("  帧数 %d（应为 %d = %d 标记 + %d 内容）、%dx%d、%.1f MB、%.2f s/循环"
          % (n, len(MARKS) + got, len(MARKS), got, w, h, sz,
             n / float(FPS)))
    print("  前 %d 帧均值 BGR：%s" % (len(MARKS), marks))

    ok_n = (n == len(MARKS) + got)
    ok_m = (marks[0] == (255, 255, 255) and marks[1] == (0, 0, 0)
            and marks[2][2] > 250 and marks[2][0] < 5
            and marks[3][1] > 250 and marks[3][2] < 5)
    ok_c = (len(codes) == 4 and all(k == v for k, v in codes.items()))
    print("  条码回读：%s（读出的帧号 %s）" % ("OK" if ok_c else "MISMATCH", codes))
    print("  核对：帧数 %s、标记 %s、条码 %s"
          % ("OK" if ok_n else "MISMATCH", "OK" if ok_m else "MISMATCH",
             "OK" if ok_c else "MISMATCH"))
    if not (ok_n and ok_m and ok_c):
        sys.exit(1)


def build_collection():
    src = os.path.join(scenes_dir(), COLL_SRC + ".json")
    dst = os.path.join(scenes_dir(), COLL_DST + ".json")
    if not os.path.isfile(src):
        sys.exit("找不到参考集合：%s" % src)
    d = json.load(open(src, encoding="utf-8"))
    hit = 0
    for s in d.get("sources", []):
        st = s.get("settings") or {}
        for k in ("local_file", "playlist"):
            v = st.get(k)
            if isinstance(v, str) and v.lower().endswith((".mp4", ".avi", ".mkv")):
                st[k] = DST.replace("\\", "/")
                hit += 1
    if not hit:
        sys.exit("参考集合里没有可替换的媒体路径")
    # OBS 按 json 里的 `name` 认集合，不是按文件名。复制过来不改这里，按名字
    # 找不到就会静默落到上一次用的集合上（实测拿到的是 image_source，画面静止、
    # 帧间差 0）—— 这个症状很像「素材没播」，很容易误诊。
    d["name"] = COLL_DST
    # 画布必须跟着素材走（2026-11-04）：模板是 1080×1920 竖屏，换 1280×720 横屏
    # 素材后若不改这里，OBS 会按竖屏画布缩放/裁切 —— §9.40.4 那个「模板残留旧画布」
    # 的坑在别处踩过两次，这里显式写全（resolution / cx,cy / 每项的 scale_ref）。
    d["resolution"] = {"x": CANVAS[0], "y": CANVAS[1]}
    for s in d.get("sources", []):
        if s.get("id") != "scene":
            continue
        st = s.get("settings") or {}
        st["custom_size"] = True
        st["cx"] = float(CANVAS[0])
        st["cy"] = float(CANVAS[1])
        for it in st.get("items", []):
            it["scale_ref"] = {"x": float(CANVAS[0]), "y": float(CANVAS[1])}
            it["scale"] = {"x": 1.0, "y": 1.0}
            it["scale_rel"] = {"x": 1.0, "y": 1.0}
            it["pos"] = {"x": 0.0, "y": 0.0}
            # ⚠ 必须是 (0,0)，不能抄模板的 (-1,-1)：模板画布是 1080x1920，
            # OBS 会把 pos_rel 按画布换算成绝对 pos —— 实测 1280x720 下算成
            # pos.x=280，画面右移 280 px 被裁（左侧黑边、内容只剩 1000 宽），
            # 而录制本身一切正常（帧率时长都对），只有对着画面看才发现。
            # 三组对照实验（pos_rel=0 / bounds 缩放 / pos_rel=1）都能填满。
            it["pos_rel"] = {"x": 0.0, "y": 0.0}
            it["align"] = 5
    if os.path.exists(dst) and not os.path.exists(dst + ".bak"):
        shutil.copy2(dst, dst + ".bak")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(d, fh, ensure_ascii=False, indent=4)
    print("集合：%s（替换 %d 处媒体路径，画布 %dx%d）"
          % (dst, hit, CANVAS[0], CANVAS[1]))


def check():
    if not os.path.isfile(DST):
        sys.exit("素材不存在：%s" % DST)
    cap = cv2.VideoCapture(DST)
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    cap.release()
    print("%s：%d 帧 @%.0ffps %dx%d，%.2f s/循环"
          % (os.path.basename(DST), n, fps, w, h, n / max(1.0, fps)))


def main():
    global SRC, DST, F0, F1, FPS, COLL_SRC, COLL_DST, CANVAS, BITS
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只核对，不重造")
    ap.add_argument("--src", default=SRC, help="源素材（默认 cam_real_1080x1920.mp4）")
    ap.add_argument("--start", type=int, default=F0, help="起始帧")
    ap.add_argument("--end", type=int, default=F1, help="结束帧（不含）")
    ap.add_argument("--fps", type=int, default=FPS, help="输出帧率，须与素材实际帧率一致")
    ap.add_argument("--out", default=DST, help="输出素材路径")
    ap.add_argument("--coll-src", default=COLL_SRC, help="模板集合名")
    ap.add_argument("--coll-dst", default=COLL_DST, help="目标集合名")
    ap.add_argument("--canvas", default="",
                    help="画布 WxH（默认跟随素材；须与素材同尺寸，否则会被缩放/裁切）")
    a = ap.parse_args()

    # 必须 abspath：集合里的 local_file 是**绝对路径**，给相对路径 OBS 会按
    # 自己的工作目录去找，日志只留一行 `MP: Failed to open media`，画面全黑
    # 但录制正常（帧率、时长都对），不查日志很难定位。
    SRC = os.path.abspath(a.src)
    DST = os.path.abspath(a.out)
    F0, F1 = a.start, a.end
    FPS = a.fps
    n_content = max(1, F1 - F0)
    BITS = max(8, int(np.ceil(np.log2(n_content + 1))))
    COLL_SRC = a.coll_src
    COLL_DST = a.coll_dst
    if a.canvas:
        cw, ch = [int(x) for x in a.canvas.lower().split("x")]
        CANVAS = (cw, ch)
    else:
        cap = cv2.VideoCapture(SRC)
        if cap.isOpened():
            CANVAS = (int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
                      int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)))
        cap.release()

    if a.check:
        check()
        return
    print("素材段：%s 帧 [%d, %d)，输出 %d fps，画布 %dx%d，条码 %d 格"
          % (os.path.basename(SRC), F0, F1, FPS, CANVAS[0], CANVAS[1], BITS))
    build_clip()
    build_collection()


if __name__ == "__main__":
    main()
