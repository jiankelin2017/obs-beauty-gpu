#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S2 帧环稳定性取证（2026-11-02）。

S2 要回答的唯一问题：帧环在 4K 上连跑 30 分钟、以及会话内来回换分辨率，会不会
重演 2026-11-01 那次「`device_copy_texture` 尺寸不一致 → `nvwgf2umx.dll c0000005`
驱动崩溃」。

**本工具不改产品代码**：用 `BEAUTY_GPU_FRAME_RING=1` 显式开启帧环，跑完再决定
是否翻默认（S2 的「先取证再翻默认」）。判据见 PROGRESS 9.3。

三个子命令：
    probe                  预检：这份素材是否真的走帧环
    soak  --minutes 30     4K 长跑（默认 ring=1）
    switch                 会话内画布 720p→1080p→4K 来回 3 轮

为什么要有 probe：帧环的启用条件是 `mg.count == 1 && !(mg.shape & 4)`
（obs-beauty.cpp:3705），**多脸时整段绕过**。用第一次那份 4K 素材（`faces=4`）
跑 30 分钟，会得到「一切正常」的假结论 —— 因为一次都没走帧环。所以先判：
    faces=1  且  seg 行 prep≈0（帧环命中时 4K 的 prep 从 ~4.65 掉到 0）

用法：
    python 05_验证/soak_s2.py probe
    python 05_验证/soak_s2.py soak --minutes 30
    python 05_验证/soak_s2.py switch
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import verify_s1_correct as V          # 复用会话准备（画布同步/预热/清滤镜/归一位置）
import obsctl
import phase0_parse_log as P

COLLECTION_4K = "beauty_soak4k"        # 单脸 4K（beauty_static_4k 是 faces=4，不能用于帧环）
CANVAS_4K = (3840, 2160)

# 长跑时的负载：尽量把链上各段拉起来，让对齐换帧频繁发生，帧环才会被真正
# 反复使用（空转 30 分钟等于没测）。
#
# 为什么**默认不带五官参数**：五官段（REGION）在 2.14.0 之前是 opt-in，没开时
# `region_blocks=true` 会连带把细节与美形一起交回 CPU（obs-beauty.cpp:3839，
# 注释写明「不是 bug，是 5c 的作用范围」）。带上五官后 4K 实测引擎 26.5 ms、
# 往返 38.5 ms，S2 的性能门槛（≤9.0 ms）就没法判了。所以：
#   默认        → 磨皮/色调/细节/美形（S/T/D/W 都能认领）——用于判 S2 性能门槛
#   --region-params → 再加五官参数（D/W 回落 CPU，真实用户场景，慢）
#   --with-region   → 再开 BEAUTY_GPU_REGION=1（T10 那条合并崩溃场景）
# ⚠ 2.14.0 起 GPU_REGION **默认是开的**。所以 env_for() 现在**两侧都显式写**：
#   不带 --with-region 时写 "0"，否则这一路会从「五官落 CPU」悄悄变成「走 GPU」，
#   与 2.14.0 之前的历史对拍就不可比了。
# ⚠ 单位是**面板刻度 0..100**，不是 0..1。
# `beauty_params.h` 的 g_params 写得很清楚：「面板刻度统一 0..100，max_val 是
# 面板 100 对应的内部量程（value = panel / 100 * max_val）」。引擎侧一律按
# `panel / 100` 归一到 0..1，所以这里传 0.5 等于「滑到 0.5%」——效果只剩 1%，
# 画面几乎不动，取整后全被抹平。2026-11-03 之前这四个键就是 0..1 尺度，
# 导致 P0-7 的全部对拍都跑在 1% 强度上（详见 PROGRESS §9.19）。
LOAD = {
    "blur_level": 60, "clarity": 40, "color_level": 40, "red_level": 30,
    "sharpen": 30, "thin_face": 0.6, "big_eye": 0.6, "chin": 0.4,
}
LOAD_REGION = {
    "eye_bright": 50, "remove_pouch_strength": 50,
    "remove_nasolabial_folds_strength": 50, "delspot_level": 40,
}

