"""量 OBS 进程在**静止画面**下的 CPU 占用（单位：核 = 一个逻辑核满载）。

为什么单独量这一项：S7（自适应检测降频）的全部立论就是「静止 0.42 核、目标 ≤0.2 核」。
这个数如果不先在本机复现，就不该花一天去改代码——所以先量，再决定做不做。

判据：异步检测默认 `maskEveryAsync=0`，即**画面完全静止时也每帧提交一次推理**
（beauty_core.cpp:2396）。把周期拉长是唯一现成的降频手段，也是 S7 的上限参考：
如果 `--every 4` 只能省 0.05 核，自适应做得再巧也拿不到 0.2。

用法（OBS 自动起停，测完关闭）：
    py -3 05_验证/cpu_probe.py --seconds 60 --every 0,2,4,8
    py -3 05_验证/cpu_probe.py --collection beauty_front4k --canvas 3840x2160

输出：每种设置的 obs64.exe 平均核数、区间最高值，以及同窗口的引擎耗时。
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import obsctl
import soak_s2 as S
import verify_s1_correct as V

COLLECTION = "beauty_front720"
CANVAS = (1280, 720)

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def cpu_seconds(pid):
    """进程累计 CPU 时间（秒）= 内核态 + 用户态。打不开进程时返回 None。"""
    h = _k32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not h:
        return None
    try:
        c = wt.FILETIME()
        e = wt.FILETIME()
        k = wt.FILETIME()
        u = wt.FILETIME()
        if not _k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e),
                                    ctypes.byref(k), ctypes.byref(u)):
            return None
        def _s(ft):
            return ((ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 1e7
        return _s(k) + _s(u)
    finally:
        _k32.CloseHandle(h)


def sample(pid, seconds, step=2.0):
    """按 step 采样，返回 (平均核数, 最高核数, 采样点数, 实际时长)。"""
    t0 = time.time()
    c0 = cpu_seconds(pid)
    if c0 is None:
        return None, None, 0, 0.0
    rows = []
    last_c, last_t = c0, t0
    while True:
        time.sleep(step)
        c = cpu_seconds(pid)
        t = time.time()
        if c is None:
            break
        rows.append((c - last_c) / (t - last_t))
        last_c, last_t = c, t
        if t - t0 >= seconds:
            break
    if not rows:
        return None, None, 0, 0.0
    return sum(rows) / len(rows), max(rows), len(rows), last_t - t0


def engine_ms(log):
    """取最后一条性能行的引擎耗时与掩码/有脸比例，做同窗口旁证。"""
    best = None
    try:
        with open(log, encoding="utf-8", errors="ignore") as f:
            for line in f:
                if "性能 " in line and "引擎" in line:
                    m = re.search(r"引擎 ([\d.]+)", line)
                    cv = re.search(r"掩码 ([\d.]+)% 有脸 ([\d.]+)%", line)
                    if m:
                        best = (float(m.group(1)),
                                float(cv.group(1)) if cv else -1.0,
                                float(cv.group(2)) if cv else -1.0)
    except OSError:
        return None
    return best


def run_once(every, seconds, collection, canvas, enable=True):
    """一种设置跑一轮：起 OBS → 下发参数 → 预热 → 采样 → 关闭。

    every = "auto" → 不设 BEAUTY_MASK_EVERY，走 2.8.0 的运动自适应默认；
            整数   → 锁死固定周期（注意这个动作本身会**关掉**自适应）；
            None   → 关滤镜，测下限。
    """
    env = S.env_for(1, False)
    if every not in (None, "auto"):
        env["BEAUTY_MASK_EVERY"] = str(every)
    proc, snap = V.start_obs(collection, extra_env=env, canvas=canvas)
    pid = proc.pid
    res = None
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(8)                      # 等首帧、等异步检测稳定
        S.apply_load(c, region=False)
        if not enable:
            c.set_filter_enabled(V.SRC, V.FILT, False)
        time.sleep(10)                     # 预热：丢弃启动期抖动
        avg, peak, n, dur = sample(pid, seconds)
        c.close()
        res = (avg, peak, n, dur)
    finally:
        V.close_obs()
    log = V.newest_log(snap)
    return res, engine_ms(log)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default=COLLECTION)
    ap.add_argument("--canvas", default="%dx%d" % CANVAS)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--every", default="0,2,4,8",
                    help="逗号分隔的提交周期；末尾自动加一档 'off'（关滤镜）作下限")
    a = ap.parse_args(argv[1:])
    w, h = a.canvas.lower().split("x")
    canvas = (int(w), int(h))
    evers = [x.strip() for x in a.every.split(",") if x.strip() != ""]

    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    S.keep_awake(True)
    print("画布 %dx%d  素材 %s  每档 %.0f s  共 %d 档 + 关滤镜下限"
          % (canvas[0], canvas[1], a.collection, a.seconds, len(evers)))

    rows = []
    for e in evers:
        arg = "auto" if e in ("auto", "adapt") else int(e)
        label = ("auto(自适应默认)" if arg == "auto" else "every=%d" % arg)
        print("\n=== %s ===" % label)
        (avg, peak, n, dur), eng = run_once(arg, a.seconds, a.collection, canvas)
        print("  核数 平均 %.3f  峰值 %.3f  （%d 点 / %.0fs）  引擎 %s ms"
              % (avg if avg else -1, peak if peak else -1, n, dur,
                 "%.2f" % eng[0] if eng else "-"))
        rows.append((label, avg, peak, eng))

    print("\n=== 关滤镜（下限） ===")
    (avg, peak, n, dur), eng = run_once(None, a.seconds, a.collection, canvas,
                                        enable=False)
    print("  核数 平均 %.3f  峰值 %.3f  （%d 点 / %.0fs）"
          % (avg if avg else -1, peak if peak else -1, n, dur))
    rows.append(("filter-off", avg, peak, eng))

    S.keep_awake(False)
    print("\n=== 汇总 ===")
    print("%-12s %8s %8s %10s" % ("设置", "平均核", "峰值核", "引擎ms"))
    for name, avg, peak, eng in rows:
        print("%-12s %8s %8s %10s"
              % (name,
                 "%.3f" % avg if avg else "-",
                 "%.3f" % peak if peak else "-",
                 "%.2f" % eng[0] if eng else "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
