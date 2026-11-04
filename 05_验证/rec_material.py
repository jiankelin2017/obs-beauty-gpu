#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""录制真实采集素材（供 T10-b 判据补覆盖 / P2-5 人眼 A/B 复用）。

为什么不用 OBS 录
-----------------
1. 产品 profile 的录制是 `Lossless + I444 + 1080p30`，一分钟就近 1 GB，取材不划算；
2. 采集卡 **1080p 只有 MJPEG 通道**（`ffmpeg -list_options` 实测：nv12 最高
   1280x720@30，mjpeg 才到 1920x1080@60）。OBS 的 dshow 分辨率/格式是属性表里的
   **索引**，改集合 JSON 只能猜索引，不可靠；ffmpeg 可以直接写 `1920x1080 / 30`。
3. 录制期间要**关掉美颜滤镜**（拿原始画面）。ffmpeg 直接抓 dshow，天然是原始画面。

像素格式
--------
采集侧是 MJPEG（`yuvj420p`，全范围）。用 `-pix_fmt yuvj420p` 原样透传，
**不做全范围→有限范围转换** —— 否则亮度被压一次，素材和 OBS 里看到的对不上，
后面拿它当对拍输入会引入一个和被测代码无关的系统偏差。

关于动作
--------
**不引导出镜人**（无法主观指挥，也不该指挥：真实场景里人不听你的）。
所以只用「连续长镜 + 自然发挥」，全靠一段里自然出现的转头、手势挡脸、前倾后仰。
代价是覆盖不可控，好处是素材真实；覆盖够不够由事后抽帧统计来判断，不靠当场摆拍。

