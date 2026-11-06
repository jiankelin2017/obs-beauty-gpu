#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""3.2.2 删滤镜再加回来：异步检测线程必须能重启。

背景（3.2.2 修掉的缺陷）：宿主销毁滤镜时走 releaseSlot() → stopAsync()，
后者只把 async_.quit 置真并 join，**从不复位**；而引擎对象刻意不析构
（槽复用省掉约 2 秒模型预热），所以「删掉美颜再加回来」时 startAsyncIfNeeded()
重新 spawn 的 worker 第一句 wait 谓词就满足，当场 return —— 该槽的异步检测
永久死亡，掩码不再更新，只有重启 OBS 能恢复。

判据（必须走**异步**路径才覆盖得到，同步路径根本没这个线程）：
  1) 有脸素材连续喂，faces 应变为 >0（worker 活着）；
  2) 调 BeautyCoreProbeSlotCycle() 模拟「删 + 加」；
  3) 换**没有脸**的纯色画面继续喂：
       worker 活着 → 新一轮检测判无脸，掩码被 onDetectFailed 清掉 → faces 归 0；
       worker 死了 → 掩码停在上一轮，faces 永远是 1（正是「效果冻住」的样子）。
  所以第 3 步的 faces 是判别位，不是「顺便测一下」。

跑法：
  cd 06_离线验证 && python _slot_cycle_probe.py
退出码 0 = PASS。
"""
import ctypes
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import offline_regress as R  # noqa: E402

W, H = 1280, 720


def to_raw(png, out):
    im = Image.open(png).convert("RGB").resize((W, H), Image.LANCZOS)
    rgba = np.dstack([np.asarray(im, np.uint8),
                      np.full((H, W, 1), 255, np.uint8)])
    rgba.tofile(out)
    return out


def feed(d, raw, out, frames):
    """连续喂 n 帧，返回每帧的 faces 列表。"""
    msg = ctypes.create_string_buffer(512)
    faces = []
    for _ in range(frames):
        rc = d.BeautyCoreRun(raw.encode("utf-8"), out.encode("utf-8"),
                             W, H, 60.0, 0, 0.0, 0.0, 1, msg, 512)
        if rc != 0:
            sys.exit("BeautyCoreRun 失败 rc=%d : %s"
                     % (rc, msg.value.decode("gbk", "ignore")))
        faces.append(R.face_coverage(msg.value.decode("gbk", "ignore")) or 0)
    return faces


def main():
    # 必须走异步：这个缺陷只存在于 worker 线程的重启路径上。
    # BeautyCoreRun 在没有这个变量时会强制改成 0（保证对拍逐字节稳定）。
    os.environ["BEAUTY_ASYNC_DETECT"] = "1"
    os.environ.pop("BEAUTY_INPUT_ROT", None)
    os.environ.pop("BEAUTY_AUTO_ROT", None)

    face_raw = to_raw(os.path.join(HERE, "..", "05_验证", "assets",
                                   "face_front_720p.png"),
                      os.path.join(HERE, "_slot_face.raw"))
    blank_png = os.path.join(HERE, "_slot_blank.png")
    Image.new("RGB", (W, H), (110, 118, 126)).save(blank_png)
    blank_raw = to_raw(blank_png, os.path.join(HERE, "_slot_blank.raw"))
    out = os.path.join(HERE, "_slot_out.raw")

    d = R.load_dll()
    if not hasattr(d, "BeautyCoreProbeSlotCycle"):
        sys.exit("core_probe.dll 没有 BeautyCoreProbeSlotCycle —— "
                 "先重跑 build_offline.ps1（这是 3.2.2 新增的探针）")
    d.BeautyCoreProbeSlotCycle.argtypes = []
    d.BeautyCoreProbeSlotCycle.restype = ctypes.c_int

    # 1) 先让 worker 跑起来并出一份有效掩码
    a = feed(d, face_raw, out, 60)
    print("阶段1 有脸 x60：末 5 帧 faces=%s" % a[-5:])
    if max(a) == 0:
        sys.exit("[FAIL] 阶段1 就没检出过脸，环境问题（模型缺失？），判据无效")

    # 2) 模拟「删掉滤镜再加回来」
    slot = int(d.BeautyCoreProbeSlotCycle())
    print("阶段2 槽位循环：返回 slot=%d（期望 0）" % slot)
    if slot < 0:
        sys.exit("[FAIL] 槽位 acquire/release 失败，探针不可用")

    # 3) 换无脸画面：判别位
    b = feed(d, blank_raw, out, 60)
    print("阶段3 无脸 x60：前 10 帧 faces=%s" % b[:10])
    print("             末 10 帧 faces=%s" % b[-10:])

    if b[-1] != 0:
        print("\n[FAIL] 槽位循环后换无脸画面，faces 仍为 %d —— "
              "异步检测线程没有重启（掩码停在上一轮，正是 3.2.2 修的那个缺陷）"
              % b[-1])
        return 1
    print("\n结论：PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
