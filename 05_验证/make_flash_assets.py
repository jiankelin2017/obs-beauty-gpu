#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""「人脸快速进出」专项素材：让人脸只在画面里停留 k 帧就消失。

为什么需要它：
    方案C（2.11.0）把运动判定挪到了 GPU 上的一张 64×64 小图上，而那张小图是
    **异步双缓冲**读回来的 —— 引擎在第 t 帧看到的运动，其实是第 t-1 帧的画面。
    于是有个必须量清楚的边界：**人脸只停留 1 帧时，方案C 还看得见吗？**

    这不是性能问题，是「会不会漏检」的问题。S7 之后静止场景每 8 帧才提交一次检测，
    只有「运动」能把它拉回每帧；如果运动信号晚一帧到，那么 1 帧的闪脸就永远
    赶不上提交那一帧 —— 提交时脸已经走了。

素材构造（刻意做成最容易被检到的形态）：
    底图 = face_front_720p.png 整幅（正脸，脸框 348×481）
    空场 = 用左侧纯背景列**横向拉伸成整幅**渐变（YuNet 必须检出 0 张）
    闪帧 = 每 CYCLE 帧里，第 OFF 帧起、连续 k 帧贴回**原图整幅**
    → 硬边 pop-in，是全库最强的检出信号。**结论因此是保守的**：
      连这种信号都追不上，真实场景（脸逐渐走进画面）更追不上。

为什么空场要「整幅渐变」而不是「盖掉人物那条竖条」：
    这张素材是拼贴出来的（1024 宽的人物贴在 1280×720 上），人物条的实际边界
    比肉眼看到的宽 —— 第一版按 x∈[250,830] 盖，右侧仍残留半张脸，YuNet 在空场
    上照样检出 1 张，整个实验就变成「两张脸」而不是「有脸 / 无脸」。
    改成整幅渐变后空场必然 0 张，没有边界要猜。

为什么不留逐帧帧号（make_motion_assets.py 是留的）：
    帧号每帧都变 = 全画面每帧都有运动 → dper 恒为 0，正好把要测的东西掩掉。
    真值改由 JSON 记录，并用「解码后实测」再核一遍（见 measure()）。

用法：
    py -3 05_验证/make_flash_assets.py                 # k=1,2,4,8 全生成
    py -3 05_验证/make_flash_assets.py --ks 1 2         # 只做两档
    py -3 05_验证/make_flash_assets.py --codec mjpg     # 换编码器
