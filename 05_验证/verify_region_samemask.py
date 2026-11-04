#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P0-7 定案判据：同一份掩码 + 两种算法（2.8.3）。

背景
----
P0-7 原来叫「五官像素差未定位」。2.8.2 已排除三件事：
  · 面板尺度（脚本传 0..1，面板是 0..100）—— 已改，五官真跑在 50% 强度上；
  · 子项归属（eye/pouch/naso/spot 四路单独开，差异一模一样）；
  · GPU 算法本身（`BEAUTY_REGION_CMP=1` 的同帧自检 maxdiff=0）。
但那个自检有致命前提：它要求 `job.mask` 非空，而 S7 静止降频之后绝大多数帧是
geomOnly（没有 CPU 掩码），所以真机上几乎从不触发 —— 3 次采集只碰巧中 1 次。

2.8.3 改的就是这一条：geomOnly 帧先把 **GPU 本帧刚画好的生产掩码**读回，当 CPU
基准的掩码。于是两边用的是同一份掩码，差异只剩算法本身。

判据
----
  · 每个样本 maxdiff ≤ 1                       → PASS（算法一致）
  · 至少一个样本 `掩码来源=GPU`                → 否则等于没验 geomOnly 路径，
                                                 记为 INCONCLUSIVE
  · 掩码覆盖率 > 0（不是空掩码假通过）          → 覆盖率 0 的样本插件自己会跳过

用法
----
    py -3 05_验证/verify_region_samemask.py
    py -3 05_验证/verify_region_samemask.py --canvas 3840x2160 --samples 5
    py -3 05_验证/verify_region_samemask.py --gpu-region 0   # 五官走 CPU 生产路径

注意：一次对拍 = 3 次全帧回读 + 一遍 CPU 五官，4K 下会让那一帧明显变长。
这是诊断路径，只在显式开 `BEAUTY_REGION_CMP=1` 时发生。
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import soak_s2 as S                # noqa: E402
import verify_s1_correct as V      # noqa: E402

SCENES = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "basic",
                      "scenes")