# 一旦命中就说明又回到那条崩溃链上了，长跑结论直接判 FAIL。
BAD_PATTERNS = [
    ("device_copy_texture", "D3D11 拷贝尺寸不一致（就是崩溃前那条报错）"),
    ("must have the same dimensions", "D3D11 拷贝尺寸不一致"),
    ("device lost", "D3D11 设备丢失"),
    ("device_lost", "D3D11 设备丢失"),
    ("device_removed", "D3D11 设备移除"),
    ("DXGI_ERROR", "DXGI 错误"),
    ("0x887A0005", "DEVICE_REMOVED"),
    ("0x887A0006", "DEVICE_HUNG"),
    ("0xc0000005", "访问违例"),
    ("gs_texture_create failed", "纹理创建失败（显存不足？）"),
]
PLUGIN_ERR = re.compile(r"\[obs-beauty\][^\n]*error", re.IGNORECASE)


# ------------------------------------------------------------------ 小工具
def keep_awake(on=True):
    """长跑期间别让显示器休眠/系统睡眠 —— 睡眠会停掉渲染，测出来是假数据。"""
    try:
        import ctypes
        ES_CONTINUOUS = 0x80000000
        ES_SYSTEM = 0x00000001
        ES_DISPLAY = 0x00000002
        flags = ES_CONTINUOUS | (ES_SYSTEM | ES_DISPLAY if on else 0)
        ctypes.windll.kernel32.SetThreadExecutionState(flags)
        return True
    except Exception:
        return False


def env_for(ring=1, with_region=False, profile=True):
    e = {"BEAUTY_GPU_FRAME_RING": "1" if ring else "0"}
    if profile:
        e["BEAUTY_PROFILE"] = "1"
    # 2.14.0：GPU_REGION 翻默认开后，这里必须**两侧都写**。"不设" 的含义已经
    # 从「关」变成「开」，沿用旧写法会让 --region-params 这一臂悄悄换路径。
    e["BEAUTY_GPU_REGION"] = "1" if with_region else "0"
    return e


def apply_load(c, region=False, extra=None, tries=15):
    """下发参数。刚连上时 OBS 常回 207（未就绪），必须重试。"""
    st = {k: 0.0 for k in V.PANEL_KEYS}
    st.update(LOAD)
    if region:
        st.update(LOAD_REGION)
    if extra:
        st.update(extra)
    st["enabled"] = True
    last = None
    for _ in range(tries):
        try:
            c.set_filter_settings(V.SRC, V.FILT, st, overlay=True)
            c.set_filter_enabled(V.SRC, V.FILT, True)
            return True
        except Exception as e:
            last = e
            time.sleep(2.0)
    print("  !! 参数下发失败：%s" % last)
    return False


def normalize_item(c):
    """把源摆到 (0,0)、1:1，保证每次运行构图一致（否则画布一变源就被挪走）。"""
    try:
        sid = None
        for it in c.scene_items("Main"):
            if it.get("sourceName") == V.SRC:
                sid = it.get("sceneItemId")
                break
        if sid is None:
            return
        c.set_item_transform("Main", sid, {
            "positionX": 0.0, "positionY": 0.0,
            "scaleX": 1.0, "scaleY": 1.0,
            "rotation": 0.0, "alignment": 5,
            "boundsType": "OBS_BOUNDS_NONE",
        })
    except Exception as e:
        print("  !! 位置归一失败：%s" % e)


