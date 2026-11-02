#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
口径 C：交替配对 A/B（证明「某改动更快」的唯一可信测法）。

背景（PROGRESS 9.1.1）：往返里的「下载」是 gs_stagesurface_map() 的**排队等待**，
同一二进制、同一素材，跨会话能漂 720p 0.8→4.0 ms、4K 4.3→5.5 ms。把两个会话的
中位数相减，得到的是背景负载差，不是代码差。

因此本脚本只做一件事：把两条臂**交替**跑，在**同一轮内配对相减**，环境漂移被抵消。

  · 交替：第 1 轮 A→B，第 2 轮 B→A，第 3 轮 A→B …… 顺序偏置也抵消。
  · 配对：d_i = B_i − A_i（同一轮），取 d 的中位数；再看符号是否一致。
  · 判读：所有 d 同号且 |中位| 超过噪声 → 真实差异；符号混乱 → 判为噪声。

用法:
    # S3：async 开 vs 关
    py -3 05_验证/ab_pair.py --only 720p --rounds 3 --seconds 20 \
        --arms "on=BEAUTY_ASYNC_STAGE=1" "off=BEAUTY_ASYNC_STAGE=0"
    # S2b 做过的那组（可复现验证本工具）
    py -3 05_验证/ab_pair.py --only 720p --rounds 2 --seconds 20 \
        --arms "always=BEAUTY_RING_ALWAYS=1" "ondemand=BEAUTY_RING_ALWAYS=0"

  · 每条臂的 ENV 用 `KEY=VAL` 逗号分隔，公共环境恒为 BEAUTY_PROFILE=1。
  · 采集条件（集合/画布/参数/位置/滤镜数量）由 s0_lock.one_run 统一保证。
