#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T10-b：五官 GPU 段（`BEAUTY_GPU_REGION`）的等价判据 —— 矩阵版。

为什么要有这支
--------------
2.8.3 的同帧自检（`verify_region_samemask.py`）已经能给出「同一份掩码 + 两种算法」
的逐像素结论，但它只跑过**一格**：全开、默认强度、单脸大脸、720p 与 4K 各 5 帧。
而且 `LOAD_REGION` 里根本没有 `face_threed` —— 历史每一格都是 `els=6+2+2`，
**「立体」那 8 个椭圆从来没进过对拍**（全开应是 `14+2+2`）。

判据（E1–E5）
-------------
  E1 逐通道 maxdiff ≤ 1        —— 1 个 UNORM8 灰阶。依据是 §9.35 的人眼 A/B：
                                 宽带 ±6 灰阶才刚好压到门限，1 灰阶不可见。
  E2 每样本掩码覆盖率 ≥ 1%     —— 空掩码下两路都不碰像素，maxdiff 恒 0，是
                                 最典型的假通过（§9.18 / §9.20.1）。阈值取 1%
                                 而不是更高：1080p 的 1% = 2 万像素，已足够证明
                                 「真的碰到了像素」；真人素材的脸只占画面 4%，
                                 用 5% 会把合法样本误杀（第一次跑就是这么错的）。
  E3 每格至少 1 个样本来自 GPU 掩码 —— 否则验的是 CPU 掩码，geomOnly 路径没碰到。
  E4 参数确实进了链            —— 按族核对 `els[0]/els[1]/spot` 的存在性，
                                 防「参数没下发、量到上一次会话」。
  E5 同一格内 maxdiff 极差 = 0  —— 静态源应该逐位可复现；有抖动说明取样混进了
                                 非确定成分（只对静态素材要求，视频素材跳过）。

样本独立性（这里最容易自欺）
--------------------------
同一格的 3 个样本是**同一张静态图的连续帧**，本质是 1 个样本。真正的独立性来自
「不同格」：不同族、不同强度、不同分辨率、不同素材。所以判据按**格**统计，
不把 3 帧当 3 个独立证据。视频那一格（真人素材）的样本才是逐帧独立的。

多脸为什么不在判据里
------------------
`beauty_region_chain()` 第一行就 `rg.faceCount != 1 → return NULL`，五官段退回 CPU
（`region_blocks` 为真，细节/美形也一起退）。所以多脸不是「未验的风险」，而是
**设计内的回退**：`GPU_REGION=0` 与 `=1` 应该走同一条路径、输出逐字节相同。
`--multiface` 就是把这条回退验成事实（4 脸素材，两臂程序截图对拍）。

用法
----
    py -3 05_验证/verify_region_equiv.py              # 全矩阵（约 6 分钟）
    py -3 05_验证/verify_region_equiv.py --quick      # 立体 + 全开（约 3 分钟）
    py -3 05_验证/verify_region_equiv.py --match 遮挡  # 只补遮挡那一格
    py -3 05_验证/verify_region_equiv.py --multiface  # 只验多脸回退
    py -3 05_验证/verify_region_equiv.py --dry-run    # 只打印矩阵，不起 OBS

⚠ OBS 必须关着（`verify_s1_correct.start_obs` 会直接退出）。
⚠ 参数写进集合文件是在**起 OBS 之前**（§9.20.4 的坑），脚本跑完原样写回。
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np                 # noqa: E402

import obsctl                      # noqa: E402
import soak_s2 as S                # noqa: E402
import verify_s1_correct as V      # noqa: E402
import verify_region_samemask as R  # noqa: E402

OUT = os.path.join(HERE, "out", "logs")

