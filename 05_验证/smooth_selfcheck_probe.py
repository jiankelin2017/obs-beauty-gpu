#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""取一行「GPU 磨皮对拍」自检日志 —— 给「改代码前后画面是否不变」当基线。

为什么要这个脚本
----------------
`beauty_gpu_smooth_selfcheck()` 每次 OBS 会话只报一行，内容是

    GPU 磨皮对拍 WxH（半分辨率 …）: maxdiff=? 平均偏差=? 差异像素=?% … 判定=通过

它把 GPU 磨皮的结果和 CPU 参考实现逐像素比过，所以**这一行就是「磨皮数学有没有变」的
唯一直接证据**。但它是诊断开关（`BEAUTY_GPU_SMOOTH=1`），日常运行不会出现 ——
历史 OBS 日志里翻不到，等于没有基线可对。于是改代码前先跑一次、改完再跑一次，
逐项比 `maxdiff` / `平均偏差` / `差异像素` / `判定` 四个数。

⚠ 这一行**只覆盖磨皮**。GPU 侧其它段（tone / detail / warp / region）各有自己的
   诊断开关与对拍函数，改哪段就开哪个，别拿磨皮这行当全网证据。

⚠ `BEAUTY_SMOOTH_PASSES` 对这里**无效**：它只在生产链钩子里覆盖 `cp.passes`
   （`obs-beauty.cpp` 里那个 `const int fp = beauty_smooth_passes_force()`），
   而自检走的是自己那份 `beautyCoreBandDiag(&bp)`，永远是产品默认 3 趟。
   实测过：passes=1 / 3 / 6 在这一行上给出**完全相同**的八个数。
   要测趟数的边界（尤其 nPass=1 时「末趟落在 A」那条分支），只能走生产路径
   录屏对比（`ab_record.py` + 环境变量），不能用自检。

用法
----
    py -3 smooth_selfcheck_probe.py --tag before
    py -3 smooth_selfcheck_probe.py --tag after --wait 12

产物：`out/smooth_selfcheck_<tag>.json`（含 dll md5 / 尺寸 / 时间与原始日志行）

判读纪律
--------
`判定=通过` 只是「maxdiff≤2 且 平均偏差<0.3」的宽门限，**不能当"画面没变"**。
"画面没变"要看改前改后四个数逐项相同；真要逐位证明，得录无损 A/B（见 T2）。
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# ⚠ 本机 shell 里带着 HTTP(S)_PROXY（127.0.0.1:13412），websocket-client 会照它
# 把 ws://127.0.0.1:4455 也塞进 HTTP CONNECT 隧道 → 代理回 502、连不上 OBS。
# 本脚本只连本机，先把代理变量摘掉；顺带也避免 OBS 子进程继承它们。
for _k in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
           "http_proxy", "https_proxy", "all_proxy"):
    os.environ.pop(_k, None)

import obsctl                    # noqa: E402
import verify_s1_correct as V    # noqa: E402
from mask_off_probe import CFGS  # noqa: E402

OUT = os.path.join(HERE, "out")
DLL = r"C:\ProgramData\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll"


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pick_log(snap):
    """取本次会话的日志：优先比快照更新的文件，否则取最新一份。"""
    logs = sorted(glob.glob(os.path.join(V.LOGDIR, "*.txt")), key=os.path.getmtime)
    if not logs:
        return None
    fresh = [p for p in logs if p not in snap]
    return fresh[-1] if fresh else logs[-1]


def parse_line(path):
    if not path or not os.path.exists(path):
        return None
    best = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if "磨皮对拍" in ln:
                best = ln.rstrip("\r\n")     # 取最后一条（同会话只该有一条）
    return best


def parse_fields(line):
    """把日志行拆成可比对的字段。取不到就给 None，别伪造 0。"""
    if not line:
        return {}
    out = {}
    m = re.search(r"磨皮对拍\s+(\d+)x(\d+)", line)
    if m:
        out["size"] = "%sx%s" % (m.group(1), m.group(2))
    for key, pat in (
        ("maxdiff", r"maxdiff=(\d+)"),
        ("avg_abs", r"平均偏差=([\d.]+)"),
        ("diff_px_pct", r"差异像素=([\d.]+)%"),
        ("gpu_mean", r"亮度 gpu=([\d.]+)"),
        ("gpu_chg", r"改动量 gpu=([\d.]+)"),
        ("mask_pct", r"掩码覆盖=([\d.]+)%"),
        ("faces", r"faces=(\d+)"),
        ("wait_frames", r"等脸=(\d+)帧"),
    ):
        m = re.search(pat, line)
        out[key] = m.group(1) if m else None
    # 日志把这些值两两写在一行里（`亮度 gpu=A cpu=B`），标签不复用，
    # 所以 cpu 侧必须从同一段里取第二组，不能单写 "亮度 cpu="。
    m = re.search(r"亮度 gpu=([\d.]+) cpu=([\d.]+)", line) if line else None
    out["cpu_mean"] = m.group(2) if m else None
    m = re.search(r"改动量 gpu=([\d.]+) cpu=([\d.]+)", line) if line else None
    out["cpu_chg"] = m.group(2) if m else None
    out["pass"] = ("判定=通过" in line) if line else None
    out["weak"] = "弱效果兜底" in line if line else None
    out["rt16f"] = "磨皮RT=RGBA16F" in line if line else None
    return out


