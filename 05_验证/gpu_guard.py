#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""采集前的 GPU 占用闸：条件不满足就拒绝启动 OBS，而不是测完再怀疑数据。

为什么需要这道闸
----------------
gate.py 判的是「这次比基线慢了多少」，但它分不清两种变慢：代码退化，和
别的程序在抢显卡。本机实测（2026-11-03）：RTX 5060 Laptop，显存 8151 MiB，
桌面空闲态已用 2373 MiB。DaVinci Resolve 一旦打开，4K 剪辑加降噪能占到
5~7 GB，还要吃 NVENC 的并发会话。这种状态下跑 s0_lock.py 或 soak_s2.py，
帧时整体抬高、窗口抖动变大，gate.py 会判「引擎超差 FAIL」——
于是人去翻源码，找一个根本不存在的问题。

所以闸放在**采集入口**（verify_s1_correct.start_obs），所有采集脚本都经过它：
s0_lock.py / soak_s2.py / ab_pair.py / mask_probe.py / cpu_probe.py /
verify_s1_correct.py 自己。一处改动，全覆盖。

两条判据，任一命中即拒绝采集
--------------------------
1. 重度 GPU 进程在跑。用 tasklist 拿全部进程名做匹配，名单见 HEAVY。
   这条专挡 DaVinci Resolve —— 它是本机最可能同时开着的重负载程序。
2. 显存已用超过预算。默认 3500 MiB，理由：空闲基线 2373 MiB，留约 1.1 GB
   给浏览器/桌面波动；而 DaVinci 一开就会越过这条线。
   阈值可用 BEAUTY_GPU_BUDGET_MB 覆盖。

刻意不做的事
------------
· 不查 GPU 利用率。它按秒采样跳动，空闲桌面也会有 30% 的瞬时尖峰，
  没有判别力，只会制造误报。
· 不查 nvidia-smi --query-compute-apps。DaVinci 剪辑走 graphics 上下文，
  那张表经常是空的 —— 拿它当判据等于没判。

逃生舱
------
    BEAUTY_NO_GPU_GATE=1     完全跳过本闸（明确知道自己在干什么时才用）
    BEAUTY_GPU_BUDGET_MB=N   改显存预算，例如 6000

单独运行（排查用，只报告不退出）
--------------------------------
    py -3 05_验证/gpu_guard.py