# (画布, 集合, only, 强度, 样本数, 说明)
# 强度 50/40 = 面板中值（历史口径）；100 = 面板上限；60 = 立体的历史口径。
CELLS = [
    ("1280x720",  "beauty_front720", "threed", 60,  3, "立体 60（历史从未覆盖）"),
    ("1280x720",  "beauty_front720", "threed", 100, 3, "立体 100 上限"),
    ("1280x720",  "beauty_front720", "eye",    50,  3, "亮眼"),
    ("1280x720",  "beauty_front720", "pouch",  50,  3, "去眼袋"),
    ("1280x720",  "beauty_front720", "naso",   50,  3, "法令纹"),
    ("1280x720",  "beauty_front720", "spot",   40,  3, "祛斑"),
    ("1280x720",  "beauty_front720", None,     50,  3, "五族全开 50"),
    ("1280x720",  "beauty_front720", None,     100, 3, "五族全开 100"),
    ("3840x2160", "beauty_front4k",  "threed", 60,  3, "立体 4K"),
    ("3840x2160", "beauty_front4k",  None,     50,  3, "五族全开 4K 50"),
    ("3840x2160", "beauty_front4k",  None,     100, 3, "五族全开 4K 100"),
    ("3840x2160", "beauty_front4k",  "spot",   40,  3, "祛斑 4K"),
    # 真人视频，逐帧几何都在变 —— 这 6 个样本才是真正独立的样本
    ("1080x1920", "beauty_real_cam_1080x1920", None, 50, 6,
     "真人视频 6 帧（样本独立）"),
    # 遮挡格（2026-11-04 真实采集素材 86–120s）：脸被包装盒反复遮住/露出，
    # 几何在遮挡边界反复重建，取样会落在「遮挡刚结束、几何刚重建」的帧上。
    # 补的是 §9.37.6 未验项 2 里「动态/遮挡」这一半（侧脸那半仍未覆盖）。
    ("1920x1080", "beauty_real_cam_occ1080", None, 50, 6,
     "真人视频·遮挡段 6 帧"),
]

QUICK = [c for c in CELLS if c[2] in ("threed",) or c[2] is None]


def grade(cell, ver, samples, sem):
    """按 E1–E5 给一格打结论。返回 (verdict, [原因...])。"""
    canvas, coll, only, strength, want, desc = cell
    why = []
    if not samples:
        return "INCONCLUSIVE", ["没取到样本（非单脸 / 几何为空 / 掩码全 0）"]
    if len(samples) < want:
        why.append("样本不足 %d/%d" % (len(samples), want))
    worst = max(s["maxdiff"] for s in samples)
    if worst > 1:
        why.append("E1 maxdiff=%d >1" % worst)
    low = [s for s in samples if s["mask_pct"] < 1.0]
    if low:
        why.append("E2 覆盖率 %.2f%% <1%%（空掩码假通过）" % low[0]["mask_pct"])
    if not any(s["mask_src"] == "GPU" for s in samples):
        why.append("E3 全部样本走 CPU 掩码，geomOnly 路径没验到")
    cfg = R.build_cfg(True, only, False, strength)
    exp = (cfg["face_threed"] > 0 or cfg["eye_bright"] > 0 or
           cfg["remove_pouch_strength"] > 0,
           cfg["remove_nasolabial_folds_strength"] > 0,
           cfg["delspot_level"] > 0)
    got = (samples[0]["els"][0] > 0, samples[0]["els"][1] > 0,
           samples[0]["spot"] > 0)
    if exp != got:
        why.append("E4 期望(主,法令纹,祛斑)=%s 实得 %s" % (
            "".join("1" if x else "0" for x in exp),
            "".join("1" if x else "0" for x in got)))
    if sem == "static":
        spread = max(s["maxdiff"] for s in samples) - min(
            s["maxdiff"] for s in samples)
        if spread != 0:
            why.append("E5 同格 maxdiff 极差 %d≠0" % spread)
    if why:
        return "FAIL", why
    return "PASS", []


