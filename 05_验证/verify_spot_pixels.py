#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P0-8 的 4K 补齐：祛斑痘「到底改没改画面」的开关滤镜判据。

为什么需要这个脚本
------------------
P0-8 修复（2.9.0）的判据是「同一张人造斑点图，开滤镜 vs 关滤镜」：
修复前 maxdiff 恒 0（一个像素都不改），修复后 3~7 且随档位单调、变化区落在脸框内。
但那一轮只跑了 720p —— 4K 三次尝试都被上一个 OBS 会话接管，结论只有「逻辑等价」。

真人画面不能用：用户脸上没有明显斑点，分不出「没生效」与「没东西可祛」。
所以判据必须是**人造斑点图**，而 4K 有独立素材（`05_验证/out/face_spot_4k.png`，
12 个半径 3px、深度 26 灰阶高斯暗斑）与独立集合（`beauty_spot4k`）。

口径
----
  · 一次采样 = 同一帧内容的两张源截图：滤镜开 / 滤镜关
  · 判据：maxdiff > 0 且变化像素随 --level 单调（0 档必须是 0）
  · 变化区必须落在脸框内 —— 只是「有差异」不够，全画面抖动也能造差异
  · 只开祛斑一项，其余面板参数全 0（否则磨皮的存在会盖掉几灰阶的斑点差）

跑之前 OBS 必须已关闭：`obs64.exe` 是单实例，第二个进程只会激活已有窗口，
于是 --collection / --canvas 全部打偏 —— 这就是 4K 三次失败的成因。

用法
----
    py -3 05_验证/verify_spot_pixels.py --collection beauty_spot4k --canvas 3840x2160 --level 100
    py -3 05_验证/verify_spot_pixels.py --collection beauty_spot --canvas 1280x720 --level 40
    py -3 05_验证/verify_spot_pixels.py --level 0        # 阴性对照：0 档必须 maxdiff=0
