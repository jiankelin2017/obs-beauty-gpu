#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""2.14.0：把那个一次性两臂检查固化成通用工具。

**为什么需要它**：《开关总表》的「回退原则」第 3 条写着「每个新开关都要实际跑一次
`=0`（或 `=1`）验证回退 —— 开关本身也是被验收对象」，但此前没有一个现成脚本干这件事，
每次都是临时写一段、跑完就丢，证据不可复现。2.14.0 翻 `GPU_REGION` 默认时正好需要
「默认侧 / 显式关侧」各起一次 OBS 看日志 —— 于是把它参数化留在这里。

判据只认**启动日志里那一行**（`[obs-beauty]` 打的一次性状态行），不抓画面：
画面等价性属于各自的等价矩阵（例如五官段是 `verify_region_equiv.py` 的 13 格），
不是这个脚本的职责。它只回答一个问题：**环境变量到底有没有把开关拨到预期那一侧。**

用法
----
    # 默认参数就是 2.14.0 的 GPU_REGION（翻默认时留下的那条）
    py -3 05_验证/verify_switch_arm.py

    # 换成别的开关：分别给「不设」与「设成关闭值」的期望串
    py -3 05_验证/verify_switch_arm.py --switch BEAUTY_MOTION_GPU \
        --off-value 0 --expect-on "方案C 生效" --expect-off ""

    --attach  复用已在运行的 OBS（此时两臂无法分别启动，只跑「不设」那一臂）

退出码：两臂都命中 = 0；任一臂没命中 = 1。
"""
import argparse
import glob
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import verify_s1_correct as V   # noqa: E402
import obsctl                   # noqa: E402


def latest_log():
    d = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "logs")
    fs = sorted(glob.glob(os.path.join(d, "*.txt")), key=os.path.getmtime)
    return fs[-1] if fs else None


def lines_hitting(path, needles):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
    except OSError:
        return []
    out = []
    for ln in txt.splitlines():
        if any(n and n in ln for n in needles):
            out.append(ln.strip())
    return out


def run_arm(tag, collection, extra, settle):
    print("\n===== arm %s  extra=%s" % (tag, extra))
    proc, _ = V.start_obs(collection, extra_env=extra)
    try:
        obsctl.wait_ready(wait=90.0, quiet=True)
        time.sleep(settle)
    finally:
        V.close_obs()
        time.sleep(2.0)
    lp = latest_log()
    print("  日志：%s" % lp)
    return lp


def main():
    ap = argparse.ArgumentParser(description="开关两臂日志验证")
    ap.add_argument("--switch", default="BEAUTY_GPU_REGION")
    ap.add_argument("--off-value", default="0",
                    help="「显式关」那一臂要设的值（默认 0）")
    ap.add_argument("--collection", default="beauty_still_real")
    ap.add_argument("--source", default="still")
    ap.add_argument("--filter", default="Beauty")
    ap.add_argument("--expect-on", default="GPU 生产链",
                    help="「不设」那一臂日志里必须出现的片段")
    ap.add_argument("--expect-off", default="CPU 老路",
                    help="「显式关」那一臂日志里必须出现的片段")
    ap.add_argument("--settle", type=float, default=6.0)
    ap.add_argument("--attach", action="store_true",
                    help="复用正在运行的 OBS（只跑「不设」臂）")
    args = ap.parse_args()

    if V.obs_running() and not args.attach:
        sys.exit("obs64 已在运行，先关掉（或加 --attach）")

    base = {"BEAUTY_PROFILE": "1"}

    if args.attach:
        print("attach 模式：复用正在运行的 OBS，只验当前这一侧")
        lp = latest_log()
        hits = lines_hitting(lp, [args.switch, "五官段", "生效"])
        for l in hits:
            print("    " + l)
        ok = any(args.expect_on in l for l in hits)
        print("\n===== 结论\n  当前臂命中 %r：%s" % (args.expect_on,
                                                 "PASS" if ok else "FAIL"))
        return 0 if ok else 1

    a_extra = dict(base)
    a_extra.pop(args.switch, None)          # 不设 = 走默认
    lp_a = run_arm("A_default", args.collection, a_extra, args.settle)
    a_hits = lines_hitting(lp_a, [args.switch, "五官段", "生效"])
    for l in a_hits:
        print("    " + l)

    b_extra = dict(base)
    b_extra[args.switch] = args.off_value
    lp_b = run_arm("B_explicit", args.collection, b_extra, args.settle)
    b_hits = lines_hitting(lp_b, [args.switch, "五官段", "生效"])
    for l in b_hits:
        print("    " + l)

    a_ok = any(args.expect_on in l for l in a_hits)
    b_ok = any(args.expect_off in l for l in b_hits)
    print("\n===== 结论")
    print("  A 默认   (%r) : %s" % (args.expect_on, "PASS" if a_ok else "FAIL"))
    print("  B 显式设  %s=%s (%r) : %s" % (args.switch, args.off_value,
                                          args.expect_off,
                                          "PASS" if b_ok else "FAIL"))
    return 0 if (a_ok and b_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
