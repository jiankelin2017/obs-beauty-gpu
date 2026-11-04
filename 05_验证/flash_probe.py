#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""闪脸边界探针：人脸只停留 k 帧时，方案C（`BEAUTY_MOTION_GPU`）还看不看得见。

要回答的问题（不是性能，是漏检）：
    方案C 的运动小图是**异步双缓冲**读回来的 —— 第 t 帧看到的运动其实是 t-1 帧的。
    S7 之后静止场景每 8 帧才提交一次检测，只有运动能把周期拉回每帧。
    若运动信号晚一帧，那么「只停留 1 帧」的人脸就永远赶不上提交那一帧。

怎么量（只用现成埋点，不加任何代码）：
    素材 = `make_flash_assets.py` 造的 flash_k*_720p.avi：每 60 帧出现 k 帧。

    读 `性能 … 有脸 X% 掩码 Y%`。这两个数要小心解读：`kMaskHold = 6 次检测`
    （`beauty_core.cpp:3661`，**不是 6 帧**）—— 一次捕获之后，掩码会一直被 hold
    到「6 次失败检测」为止，静止节奏下 ≈ 48 帧。所以中位数没有分辨力，
    **要看逐窗口序列**：一个 180 帧窗口 = 3 次闪脸，窗口内「有脸% 高 / 低」基本
    就是「这 3 次里捕到过 / 一次也没捕到」。能比的是同一 k 下 ON 与 OFF 两条臂：
    同素材、同参数、同会话、交替跑。

用法：
    py -3 05_验证/flash_probe.py                       # k=1,2,4,8 × 两臂，各 25 s
    py -3 05_验证/flash_probe.py --ks 1 --seconds 20
    py -3 05_验证/flash_probe.py --ks 1 --arms on      # 只跑一臂（排障用）
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import verify_s1_correct as V   # noqa: E402  起停 OBS / 日志发现 / 画布同步
import soak_s2 as S             # noqa: E402  LOAD 参数 / normalize_item / keep_awake
import obsctl                    # noqa: E402
import phase0_parse_log as P     # noqa: E402  性能行解析器

CANVAS = (1280, 720)
DPER_RE = re.compile(r"dper=(\d+)\s+dmot=(-?\d+)")
NOPIX_RE = re.compile(r"方案C 窗口 (\d+)x(\d+)：nopix=(\d+)/(\d+)")
# 2.11.1：窗口行多了「判动=hit/calls 提交=N」。老 dll 没有这两段 → 全 None。
MHOT_RE = re.compile(r"判动=(\d+)/(\d+)")
SUB_RE = re.compile(r"提交=(\d+)")


def run_one(coll, seconds, extra_env):
    """跑一轮，返回日志路径。流程与 s0_lock.one_run 一致（少打一轮汇总）。"""
    if not V.close_obs():
        return None
    env = {"BEAUTY_PROFILE": "1"}
    if extra_env:
        env.update(extra_env)
    proc, snap = V.start_obs(coll, extra_env=env, canvas=CANVAS)
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(2.5)
        for where in (V.SRC, "Main"):
            try:
                for f in c.filters(where):
                    nm = f.get("filterName")
                    if nm != V.FILT:
                        c.req("RemoveFilter",
                              {"sourceName": where, "filterName": nm})
            except Exception:
                pass
        S.normalize_item(c)
        if not S.apply_load(c):
            print("  !! 参数下发失败，本轮数据不可用")
        time.sleep(seconds)
    finally:
        V.close_obs()
    return V.newest_log(snap)