def clear_stale_locks():
    """清 OBS 的实例锁 —— 上一次 `taskkill /F` 会把它留在 `.sentinel/` 里。

    不先清会怎样：OBS 起来后认为「已有一个实例在跑」，弹一个模态框就再也不往下
    走 —— 进程活着、日志只有一行 `Crash or unclean shutdown detected`、
    websocket 端口永远不监听。现象很像卡死，其实是弹窗在等点击。
    """
    base = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", ".sentinel")
    n = 0
    for p in glob.glob(os.path.join(base, "run_*")):
        try:
            os.remove(p)
            n += 1
        except OSError:
            pass
    return n


def stop_obs(p, timeout=20.0):
    """先礼后兵：CloseMainWindow（等 OBS 自己落盘退出）→ 超时才 taskkill。

    用 `taskkill /F` 收尾会留下 unclean-shutdown 标记与实例锁，下一次启动就被
    弹窗挡住 —— 这个坑本轮踩过一次，别再踩。
    """
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-Process obs64 -ErrorAction SilentlyContinue | "
         "ForEach-Object { $_.CloseMainWindow() | Out-Null }"],
        capture_output=True)
    try:
        p.wait(timeout=timeout)
        return "graceful"
    except Exception:
        subprocess.run(["taskkill", "/IM", "obs64.exe", "/F"], capture_output=True)
        return "killed"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coll", default="beauty_real_ab720p60")
    ap.add_argument("--cfg", default="combo", choices=sorted(CFGS))
    ap.add_argument("--wait", type=float, default=10.0,
                    help="启动后等多久再关（对拍要等 bandDiag + 脸，正常 1~2 s）")
    ap.add_argument("--tag", required=True)
    a = ap.parse_args()

    if not os.path.exists(DLL):
        sys.exit("找不到已装 dll：%s" % DLL)
    dll_info = {"path": DLL, "md5": md5_of(DLL),
                "size": os.path.getsize(DLL),
                "mtime": time.strftime("%Y-%m-%d %H:%M:%S",
                                       time.localtime(os.path.getmtime(DLL)))}
    print("dll  md5=%s  size=%d  mtime=%s"
          % (dll_info["md5"], dll_info["size"], dll_info["mtime"]))

    # BEAUTY_GPU_SMOOTH 是诊断开关；PROD 保持默认开（画面确实由 GPU 磨皮产出）
    env = {"BEAUTY_GPU_SMOOTH": "1", "BEAUTY_PROFILE": "1"}
    nlock = clear_stale_locks()
    if nlock:
        print("清掉 %d 个残留实例锁（上次 taskkill 留下的）" % nlock)
    p, snap = V.start_obs(a.coll, extra_env=env)
    print("OBS 已拉起（collection=%s，诊断=%s），等 %.1f s"
          % (a.coll, "BEAUTY_GPU_SMOOTH=1", a.wait))

    try:
        c = obsctl.wait_ready(wait=60.0)
        # 连上 ≠ 可下发请求：OBS 启动初期会回 code=207（not ready），
        # 只试一次就失败整轮，所以这里带重试。
        for attempt in range(6):
            try:
                V.set_config(c, CFGS[a.cfg][1])
                break
            except RuntimeError as e:
                if "207" not in str(e) or attempt == 5:
                    raise
                time.sleep(1.0)
        print("滤镜参数已设为 %s（%s）" % (a.cfg, CFGS[a.cfg][0]))
        time.sleep(a.wait)
    finally:
        how = stop_obs(p)
        print("OBS 退出方式：%s" % how)
        time.sleep(1.0)

    log = pick_log(snap)
    line = parse_line(log)
    fields = parse_fields(line)
    rec = {"tag": a.tag, "coll": a.coll, "cfg": a.cfg, "wait": a.wait,
           "log": log, "dll": dll_info, "line": line, "fields": fields}
    os.makedirs(OUT, exist_ok=True)
    dst = os.path.join(OUT, "smooth_selfcheck_%s.json" % a.tag)
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(rec, fh, ensure_ascii=False, indent=1)

    print()
    if not line:
        print("[缺] 日志里没有「磨皮对拍」行。可能原因：磨皮没开 / bandDiag 未置位 /")
        print("     本帧没到 CPU 回读位置。查 %s" % log)
        sys.exit(2)
    print("日志 %s" % os.path.basename(log or ""))
    print("原文 %s" % line)
    print("字段 %s" % json.dumps(fields, ensure_ascii=False))
    print("已写 %s" % dst)


if __name__ == "__main__":
    main()
