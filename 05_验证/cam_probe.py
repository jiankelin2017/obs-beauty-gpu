#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""摄像头劣化输入观测（附加项，不进 gate.py 判据）。

为什么单独一支：`soak_s2.py` 里的源名/滤镜名是静态集合那一套（`still` / `Beauty`），
拿它去驱动摄像头集合 `beauty_s1` 会**静默失败** —— 参数没下发、`blur=0`、
`ALLZERO_SKIP` 生效、全程 `有脸 0%`、往返 0.00。2026-11-03 19:15 那一轮就是这么废掉的
（日志 `self-check ... blur=0`、`性能 ... 有脸 0%` 全 56 窗）。所以这里显式指定名字。

观测什么：低照度 + 720p 内置头这种劣化输入下，
    · 人脸检出会不会抖（`有脸%` 在窗间跳 1↔0）
    · 掩码覆盖率会不会乱跳（`掩码%`）
    · 噪点会不会被误判成运动（`dper` 掉到 0、`dmot>0`）

判据：**这一支不判 PASS/FAIL**。它只产出观察值 —— 劣化输入的结论只能作附加参考，
不能进基线（同一段画面两次运行本来就不一样，见 PROGRESS 里对「为什么对拍必须用静态图」的说明）。

用法：
    py -3 05_验证/cam_probe.py --minutes 3
    py -3 05_验证/cam_probe.py --source 视频采集设备 --filter OBS美颜 --minutes 5
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import verify_s1_correct as V      # noqa: E402

PARAMS = {
    "blur_level": 60, "clarity": 40, "color_level": 40, "red_level": 30,
    "sharpen": 30,
    # 五官参数保持 0：这一支测的是「基础链在噪声下的稳定性」，
    # 五官是 opt-in 且当前像素未收敛（P0-7），混进来会让结论含糊。
}


def numeric(line, pat, default=None):
    m = re.search(pat, line)
    return float(m.group(1)) if m else default


def parse_perf(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if "性能 1280x720" not in ln and "性能 3840x2160" not in ln:
                continue
            rows.append({
                "flower": numeric(ln, r"有脸 (\d+)%"),
                "mask": numeric(ln, r"掩码 ([\d.]+)%"),
                "dper": numeric(ln, r"dper=(\d+)"),
                "dmot": numeric(ln, r"dmot=(\d+)"),
                "gskip": numeric(ln, r"gskip=(\d+)"),
                "tskip": numeric(ln, r"tskip=(\d+)"),
                "tot": numeric(ln, r"= ([\d.]+) ms/帧"),
            })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default="beauty_s1")
    ap.add_argument("--canvas", default="1280x720")
    ap.add_argument("--source", default="视频采集设备")
    ap.add_argument("--filter", default="OBS美颜")
    ap.add_argument("--minutes", type=float, default=3.0)
    args = ap.parse_args()

    w, h = [int(x) for x in args.canvas.lower().split("x")]
    # BEAUTY_PROFILE 必须开：性能行（`性能 1280x720：…`）没有它就不打印，
    # 解析器会得到 0 窗。soak_s2 是通过 env_for() 传的，这里漏过一次（19:22 那轮）。
    proc, snap = V.start_obs(args.collection,
                             extra_env={"BEAUTY_PROFILE": "1"},
                             canvas=(w, h))
    try:
        c = obsctl.wait_ready(wait=90)
        st = dict(PARAMS)
        st["enabled"] = True
        # 刚连上时 OBS 常回 207（not ready）—— soak_s2.apply_load 为此重试 15 次，
        # 这里照抄。19:25 那轮就是没重试直接抛的。
        last = None
        for _ in range(15):
            try:
                c.set_filter_settings(args.source, args.filter, st, overlay=True)
                c.set_filter_enabled(args.source, args.filter, True)
                last = None
                break
            except Exception as e:      # noqa: BLE001
                last = e
                time.sleep(2.0)
        if last is not None:
            print("!! 参数下发失败：%s" % last)
            return 2
        time.sleep(0.8)
        got = c.filter_settings(args.source, args.filter)
        print("已下发 blur_level=%s color_level=%s（读回确认）" %
              (got.get("blur_level"), got.get("color_level")))
        if not got.get("blur_level"):
            print("!! 读回的 blur_level 是 0 —— 参数没生效，这一轮无效")
            return 2
        t0 = time.time()
        end = t0 + args.minutes * 60.0
        while time.time() < end:
            time.sleep(15)
            print("  [%5.1f%%] 存活" % (100.0 * (time.time() - t0) / (args.minutes * 60.0)))
        c.close()
    finally:
        V.close_obs()

    log = V.newest_log(snap)
    rows = parse_perf(log)
    print()
    print("=== 摄像头劣化观测（%s，%d 窗）===" % (os.path.basename(log), len(rows)))
    if not rows:
        print("!! 没有解析到性能行")
        return 1
    fl = [r["flower"] for r in rows]
    mk = [r["mask"] for r in rows]
    dp = [r["dper"] for r in rows]
    dm = [r["dmot"] for r in rows]
    print("  有脸%%   最小 %.0f  最大 %.0f  平均 %.1f  （<100 的窗 %d/%d）"
          % (min(fl), max(fl), sum(fl) / len(fl),
             sum(1 for v in fl if v < 100), len(fl)))
    print("  掩码%%   最小 %.2f  最大 %.2f  平均 %.2f"
          % (min(mk), max(mk), sum(mk) / len(mk)))
    print("  dper    取值 %s（0 = 每帧检测，7 = 每 8 帧）" % sorted(set(dp)))
    print("  dmot    取值 %s（>0 的窗 %d 个）"
          % (sorted(set(dm)), sum(1 for v in dm if v and v > 0)))
    print("  gskip/tskip 非零窗 %d / %d"
          % (sum(1 for r in rows if r["gskip"]), sum(1 for r in rows if r["tskip"])))
    print("  往返    最小 %.2f 最大 %.2f 中位 %.2f"
          % (min(r["tot"] for r in rows), max(r["tot"] for r in rows),
             sorted(r["tot"] for r in rows)[len(rows) // 2]))
    print()
    print("注：本支不判 PASS/FAIL，也不进基线 —— 劣化输入只作附加参考。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