"""
import argparse
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import verify_s1_correct as V   # 起停 / 画布
import s0_lock as L            # one_run / TARGETS
import soak_s2 as S            # keep_awake
import phase0_parse_log as P   # 日志解析


def parse_arm(spec):
    """'名字=K=V,K2=V2' -> (名字, {K:V})"""
    if "=" not in spec:
        raise SystemExit("臂格式应为 名字=KEY=VAL[,KEY=VAL]：%s" % spec)
    name, envs = spec.split("=", 1)
    env = {}
    for kv in envs.split(","):
        kv = kv.strip()
        if not kv:
            continue
        if "=" not in kv:
            raise SystemExit("环境变量格式应为 KEY=VAL：%s" % kv)
        k, v = kv.split("=", 1)
        env[k] = v
    return name.strip(), env


def metrics(log, res_key):
    """从日志里取该分辨率的中位数指标。"""
    perf, _ = P.parse_file(log)
    arr = [r for r in perf if r["res"] == res_key]
    if not arr:
        return None
    dl = statistics.median([r["dl"] for r in arr])
    up = statistics.median([r["up"] for r in arr])
    return {
        "n": len(arr),
        "dl": dl, "up": up, "tr": dl + up,
        "pr": statistics.median([r["pr"] for r in arr]),
        "tot": statistics.median([r["tot"] for r in arr]),
        "prep": statistics.median([r["seg"]["prep"] for r in arr
                                   if r.get("seg") and r["seg"].get("prep") is not None]
                                  or [0.0]),
        "cover": statistics.median([r["cover"] for r in arr if r["cover"] is not None] or [0.0]),
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", nargs=2, required=True, metavar="NAME=ENV",
                    help="两条臂，前者为基线，后者为待测（差值 = 后者 − 前者）")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--only", choices=("720p", "4K"), help="只跑一档（默认两档都跑）")
    args = ap.parse_args(argv)

    armA, envA = parse_arm(args.arms[0])
    armB, envB = parse_arm(args.arms[1])
    print("臂 A（基线）= %s  %s" % (armA, envA))
    print("臂 B（待测）= %s  %s" % (armB, envB))

    if V.obs_running() and not V.close_obs():
        sys.exit("关不掉 OBS")

    targets = [t for t in L.TARGETS if (not args.only or t[0] == args.only)]
    res_of = {"720p": "1280x720", "4K": "3840x2160"}

    S.keep_awake(True)
    pairs = {t[0]: [] for t in targets}   # name -> [(A_metrics, B_metrics, round)]
    try:
        for r in range(1, args.rounds + 1):
            # 交替顺序：奇数轮 A→B，偶数轮 B→A
            order = [(armA, envA), (armB, envB)] if r % 2 else [(armB, envB), (armA, envA)]
            for tname, coll, canvas in targets:
                got = {}
                for name, env in order:
                    print("\n==== 第 %d 轮 / %s / %s ====" % (r, tname, name))
                    log, note = L.one_run(coll, canvas, args.seconds, env)
                    if not log:
                        print("  !! %s" % note)
                        got[name] = None
                        continue
                    m = metrics(log, res_of[tname])
                    if not m:
                        print("  !! 日志无 %s 性能行: %s" % (res_of[tname], log))
                        got[name] = None
                        continue
                    print("  %s：n=%d 搬运 %.2f 引擎 %.2f 往返 %.2f prep %.2f（%s）" % (
                        name, m["n"], m["tr"], m["pr"], m["tot"], m["prep"], log))
                    got[name] = m
                if got.get(armA) and got.get(armB):
                    pairs[tname].append((got[armA], got[armB], r))
    finally:
        S.keep_awake(False)
        V.close_obs()

    rc = 0
    for tname, _c, _cv in targets:
        lst = pairs[tname]
        print("\n########## %s 配对结果（差值 = %s − %s）##########" % (tname, armB, armA))
        if not lst:
            print("  本轮没有任何可配对的臂，无结论。")
            rc = 5
            continue
        hdr = "%-6s %9s %9s %9s %9s %9s %9s" % (
            "轮", "A搬运", "B搬运", "Δ搬运", "A引擎", "B引擎", "Δ引擎")
        print(hdr)
        print("-" * len(hdr))
        for a, b, r in lst:
            print("%-6d %9.2f %9.2f %+9.2f %9.2f %9.2f %+9.2f" % (
                r, a["tr"], b["tr"], b["tr"] - a["tr"], a["pr"], b["pr"], b["pr"] - a["pr"]))

        d_tr = [b["tr"] - a["tr"] for a, b, _ in lst]
        d_pr = [b["pr"] - a["pr"] for a, b, _ in lst]
        med_tr = statistics.median(d_tr)
        med_pr = statistics.median(d_pr)
        same_tr = all(d > 0 for d in d_tr) or all(d < 0 for d in d_tr)
        same_pr = all(d > 0 for d in d_pr) or all(d < 0 for d in d_pr)
        print("\n  Δ搬运 中位 %+.2f ms（各轮 %s）→ %s" % (
            med_tr, " ".join("%+.2f" % d for d in d_tr),
            "同号，可信" if same_tr else "**符号混乱，判为噪声**"))
        print("  Δ引擎 中位 %+.2f ms（各轮 %s）→ %s" % (
            med_pr, " ".join("%+.2f" % d for d in d_pr),
            "同号，可信" if same_pr else "**符号混乱，判为噪声**"))
        # 公平性：两臂的画面条件必须接近，否则毫秒差没有意义
        dc = max(abs(a["cover"] - b["cover"]) for a, b, _ in lst)
        if dc > 1.0:
            print("  [!] 两臂掩码占比最大差 %.2f 个百分点 —— 画面条件不一致，差值不可采信" % dc)
            rc = 6
        # 污染识别：prep 高 = 走了对齐换帧的整帧上传（worker 掉队）
        preps = [max(a["prep"], b["prep"]) for a, b, _ in lst]
        if preps and max(preps) > 1.0:
            print("  [!] 有配对 prep 高达 %.2f ms —— worker 掉队污染，该轮请剔除" % max(preps))
            rc = 6
        print("  参考：A 往返 %.2f / B 往返 %.2f（往返不参与判定，含 map 排队等待）" % (
            statistics.median([a["tot"] for a, _b, _r in lst]),
            statistics.median([b["tot"] for _a, b, _r in lst])))
    return rc


if __name__ == "__main__":
    sys.exit(main())
