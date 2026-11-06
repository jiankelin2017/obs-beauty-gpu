#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""3.2.0 自动输入方向：离线判据。

做法：拿一张「正立脸被转 90°」的素材（720x1280，脸横躺），input_rot 保持 0，
连续喂同一帧。判据三条：
  1) 探测前：faces=0、effRot=0（复现「竖屏美颜没效果」那条链）；
  2) 某帧之后：effRot 变成 90 或 270（自动认定的方向）；
  3) 认定之后：faces=1（检出脸 = 掩码有内容 = 美颜真的会生效）。

反向对照：同一素材显式 BEAUTY_INPUT_ROT=90，effRot 应恒为 90 且 faces=1；
正立素材（face_front_720p.png）应始终 effRot=0、faces=1（不误判）。
"""
import ctypes
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import offline_regress as R  # noqa: E402


def to_raw(png, w, h, out):
    im = Image.open(png).convert("RGB").resize((w, h), Image.LANCZOS)
    rgba = np.dstack([np.asarray(im, np.uint8),
                      np.full((h, w, 1), 255, np.uint8)])
    rgba.tofile(out)
    return out


def run(png, w, h, frames, label):
    raw = to_raw(png, w, h, os.path.join(HERE, "_autorot_in.raw"))
    out = os.path.join(HERE, "_autorot_out.raw")
    d = R.load_dll()
    d.BeautyCoreGetEffectiveRot.argtypes = []
    d.BeautyCoreGetEffectiveRot.restype = ctypes.c_int

    msg = ctypes.create_string_buffer(512)
    first_rot = None
    first_face_after = None
    hist = []
    for i in range(frames):
        rc = d.BeautyCoreRun(raw.encode("utf-8"), out.encode("utf-8"),
                             w, h, 60.0, 0, 0.0, 0.0, 1, msg, 512)
        if rc != 0:
            sys.exit("BeautyCoreRun 失败 rc=%d : %s"
                     % (rc, msg.value.decode("gbk", "ignore")))
        txt = msg.value.decode("gbk", "ignore")
        rot = int(d.BeautyCoreGetEffectiveRot())
        faces = R.face_coverage(txt) or 0
        hist.append((i, faces, rot))
        if rot != 0 and first_rot is None:
            first_rot = (i, rot)
        if first_rot is not None and faces > 0 and first_face_after is None:
            first_face_after = i

    print("\n==== %s（%dx%d，%d 帧）" % (label, w, h, frames))
    print("  前 3 帧: %s" % (hist[:3],))
    tail = [t for t in hist if t[0] >= frames - 3]
    print("  末 3 帧: %s" % (tail,))
    print("  首次认定方向: %s" % (first_rot,))
    print("  认定后首次检出脸: %s" % (first_face_after,))
    return hist, first_rot, first_face_after


def main():
    A = os.path.join(HERE, "..", "05_验证", "assets")
    # 1) 躺脸 + 无声明方向 → 期望自动命中
    h1, r1, f1 = run(os.path.join(A, "face_front_720p_r90.png"),
                     720, 1280, 120, "躺脸 720x1280，input_rot=0（期望自动命中）")
    # 2) 正立脸 + 无声明方向 → 期望不误判
    h2, r2, f2 = run(os.path.join(A, "face_front_720p.png"),
                     1280, 720, 40, "正立脸 1280x720，input_rot=0（期望不误判）")
    # 3) 画面里没人 → 期望一直不认定（探测失败不得乱锁方向）
    blank = os.path.join(HERE, "_autorot_blank.png")
    Image.new("RGB", (720, 1280), (110, 118, 126)).save(blank)
    h3, r3, f3 = run(blank, 720, 1280, 120, "纯色空画面，input_rot=0（期望不误判）")

    ok = True
    if r1 is None:
        print("\n[FAIL] 躺脸素材 120 帧内没有触发自动方向认定")
        ok = False
    elif f1 is None:
        print("\n[FAIL] 方向已认定但之后仍未检出脸")
        ok = False
    if r2 is not None:
        print("\n[FAIL] 正立素材被误判成 %d°" % r2)
        ok = False
    if r3 is not None:
        print("\n[FAIL] 空画面被误判成 %d°（不存在的脸不该锁死方向）" % r3)
        ok = False
    print("\n结论：%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