"""
import argparse
import json
import os
import shutil
import sys
import tempfile

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import count_faces as CF   # noqa: E402  复用与插件对齐的 YuNet 判据

ASSETS = os.path.join(HERE, "assets")
SRC_NAME = "face_front_720p.png"

FPS = 30
CYCLE = 60        # 2 s 一个周期
FRAMES = 600      # 20 s（10 个周期）
OFF = 20          # 周期内第几帧开始闪（避开 0，让画面先以空场起步）
KS = (1, 2, 4, 8)

# 空场只用左侧这段纯背景列横向拉伸成整幅。人物条的真实边界不好猜（见模块说明），
# 整幅渐变最省事，且保证「没有任何脸的结构」。
BG_COLS = (20, 120)
MEAS = (0, 0, 1280, 720)       # 实测「有没有脸」的判据区域（整幅）


def imread_uni(path):
    """cv2.imread 不认中文路径（Windows 下按 ANSI 打开）—— 走 imdecode。"""
    with open(path, "rb") as fh:
        buf = fh.read()
    arr = cv2.imdecode(np.frombuffer(buf, dtype=np.uint8), cv2.IMREAD_COLOR)
    if arr is None:
        raise SystemExit("读不出素材：%s" % path)
    return arr


def build_plate(base):
    """空场：左侧纯背景列横向拉伸成整幅渐变。"""
    sh, sw = base.shape[:2]
    cols = base[:, BG_COLS[0]:BG_COLS[1], :]
    return cv2.resize(cols, (sw, sh), interpolation=cv2.INTER_LINEAR)


def write_video(path, frames_iter, wh, codec):
    """VideoWriter 不认中文路径：先写系统临时目录（ASCII），再搬回来。"""
    tmp = os.path.join(tempfile.gettempdir(), "flash_tmp." + (
        "avi" if codec == "mjpg" else "mp4"))
    fourcc = cv2.VideoWriter_fourcc(*("MJPG" if codec == "mjpg" else "mp4v"))
    vw = cv2.VideoWriter(tmp, fourcc, FPS, wh)
    if not vw.isOpened():
        raise SystemExit("VideoWriter 打不开（缺 %s 编码器）" % codec)
    for fr in frames_iter:
        vw.write(fr)
    vw.release()
    try:
        os.replace(tmp, path)
    except OSError:
        shutil.copy2(tmp, path)
        try:
            os.remove(tmp)
        except OSError:
            pass


def measure(path, plate):
    """解码后实测「每帧到底有没有脸」。返回 (有效帧数列表, 每次闪现的时长列表)。

    判据用**判据区域的平均绝对差**：闪脸时整幅换掉，差值是几十级；
    压缩噪声只有个位数。阈值 3.0 灰阶。
    """
    x0, y0, x1, y1 = MEAS
    cap = cv2.VideoCapture(path)
    roi_plate = plate[y0:y1, x0:x1, :].astype(np.int16)
    present = []
    n = 0
    while True:
        ok, fr = cap.read()
        if not ok:
            break
        n += 1
        d = np.abs(fr[y0:y1, x0:x1, :].astype(np.int16) - roi_plate).mean()
        present.append(1 if d > 3.0 else 0)
    cap.release()
    # 连续 1 的段长 = 每次闪现的实际时长
    runs, cur = [], 0
    for v in present:
        if v:
            cur += 1
        elif cur:
            runs.append(cur)
            cur = 0
    if cur:
        runs.append(cur)
    return n, present, runs


def verify_detect(path, present, det):
    """逐帧过一遍插件同款 YuNet：闪帧必须检到 1 张，空场必须 0 张。

    为什么非做不可：mjpg 是逐帧独立压缩，但量化仍会把硬边啃掉一点；如果 1 帧的
    闪脸被压到检不出，那实验就变成「测压缩损伤」而不是「测方案C 的时序」。
    """
    cap = cv2.VideoCapture(path)
    hit_on = miss_on = hit_off = miss_off = 0
    i = 0
    while True:
        ok, fr = cap.read()
        if not ok:
            break
        if i < len(present):
            n = CF.count_one(det, fr)
            if present[i]:
                if n >= 1:
                    hit_on += 1
                else:
                    miss_on += 1
            else:
                if n == 0:
                    hit_off += 1
                else:
                    miss_off += 1
        i += 1
    cap.release()
    return hit_on, miss_on, hit_off, miss_off


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ks", type=int, nargs="*", default=list(KS))
    ap.add_argument("--codec", choices=("mjpg", "mp4v"), default="mjpg",
                    help="mjpg = 逐帧独立压缩，不会把 1 帧的闪光糊到邻居帧")
    args = ap.parse_args(argv)

    src = imread_uni(os.path.join(ASSETS, SRC_NAME))
    sh, sw = src.shape[:2]
    plate = build_plate(src)

    det = CF.make_detector(CF.find_model())
    n_plate = CF.count_one(det, plate)
    n_face = CF.count_one(det, src)
    print("素材 %s  %dx%d" % (SRC_NAME, sw, sh))
    print("空场检出 %d 张脸（必须 0）；原图检出 %d 张（必须 ≥1）"
          % (n_plate, n_face))
    if n_plate != 0 or n_face < 1:
        raise SystemExit("素材不合格：空场还有脸，或原图检不到脸")

    # 人眼核对用：空场 / 闪帧 / 下一帧 三联图
    x0, y0, x1, y1 = MEAS
    sheet = np.concatenate([plate, src, plate], axis=0)
    sheet = cv2.resize(sheet, (sw // 3, sh))
    sheet_path = os.path.join(ASSETS, "flash_probe_sheet.png")
    cv2.imwrite(os.path.join(tempfile.gettempdir(), "sheet.png"),
                sheet)
    shutil.copy2(os.path.join(tempfile.gettempdir(), "sheet.png"), sheet_path)
    print("三联图（空场 / 闪帧 / 空场）: %s" % sheet_path)

    for k in args.ks:
        name = "flash_k%d_720p" % k
        path = os.path.join(ASSETS, name + (".avi" if args.codec == "mjpg" else ".mp4"))

        def gen(k=k):
            for t in range(FRAMES):
                ph = t % CYCLE
                yield src if OFF <= ph < OFF + k else plate

        write_video(path, gen(), (sw, sh), args.codec)
        n, present, runs = measure(path, plate)
        ho, mo, hf, mf = verify_detect(path, present, det)
        exp = len(range(0, FRAMES, CYCLE)) * k
        meta = {
            "tag": name, "file": os.path.basename(path), "wh": [sw, sh],
            "fps": FPS, "frames": n, "cycle": CYCLE, "k": k, "off": OFF,
            "meas": list(MEAS), "codec": args.codec,
            "expect_on_frames": exp,
            "measured_on_frames": int(sum(present)),
            "measured_runs": runs,
            "yunet_on_hit": ho, "yunet_on_miss": mo,
            "yunet_off_hit": hf, "yunet_off_miss": mf,
            "note": "空场=整幅背景渐变；闪帧=原图整幅。硬边 pop-in，"
                    "是最强检出信号，结论对方案C保守。",
        }
        with open(os.path.join(ASSETS, name + ".json"), "w",
                  encoding="utf-8") as fh:
            json.dump(meta, fh, ensure_ascii=False, indent=1)
        bad = "" if sum(present) == exp else "  ← 与预期不符！"
        print("写出 %-22s %d 帧  %6.1f KB  闪 %d 次 实测时长 %s（预期每段 %d）%s"
              % (os.path.basename(path), n, os.path.getsize(path) / 1024.0,
                 len(runs), sorted(set(runs)), k, bad))
        print("        YuNet 核对：闪帧检出 %d/%d，空场干净 %d/%d%s"
              % (ho, ho + mo, hf, hf + mf,
                 "" if (mo == 0 and mf == 0) else "  ← 有误判，素材不可用！"))
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