提示音
------
系统 TTS 与 COM 都被沙箱拦（`Add-Type` / `New-Object -ComObject` 均被拒），
所以用 `winsound.Beep`：长音 = 开始 / 两声 = 结束。中间**不响**，避免干扰。
"""

import argparse
import datetime as _dt
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FFMPEG = os.path.join(HERE, "tools", "ffmpeg.exe")
DEVICE = "UGREEN 25173"


def dur_to_sec(text):
    """'00:01:20.03' -> 80.03；兼容 '01:20.03'。"""
    parts = text.replace(",", ".").split(":")
    try:
        nums = [float(p) for p in parts]
    except ValueError:
        return None
    sec = 0.0
    for n in nums:
        sec = sec * 60 + n
    return sec


def beep(kind):
    try:
        import winsound
        if kind == "start":
            winsound.Beep(1500, 600)
        elif kind == "end":
            winsound.Beep(900, 200)
            time.sleep(0.15)
            winsound.Beep(900, 200)
        elif kind == "tick":
            winsound.Beep(800, 90)
    except Exception as e:                      # 无 winsound 也不致命
        print("  (提示音不可用: %s)" % e)


def capture(out_path, w, h, fps, seconds, lead):
    dur = seconds + 2.0                         # 结尾多录 2 s，防最后一秒被截断
    cmd = [
        FFMPEG, "-hide_banner", "-loglevel", "warning", "-nostdin",
        "-f", "dshow",
        # 默认 rtbufsize 只有 3 MB，1080p MJPEG 一帧就近 3 MB，
        # 上一轮结尾就报了 "real-time buffer too full ... frame dropped"。
        # 抬到 256 MB 换稳定，宁可多占内存也不要丢帧。
        "-rtbufsize", "256M",
        "-video_size", "%dx%d" % (w, h),
        "-framerate", str(fps),
        "-vcodec", "mjpeg",
        "-i", "video=%s" % DEVICE,
        "-t", "%.2f" % dur,
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "12",
        "-pix_fmt", "yuvj420p",                  # 原样透传，不做范围转换
        "-movflags", "+faststart",
        "-y", out_path,
    ]
    print("\n" + "=" * 72)
    print("录制 %dx%d @ %d fps  %d 秒  ->  %s"
          % (w, h, fps, seconds, os.path.basename(out_path)))
    print("=" * 72)
    print("  不引导动作：正常讲话、自然发挥即可，中途没有提示音。")

    print("\n准备……", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < lead:
        left = lead - (time.time() - t0)
        if left <= 3.05:
            beep("tick")
            time.sleep(1.0)
        else:
            time.sleep(0.2)
    print(" 开始（中途不提醒，安静录完）")

    p = subprocess.Popen(cmd, cwd=HERE)
    beep("start")
    t_start = time.time()
    # 只报开始/结束，中间不打点：任何提示都会干扰自然动作
    while p.poll() is None:
        el = time.time() - t_start
        if int(el) % 30 == 0 and el - int(el) < 0.25:
            print("  [%3ds] 录制中…" % int(el), flush=True)
        time.sleep(0.25)
    beep("end")
    rc = p.wait()
    print("  ffmpeg 退出码 %d" % rc)
    if rc != 0 or not os.path.exists(out_path):
        print("  ✗ 录制失败")
        return None
    return out_path


def probe(path):
    """用 ffmpeg -i 拿时长/流信息，作为「确实录到了」的凭据。"""
    r = subprocess.run([FFMPEG, "-hide_banner", "-i", path],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    txt = r.stderr
    info = {"size_mb": round(os.path.getsize(path) / 1048576.0, 1)}
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("Duration:"):
            info["duration"] = line.split(",")[0].split("Duration:")[1].strip()
        if "Video:" in line and "Stream #" in line:
            info["stream"] = line
    return info


def count_frames(path):
    r = subprocess.run([FFMPEG, "-hide_banner", "-i", path,
                        "-map", "0:v", "-c", "copy", "-f", "null", "-"],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    last = None
    for line in r.stderr.splitlines():
        if line.strip().startswith("frame="):
            last = line.strip()
    try:
        return int(last.split("frame=")[1].split()[0])
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lead", type=int, default=15, help="开场准备秒数（默认 15）")
    ap.add_argument("--only", choices=["all", "1080", "720"], default="all")
    ap.add_argument("--sec1080", type=int, default=180, help="1080p30 时长")
    ap.add_argument("--sec720", type=int, default=90, help="720p60 时长")
    args = ap.parse_args()

    if not os.path.exists(FFMPEG):
        sys.exit("找不到 ffmpeg: %s" % FFMPEG)
    dst = os.path.join(HERE, "assets", "capture")
    os.makedirs(dst, exist_ok=True)
    ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")

    jobs = []
    if args.only in ("all", "1080"):
        jobs.append((1920, 1080, 30, args.sec1080,
                     "real_1080p30_%s.mp4" % ts))
    if args.only in ("all", "720"):
        jobs.append((1280, 720, 60, args.sec720,
                     "real_720p60_%s.mp4" % ts))

    results = []
    for w, h, fps, secs, fname in jobs:
        out = os.path.join(dst, fname)
        got = capture(out, w, h, fps, secs, args.lead)
        if not got:
            continue
        info = probe(got)
        nf = count_frames(got)
        dsec = dur_to_sec(info.get("duration") or "")
        real_fps = (nf / dsec) if (nf and dsec) else None
        results.append({
            "file": got, "size_mb": info.get("size_mb"),
            "duration": info.get("duration"), "seconds": round(dsec, 2) if dsec else None,
            "frames": nf, "fps_real": round(real_fps, 2) if real_fps else None,
            "fps_want": fps, "canvas": "%dx%d" % (w, h),
            "want_s": secs,
            "fps_ok": (real_fps is not None and abs(real_fps - fps) < 0.6),
        })
        print("  → %s  %s MB  %s 帧  实际 %.2f fps  %s"
              % (info.get("duration"), info.get("size_mb"), nf,
                 real_fps or 0, "OK" if results[-1]["fps_ok"] else "⚠ 帧率不达标"))

    if not results:
        sys.exit("没有任何素材产出")

    log = os.path.join(HERE, "out", "logs")
    os.makedirs(log, exist_ok=True)
    lp = os.path.join(log, "material_capture_%s.md" % ts)
    with open(lp, "w", encoding="utf-8") as f:
        f.write("# 真实采集素材录制记录（%s）\n\n" % ts)
        f.write("设备 `%s`；采集侧 MJPEG，`-pix_fmt yuvj420p` 原样透传（全范围不转换）；"
                "libx264 crf 12；**无音轨**；未开美颜滤镜（原始画面）。\n\n" % DEVICE)
        f.write("录制方式：**连续长镜、不引导动作**，中途无提示音。\n\n")
        f.write("| 文件 | 画布 | 目标 fps | 实际 fps | 帧数 | 时长 | 体积 | 判定 |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for r in results:
            f.write("| `%s` | %s | %d | %s | %s | %s | %s MB | %s |\n" % (
                os.path.basename(r["file"]), r["canvas"], r["fps_want"],
                r["fps_real"], r["frames"], r["duration"], r["size_mb"],
                "OK" if r["fps_ok"] else "帧率不达标"))
    print("\n记录已写: %s" % lp)
    print(json.dumps(results, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