def patch_collection(name, cfg):
    """把参数直接写进集合文件，**在起 OBS 之前**。

    为什么不在起来之后用 obsctl 下发：插件的对拍是「第一个满足条件的帧就
    取样」，而 OBS 从启动到 WebSocket 可用要 2~4 s —— 等下发完，样本早取完了
    （第一版就是这么错的：`--only spot` 明明把另外三项置 0，日志里却仍是
    `ellipses=6+2+2`，量到的是上一次会话存下来的参数）。写进集合就没有时序问题。

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
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(d, fp, ensure_ascii=False, indent=4)
    print("  已写死参数（%d 处滤镜）：%s" % (hit[0], name))
    return raw


def restore_collection(name, raw):
    path = os.path.join(SCENES, name + ".json")
    with open(path, "wb") as fp:
        fp.write(raw)

RE_SAMPLE = re.compile(
    r"五官整段对拍 (\d+)x(\d+)（样本 (\d+)/(\d+)）：maxdiff=(\d+) "
    r"差异像素=(\d+)\(([\d.]+)%\) bbox=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) "
    r"平均偏差=([\d.]+) \| 掩码来源=(\w+) 覆盖率=([\d.]+)% 均值=([\d.]+) \| "
    r"faces=(\d+) ellipses=(\d+)\+(\d+)\+(\d+) spot=(\d+) 判定=(\S+)")
RE_WAIT = re.compile(r"五官对拍等待中：本帧掩码来源=(\w+)")
RE_GEOM = re.compile(r"五官几何快照：(.*)$")


# 2.13.2（T10-b）：「立体」补进来了。它一直不在 LOAD_REGION 里，所以 2.8.3 起的
# 每一次五官对拍都是 `els=6+2+2`（只有亮眼/眼袋），**立体那 8 个椭圆从没被验过**。
# 全开应当是 `14+2+2`。ONLY 里显式写 face_threed:0 只是让「只开 X」的语义完整，
# 数值上与历史一致（LOAD_REGION 本来就没这个键）。
ONLY = {
    "threed": {"face_threed": 60, "eye_bright": 0,
               "remove_pouch_strength": 0,
               "remove_nasolabial_folds_strength": 0, "delspot_level": 0},
    "eye":   {"face_threed": 0, "eye_bright": 50,
              "remove_pouch_strength": 0,
              "remove_nasolabial_folds_strength": 0, "delspot_level": 0},
    "pouch": {"face_threed": 0, "eye_bright": 0,
              "remove_pouch_strength": 50,
              "remove_nasolabial_folds_strength": 0, "delspot_level": 0},
    "naso":  {"face_threed": 0, "eye_bright": 0, "remove_pouch_strength": 0,
              "remove_nasolabial_folds_strength": 50, "delspot_level": 0},
    "spot":  {"face_threed": 0, "eye_bright": 0, "remove_pouch_strength": 0,
              "remove_nasolabial_folds_strength": 0, "delspot_level": 40},
}

# 五官五族的面板键（顺序与 beauty_params.h 的 P_THREED… 一致）。
REGION_KEYS = ["face_threed", "eye_bright", "remove_pouch_strength",
               "remove_nasolabial_folds_strength", "delspot_level"]
ONLY_KEY = {"threed": "face_threed", "eye": "eye_bright",
            "pouch": "remove_pouch_strength",
            "naso": "remove_nasolabial_folds_strength", "spot": "delspot_level"}


def build_cfg(region, only, zero, strength=None):
    """面板刻度是 0..100（beauty_params.h 的 g_params 注释），不是 0..1。

    `strength` 只在显式给出时覆盖，默认 None = 沿用历史口径（eye/pouch/naso 50、
    spot 40），否则与 §9.20/§9.26 的历史数据不可比。
    """
    cfg = {k: 0.0 for k in V.PANEL_KEYS}
    cfg.update(S.LOAD)
    if region and not zero:
        cfg.update(S.LOAD_REGION)
    if only:
        cfg.update(ONLY[only])
    if strength is not None and region and not zero:
        if only:
            cfg[ONLY_KEY[only]] = float(strength)
        else:
            for k in REGION_KEYS:
                cfg[k] = float(strength)
    cfg["enabled"] = True
    return cfg


def run(canvas, collection, samples, gpu_region, only, zero, delay,
        strength=None):
    env = {
        "BEAUTY_GPU_REGION": "1" if gpu_region else "0",
        "BEAUTY_REGION_CMP": "1",
        "BEAUTY_REGION_CMP_N": str(samples),
        "BEAUTY_REGION_CMP_DELAY": str(delay),
    }
    cfg = build_cfg(not zero, only, zero, strength)
    raw = patch_collection(collection, cfg)
    print("  起 OBS：BEAUTY_GPU_REGION=%d BEAUTY_REGION_CMP=1 N=%d delay=%d%s%s"
          % (1 if gpu_region else 0, samples, delay,
             "" if not only else " only=%s" % only,
             "" if strength is None else " strength=%g" % strength))
    try:
        p, snap = V.start_obs(collection, extra_env=env, canvas=canvas)
        try:
            c = obsctl.wait_ready(wait=90)
            # 参数在集合里已经是对的；这里只是确认滤镜处于启用态并触发一次
            # 「设置变更 → 重建掩码」，值完全相同，不会改变测量口径。
            # ⚠ 必须把 cfg 里那 5 个五官值原样再下发一遍：apply_load 会先铺一遍
            # LOAD_REGION（50/40），自定义 strength 会被它覆盖回去。
            S.apply_load(c, region=False,
                         extra={k: cfg[k] for k in REGION_KEYS})
            # 取样发生在 delay 个「可对拍帧」之后（30fps × 120 ≈ 4 s），
            # 所以这里必须等够，否则不等样本写进日志就关 OBS。
            time.sleep(max(4.0, delay / 30.0 + 3.0))
            c.close()
        finally:
            V.close_obs()
    finally:
        restore_collection(collection, raw)
    return snap


def parse_log(snap):
    log = V.newest_log(snap)
    if not log:
        print("  !! 没找到本次 OBS 日志")
        return None, [], [], None
    samples, waits, ver, perf, geom = [], [], None, None, None
    with open(log, "r", encoding="utf-8", errors="replace") as fp:
        for line in fp:
            if ver is None and "init OK (v" in line:
                m = re.search(r"init OK \(v([\d.]+)", line)
                if m:
                    ver = m.group(1)
            else:
                m = RE_SAMPLE.search(line)
                if m:
                    g = m.groups()
                    samples.append({
                        "w": int(g[0]), "h": int(g[1]),
                        "i": int(g[2]), "n": int(g[3]),
                        "maxdiff": int(g[4]),
                        "diffpx": int(g[5]), "pct": float(g[6]),
                        "bbox": tuple(int(x) for x in g[7:11]),
                        "avg": float(g[11]),
                        "mask_src": g[12],
                        "mask_pct": float(g[13]), "mask_avg": float(g[14]),
                        "faces": int(g[15]),
                        "els": (int(g[16]), int(g[17]), int(g[18])),
                        "spot": int(g[19]),
                        "verdict": g[20],
                    })
                    continue
                m = RE_GEOM.search(line)
                if m and geom is None:
                    geom = m.group(1).strip()
                    continue
                m = RE_WAIT.search(line)
                if m:
                    waits.append(m.group(1))
                if "gonly=" in line:
                    perf = line.strip()
    print("  日志：%s" % log)
    return ver, samples, waits, perf, geom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--canvas", default="3840x2160")
    ap.add_argument("--collection", default="beauty_soak4k")
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--gpu-region", type=int, default=1, choices=(0, 1))
    ap.add_argument("--only", choices=["threed", "eye", "pouch", "naso",
                                       "spot"])
    ap.add_argument("--zero", action="store_true")
    ap.add_argument("--strength", type=float, default=None,
                    help="五官强度（面板 0..100）。不给 = 沿用历史口径"
                         "（eye/pouch/naso 50、threed 60、spot 40）")
    ap.add_argument("--delay", type=int, default=120,
                    help="预热帧数（可对拍帧），30fps 下 120 ≈ 4 s；默认 120")
    args = ap.parse_args()

    w, h = [int(x) for x in args.canvas.lower().split("x")]
    snap = run((w, h), args.collection, args.samples, bool(args.gpu_region),
               args.only, args.zero, args.delay, args.strength)
    ver, samples, waits, perf, geom = parse_log(snap)

    print()
    print("=== 对拍结果（%s，插件 v%s）===" % (args.canvas, ver))
    if geom:
        print("  几何快照：%s" % geom)
    if not samples:
        print("  没有任何对拍样本。可能原因：")
        print("   · 本帧不是单脸 / 五官几何为空（rg.faceCount != 1）")
        print("   · 掩码全 0（等待次数 %d）" % len(waits))
        print("  判定：**INCONCLUSIVE**（没取到样本，不能当通过也不能当失败）")
        return 2

    for s in samples:
        print("  样本 %d/%d：maxdiff=%-3d 差异 %d(%.3f%%) bbox=%s "
              "掩码=%s 覆盖 %.1f%% 均值 %.1f els=%d+%d+%d spot=%d → %s" % (
                  s["i"], s["n"], s["maxdiff"], s["diffpx"], s["pct"],
                  s["bbox"], s["mask_src"], s["mask_pct"], s["mask_avg"],
                  s["els"][0], s["els"][1], s["els"][2], s["spot"],
                  s["verdict"]))
    if waits:
        print("  （等待掩码非零 %d 次，来源=%s）" % (len(waits), ",".join(
            sorted(set(waits)))))

    # 参数一致性自检：`--only X` 时**别的族**必须不在链上。
    # 旧写法只认「全开签名 6+2+2」，可 2.13.2 起全开会随 face_threed 变化（14+2+2），
    # 而且 eyebright 与 pouch 都落在 els[0] 这一桶里，按计数区分不了。改成按
    # `build_cfg` 的期望逐族核对，比「不等于 6+2+2」严格。
    cfg = build_cfg(not args.zero, args.only, args.zero)
    sig = samples[0]["els"]
    exp_main = (cfg["face_threed"] > 0 or cfg["eye_bright"] > 0 or
                cfg["remove_pouch_strength"] > 0)
    exp_naso = cfg["remove_nasolabial_folds_strength"] > 0
    exp_spot = cfg["delspot_level"] > 0
    bad = []
    if exp_main != (sig[0] > 0):
        bad.append("主椭圆族 期望%s 实得 els[0]=%d"
                   % ("有" if exp_main else "无", sig[0]))
    if exp_naso != (sig[1] > 0):
        bad.append("法令纹 期望%s 实得 els[1]=%d"
                   % ("有" if exp_naso else "无", sig[1]))
    if exp_spot != (samples[0]["spot"] > 0):
        bad.append("祛斑 期望%s 实得 spot=%d"
                   % ("有" if exp_spot else "无", samples[0]["spot"]))
    if bad:
        print("  ⚠ 参数未生效（本次结论无效）：%s" % "；".join(bad))
        return 2

    gpu_src = [s for s in samples if s["mask_src"] == "GPU"]
    worst = max(s["maxdiff"] for s in samples)
    print()
    print("  样本数 %d（其中 geomOnly/GPU 掩码 %d 个），最坏 maxdiff=%d" % (
        len(samples), len(gpu_src), worst))
    if not gpu_src:
        print("  判定：**INCONCLUSIVE** —— 全部样本走的是 CPU 掩码，"
              "geomOnly 路径（P0-7 的争议点）一次都没验到")
        return 2
    if worst <= 1:
        print("  判定：**PASS** —— 同一份掩码下 GPU 与 CPU 五官算法一致（≤1 灰阶）")
        return 0
    print("  判定：**FAIL** —— 同一份掩码下仍有 %d 灰阶差异，算法本身不同" % worst)
    if perf:
        print("  参考性能行：%s" % perf[:200])
    return 1


if __name__ == "__main__":
    sys.exit(main())
