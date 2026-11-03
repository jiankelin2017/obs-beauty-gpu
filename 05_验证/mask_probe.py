#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""量「有脸率 / 掩码 / 往返」：同一套参数下，不同素材的检出与耗时对照。

为什么单独测这一项：素材能不能被插件认出来，决定了长跑有没有意义——认不出就是
「跑了 30 分钟但一直走无脸直通」。耗时类结论与素材朝向无关（磨皮走全帧、draw 数
固定），但检出与掩码相关指标强依赖素材。

⚠ 本构建的 `性能` 行里「掩码」字段恒为 0.00%（侧脸素材同样为 0），**不能用它比较
覆盖率**；判素材是否被认出，看「有脸」与 `faces` 取值。

用法（OBS 会自动起停）：
    py -3 05_验证/mask_probe.py beauty_soak4k 3840x2160
    py -3 05_验证/mask_probe.py beauty_front4k 3840x2160
    py -3 05_验证/mask_probe.py beauty_front720 1280x720

输出：末窗 `性能` 行的「掩码 / 有脸」两项，以及往返耗时。
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import obsctl                    # noqa: E402
import verify_s1_correct as V    # noqa: E402

BLUR = 80.0     # 磨皮强度：必须 >0 才会走掩码与磨皮段


def last_perf(log_path):
    """取最后一条 性能 行里的掩码/有脸/往返。"""
    mask = face = rt = None
    with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        for ln in f:
            if "性能 " not in ln or "ms/帧" not in ln:
                continue
            m = re.search(r"掩码\s+([\d.]+)%", ln)
            if m:
                mask = float(m.group(1))
            m = re.search(r"有脸\s+([\d.]+)%", ln)
            if m:
                face = float(m.group(1))
            m = re.search(r"=\s*([\d.]+)\s*ms/帧", ln)
            if m:
                rt = float(m.group(1))
    return mask, face, rt


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    coll = argv[1]
    canvas = None
    if len(argv) > 2 and "x" in argv[2]:
        w, h = argv[2].lower().split("x")
        canvas = (int(w), int(h))
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    proc, snap = V.start_obs(coll, extra_env={"BEAUTY_PROFILE": "1",
                                             "BEAUTY_GPU_FRAME_RING": "1"},
                             canvas=canvas)
    c = None
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(6)
        V.set_config(c, {"blur_level": BLUR})
        # 性能行是**每 180 帧**打一条（30fps 下约 6s），等太短会一条都抓不到；
        # 掩码重建本身还要 1.3s 以上。这里给 10s。
        time.sleep(10.0)
        log = V.newest_log(snap)
        mask, face, rt = last_perf(log)
        print("%-18s %-10s 掩码=%s%%  有脸=%s%%  往返=%s ms"
              % (coll, "%dx%d" % canvas if canvas else "默认", mask, face, rt))
        V.set_config(c, None)
        c.close()
        c = None
    finally:
        if c is not None:
            try:
                c.close()
            except Exception:
                pass
        V.close_obs()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