def parse(log):
    """返回 (perf_rows, dper, dmot, nopix, mhot, sub)。"""
    perf, _ = P.parse_file(log)
    dper, dmot, nopix, mhot, sub = [], [], [], [], []
    with open(log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = DPER_RE.search(line)
            if m:
                dper.append(int(m.group(1)))
                dmot.append(int(m.group(2)))
            m = NOPIX_RE.search(line)
            if m:
                nopix.append((int(m.group(3)), int(m.group(4))))
                h = MHOT_RE.search(line)
                if h:
                    mhot.append((int(h.group(1)), int(h.group(2))))
                b = SUB_RE.search(line)
                if b:
                    sub.append(int(b.group(1)))
    return perf, dper, dmot, nopix, mhot, sub


def med(vals):
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    n = len(vals)
    return vals[n // 2] if n % 2 else 0.5 * (vals[n // 2 - 1] + vals[n // 2])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ks", type=int, nargs="*", default=[1, 2, 4, 8])
    ap.add_argument("--seconds", type=float, default=25.0)
    ap.add_argument("--arms", nargs="*", default=["off", "on"],
                    choices=("off", "on"))
    ap.add_argument("--json", metavar="PATH")
    ap.add_argument("--family", default="flash", choices=("flash", "drop"),
                    help="flash=空场里闪出人脸；drop=脸持续在场、每 60 帧消失 k 帧")
    ap.add_argument("--coll", metavar="NAME",
                    help="直接指定集合名（real 族等非 k 编号的集合用这个）；"
                         "给了它就不再按 --ks 生成")
    ap.add_argument("--canvas", metavar="WxH",
                    help="画布尺寸，默认 1280x720；real 族是 1080x1920")
    args = ap.parse_args(argv)

    global CANVAS
    if args.canvas:
        w, h = args.canvas.lower().split("x")
        CANVAS = (int(w), int(h))

    if V.obs_running():
        print("OBS 正在运行，先关闭")
        if not V.close_obs():
            sys.exit("关不掉 OBS")

    arms = {"off": {}, "on": {"BEAUTY_MOTION_GPU": "1"}}
    S.keep_awake(True)
    rows, out = [], []
    try:
        targets = ([(None, args.coll)] if args.coll else
                   [(k, "beauty_%s%d_720" % (args.family, k)) for k in args.ks])
        for k, coll in targets:
            for arm in args.arms:
                print("\n==== k=%s / 臂=%s（%s, %ss）===="
                      % ("-" if k is None else k, arm, coll, args.seconds))
                log = run_one(coll, args.seconds, arms[arm])
                if not log:
                    print("  !! 采集失败")
                    continue
                perf, dper, dmot, nopix, mhot, sub = parse(log)
                faces = med([r["faces"] for r in perf])
                cover = med([r["cover"] for r in perf])
                fs = [r["faces"] for r in perf]
                rec = {
                    "k": k, "arm": arm, "log": log, "windows": len(perf),
                    "faces_med": faces, "cover_med": cover,
                    "faces_series": fs,
                    "cold_windows": sum(1 for v in fs if v is not None and v < 5),
                    "dper_max": max(dper) if dper else None,
                    "dper_min": min(dper) if dper else None,
                    "dmot_max": max(dmot) if dmot else None,
                    "nopix": (round(sum(a for a, _ in nopix) /
                                    max(1, sum(b for _, b in nopix)), 3)
                              if nopix else None),
                }
                hot = sum(a for a, _ in mhot)
                calls = sum(b for _, b in mhot)
                rec["hot"] = hot
                rec["calls"] = calls
                rec["submits"] = sum(sub) if sub else None
                out.append(rec)
                rows.append(rec)
                print("  窗口 %d | 有脸逐窗口 %s | 掩码中位 %s%% | "
                      "dper %s..%s | dmot max %s | nopix %s | 判动 %s/%s | 提交 %s"
                      % (rec["windows"],
                         " ".join("-" if v is None else "%.0f" % v for v in fs),
                         "-" if cover is None else "%.2f" % cover,
                         rec["dper_min"], rec["dper_max"], rec["dmot_max"],
                         "-" if rec["nopix"] is None else "%.1f%%"
                         % (rec["nopix"] * 100),
                         hot, calls,
                         "-" if not sub else sum(sub)))
    finally:
        S.keep_awake(False)

    print("\n================ 汇总（同一 k 下两臂对比）================")
    print("k(帧) | 臂  | 有脸%中位 | 冷窗口 | 有脸%逐窗口")
    print("------+-----+----------+--------+--------------------------")
    by_k = {}
    for r in rows:
        by_k.setdefault(r["k"], {})[r["arm"]] = r
    for k in sorted(by_k):
        for arm in args.arms:
            r = by_k[k].get(arm)
            if not r:
                continue
            print("%5s | %-3s | %8s | %5d/%d | %s"
                  % ("-" if k is None else k, arm,
                     "-" if r["faces_med"] is None else "%.1f" % r["faces_med"],
                     r["cold_windows"], r["windows"],
                     " ".join("-" if v is None else "%.0f" % v
                              for v in r["faces_series"])))
    print("\n「冷窗口」= 有脸% < 5 的窗口数。一个窗口 180 帧 ≈ 3 次闪脸，"
          "\n所以冷窗口 ≈ 这 3 次一次都没捕到。两臂相减就是方案C 丢掉的窗口数。")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, ensure_ascii=False, indent=2)
        print("\nJSON: %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
