# -*- coding: utf-8 -*-
"""掩码偏移代价探针：掩码相对脸错 d 像素，画面上值多少灰阶（2.13.0）。

要回答的问题
------------
2.12.0 把 S7 的块聚合从 `max` 换成 `RMS`，代价是掩码平均年龄 1→3 帧、
p90 位置偏移 **16.84 px**、超羽化宽度的帧从 0.0% 涨到 2.9%（`PROGRESS §9.33.4`）。
但这些全是**像素**，没有一个数回答「看不看得出来」。

方法
----
`BEAUTY_DET_OFFSET=dx,dy`（2.13.0 新增，纯诊断）把掩码几何整体平移，
其余一切不变；输入是 `assets/real_1080x1920.png`（真实素材第 150 帧，
脸宽 261.7 px → 羽化 21 px），所以同一张图、同一份参数、只差一个 d。

    d=0            基准
    d=0 再跑一次   阴性对照：静态管线应当逐字节可复现，否则噪声底会吃掉小 d
    d=8..64        扫描（≈ 实测 lag 的 p50 5 px 到 p90 48 px，两倍余量）
    (24,24)        对角，确认代价是位移的函数而不是只有横向

读法：`≥1 灰阶` 是 8-bit 输出的最小可表示差；皮肤这种平缓区域里
1–2 灰阶属于「几乎不可见」，≥4 已经能在边界带上看出起伏。

用法
----
    py -3 05_验证/mask_off_probe.py
    py -3 05_验证/mask_off_probe.py --ds 0 16 32 48 --cfg blur
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                    # noqa: E402
import verify_s1_correct as V    # noqa: E402

COLL = "beauty_still_real"
CANVAS = (1080, 1920)
OUT = os.path.join(HERE, "out", "maskoff")

# 两档参数：纯磨皮（隔离掩码本身）与综合（看真实用户设置下放大了多少）
CFGS = {
    "blur": ("磨皮 80", {"blur_level": 80}),
    "combo": ("磨皮 60 + 清晰 40 + 美白 40 + 红润 30",
              {"blur_level": 60, "clarity": 40,
               "color_level": 40, "red_level": 30}),
    # 重度档：给「代价随效果幅度一起涨」这条留个上界
    "strong": ("磨皮 100 + 清晰 70 + 美白 70 + 红润 60 + 祛斑 60",
               {"blur_level": 100, "clarity": 70, "color_level": 70,
                "red_level": 60, "delspot_level": 60}),
}


def ready_client(wait=90):
    """等 websocket 起来 **并且** 集合真的加载完（滤镜能查到）。

    wait_ready 只保证 obs-websocket 在监听，此时场景/源可能还没建好，
    紧接着发 SetSourceFilterEnabled 会拿到 207「OBS is not ready」。
    """
    import time
    c = obsctl.wait_ready(wait=wait)
    t0 = time.time()
    while time.time() - t0 < 60:
        try:
            if any(f.get("filterName") == V.FILT for f in c.filters(V.SRC)):
                time.sleep(1.5)      # 留一点时间给缩略图/掩码首帧
                return c
        except Exception:
            pass
        time.sleep(0.5)
    raise RuntimeError("等滤镜 %r 出现超时" % V.FILT)


def shot_session(dx, dy, cfg, name):
    """起一轮 OBS，设参数，抓图存 out/maskoff/<name>.png。"""
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    env = {"BEAUTY_PROFILE": "1", "BEAUTY_DET_OFFSET": "%d,%d" % (dx, dy)}
    proc, snap = V.start_obs(COLL, extra_env=env, canvas=CANVAS)
    try:
        c = ready_client()
        px = os.path.join(OUT, name + ".png")
        try:
            V.set_config(c, dict(cfg))
            V.grab(c, px, settle=3.0, rounds=4)
        finally:
            c.close()
    finally:
        V.close_obs()
    return px


def load(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32)


def diff_stats(a, b, roi=None):
    """两张同尺寸 RGB → 差异统计。|Δ| 取三通道的最大者（最坏通道）。"""
    d = np.abs(a - b).max(axis=2)
    if roi is not None:
        d = d[roi]
    n = d.size
    return {
        "n": int(n),
        "f1": float((d >= 1).mean() * 100.0),
        "f2": float((d >= 2).mean() * 100.0),
        "f4": float((d >= 4).mean() * 100.0),
        "f8": float((d >= 8).mean() * 100.0),
        "mean_chg": float(d[d > 0].mean()) if (d > 0).any() else 0.0,
        "p999": float(np.percentile(d, 99.9)),
        "mx": float(d.max()),
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ds", nargs="*", default=["0", "0b", "8", "16", "24", "32",
                                                "40", "48", "64", "24x24", "0c"])
    ap.add_argument("--cfg", default="blur", choices=tuple(CFGS))

    a = ap.parse_args(argv)

    os.makedirs(OUT, exist_ok=True)
    label, cfg = CFGS[a.cfg]
    print("=" * 72)
    print("掩码偏移代价  集合=%s  画布=%dx%d  参数=%s" % (COLL, CANVAS[0], CANVAS[1], label))
    print("=" * 72)

    # 滤镜禁用基准：用来算「磨皮实际作用在哪」，也顺手做一次效果生效自检
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    V.start_obs(COLL, extra_env={"BEAUTY_PROFILE": "1"}, canvas=CANVAS)
    try:
        c = ready_client()
        p_off = os.path.join(OUT, "off.png")
        V.set_config(c, None)
        V.grab(c, p_off, settle=2.0, rounds=3)
        c.close()
    finally:
        V.close_obs()
    off = load(p_off)

    rows = []
    for tag in a.ds:
        if tag.endswith("b") or tag.endswith("c"):
            dx, dy = 0, 0
        elif "x" in tag:
            v = int(tag.split("x")[0])
            dx, dy = v, v
        else:
            dx, dy = int(tag), 0
        print("\n--- d=(%d,%d)  [%s] ---" % (dx, dy, tag))
        p = shot_session(dx, dy, cfg, "d%s_%s" % (tag, a.cfg))
        im = load(p)
        rows.append({"tag": tag, "dx": dx, "dy": dy, "path": p, "img": im})

    base = rows[0]["img"]
    # 磨皮真正改动的区域：d=0 的输出 vs 滤镜禁用。没有这块的自检就是空对空。
    rec = diff_stats(base, off)
    print("\n效果自检（d=0 vs 滤镜禁用）：≥1 灰阶 %.3f%% / ≥4 灰阶 %.3f%% / 最大 %d"
          % (rec["f1"], rec["f4"], int(rec["mx"])))
    if rec["f4"] < 1.0:
        print("!! 磨皮几乎没生效 —— 下面的差异无意义")
    roi = np.abs(base - off).max(axis=2) >= 1

    print("\n%-8s %5s %5s | %8s %8s %8s %8s | %8s %8s %6s"
          % ("tag", "dx", "dy", "≥1灰阶%", "≥2灰阶%", "≥4灰阶%", "≥8灰阶%",
             "改动区均值", "p99.9", "最大"))
    print("-" * 96)
    out = []
    for r in rows:
        s = diff_stats(base, r["img"])
        s2 = diff_stats(base, r["img"], roi)
        print("%-8s %5d %5d | %7.3f%% %7.3f%% %7.3f%% %7.3f%% | %8.2f %8.1f %6d"
              % (r["tag"], r["dx"], r["dy"], s["f1"], s["f2"], s["f4"], s["f8"],
                 s2["mean_chg"], s2["p999"], int(s["mx"])))
        out.append({"tag": r["tag"], "dx": r["dx"], "dy": r["dy"], **s,
                    "face_mean_chg": s2["mean_chg"], "face_p999": s2["p999"],
                    "face_frac4": s2["f4"], "face_frac2": s2["f2"]})

    meta = {"cfg": a.cfg, "label": label, "canvas": CANVAS,
            "effect_check": rec, "rows": out}
    with open(os.path.join(OUT, "res_%s.json" % a.cfg), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
    print("\n写入 %s" % os.path.join(OUT, "res_%s.json" % a.cfg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