def scan_log(path):
    """扫日志：异常模式 + 插件 error 行。返回 (hits, first_line_of_each)。"""
    hits = {}
    perr = []
    if not path or not os.path.isfile(path):
        return hits, perr
    with open(path, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            low = line.lower()
            for pat, desc in BAD_PATTERNS:
                if pat.lower() in low:
                    k = (pat, desc)
                    hits[k] = hits.get(k, 0) + 1
            if PLUGIN_ERR.search(line):
                perr.append(line.rstrip()[:160])
    return hits, perr


def progress(log):
    """从日志里数出「跑了多少个 perf 窗口」。每窗 180 帧，所以帧数≈窗数×180。"""
    n = 0
    last = None
    if not log or not os.path.isfile(log):
        return 0, None
    with open(log, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            if "性能 " in line:
                n += 1
                last = line.rstrip()
    return n, last


def hhmmss(sec):
    sec = int(sec)
    return "%02d:%02d:%02d" % (sec // 3600, (sec % 3600) // 60, sec % 60)


def raw_bits(log):
    """抓两条正则块解析不了的原始判据：S/T/R/D/W 认领位、引擎检出的人脸数。"""
    stw = None
    faces = set()
    if not log or not os.path.isfile(log):
        return stw, faces
    with open(log, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            m = re.search(r"S/T/R/D/W=([\d/]+)", line)
            if m:
                stw = m.group(1)
            for v in re.findall(r"faces=(\d+)", line):
                faces.add(int(v))
    return stw, faces


def report(log, el=True):
    """统一出结论：perf 窗口、最后窗口数值、帧环是否命中、异常统计。"""
    perf, segs = P.parse_file(log) if log else ([], [])
    print("\n=============== 判据 ===============")
    print("日志: %s" % (os.path.basename(log) if log else "(无)"))
    print("perf 窗口数: %d%s" % (len(perf), "（≈%d 帧）" % (len(perf) * 180) if perf else ""))
    if perf:
        r = perf[-1]
        print("末窗 %s：下载 %.2f + 引擎 %.2f + 上传 %.2f = **%.2f ms/帧**"
              % (r["res"], r["dl"], r["pr"], r["up"], r["tot"]))
        print("        掩码 %.1f%%  有脸 %.1f%%  对齐 %.1f%%"
              % (r["cover"], r["faces"], r["align"]))
    if segs:
        s = segs[-1]
        print("末窗 seg：磨皮 %.2f 色调 %.2f 五官 %.2f 细节 %.2f 美形 %.2f"
              % (s["seg"].get("磨皮") or 0, s["seg"].get("色调") or 0,
                 s["seg"].get("五官") or 0, s["seg"].get("细节") or 0,
                 s["seg"].get("美形") or 0))
        print("        chain=%s/%s  prep=%s/%s"
              % (s.get("chain_hit"), s.get("chain_call"),
                 s.get("prep"), s.get("prep_mask")))
        prep = s.get("prep")
        if prep is not None:
            print("        帧环判据 prep=%.2f → %s"
                  % (prep, "命中（ring 生效）" if prep < 1.0
                     else "未命中（ring 没起作用，检查是否多脸）"))
    stw, faces = raw_bits(log)
    print("段认领 S/T/R/D/W: %s" % (stw or "-"))
    print("引擎检出 faces 取值: %s" % (sorted(faces) or "-"))
    hits, perr = scan_log(log)
    print("异常模式: %s" % ("无" if not hits else ""))
    for (pat, desc), cnt in hits.items():
        print("        ! %-34s ×%-4d  %s" % (pat, cnt, desc))
    print("插件 error 行: %d%s" % (len(perr), "" if not perr else "（首条：%s）" % perr[0]))
    return {"perf": perf, "seg": segs[-1] if segs else None, "hits": hits,
            "perr": perr, "stw": stw, "faces": faces}


def target(args):
    """集合 + 画布：默认 4K 单脸；--collection/--canvas 可覆盖。"""
    coll = getattr(args, "collection", None) or COLLECTION_4K
    cv = getattr(args, "canvas", None)
    if cv:
        w, h = cv.lower().split("x")
        return coll, (int(w), int(h))
    return coll, ((1280, 720) if "720" in coll else CANVAS_4K)


# ------------------------------------------------------------------ probe
def cmd_probe(args):
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    coll, canvas = target(args)
    print("预检：%dx%d + ring=1，跑 %.0fs，确认这份素材真的走帧环"
          % (canvas[0], canvas[1], args.seconds))
    proc, snap = V.start_obs(coll, extra_env=env_for(1, args.with_region),
                             canvas=canvas)
    c = None
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(8)
        apply_load(c, region=args.region_params)
        time.sleep(args.seconds)
        c.close()
        c = None
    finally:
        V.close_obs()
    log = V.newest_log(snap)
    info = report(log)
    prep = info["seg"].get("prep") if info["seg"] else None
    ok_ring = prep is not None and prep < 1.0
    print("\n预检结论: prep=%s（ring %s）  段认领=%s  检出 faces=%s → %s"
          % (prep, "on" if ok_ring else "off", info["stw"],
             sorted(info["faces"]) or "-",
             "可以开始长跑" if (ok_ring and not info["hits"]) else "!! 先修素材/环境再跑长跑"))
    return 0 if (ok_ring and not info["hits"]) else 1


# ------------------------------------------------------------------ soak
def cmd_soak(args):
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    mins = args.minutes
    interval = args.interval
    coll, canvas = target(args)
    print("%dx%d 长跑：%.1f 分钟  ring=%d  region=%s  每 %.0fs 探活"
          % (canvas[0], canvas[1], mins, args.ring,
             "on" if args.with_region else "off", interval))
    keep_awake(True)

    proc, snap = V.start_obs(coll,
                             extra_env=env_for(args.ring, args.with_region),
                             canvas=canvas)
    c = None
    t0 = time.time()
    dead_at = None
    stalled = False
    last_size = 0
    last_growth = time.time()
    checks = []
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(8)
        apply_load(c, region=args.region_params)
        normalize_item(c)
        t0 = time.time()
        print("开始计时 %s\n" % time.strftime("%H:%M:%S"))
        end = t0 + mins * 60
        n_verify = 0
        while time.time() < end:
            time.sleep(min(interval, max(1.0, end - time.time())))
            el = time.time() - t0
            alive = V.obs_running()
            n, last = progress(V.newest_log(snap))
            # 渲染是否还在推进：日志 3 分钟内没有任何增长就是停了
            size = os.path.getsize(V.newest_log(snap)) if V.newest_log(snap) else 0
            if size > last_size:
                last_size = size
                last_growth = time.time()
            elif time.time() - last_growth > 180:
                stalled = True
            fps = (n * 180.0 / el) if el > 0 else 0
            tot = re.search(r"=\s*([\d.]+)\s*ms/帧", last or "")
            align = re.search(r"对齐\s+(\d+/\d+（[\d.]+%）)", last or "")
            line = ("[%s] 窗 %-4d 帧率≈%5.1f fps  往返 %-6s 对齐 %-14s %s"
                    % (hhmmss(el), n, fps,
                       (tot.group(1) if tot else "-"),
                       (align.group(1) if align else "-"),
                       "存活" if alive else "!! 已退出"))
            if stalled:
                line += "  !! 日志 3 分钟无增长"
            print(line)
            checks.append((el, n, alive))
            if not alive:
                dead_at = el
                print("\n!! OBS 在 %s 退出（疑似崩溃）" % hhmmss(el))
                break
            # 定期取一帧真实输出，确认画面没黑/没花
            if args.verify_every and el > n_verify * args.verify_every + args.verify_every:
                n_verify += 1
                try:
                    dst = os.path.join(V.OUT, "soak", "t%05d.png" % int(el))
                    im = V.prog_shot(c, dst, settle=0.6)
                    m = V._mean(im)
                    print("        画面校验 %s mean=%.2f %s"
                          % (im.size, m, "!! 偏黑" if m < 8 else "ok"))
                except Exception as e:
                    print("        画面校验失败：%s" % e)
        if c:
            c.close()
            c = None
    finally:
        keep_awake(False)
        V.close_obs()

    log = V.newest_log(snap)
    info = report(log)
    print("\n================ S2 长跑结论 ================")
    print("时长 %s / 计划 %.0f 分钟" % (hhmmss(time.time() - t0), mins))
    print("OBS 存活到结束: %s%s" % (
        "是" if dead_at is None else "否",
        "" if dead_at is None else "（%s 退出）" % hhmmss(dead_at)))
    print("D3D11/驱动异常: %s" % ("无" if not info["hits"] else "%d 类" % len(info["hits"])))
    prep = info["seg"].get("prep") if info["seg"] else None
    if args.ring:
        ring_ok = prep is not None and prep < 1.0
        print("帧环命中(prep<1.0): %s" % ("是" if ring_ok else "否"))
    else:
        # 对照跑：**不**用 prep 断言。prep 是 180 帧均值，换帧次数少的分辨率
        # （720p）即便 ring=0 均值也只有 0.28，拿它当开关判据会误判。真正的
        # 判据是同一负载下 ring=0 与 ring=1 的往返差，见 9.3.1 的 A/B。
        ring_ok = True
        print("对照跑（ring=0）：prep=%.2f（仅记录，不作断言）"
              % (prep if prep is not None else -1))
    passed = (dead_at is None and not info["hits"] and not stalled and ring_ok)
    print("判定: %s" % ("PASS" if passed else "FAIL（见上）"))
    return 0 if passed else 1


# ------------------------------------------------------------------ switch
def cmd_switch(args):
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    print("分辨率来回切换：%d 轮 720p→1080p→4K" % args.rounds)
    keep_awake(True)
    proc, snap = V.start_obs(COLLECTION_4K, extra_env=env_for(1, args.with_region),
                             canvas=CANVAS_4K)
    c = None
    sizes = [(1280, 720), (1920, 1080), (3840, 2160)]
    fails = 0
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(8)
        apply_load(c, region=args.region_params)
        vs = c.req("GetVideoSettings")
        fps_n, fps_d = vs.get("fpsNumerator", 30), vs.get("fpsDenominator", 1)
        print("当前画布 %sx%s @ %s/%s fps"
              % (vs.get("baseWidth"), vs.get("baseHeight"), fps_n, fps_d))
        for r in range(args.rounds):
            for (w, h) in sizes:
                try:
                    c.req("SetVideoSettings", {
                        "baseWidth": w, "baseHeight": h,
                        "outputWidth": w, "outputHeight": h,
                        "fpsNumerator": fps_n, "fpsDenominator": fps_d,
                    })
                except Exception as e:
                    print("  !! 切到 %dx%d 失败：%s" % (w, h, e))
                    fails += 1
                    continue
                time.sleep(args.settle)
                alive = V.obs_running()
                hits, perr = scan_log(V.newest_log(snap))
                flag = "存活" if alive else "!! 已退出"
                if hits or perr:
                    flag += "  !! 异常 %d 类/error %d 行" % (len(hits), len(perr))
                print("  第 %d 轮 %4dx%-4d  %s" % (r + 1, w, h, flag))
                if not alive:
                    fails += 1
                    raise SystemExit(_switch_verdict(True))
                # 换完画布源会被重排，拉回 1:1 保证下一次切换仍覆盖整幅
                normalize_item(c)
        if c:
            c.close()
            c = None
    finally:
        keep_awake(False)
        V.close_obs()
    log = V.newest_log(snap)
    info = report(log)
    print("\n================ 切换结论 ================")
    print("切了 %d 轮 × %d 档，失败 %d" % (args.rounds, len(sizes), fails))
    print("异常: %s" % ("无" if not info["hits"] else info["hits"]))
    ok = fails == 0 and not info["hits"] and not info["perr"]
    print("判定: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def _switch_verdict(dead):
    print("\n判定: FAIL（切换过程中 OBS 退出）")
    return 1


# ------------------------------------------------------------------ reload
def cmd_reload(args):
    """热重载滤镜：关→开→换参数 循环 N 次。

    关心的是帧环纹理在「滤镜停摆再启动」之后还能不能重建对（4K 每张 33 MB，
    销毁/重建最容易暴露尺寸与格式的陈旧引用），以及进程会不会崩。
    """
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    print("热重载：4K 下开关滤镜 + 换参数，共 %d 轮" % args.rounds)
    proc, snap = V.start_obs(COLLECTION_4K, extra_env=env_for(1, args.with_region),
                             canvas=CANVAS_4K)
    c = None
    fails = 0
    try:
        c = obsctl.wait_ready(wait=90)
        time.sleep(8)
        apply_load(c, region=args.region_params)
        normalize_item(c)
        time.sleep(3)
        for i in range(1, args.rounds + 1):
            c.set_filter_enabled(V.SRC, V.FILT, False)
            time.sleep(1.5)
            c.set_filter_enabled(V.SRC, V.FILT, True)
            time.sleep(1.5)
            # 换一组参数：掩码/几何必须重建，帧环槽位也跟着换代
            extra = {"blur_level": 40 if i % 2 else 80,
                     "clarity": 30 if i % 2 else 50}
            apply_load(c, region=args.region_params, extra=extra)
            time.sleep(args.settle)
            alive = V.obs_running()
            hits, perr = scan_log(V.newest_log(snap))
            n, _ = progress(V.newest_log(snap))
            print("  第 %d 轮  %s  窗口 %-3d  %s%s"
                  % (i, "存活" if alive else "!! 已退出", n,
                     "ok" if (alive and not hits and not perr) else "!! 异常",
                     "" if not hits else "（%d 类）" % len(hits)))
            if not alive:
                fails += 1
                break
            fails += 1 if (hits or perr) else 0
        if c:
            c.close()
            c = None
    finally:
        V.close_obs()
    log = V.newest_log(snap)
    info = report(log)
    prep = info["seg"].get("prep") if info["seg"] else None
    print("\n================ 热重载结论 ================")
    print("%d 轮，失败 %d" % (args.rounds, fails))
    print("重载后帧环命中: %s（prep=%s）"
          % ("是" if (prep is not None and prep < 1.0) else "否", prep))
    ok = fails == 0 and not info["hits"] and prep is not None and prep < 1.0
    print("判定: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description="S2 帧环稳定性取证")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="预检：这份素材是否真的走帧环")
    p.add_argument("--seconds", type=float, default=25.0)
    p.add_argument("--with-region", action="store_true")
    p.add_argument("--region-params", action="store_true",
                   help="下发五官参数（会让细节/美形回落 CPU）")
    p.add_argument("--collection")
    p.add_argument("--canvas")
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("soak", help="4K 长跑")
    p.add_argument("--minutes", type=float, default=30.0)
    p.add_argument("--interval", type=float, default=30.0, help="探活间隔（秒）")
    p.add_argument("--ring", type=int, default=1, choices=(0, 1))
    p.add_argument("--with-region", action="store_true",
                   help="同时开 GPU 五官（T10 那条合并崩溃场景）")
    p.add_argument("--region-params", action="store_true",
                   help="下发五官参数（会让细节/美形回落 CPU）")
    p.add_argument("--collection")
    p.add_argument("--canvas")
    p.add_argument("--verify-every", type=float, default=600.0,
                   help="每 N 秒取一帧真实输出查黑帧/花屏（0=不查）")
    p.set_defaults(func=cmd_soak)

    p = sub.add_parser("switch", help="会话内来回切分辨率")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--settle", type=float, default=6.0)
    p.add_argument("--with-region", action="store_true")
    p.add_argument("--region-params", action="store_true")
    p.set_defaults(func=cmd_switch)

    p = sub.add_parser("reload", help="热重载滤镜（关→开→换参数）N 轮")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--settle", type=float, default=5.0)
    p.add_argument("--with-region", action="store_true")
    p.add_argument("--region-params", action="store_true")
    p.set_defaults(func=cmd_reload)

    a = ap.parse_args(argv)
    try:
        return a.func(a)
    finally:
        if getattr(V, "_CANVAS_BAK", None):
            V.canvas_restore(V._CANVAS_BAK)
            V._CANVAS_BAK = None


if __name__ == "__main__":
    sys.exit(main())