def run_matrix(cells):
    rows = []
    for i, cell in enumerate(cells, 1):
        canvas, coll, only, strength, want, desc = cell
        w, h = [int(x) for x in canvas.split("x")]
        sem = "video" if coll.startswith("beauty_real_cam") else "static"
        print("\n[%d/%d] %s ｜ %s %s 强度 %s ｜ %s" % (
            i, len(cells), desc, canvas, "only=%s" % only if only else "全开",
            strength, coll))
        snap = R.run((w, h), coll, want, True, only, False, 120, strength)
        ver, samples, waits, perf, geom = R.parse_log(snap)
        verdict, why = grade(cell, ver, samples, sem)
        worst = max([s["maxdiff"] for s in samples], default=-1)
        pct = max([s["pct"] for s in samples], default=-1.0)
        rows.append({
            "desc": desc, "canvas": canvas, "only": only or "全开",
            "strength": strength, "ver": ver, "n": len(samples),
            "worst": worst, "pct": pct, "verdict": verdict, "why": why,
            "els": samples[0]["els"] if samples else None,
            "spot": samples[0]["spot"] if samples else None,
            "mask_src": ",".join(sorted({s["mask_src"] for s in samples})),
            "mask_pct": samples[0]["mask_pct"] if samples else 0.0,
            "md_list": [s["maxdiff"] for s in samples],
            "diffpx_list": [s["diffpx"] for s in samples],
        })
        print("  → %s  最坏 maxdiff=%s  差异占比=%.4f%%  样本=%d  els=%s spot=%s"
              % (verdict, worst, pct, len(samples),
                 rows[-1]["els"], rows[-1]["spot"]))
        if why:
            for x in why:
                print("     · %s" % x)
    return rows