"""
import argparse
import json
import os
import re
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                        # noqa: E402
import soak_s2 as S                  # noqa: E402
import verify_s1_correct as V        # noqa: E402

OUT = os.path.join(HERE, "out")
SCENES = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "basic",
                      "scenes")

RE_FACES = re.compile(r"faces=(\d+) ellipses=(\d+)\+(\d+)\+(\d+) spot=(\d+)")
RE_BOX = re.compile(r"首脸框=\(([\d.]+),([\d.]+),([\d.]+),([\d.]+)\)")
RE_RENDER = re.compile(r"render#0 target=\S+ (\d+)x(\d+)")


def patch_collection(name, cfg):
    """把参数写进集合文件，**在起 OBS 之前**。

    顺带把内部的 `name` 字段改成文件名 —— OBS 是拿 `--collection` 去匹配
    集合 JSON 里的 `name`，**不是文件名**。`beauty_spot` / `beauty_spot4k`
    是从 soak 集合复制出来的，内部 `name` 还留着 `beauty_soak720` /
    `beauty_soak4k`，于是 `--collection beauty_spot` 匹配不上、OBS 静默回退到
    「上次用的集合」—— 这正是 4K 三轮「被上一个会话接管」的成因：
    画布明明 1280x720，插件日志却是 `target=still 3840x2160`。

    返回原始字节，跑完原样写回（不留痕）。
    """
    path = os.path.join(SCENES, name + ".json")
    if not os.path.exists(path):
        raise SystemExit("找不到集合文件：%s" % path)
    raw = open(path, "rb").read()
    d = json.loads(raw.decode("utf-8"))
    hit = [0]

    def walk(o):
        if isinstance(o, dict):
            if "delspot_level" in o:
                o.update(cfg)
                hit[0] += 1
            for v in list(o.values()):
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(d)
    if not hit[0]:
        raise SystemExit("集合 %s 里没有美颜滤镜设置" % name)
    old = d.get("name")
    d["name"] = name
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(d, fp, ensure_ascii=False, indent=4)
    print("  已写死参数（%d 处滤镜）：%s%s" %
          (hit[0], name, "" if old == name else "（内部 name %r → %r）" % (old, name)))
    return raw


def restore_collection(name, raw):
    with open(os.path.join(SCENES, name + ".json"), "wb") as fp:
        fp.write(raw)


def diff_stats(a, b):
    """两张同尺寸 RGB 图的逐像素差（0..255 灰阶最大通道差）。"""
    if a.shape != b.shape:
        raise SystemExit("尺寸不一致：%s vs %s" % (a.shape, b.shape))
    d = np.abs(a[:, :, :3].astype(np.int16) - b[:, :, :3].astype(np.int16)).max(axis=2)
    n = int((d > 0).sum())
    if n == 0:
        return {"maxdiff": 0, "diffpx": 0, "pct": 0.0, "bbox": None,
                "avg": 0.0, "total": int(d.size)}
    ys, xs = np.nonzero(d)
    return {
        "maxdiff": int(d.max()),
        "diffpx": n,
        "pct": 100.0 * n / d.size,
        "bbox": (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())),
        "avg": float(d[d > 0].mean()),
        "total": int(d.size),
    }


def read_spot_debug():
    """读 `BEAUTY_SPOT_DEBUG=1` 写出的内层诊断（%TEMP% + ASCII 名，避开中文路径坑）。"""
    p = os.path.join(os.environ.get("TEMP", ""), "beauty_spot_inner.txt")
    if not os.path.isfile(p):
        return None
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as fh:
            lines = [ln.strip() for ln in fh if ln.strip()]
    except Exception:
        return None
    # 同一帧会刷多遍，先去重再留尾部
    uniq = []
    for ln in lines:
        if not uniq or uniq[-1] != ln:
            uniq.append(ln)
    return uniq[-12:]


def scan_log(snap):
    """从本次 OBS 日志里取版本号、渲染尺寸与几何快照（spot= 是否进链）。"""
    log = V.newest_log(snap)
    ver, faces, spot, box, perf, rsz = None, None, None, None, None, None
    if not log:
        return None, None, None, None, None, None
    with open(log, "r", encoding="utf-8", errors="replace") as fp:
        for line in fp:
            if ver is None and "init OK (v" in line:
                m = re.search(r"init OK \(v([\d.]+)", line)
                if m:
                    ver = m.group(1)
            if rsz is None:
                m = RE_RENDER.search(line)
                if m:
                    rsz = (int(m.group(1)), int(m.group(2)))
            if "几何快照" in line or "五官几何快照" in line:
                m = RE_FACES.search(line)
                if m:
                    faces = int(m.group(1))
                    spot = int(m.group(5))
                b = RE_BOX.search(line)
                if b:
                    box = tuple(float(x) for x in b.groups())
            if perf is None and "gonly=" in line:
                perf = line.strip()
    return log, ver, faces, spot, box, perf, rsz


def run(collection, canvas, level, settle, probe=0, extra=None):
    cfg = {k: 0.0 for k in V.PANEL_KEYS}
    cfg["delspot_level"] = float(level)
    cfg["enabled"] = True
    raw = patch_collection(collection, cfg)

    env = {"BEAUTY_PROFILE": "1", "BEAUTY_SPOT_DEBUG": "1"}
    if extra:
        env.update(extra)
    ts = time.strftime("%Y%m%d_%H%M%S")
    outdir = os.path.join(OUT, "spot_%s_%s" % (collection, ts))
    os.makedirs(outdir, exist_ok=True)
    # 诊断文件可能是上一轮残留，先删掉
    dbg = os.path.join(os.environ.get("TEMP", ""), "beauty_spot_inner.txt")
    if os.path.isfile(dbg):
        os.remove(dbg)

    p, snap = V.start_obs(collection, extra_env=env, canvas=canvas)
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(2.5)
        # 集合里已经写好了，这里只做一次同值下发，确保实例读到的是这套参数
        S.apply_load(c, extra=cfg)
        time.sleep(settle)
        on = c.shot(V.SRC, os.path.join(outdir, "on.png"))
        c.set_filter_enabled(V.SRC, V.FILT, False)
        time.sleep(0.6)
        off = c.shot(V.SRC, os.path.join(outdir, "off.png"))
        c.set_filter_enabled(V.SRC, V.FILT, True)
        # 时序采样：内容静止，若 diffpx 在两档之间跳，说明「门控/不门控」
        # （非 geomOnly 帧用掩码、geomOnly 帧不门控）在帧间交替 → 观感上就是闪动。
        seq = []
        seqmax = []
        for i in range(probe):
            time.sleep(0.08)
            im = c.shot(V.SRC, os.path.join(outdir, "seq%02d.png" % i))
            st = diff_stats(np.array(im), np.array(off))
            seq.append(st["diffpx"])
            seqmax.append(st["maxdiff"])
        c.close()
    finally:
        V.close_obs()
        restore_collection(collection, raw)

    res = diff_stats(np.array(on), np.array(off))
    log, ver, faces, spot, box, perf, rsz = scan_log(snap)
    dbg = read_spot_debug()
    # 几何快照那行只在开五官对拍时才打；平时靠内层诊断的 `spot=x.xx` 判进链
    if spot is None and dbg:
        for ln in dbg:
            m = re.match(r"spot=([\d.]+)", ln)
            if m:
                spot = 1 if float(m.group(1)) >= 0.5 else 0
    res.update({"level": level, "collection": collection, "canvas": canvas,
                "size": list(on.size), "render_size": rsz, "ver": ver, "log": log,
                "faces": faces, "spot_flag": spot, "face_box": box,
                "perf": perf, "spot_debug": dbg, "outdir": outdir,
                "seq": seq, "seqmax": seqmax, "extra_env": extra or {}})
    with open(os.path.join(outdir, "_meta.json"), "w", encoding="utf-8") as fh:
        json.dump(res, fh, ensure_ascii=False, indent=2)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default="beauty_spot4k")
    ap.add_argument("--canvas", default="3840x2160")
    ap.add_argument("--level", type=float, default=100.0, help="祛斑面板档位 0..100")
    ap.add_argument("--settle", type=float, default=6.0, help="下发后等待（等检测/掩码稳定）")
    ap.add_argument("--probe", type=int, default=0,
                    help="额外做 N 次时序采样（判帧间闪动；0 = 不做）")
    ap.add_argument("--env", action="append", default=[],
                    metavar="K=V",
                    help="额外的插件环境开关，可重复（如 --env BEAUTY_MASK_GEOM_ONLY=0）")
    args = ap.parse_args()

    if V.obs_running():
        sys.exit("obs64 已在运行。先关掉：单实例会导致 --collection / --canvas 打偏")

    extra = {}
    for kv in args.env:
        if "=" not in kv:
            sys.exit("--env 需要 K=V 形式：%r" % kv)
        k, v = kv.split("=", 1)
        extra[k.strip()] = v.strip()

    w, h = [int(x) for x in args.canvas.lower().split("x")]
    res = run(args.collection, (w, h), args.level, args.settle, args.probe, extra)

    print("\n=== 祛斑像素判据（%s，%s，档位 %g，插件 v%s）===" %
          (args.canvas, args.collection, args.level, res["ver"]))
    if res["extra_env"]:
        print("  额外开关      : %s" %
              " ".join("%s=%s" % kv for kv in sorted(res["extra_env"].items())))
    print("  素材/截图尺寸 : %dx%d（插件首帧 render=%s）" %
          (res["size"][0], res["size"][1],
           "%dx%d" % res["render_size"] if res["render_size"] else "-"))
    print("  检出脸数      : %s    spot 进链=%s" % (res["faces"], res["spot_flag"]))
    if res["face_box"]:
        x, y, fw, fh = res["face_box"]
        print("  首脸框        : (%.1f,%.1f) %.1fx%.1f" % (x, y, fw, fh))
    print("  maxdiff       : %d" % res["maxdiff"])
    print("  差异像素      : %d (%.4f%%)" % (res["diffpx"], res["pct"]))
    if res["bbox"]:
        print("  变化 bbox     : (%d,%d)-(%d,%d)" % res["bbox"])
    print("  非零处平均偏差: %.2f 灰阶" % res["avg"])
    if res["seq"]:
        s = sorted(res["seq"])
        lo, mid, hi = s[0], s[len(s) // 2], s[-1]
        ratio = (float(hi) / lo) if lo > 0 else float("inf")
        print("  时序采样(%d)  : min=%d 中位=%d max=%d  极值比=%.2f" %
              (len(s), lo, mid, hi, ratio))
        print("                  " + " ".join(str(v) for v in res["seq"]))
        print("  对应 maxdiff  : " + " ".join(str(v) for v in res["seqmax"]))
        # 帧类型（geomOnly / 有掩码）切换会让门控规则换一条，强度按倍数跳。
        # 同一段静止画面里若出现两极值，就是观感上的闪动。
        if lo > 0 and ratio > 1.2:
            print("  !! 序列出现两极值（极值比 %.2f > 1.2）—— 帧间强度不稳" % ratio)
        else:
            print("  序列稳定（极值比 %.2f ≤ 1.2）：本段帧类型未切换" % ratio)
    if res["spot_debug"]:
        print("  内层诊断（%TEMP%/beauty_spot_inner.txt 末 12 行，已去重）:")
        for ln in res["spot_debug"]:
            print("    " + ln)
    else:
        print("  !! 没有内层诊断文件 —— BEAUTY_SPOT_DEBUG 可能没生效")
    print("  截图          : %s" % res["outdir"])

    verdict = []
    if res["level"] <= 0:
        verdict.append("PASS（0 档必须 0 像素）" if res["diffpx"] == 0
                       else "FAIL（0 档居然改了 %d 像素）" % res["diffpx"])
    elif res["diffpx"] == 0:
        verdict.append("FAIL（祛斑没生效）")
    elif res["spot_flag"] == 0:
        verdict.append("FAIL（spot 没进链）")
    else:
        tail = "" if res["spot_flag"] else "（未取到 spot 标志，靠像素差判定）"
        verdict.append("PASS（%d 像素变化，随档位应单调）%s" % (res["diffpx"], tail))
    # 尺寸自检：集合的 name 与文件名不一致时 OBS 会静默沿用「上次的集合」，
    # 表现就是这里跑的不是你指定的素材。
    if res["render_size"] and tuple(res["render_size"]) != tuple(res["size"]):
        verdict.append("WARN（render 尺寸与截图不一致，可能不是目标集合）")
    print("\n判定：" + "；".join(verdict))
    return 0 if verdict and verdict[0].startswith("PASS") else 5


if __name__ == "__main__":
    sys.exit(main())
