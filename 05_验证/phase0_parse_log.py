#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OBS 美颜插件性能日志解析/聚合器（Phase 0 起，3.1.51 扩到分项）。

用法:
    python phase0_parse_log.py [日志路径 ...] [--csv out.csv] [--list]

  · 不传路径: 自动扫描 %AppData%\\obs-studio\\logs\\ 下最新的 .txt 日志。
  · 传路径:   解析指定日志（可多个）。目录也行（取其下所有 .txt）。
  · --list:   额外逐窗口列出原始值（默认只打聚合表）。
  · --csv:    把「按窗口」的数据写成 CSV，便于自己再画图。

每个 180 帧窗口会打**两行**，本脚本把它们配对使用：

  [obs_beauty] 性能 1280x720：下载 0.98 + 引擎 3.06 + 上传 0.55 = 4.58 ms/帧
               （180 帧平均）掩码 11.54% 有脸 100% 对齐 168/180（93%）边框 15.9%
  [obs_beauty] seg 分项 1280x720：掩码 0.20 + 磨皮 1.74 + 色调 0.00 + 五官 0.00
               + 细节 1.01 + 美形 0.00 = 2.95 ms（引擎 3.06，差 0.11）
               gpusm=180/180(100%) sm=0.04/0.81/0.34(n=180) prep=0.55/0.30

配对规则：同一文件内「第 i 条 seg 行」对应「第 i 条同分辨率性能行」——插件就是
先打性能行、紧接着打分项行，顺序固定。旧版本（< 3.1.47）没有 seg 行，那些列
显示 '-'，不报错。行尾的 gpusm= / sm= / prep= 都是**追加**的 ASCII 锚点，缺哪个
都不影响前面的解析（3.1.49 / 3.1.50 / 3.1.51 各自加了一项）。

⚠ 比较两个窗口的毫秒数之前，先确认「掩码% / 有脸%」接近。Phase 0 实测同分辨率
  下引擎耗时可差 2.3 倍，唯一变量就是这两个数——差异来自画面而不是代码。