def report(rows, path):
    ok = [r for r in rows if r["verdict"] == "PASS"]
    bad = [r for r in rows if r["verdict"] != "PASS"]
    lines = []
    lines.append("# T10-b 五官 GPU 段等价判据 —— 矩阵结果\n")
    lines.append("- 生成：%s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    vers = sorted({str(r["ver"]) for r in rows})
    lines.append("- 插件版本：%s" % ", ".join(vers))
    lines.append("- 判据：E1 maxdiff≤1、E2 覆盖率≥1%、E3 至少一个 GPU 掩码样本、"
                 "E4 参数确实进链、E5 静态格内极差=0")
    lines.append("- 结论：**%d/%d 格 PASS**%s\n" % (
        len(ok), len(rows), "" if not bad else "，%d 格未过" % len(bad)))
    lines.append("| 格 | 画布 | 参数 | 强度 | 样本 | 逐样本 maxdiff | 最坏 | "
                 "差异像素 | 掩码 | 覆盖 | els | spot | 判定 |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        lines.append("| %s | %s | %s | %g | %d | `%s` | %s | %s | %s | "
                     "%.1f%% | %s | %s | **%s** |" % (
                         r["desc"], r["canvas"], r["only"], r["strength"],
                         r["n"],
                         ",".join(str(x) for x in r["md_list"]) or "--",
                         r["worst"],
                         ",".join(str(x) for x in r["diffpx_list"]) or "--",
                         r["mask_src"], r["mask_pct"],
                         "+".join(str(x) for x in r["els"])
                         if r["els"] else "--",
                         r["spot"] if r["spot"] is not None else "--",
                         r["verdict"]))
    if bad:
        lines.append("\n## 未过的格\n")
        for r in bad:
            lines.append("- **%s**（%s）" % (r["desc"], r["canvas"]))
            for x in r["why"]:
                lines.append("  - %s" % x)
    text = "\n".join(lines) + "\n"
    os.makedirs(OUT, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(text)
    print("\n" + text)
    print("已写：%s" % path)
    return not bad


# ---------------------------------------------------------------------------
# 多脸回退：GPU_REGION=0 与 =1 必须逐字节相同（五官链在 faceCount≠1 时主动放弃）
# ---------------------------------------------------------------------------
def multiface(canvas="3840x2160", collection="beauty_static_4k",
              strength=50, tag="multiface"):
    """4 脸素材，两臂各抓一张源截图，逐像素比。返回 (maxdiff, 说明)。"""
    w, h = [int(x) for x in canvas.split("x")]
    cfg = R.build_cfg(True, None, False, strength)
    dst = os.path.join(HERE, "out", "region_equiv")
    os.makedirs(dst, exist_ok=True)
    shots = {}
    raws = {}
    for arm in (0, 1):
        name = "gpu_region%d" % arm
        print("\n[多脸 %s] 起 OBS（BEAUTY_GPU_REGION=%d）" % (name, arm))
        raws[arm] = R.patch_collection(collection, cfg)
        try:
            p, snap = V.start_obs(collection,
                                  extra_env={"BEAUTY_GPU_REGION": str(arm)},
                                  canvas=(w, h))
            try:
                c = obsctl.wait_ready(wait=90)
                S.apply_load(c, region=False,
                             extra={k: cfg[k] for k in R.REGION_KEYS})
                time.sleep(5.0)
                os.makedirs(dst, exist_ok=True)
                path = os.path.join(dst, "%s_%s.png" % (tag, name))
                c.shot(V.SRC, path)
                shots[arm] = path
                c.close()
            finally:
                V.close_obs()
        finally:
            R.restore_collection(collection, raws[arm])
    from PIL import Image
    a = np.asarray(Image.open(shots[0]).convert("RGB")).astype(np.int16)
    b = np.asarray(Image.open(shots[1]).convert("RGB")).astype(np.int16)
    if a.shape != b.shape:
        return -1, "两臂尺寸不同 %s vs %s" % (a.shape, b.shape)
    d = np.abs(a - b)
    md = int(d.max())
    nz = int((d.max(axis=2) > 0).sum())
    note = "两臂 %dx%d，maxdiff=%d，差异像素=%d(%.4f%%)" % (
        a.shape[1], a.shape[0], md, nz, 100.0 * nz / (a.shape[0] * a.shape[1]))
    print("  " + note)
    return md, note


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="只跑立体 + 全开")
    ap.add_argument("--multiface", action="store_true", help="只验多脸回退")
    ap.add_argument("--dry-run", action="store_true", help="只打印矩阵")
    ap.add_argument("--match", default=None,
                    help="只跑说明里含该子串的格（如 遮挡），且不跑多脸回退")
    args = ap.parse_args()

    cells = QUICK if args.quick else CELLS
    if args.match:
        cells = [c for c in cells if args.match in c[5]]
        if not cells:
            sys.exit("--match %r 没匹配到任何格" % args.match)
        print("--match %r → %d 格" % (args.match, len(cells)))
    if args.dry_run:
        print("%-32s %-11s %-7s %-5s %s" % ("格", "画布", "参数", "强度", "样本"))
        for c in cells:
            print("%-32s %-11s %-7s %-5g %d" % (
                c[5], c[0], c[2] or "全开", c[3], c[4]))
        return 0

    if args.multiface:
        md, note = multiface()
        print("\n判定：%s" % ("**PASS** —— 多脸两臂逐字节相同，回退如设计"
                             if md == 0 else
                             "**FAIL** —— 两臂不同，说明 GPU_REGION 在多脸下"
                             "改变了路径，需查明"))
        return 0 if md == 0 else 1

    stamp = time.strftime("%Y%m%d_%H%M%S")
    rows = run_matrix(cells)
    suffix = "" if not args.match else "_" + args.match
    ok = report(rows, os.path.join(OUT, "T10b_region_equiv%s_%s.md" % (
        suffix, stamp)))

    # 只补某一格时不再顺带跑多脸（那要多起两次 OBS），除非显式 --multiface
    if not args.match:
        print("\n=== 多脸回退 ===")
        md, note = multiface()
        ok = ok and md == 0
    # --match 的全局 PASS 只代表「这一格」，报告标题里已写明格数
    print("\n总判定：%s" % ("**PASS**" if ok else "**FAIL/INCONCLUSIVE**"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
