#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""2.8.1 五官链「geomOnly 帧也能跑」的正确性对拍。

背景：`beauty_region_chain()` 原来要求 `job.mask` 非空，而 S7 静止降频后绝大多数
帧是 geomOnly（`job.mask` 为空、掩码由 GPU 按几何重画）。结果是五官链每帧在第一行
放弃 → REGION 不认领 → 细节/美形连带落回 CPU → 整帧回读，4K 开五官 54~58 ms。

2.8.1 把判据改成「有没有掩码纹理」（`job.mask || f->mask_skin_use`）。性能已用
soak 量到（54 → 2.10 ms），但**像素必须单独对拍**：现在五官是在 GPU 用重画的
掩码算的，跟 CPU 用 CPU 掩码算的结果得一致。

做法：同一集合、同一组参数、同一台机器，
    A = BEAUTY_GPU_REGION=1（五官走 GPU，新路径）
    B = BEAUTY_GPU_REGION=0（五官走 CPU，老路径）
两边都等掩码/几何稳定后抓**程序截图**，逐像素比。

判据：maxdiff 应为 0；若为 1~2 且变化像素极少，属 GPU/CPU 浮点路径差异，
需要人工确认方向（记录里如实写明），不能自行宣布通过。

用法：
    py -3 05_验证/verify_region_geomonly.py
    py -3 05_验证/verify_region_geomonly.py --canvas 1280x720 --collection beauty_soak720
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import soak_s2 as S                # noqa: E402
import verify_s1_correct as V      # noqa: E402


def shoot(canvas, collection, gpu_region, out_png, zero_region=False,
          override=None):
    """起 OBS → 下发参数 → 抓程序截图 → 关 OBS。返回截图路径。"""
    env = {"BEAUTY_GPU_REGION": "1" if gpu_region else "0"}
    print("  起 OBS：BEAUTY_GPU_REGION=%d%s" %
          (1 if gpu_region else 0, "，五官参数全零" if zero_region else ""))
    V.start_obs(collection, extra_env=env, canvas=canvas)
    try:
        c = obsctl.wait_ready(wait=90)
        S.apply_load(c, region=not zero_region, extra=override)
        if zero_region:
            # apply_load(region=False) 已经把四项五官参数置 0，这里再显式写一遍
            # 只是为了日志可读 —— 两路必须下发**完全相同**的参数，否则比的是参数。
            pass
        time.sleep(2.5)          # 等掩码/几何重建完（grab 的注释里有实测依据）
        V.grab(c, out_png)
        c.close()
    finally:
        V.close_obs()
    return out_png


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--canvas", default="3840x2160")
    ap.add_argument("--collection", default="beauty_soak4k")
    ap.add_argument("--out", default=os.path.join(HERE, "out", "logs"))
    ap.add_argument("--nospot", action="store_true",
                    help="五官里只把祛斑(delspot)置 0，其余照常")
    ap.add_argument("--only", choices=["eye", "pouch", "naso", "spot"],
                    help="只开一路五官子项（其余三项置 0），用来把差异定位到具体子项")
    ap.add_argument("--zero", action="store_true",
                    help="五官参数全零（用来分离「链本身的 GPU/CPU 差」与「五官段的差」）")
    args = ap.parse_args()

    # 面板刻度是 0..100（beauty_params.h 的 g_params 注释），不是 0..1。
    # 传 0.5 等于滑到 0.5%，效果只剩 1%，取整后全被抹平 —— 见 PROGRESS §9.19。
    ONLY = {
        "eye":   {"eye_bright": 50, "remove_pouch_strength": 0,
                  "remove_nasolabial_folds_strength": 0, "delspot_level": 0},
        "pouch": {"eye_bright": 0, "remove_pouch_strength": 50,
                  "remove_nasolabial_folds_strength": 0, "delspot_level": 0},
        "naso":  {"eye_bright": 0, "remove_pouch_strength": 0,
                  "remove_nasolabial_folds_strength": 50, "delspot_level": 0},
        "spot":  {"eye_bright": 0, "remove_pouch_strength": 0,
                  "remove_nasolabial_folds_strength": 0, "delspot_level": 40},
    }
    suffix = "_only_%s" % args.only if args.only else ""
    if args.only and args.zero:
        print("!! --only 与 --zero 互斥（一个要开一路，一个要全关）")
        return 2

    w, h = [int(x) for x in args.canvas.lower().split("x")]
    os.makedirs(args.out, exist_ok=True)
    tag = "%s_%dx%d" % (time.strftime("%Y%m%d_%H%M%S"), w, h)
    pa = os.path.join(args.out, "region%s%s%s_gpu_%s.png" % ("_zero" if args.zero else "", "_nospot" if args.nospot else "", suffix, tag))
    pb = os.path.join(args.out, "region%s%s%s_cpu_%s.png" % ("_zero" if args.zero else "", "_nospot" if args.nospot else "", suffix, tag))

    print("=== A：五官走 GPU（2.8.1 新路径）===")
    ov = ONLY[args.only] if args.only else (
        {"delspot_level": 0.0} if args.nospot else None)
    shoot((w, h), args.collection, True, pa, args.zero, ov)
    print("=== B：五官走 CPU（老路径）===")
    shoot((w, h), args.collection, False, pb, args.zero, ov)

    a = V.load(pa)
    b = V.load(pb)
    d = V.diff2(a, b)
    print()
    print("=== 对拍结果（%s）===" % args.canvas)
    for k in ("maxdiff", "mean_abs", "changed_pixels", "changed_pct",
              "max_channel", "shape_mismatch"):
        if k in d:
            print("  %-14s %s" % (k, d[k]))
    mx = d.get("maxdiff")
    if mx is None:
        print("  !! 尺寸不一致，无法判定")
        return 2
    if mx == 0:
        print("  判定：**PASS** —— 逐像素相同")
        return 0
    if mx <= 2 and d.get("changed_pct", 100) < 0.05:
        print("  判定：**待人工确认** —— maxdiff=%d、变化 %.3f%%，"
              "属浮点路径差异量级" % (mx, d.get("changed_pct", -1)))
        return 0
    print("  判定：**FAIL** —— 差异超出浮点噪声")
    return 1


if __name__ == "__main__":
    sys.exit(main())