"""
import os
import subprocess
import sys

NVIDIA_SMI = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                          "System32", "nvidia-smi.exe")

# 空闲基线。2026-11-03 实测：8151 MiB 总量，桌面+ToDesk+浏览器已用 2373 MiB。
DEFAULT_BUDGET_MB = 3500
# 预算的 85% 处给软告警：不拦，但把数字打出来，让人知道离红线还有多远。
WARN_RATIO = 0.85

# 进程名 -> 人话。tasklist 给的是不带 .exe 的名字。
# 只收「一开就吃走半张卡」的程序；编辑器、浏览器一律不收，它们本来就常年开着。
#
# 刻意不收 obs64：start_obs() 的第一步 obs_running() 已经在管它，且给的提示
# （"obs64 已在运行，先关掉"）比这里能给的更准确。把它混进来会让拒绝理由
# 变成"有重度 GPU 程序在跑：OBS"，而 OBS 恰恰是我们自己要起的那个 —— 误导。
HEAVY = {
    "resolve": "DaVinci Resolve",
    "davinciresolve": "DaVinci Resolve",
    "adobe premiere pro": "Premiere Pro",
    "afterfx": "After Effects",
    "blender": "Blender",
    "topaz video ai": "Topaz Video AI",
    "handbrake": "HandBrake",
    "fusion": "Fusion（Blackmagic）",
    "vmware-vmx": "VMware 虚拟机",
    "virtualboxvm": "VirtualBox 虚拟机",
}


def _run(cmd, timeout=15):
    """跑命令，吞掉一切异常，失败返回 None。闸本身绝不能成为新的崩溃源。"""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        if p.returncode != 0:
            return None
        return p.stdout
    except Exception:
        return None


def _decode(raw):
    """tasklist 走 OEM 代码页（中文 Windows 是 cp936）。进程名是 ASCII，
    正文乱码不影响匹配，所以宁可宽松解码也不能抛。"""
    if not raw:
        return ""
    for enc in ("cp936", "mbcs", "utf-8"):
        try:
            return raw.decode(enc)
        except Exception:
            continue
    return raw.decode("ascii", "ignore")


def busy_processes():
    """返回 (hits, ok)。hits 是 [(exe, PID, 人话)]，只列黑名单里命中的。

    第二个返回值区分「查了，没有」和「没查成」—— 前者可以放行，
    后者必须留痕：闸静默失效比闸不存在更危险。
    """
    raw = _run(["tasklist", "/fo", "csv", "/nh"])
    if raw is None:
        return [], False
    text = _decode(raw)
    hits = []
    for line in text.splitlines():
        # CSV 形如 "obs64.exe","12345","Console","1","123,456 K"
        parts = [x.strip().strip('"') for x in line.split(",")]
        if len(parts) < 2:
            continue
        exe = parts[0].lower()
        name = exe[:-4] if exe.endswith(".exe") else exe
        if name in HEAVY:
            hits.append((parts[0], parts[1], HEAVY[name]))
    return hits, True


def vram():
    """返回 (已用 MiB, 总量 MiB)，取不到返回 (None, None)。"""
    raw = _run([NVIDIA_SMI, "--query-gpu=memory.used,memory.total",
                "--format=csv,noheader,nounits"])
    text = _decode(raw).strip()
    if not text:
        return None, None
    for line in text.splitlines():
        bits = [b.strip() for b in line.split(",")]
        if len(bits) >= 2 and bits[0].isdigit() and bits[1].isdigit():
            return int(bits[0]), int(bits[1])
    return None, None


def snapshot():
    """当前 GPU 状态，给 check() 和单独运行共用。"""
    used, total = vram()
    try:
        budget = int(os.environ.get("BEAUTY_GPU_BUDGET_MB", DEFAULT_BUDGET_MB))
    except ValueError:
        budget = DEFAULT_BUDGET_MB
    procs, proc_ok = busy_processes()
    return {"used": used, "total": total, "budget": budget,
            "procs": procs, "proc_ok": proc_ok}


def check():
    """返回 (ok, lines)。ok=False 表示不该开始采集。

    lines 是给人看的诊断行，由调用方决定打印到哪儿。
    """
    skip = os.environ.get("BEAUTY_NO_GPU_GATE", "") not in ("", "0")
    st = snapshot()
    used, total, budget = st["used"], st["total"], st["budget"]
    procs = st["procs"]
    lines = []

    if used is not None and total:
        lines.append("显存 %d / %d MiB（预算 %d）" % (used, total, budget))
    else:
        lines.append("显存读取失败（nvidia-smi 不可用或非 NVIDIA 显卡）")

    if not st["proc_ok"]:
        lines.append("进程表读取失败（tasklist 异常）→ 本次只能看显存")

    if procs:
        for exe, pid, human in procs:
            lines.append("占卡进程：%s（PID %s）→ %s" % (exe, pid, human))

    if skip:
        lines.append("BEAUTY_NO_GPU_GATE 已设置 → 跳过检查（数据可信度自负）")
        return True, lines

    problems = []
    if procs:
        problems.append("有重度 GPU 程序在跑：%s"
                        % "、".join(sorted({h for _e, _p, h in procs})))
    if used is not None and used > budget:
        problems.append("显存已用 %d MiB，超过预算 %d MiB" % (used, budget))

    if problems:
        lines.extend("拒绝采集：" + p for p in problems)
        lines.append("处理：关掉上面这些程序后重跑；"
                     "若确认无碍，用 BEAUTY_GPU_BUDGET_MB=6000 或 "
                     "BEAUTY_NO_GPU_GATE=1 放行（此时测出的数不能进基线）")
        return False, lines

    if used is not None and used > budget * WARN_RATIO:
        lines.append("注意：显存已用 %d MiB，达预算的 %.0f%%，"
                     "边界附近建议先确认没有后台渲染" %
                     (used, 100.0 * used / budget))
    return True, lines


def require():
    """在 start_obs() 里调用。不通过就 sys.exit —— 拒绝比脏数据便宜。

    诊断行走 stderr：s0_lock.py 之类的脚本会往 stdout 打供 gate.py 吃的
    JSON / 命令，闸的输出不能混进去。
    """
    ok, lines = check()
    for ln in lines:
        print("  [GPU闸] %s" % ln, file=sys.stderr)
    if not ok:
        sys.exit("GPU 闸未通过，已取消本次采集（见上方 [GPU闸] 各行）")
    return ok


def main():
    ok, lines = check()
    print("【GPU 闸自检】")
    for ln in lines:
        print("  " + ln)
    print("\n结论：" + ("可以采集" if ok else "拒绝采集"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
