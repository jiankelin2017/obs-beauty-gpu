#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S0 两遍固化：同一二进制、同一素材、同一负载连跑 N 轮，产出可供 gate.py
比对的一批日志路径。

为什么需要这个脚本：
  gate.py 只会「算」，不会「采」。而 S0 的可重复性门槛（同一二进制连跑两遍，
  往返中位数差 ≤ 0.15 ms）要求两轮的**采集条件完全相同** —— 换集合、换画布、
  换参数、换滤镜数量，任何一项不同都会把 0.15 ms 的门槛淹掉。
  手工敲这些步骤（同步画布 → 起 OBS → 清滤镜 → 归一位置 → 下发参数 →
  等够窗口 → 干净退出 → 还原画布）每轮十几步，第二轮必然走样。

采什么：
  720p : beauty_soak720 / 1280x720 / face_720p_single.png
  4K   : beauty_soak4k  / 3840x2160 / face_4k_single.png
  负载 : soak_s2.py 的 LOAD（磨皮 60 / 清晰 40 / 美白 40 / 红润 30 / 锐化 30
         + 瘦脸 0.6 / 大眼 0.6 / 下巴 0.4），不带五官参数 —— 见 soak_s2.py
         里 LOAD 上方的说明（region_blocks 会把细节/美形拖回 CPU）。
  开关 : 只设 BEAUTY_PROFILE=1，**不设** BEAUTY_GPU_FRAME_RING —— 测的是
         出货默认值（S2 起默认开）。

用法:
    py -3 05_验证/s0_lock.py --rounds 2 --seconds 30
    # 打印每轮的日志路径（JSON），再：
    py -3 05_验证/gate.py <日志1> <日志2> --save 05_验证/S0_baseline.json
    py -3 05_验证/gate.py <日志3> <日志4> --cmp  05_验证/S0_baseline.json
"""
import argparse
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import verify_s1_correct as V   # 画布同步 / 起停 OBS / 日志发现
import soak_s2 as S             # LOAD 负载 / apply_load / normalize_item / keep_awake
import obsctl

TARGETS = [
    ("720p", "beauty_soak720", (1280, 720)),
    ("4K", S.COLLECTION_4K, S.CANVAS_4K),
]


def one_run(coll, canvas, seconds, extra_env=None):
    """跑一轮，返回 (日志路径, 备注)。调用前 OBS 必须已关闭。"""
    if not V.close_obs():
        return None, "关不掉上一轮 OBS"
    env = {"BEAUTY_PROFILE": "1"}
    if extra_env:
        env.update(extra_env)
    proc, snap = V.start_obs(coll, extra_env=env, canvas=canvas)
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(2.5)
        # 清掉场景/源上不属于本滤镜的滤镜（借骨架生成集合时会继承）
        for where in (V.SRC, "Main"):
            try:
                for f in c.filters(where):
                    nm = f.get("filterName")
                    if nm != V.FILT:
                        c.req("RemoveFilter",
                              {"sourceName": where, "filterName": nm})
                        print("  已移除多余滤镜: %s.%s" % (where, nm))
            except Exception as e:
                print("  !! 清理 %s 滤镜失败: %s" % (where, e))
        S.normalize_item(c)
        if not S.apply_load(c):
            print("  !! 参数下发失败，本轮数据不可用")
        time.sleep(seconds)
    finally:
        V.close_obs()
    log = V.newest_log(snap)
    return log, ""


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="每次运行的采集时长（30fps 下 30s ≈ 5 个 180 帧窗口）")
    ap.add_argument("--only", choices=("720p", "4K"), help="只跑一档")
    ap.add_argument("--env", action="append", metavar="K=V",
                    help="附加环境变量，可多次（如 BEAUTY_RING_ALWAYS=1）")
    args = ap.parse_args(argv)

    if V.obs_running():
        print("OBS 正在运行，先关闭（脚本会自己发 WM_CLOSE）")
        if not V.close_obs():
            sys.exit("关不掉 OBS")

    envx = dict(kv.split("=", 1) for kv in (args.env or []))
    S.keep_awake(True)
    out = {}
    for r in range(1, args.rounds + 1):
        for name, coll, canvas in TARGETS:
            if args.only and args.only != name:
                continue
            print("\n==== 第 %d 轮 / %s（%s, %dx%d, %ss）====" %
                  (r, name, coll, canvas[0], canvas[1], args.seconds))
            log, note = one_run(coll, canvas, args.seconds, envx)
            if not log:
                print("  !! %s" % note)
            else:
                print("  日志: %s" % log)
            out.setdefault(name, []).append(log)
    S.keep_awake(False)

    print("\n---- 汇总（喂给 gate.py）----")
    print(json.dumps(out, ensure_ascii=False, indent=2))
    # 直接给出两条可复制的命令，省得手工拼路径
    r1 = [p for k in ("720p", "4K") for p in (out.get(k) or [])[:1] if p]
    r2 = [p for k in ("720p", "4K") for p in (out.get(k) or [])[1:2] if p]
    base = os.path.join(HERE, "S0_baseline.json")
    if r1:
        print("\n固化:\n  py -3 05_验证/gate.py %s --save %s" %
              (" ".join('"%s"' % p for p in r1), base))
    if r2:
        print("\n比对:\n  py -3 05_验证/gate.py %s --cmp %s" %
              (" ".join('"%s"' % p for p in r2), base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