"""
import os
import re
import sys
import csv
import glob

# ---- 性能行（3.1.44 起带掩码统计，3.1.48 起带边框）------------------------
LINE_RE = re.compile(
    r"性能\s+(\d+)x(\d+)\s*：\s*下载\s+([\d.]+)\s*\+\s*引擎\s+([\d.]+)\s*\+\s*上传\s+([\d.]+)\s*=\s*([\d.]+)\s*ms/帧"
)
MASK_RE = re.compile(r"掩码\s+([\d.]+)%\s*有脸\s+([\d.]+)%")
# 3.1.48：ROI 包围盒占比（人脸框并集 / 整帧），追加在性能行末尾。
BOUNDS_RE = re.compile(r"边框\s+([\d.]+)%")
# 3.1.10：掩码↔帧对齐（本窗口差值）。形如「对齐 168/180（93%）」。
ALIGN_RE = re.compile(r"对齐\s+(\d+)/(\d+)\s*（\s*([\d.]+)%\s*）")

# ---- 分项行（3.1.47 起）---------------------------------------------------
SEG_RE = re.compile(r"seg\s+分项\s+(\d+)x(\d+)\s*：(.+)$")
# 「标签 数字」成对抽，标签即 beautyCoreSegLabel 的 6 个中文名。
SEG_ITEM_RE = re.compile(r"([\u4e00-\u9fa5]{2})\s+([\d.]+)")
SEG_SUM_RE = re.compile(r"=\s*([\d.]+)\s*ms（引擎\s+([\d.]+)，差\s+(-?[\d.]+)")
# 3.1.49：磨皮接管率  gpusm=命中/调用(占比%)
GPUSM_RE = re.compile(r"gpusm=(\d+)/(\d+)\(")
# 3.1.50：磨皮三段  sm=提交/回读/拷贝(n=帧数)
SM_RE = re.compile(r"sm=([\d.]+)/([\d.]+)/([\d.]+)\(n=(\d+)\)")
# 3.1.51：准备段  prep=合计/其中掩码
PREP_RE = re.compile(r"prep=([\d.]+)/([\d.]+)")
# 3.1.52：段区间钩子  chain=命中/询问，ch=色调/细节（提交时间）
CHAIN_RE = re.compile(r"chain=(\d+)/(\d+)")
CH_RE = re.compile(r"ch=([\d.]+)/([\d.]+)")
# 3.1.57 / T2：本窗口跑的是哪条臂。追加在**性能行**末尾（不是 seg 行），
# 因为 A/B 要比的是 下载/引擎/上传/往返 这四个数，它们都在性能行上。
# 3.1.58：多了 out / rb 一对（BEAUTY_AB_TARGET=out 时翻 5b-1 开关）。
AB_RE = re.compile(r"ab=(chain|1seg|out|rb)")

SEG_KEYS = ["掩码", "磨皮", "色调", "五官", "细节", "美形"]


def scan_default_log():
    appdata = os.environ.get("AppData") or os.path.expanduser("~\\AppData\\Roaming")
    logdir = os.path.join(appdata, "obs-studio", "logs")
    if not os.path.isdir(logdir):
        return None
    txts = glob.glob(os.path.join(logdir, "*.txt"))
    if not txts:
        return None
    return max(txts, key=os.path.getmtime)


def expand_paths(paths):
    """目录 → 其下所有 .txt；文件原样保留。"""
    out = []
    for p in paths:
        if os.path.isdir(p):
            out.extend(sorted(glob.glob(os.path.join(p, "*.txt"))))
        else:
            out.append(p)
    return out


def _f(m, i):
    return float(m.group(i)) if m else None


def parse_file(path):
    """返回 (perf_rows, seg_rows)。

    perf_rows: dict(res, dl, pr, up, tot, cover, faces, align, bounds)
    seg_rows:  dict(res, seg{6}, seg_sum, eng, diff, gpusm, sm, prep)
    """
    perf, segs = [], []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = LINE_RE.search(line)
                if m:
                    w, h, dl, pr, up, tot = m.groups()
                    mm = MASK_RE.search(line)
                    bm = BOUNDS_RE.search(line)
                    am = ALIGN_RE.search(line)
                    ab = AB_RE.search(line)
                    perf.append({
                        "res": "%sx%s" % (w, h),
                        "w": int(w), "h": int(h),
                        "dl": float(dl), "pr": float(pr),
                        "up": float(up), "tot": float(tot),
                        "cover": _f(mm, 1),
                        "faces": _f(mm, 2),
                        "align": _f(am, 3),
                        "bounds": _f(bm, 1),
                        "ab": ab.group(1) if ab else None,
                    })
                    continue

                ms = SEG_RE.search(line)
                if ms:
                    body = ms.group(3)
                    seg = {k: None for k in SEG_KEYS}
                    for k, v in SEG_ITEM_RE.findall(body):
                        if k in seg:
                            seg[k] = float(v)
                    sm_m = SEG_SUM_RE.search(body)
                    gm = GPUSM_RE.search(body)
                    s3 = SM_RE.search(body)
                    pm = PREP_RE.search(body)
                    cm = CHAIN_RE.search(body)
                    ch = CH_RE.search(body)
                    segs.append({
                        "res": "%sx%s" % (ms.group(1), ms.group(2)),
                        "seg": seg,
                        "seg_sum": _f(sm_m, 1),
                        "eng": _f(sm_m, 2),
                        "diff": _f(sm_m, 3),
                        "gpusm_hit": int(gm.group(1)) if gm else None,
                        "gpusm_call": int(gm.group(2)) if gm else None,
                        "sm_draw": _f(s3, 1), "sm_map": _f(s3, 2),
                        "sm_copy": _f(s3, 3),
                        "sm_n": int(s3.group(4)) if s3 else None,
                        "prep": _f(pm, 1), "prep_mask": _f(pm, 2),
                        "chain_hit": int(cm.group(1)) if cm else None,
                        "chain_call": int(cm.group(2)) if cm else None,
                        "ch_tone": _f(ch, 1), "ch_detail": _f(ch, 2),
                    })
    except Exception as e:
        print("  [warn] 读取 %s 失败: %s" % (path, e), file=sys.stderr)

    # 按「同分辨率内第 i 条」配对：插件先打性能行、紧接着打分项行，顺序固定。
    # 用绝对下标配对会错位（一帧里可能只有其中一行），所以按分辨率分桶计数。
    idx = {}
    for r in perf:
        k = r["res"]
        i = idx.get(k, 0)
        idx[k] = i + 1
        r["seg"] = None
    idx = {}
    for s in segs:
        k = s["res"]
        i = idx.get(k, 0)
        idx[k] = i + 1
        bucket = [r for r in perf if r["res"] == k]
        if i < len(bucket):
            s["perf"] = bucket[i]
            bucket[i]["seg"] = s
        else:
            s["perf"] = None
    return perf, segs


def agg(vals):
    vals = [v for v in vals if v is not None]
    return (sum(vals) / len(vals)) if vals else None


def fmt(v, spec, width):
    return ("%" + str(width) + spec) % v if v is not None else ("%" + str(width) + "s") % "-"


def med(vals):
    """中位数（None 自动剔除）。"""
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2.0


AB_MODES = {
    "chain": ("chain", "1seg",
              "chain = 5a 段区间钩子（磨皮+色调+细节一次回读）；"
              "1seg = 3.1.51 单段钩子（只接管磨皮，色调/细节回 CPU）"),
    "out": ("out", "rb",
            "out = 5b-1 链尾不回读（直接绘制显存里的纹理）；"
            "rb = 回读+上传后绘制"),
}


def print_ab_compare(perf):
    """同会话 A/B —— 自动识别是哪一对臂。

    两对臂共用这套输出：
      chain / 1seg  段区间钩子开/关（T2，量 5a 的净收益）
      out   / rb    链尾不回读开/关（5b-1，量省掉 map+copy+上传 的收益）

    为什么必须同会话：跨会话连**没改过代码**的掩码段都能从 0.14 漂到
    0.30 ms（经验 E9），拿两个会话的中位数相减得到的是背景负载差，不是
    代码差。这里两条臂在同一个进程、同一段画面里交替 180 帧跑出来，
    环境项直接相消 —— 这是唯一可信的测法。

    为什么用中位数：4K 整帧分布宽到 24~39 ms，均值会被个别高窗口拉偏；
    中位数对宽分布更稳，也与 PROGRESS 里其他表的口径一致。
    """
    rows = [r for r in perf if r.get("ab")]
    if not rows:
        return

    by_res = {}
    for r in rows:
        by_res.setdefault(r["res"], {}).setdefault(r["ab"], []).append(r)

    # 每个分辨率各自判定它属于哪一对臂（同一份日志里不该混，混了就按先命中的）
    modes = {}
    for res in by_res:
        arms = by_res[res]
        for key, (fast, slow, _desc) in AB_MODES.items():
            if fast in arms and slow in arms:
                modes[res] = key
                break

    descs = sorted({AB_MODES[k][2] for k in modes.values()})
    print("\n【同会话 A/B】" + ("；".join(descs) if descs else "（只有单臂，无法比较）"))
    if modes:
        print("差值 = 慢臂 − 快臂，**正值表示开的那条更快**（省下来的时间）")

    hdr = ("%-9s %-6s %4s %6s %6s %6s %6s %7s %5s %6s"
           % ("分辨率", "臂", "窗口", "下载", "引擎", "上传", "往返",
              "掩码%", "脸%", "对齐%"))
    print(hdr)
    print("-" * len(hdr))

    warn = []
    for res in sorted(by_res):
        arms = by_res[res]
        key = modes.get(res)
        order = AB_MODES[key][:2] if key else tuple(sorted(arms))
        base = {}
        for arm in order:
            arr = arms.get(arm) or []
            if not arr:
                print("%-9s %-6s %4d  (no window for this arm)" % (res, arm, 0))
                continue
            b = dict(
                n=len(arr),
                dl=med([r["dl"] for r in arr]),
                pr=med([r["pr"] for r in arr]),
                up=med([r["up"] for r in arr]),
                tot=med([r["tot"] for r in arr]),
                cover=agg([r["cover"] for r in arr]),
                faces=agg([r["faces"] for r in arr]),
                align=agg([r["align"] for r in arr]),
            )
            base[arm] = b
            print("%-9s %-6s %4d %6.2f %6.2f %6.2f %6.2f %s %s %s"
                  % (res, arm, b["n"], b["dl"], b["pr"], b["up"], b["tot"],
                     fmt(b["cover"], ".2f", 7),
                     fmt(b["faces"], ".0f", 5),
                     fmt(b["align"], ".0f", 6)))
        if key and len(base) == 2:
            c, s = base[order[0]], base[order[1]]
            print("%-9s %-6s %4s %+6.2f %+6.2f %+6.2f %+6.2f %s"
                  % (res, "差值", "", s["dl"] - c["dl"], s["pr"] - c["pr"],
                     s["up"] - c["up"], s["tot"] - c["tot"], ""))
            # 公平性校验：两臂的画面条件必须接近，否则毫秒差没有意义。
            dc = abs((s["cover"] or 0) - (c["cover"] or 0))
            df = abs((s["faces"] or 0) - (c["faces"] or 0))
            da = abs((s["align"] or 0) - (c["align"] or 0))
            if dc > 1.0 or df > 5.0 or da > 3.0:
                warn.append("%s：掩码差 %.2f 个百分点 / 有脸差 %.0f / 对齐差 %.0f"
                            % (res, dc, df, da))

    # 臂自证（只对 chain / 1seg 有意义）：1seg 臂里 chain= 计数**必然**停涨
    # （钩子根本没挂），所以那些窗口的 seg 行不该出现 chain= 这个 token；
    # chain 臂则每个窗口都该有。这条既是「臂真的切了」的证据，也能抓最致命
    # 的失效 —— 切臂没生效、两条臂其实是同一条，那时差值接近 0，看起来就
    # 像「没有收益」。out / rb 两条臂都挂着钩子，这一项不适用。
    for res in sorted(by_res):
        if modes.get(res) != "chain":
            continue
        for arm in ("chain", "1seg"):
            arr = by_res[res].get(arm) or []
            if not arr:
                continue
            has = sum(1 for r in arr
                      if (r.get("seg") or {}).get("chain_hit") is not None)
            print("%-9s %-6s 臂自证 %d/%d 窗口带 chain= "
                  "（chain 臂应=全部，1seg 臂应=0）"
                  % (res, arm, has, len(arr)))

    if warn:
        print("\n[!] 两臂画面条件不够接近，差值仅供参考：")
        for w in warn:
            print("  - " + w)
    elif modes:
        print("\n两臂的掩码/有脸/对齐条件接近，差值可直接采信。")


def print_window_table(rows):
    print("\n【逐窗口】")
    hdr = ("%-9s %6s %6s %6s %7s %6s %5s %6s %6s %6s %6s %6s"
           % ("分辨率", "下载", "引擎", "上传", "往返", "掩码%", "脸%",
              "对齐%", "磨皮", "回读", "准备", "掩码传"))
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        s = r.get("seg") or {}
        seg = s.get("seg") or {}
        sm_map = s.get("sm_map")
        # 「回读」= map 段；「准备」= prep 合计；「掩码传」= prep 里的掩码分量
        print("%-9s %6.2f %6.2f %6.2f %7.2f %s %s %s %s %s %s %s"
              % (r["res"], r["dl"], r["pr"], r["up"], r["tot"],
                 fmt(r["cover"], ".2f", 6), fmt(r["faces"], ".0f", 5),
                 fmt(r["align"], ".0f", 6),
                 fmt(seg.get("磨皮"), ".2f", 6),
                 fmt(sm_map, ".2f", 6),
                 fmt(s.get("prep"), ".2f", 6),
                 fmt(s.get("prep_mask"), ".2f", 6)))


def print_summary(perf, segs):
    by_res = {}
    for r in perf:
        by_res.setdefault(r["res"], []).append(r)

    print("\n【按分辨率聚合】")
    hdr = ("%-9s %5s %7s %7s %7s %8s %6s %5s %6s %6s"
           % ("分辨率", "窗口", "下载", "引擎", "上传", "往返", "掩码%", "脸%", "对齐%", "边框%"))
    print(hdr)
    print("-" * len(hdr))
    for res in sorted(by_res):
        arr = by_res[res]
        n = len(arr)
        print("%-9s %5d %7.2f %7.2f %7.2f %8.2f %s %s %s %s"
              % (res, n,
                 sum(r["dl"] for r in arr) / n,
                 sum(r["pr"] for r in arr) / n,
                 sum(r["up"] for r in arr) / n,
                 sum(r["tot"] for r in arr) / n,
                 fmt(agg([r["cover"] for r in arr]), ".2f", 6),
                 fmt(agg([r["faces"] for r in arr]), ".0f", 5),
                 fmt(agg([r["align"] for r in arr]), ".0f", 6),
                 fmt(agg([r["bounds"] for r in arr]), ".1f", 6)))

    # ---- T2 同会话 A/B ----
    # 放在 seg 判空之前：A/B 只依赖性能行，没有 seg 行也该打（那是插件太旧
    # 或磨皮关掉了，此时 A/B 本来也没意义，但让调用方看到「没有 ab=」比
    # 静默跳过更好排查）。
    print_ab_compare(perf)

    # ---- 分项 ----
    if not segs:
        print("\n【分项】无 seg 行 —— 插件 < 3.1.47，或本窗口磨皮档位为 0（引擎直通）。")
        return

    print("\n【分项聚合】（单位 ms；带 * 的是占引擎 ≥0.5% 的主要成本项）")
    tail = " %7s %7s %7s" % ("引擎", "分项和", "差")
    hdr = ("%-9s %5s " % ("分辨率", "窗口")) \
        + " ".join("%6s" % k for k in SEG_KEYS) + tail
    print(hdr)
    print("-" * len(hdr))
    for res in sorted(by_res):
        arr = [r for r in by_res[res] if r.get("seg")]
        if not arr:
            continue
        n = len(arr)
        eng = sum(r["pr"] for r in arr) / n
        cells = []
        for k in SEG_KEYS:
            v = agg([r["seg"]["seg"].get(k) for r in arr])
            if v is None:
                cells.append("%6s" % "-")
            else:
                pct = (v * 100.0 / eng) if eng > 1e-9 else 0.0
                cells.append("%6.2f" % v if pct < 0.5 else "%5.2f*" % v)
        print("%-9s %5d %s %7.2f %7s %7s"
              % (res, n, " ".join(cells), eng,
                 fmt(agg([r["seg"]["seg_sum"] for r in arr]), ".2f", 7),
                 fmt(agg([r["seg"]["diff"] for r in arr]), ".2f", 7)))
    print("  （带 * 的格表示该项占引擎 ≥0.5%，是主要成本项；纯 0.00 说明该档位为 0）")

    # ---- 磨皮拆解（3.1.49+ / 3.1.50 sm= / 3.1.51 prep=）----
    arr = [r for r in perf if r.get("seg") and r["seg"].get("sm_map") is not None]
    if not arr:
        print("\n【磨皮段拆解】无 sm=/prep= —— 插件 < 3.1.50，或磨皮未走 GPU 生产路径。")
        return

    print("\n【磨皮段拆解】（单位 ms/帧）")
    hdr = ("%-9s %5s %8s %8s %8s %8s %8s %8s %8s"
           % ("分辨率", "窗口", "磨皮项", "准备", " 掩码传", "提交", "回读", "拷贝", "钩子外"))
    print(hdr)
    print("-" * len(hdr))
    have_prep = False
    for res in sorted(by_res):
        arr2 = [r for r in by_res[res] if r.get("seg")
                and r["seg"].get("sm_map") is not None]
        if not arr2:
            continue
        n = len(arr2)

        def peel_of(r):
            return r["seg"]["seg"].get("磨皮")

        peel = agg([peel_of(r) for r in arr2])
        prep = agg([r["seg"].get("prep") for r in arr2])
        draw = agg([r["seg"].get("sm_draw") for r in arr2])
        mapv = agg([r["seg"].get("sm_map") for r in arr2])
        copy = agg([r["seg"].get("sm_copy") for r in arr2])
        if prep is not None:
            have_prep = True
        # 「钩子外」= 磨皮项 − 钩子内已埋点的部分。3.1.51 起 prep 也在钩子里，
        # 所以四项全减；只有三段时（3.1.50）减三项，剩下的正是准备段。
        known = [v for v in (draw, mapv, copy) if v is not None]
        if prep is not None:
            known.append(prep)
        outside = (peel - sum(known)) if (peel is not None and known) else None
        print("%-9s %5d %8s %8s %8s %8s %8s %8s %8s"
              % (res, n, fmt(peel, ".2f", 8), fmt(prep, ".2f", 8),
                 fmt(agg([r["seg"].get("prep_mask") for r in arr2]), ".2f", 8),
                 fmt(draw, ".2f", 8), fmt(mapv, ".2f", 8), fmt(copy, ".2f", 8),
                 fmt(outside, ".2f", 8)))
    print("  准备 = 整帧上传（对齐换帧时才有）+ 掩码两张纹理上传；'掩码传' 是其中掩码那一半。")
    print("  提交 = 23 次 draw 的**命令提交**（D3D11 延迟执行，不是 GPU 计算时间）。")
    if have_prep:
        print("  '钩子外' 应当接近 0：磨皮项减掉钩子内四项后若仍明显大于 0，"
              "说明有帧在钩子之外回退或分支丢进了这一项。")
    else:
        print("  本日志无 prep=（3.1.50 及以前），'钩子外' 只减了三段，"
              "**它正是准备段的实测值**——3.1.51 起这一列应趋近 0，差额转到「准备」。")

    # ---- 段区间钩子（3.1.52+）----
    # 与「磨皮 GPU 接管率」是两个不同的口径：gpusm 统计单段钩子，chain 统计
    # 段区间钩子。3.1.52 起两者都在（单段钩子作兜底），chain 命中才是我们想要的。
    arr3 = [r for r in perf if r.get("seg") and r["seg"].get("chain_call")]
    if arr3:
        chit = sum(r["seg"]["chain_hit"] for r in arr3)
        ccall = sum(r["seg"]["chain_call"] for r in arr3)
        ctone = agg([r["seg"].get("ch_tone") for r in arr3])
        cdet = agg([r["seg"].get("ch_detail") for r in arr3])
        print("\n【段区间钩子】命中 %d/%d（%.1f%%）；链上提交 色调 %s / 细节 %s ms"
              % (chit, ccall, 100.0 * chit / ccall if ccall else 0.0,
                 fmt(ctone, ".2f", 5), fmt(cdet, ".2f", 5)))
        print("  chain 生效时 sm= 的『回读』等的是**整条链**的 GPU 时间"
              "（磨皮+色调+细节），不能再与 3.1.51 的 sm= 横比。")
        print("  chain 明显小于 gpusm 说明引擎判了『非前缀』把认领整份丢弃 —— "
              "画面仍正确，只是白跑一趟 GPU，去搜 chain= 的 S/T/R/D/W 五项。")

    # ---- 接管率 ----
    gm = [(r["seg"]["gpusm_hit"], r["seg"]["gpusm_call"])
          for r in perf if r.get("seg") and r["seg"].get("gpusm_call")]
    if gm:
        hit = sum(a for a, _ in gm)
        call = sum(b for _, b in gm)
        print("\n【磨皮 GPU 接管率】合计 %d/%d（%.1f%%），%d 个窗口。"
              % (hit, call, 100.0 * hit / call, len(gm)))
        print("  低于 100% 说明有帧在 map 阶段失败或着色器缺失而回退了 CPU——"
              "去日志里搜「GPU 磨皮生产」「回读失败」「版本不匹配」。")


def write_csv(path, perf):
    cols = ["res", "dl", "pr", "up", "tot", "cover", "faces", "align", "bounds",
            "seg_mia", "seg_mo", "seg_se", "seg_wg", "seg_xj", "seg_mx",
            "sm_draw", "sm_map", "sm_copy", "prep", "prep_mask",
            "gpusm_hit", "gpusm_call"]
    keys = ["掩码", "磨皮", "色调", "五官", "细节", "美形"]
    with open(path, "w", newline="", encoding="utf-8-sig") as fh:
        wr = csv.writer(fh)
        wr.writerow(cols)
        for r in perf:
            s = r.get("seg") or {}
            seg = s.get("seg") or {}
            wr.writerow([r["res"], r["dl"], r["pr"], r["up"], r["tot"],
                         r["cover"], r["faces"], r["align"], r["bounds"]]
                        + [seg.get(k) for k in keys]
                        + [s.get("sm_draw"), s.get("sm_map"), s.get("sm_copy"),
                           s.get("prep"), s.get("prep_mask"),
                           s.get("gpusm_hit"), s.get("gpusm_call")])
    print("\nCSV 已写出: %s" % path)


def main():
    argv = sys.argv[1:]
    do_list = "--list" in argv
    csv_out = None
    if "--csv" in argv:
        i = argv.index("--csv")
        if i + 1 < len(argv):
            csv_out = argv[i + 1]
            argv = argv[:i] + argv[i + 2:]
        else:
            argv = argv[:i]
    paths = [a for a in argv if not a.startswith("--")]

    if not paths:
        d = scan_default_log()
        if not d:
            print("未找到 OBS 日志，请手动传入日志路径。", file=sys.stderr)
            return 1
        print("自动选用日志: %s" % d)
        paths = [d]
    paths = expand_paths(paths)

    perf, segs = [], []
    for p in paths:
        a, b = parse_file(p)
        print("  %s : 性能行 %d 条，分项行 %d 条" % (p, len(a), len(b)))
        perf.extend(a)
        segs.extend(b)

    if not perf:
        print("\n未匹配到任何 '性能 WxH：下载 ...' 行。请确认:")
        print("  1) 启动 OBS 前设置了 BEAUTY_PROFILE=1")
        print("  2) 滤镜参数非全 0（全 0 会 skip_sdk 直通，引擎项=0）")
        print("  3) 至少跑满 180 帧（约 3 秒 @60fps）")
        return 2

    if do_list:
        print_window_table(perf)
    print_summary(perf, segs)
    if csv_out:
        write_csv(csv_out, perf)

    print("\n提示: 往返 = 下载 + 引擎 + 上传，是全上 GPU 后每帧可消除的往返耗时；")
    print("      引擎 = 纯 CPU 像素算力（降功耗核心）。比较窗口前先确认掩码%/脸% 接近。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
