#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P0-11 阶段 2 实机验收：**全帧检测路径**的输入旋转（2.13.1）。

背景
----
2.10.0 把输入旋转做进了「检测缩略图」路径，但只在 S4b 缩略图路径生效。一旦这一帧
走全帧检测（上一帧没被链全认领 / 多脸 / mirror / 首帧），`input_rot` 完全不起作用。
最容易踩的是**开了五官参数、但 BEAUTY_GPU_REGION 没开**：REGION 段落 CPU → 本帧
不是全段认领 → `chain_out_valid=0` → 缩略图路径永不启用 → 旋转失效。
（2.13.1 起该失效本身已修 —— 旋转进了全帧路径；2.14.0 起 `GPU_REGION` 默认开，
所以本脚本在 start_obs 里**显式设 `=0`**，保留「最苛刻条件」这个测试语义。）

2.10.0 的实测记录（`out/logs/input_rot_2.10.0_20261104.md` §4.2）写着：
同样一张正脸躺放素材，`input_rot=0/90/270` 三臂输出**逐字节相同**。

2.13.1 把旋转提到 `letterboxRgb` 之前，全帧路径也会转。本脚本就是验收这一条。

判据（两条，主判据不受真人移动影响）
------------------------------------
主判据   perf 行的「有脸 %」。`BEAUTY_PROFILE=1` 时插件每 180 帧打一行
         `掩码 X% 有脸 Y%`。躺放画面 + `input_rot=0` → 检不到脸 → 有脸 0%；
         `input_rot=90` → 脸被摆正 → 有脸接近 100%。这个数只看检测成败，
         与人在镜头前动不动无关，是唯一可靠的实机判据。
辅助     footprint = |开滤镜 − 关滤镜| > 0 的面积占比与质心。真人会在两帧之间
         移动，所以这个数天然含运动分量，只用来佐证「效果落在画面上」，
         不作为通过/不通过的门槛。

臂
--
  off    滤镜禁用（取参考帧用）
  ir0    input_rot=0     ← 阴性对照，必须复现 2.10.0 的「检不到脸」
  ir90   input_rot=90    ← 修复目标，必须检出
  ir270  input_rot=270   ← 方向反证，必须仍然检不到（否则是「随便转都行」的假阳性）

用法
----
    py -3 05_验证/rot_live_check.py --collection beauty_real_ab \
        --source 视频采集设备 --filter OBS美颜

    # 只跑某几臂
    py -3 05_验证/rot_live_check.py --arms ir0 ir90

    # 不想让脚本起停 OBS（OBS 已经开好且已挂滤镜）
    py -3 05_验证/rot_live_check.py --attach

注意
----
* 场景集合里的**场景项旋转**（本例 `rot: 90`）发生在滤镜之后，所以面板「输入方向」
  必须与摄像机的**物理摆放**对应：相机侧转 90°，面板才填 90。人脸本来就正的话，
  填 90 会把它转成躺着、检测直接失败 —— 这是预期的阴性结果，不是 bug。
* 输出落在 `05_验证/out/rot_live/<tag>/`。
"""
import argparse
import glob
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import verify_s1_correct as V      # noqa: E402

ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "out", "rot_live")

FILTER_KIND = "obs_beauty_filter"

# 全 0 打底，只开五官四项 —— 这组正是 2.10.0 §4.2 里「必然失效」的配置。
PANEL_KEYS = [
    "blur_level", "clarity", "color_level", "red_level", "sharpen",
    "global_sharpen", "face_threed", "eye_bright", "remove_pouch_strength",
    "remove_nasolabial_folds_strength", "delspot_level", "thin_face",
    "big_eye", "chin",
]
SHAPE = {"thin_face": 0.6, "big_eye": 0.6, "chin": 0.6, "face_threed": 0.5}
# 必须同时开磨皮：只开五官时皮肤掩码覆盖率恒为 0（掩码由磨皮/色调段驱动），
# 「掩码 %」就失去意义，只剩「有脸 %」一个指标 —— 而它会随真人抬头/低头大幅
# 摆动（实测同一臂两个窗口 72% / 64%），不足以判方向。开了磨皮之后
# 「掩码 > 0」等价于「本帧检出了脸」，是二值信号。
BLUR = 60.0

ARMS = [
    ("ir0",   0,    "input_rot=0（正放参考）"),
    ("ir90",  90,   "input_rot=90（顺时针）"),
    ("ir180", 180,  "input_rot=180（倒置）"),
    ("ir270", 270,  "input_rot=270（逆时针）"),
]

# perf 行：「... 掩码 17.66% 有脸 100% ...」
PERF_RE = re.compile(r"掩码\s+([\d.]+)%\s+有脸\s+(\d+)%")
THUMB_RE = re.compile(r"thumb=(\d+)")
FACES_RE = re.compile(r"faces=(\d+)")


# ------------------------------------------------------------------ 日志
def log_path():
    d = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "logs")
    fs = sorted(glob.glob(os.path.join(d, "*.txt")), key=os.path.getmtime)
    if not fs:
        return None
    return fs[-1]


def read_lines(path):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read().splitlines()
    except OSError:
        return []


def wait_perf(path, from_line, timeout=25.0):
    """等到 from_line 之后出现一条新的 perf 行，返回 (行号, (掩码, 有脸, thumb))。"""
    t0 = time.time()
    n = from_line
    while time.time() - t0 < timeout:
        ls = read_lines(path)
        for i in range(n, len(ls)):
            m = PERF_RE.search(ls[i])
            if m:
                th = THUMB_RE.search(ls[i])
                return i + 1, (float(m.group(1)), int(m.group(2)),
                               int(th.group(1)) if th else -1)
        n = len(ls)
        time.sleep(0.4)
    return from_line, None


# ------------------------------------------------------------------ 滤镜
def ensure_filter(c, source, filt):
    names = [f.get("filterName") for f in c.filters(source)]
    if filt in names:
        print("  滤镜 %r 已存在，复用" % filt)
        return
    c.req("CreateSourceFilter", {"sourceName": source, "filterName": filt,
                                 "filterKind": FILTER_KIND})
    print("  已建滤镜 %r (kind=%s)" % (filt, FILTER_KIND))


def apply_shape(c, source, filt, rot):
    st = {k: 0.0 for k in PANEL_KEYS}
    st.update(SHAPE)
    st["blur_level"] = float(BLUR)
    st["enabled"] = True
    if rot is not None:
        st["input_rot"] = int(rot)
    c.set_filter_settings(source, filt, st, overlay=True)
    c.set_filter_enabled(source, filt, True)


# ------------------------------------------------------------------ 图像
def load_rgb(path):
    from PIL import Image
    import numpy as np
    im = Image.open(path).convert("RGB")
    return np.asarray(im).astype(np.int16)


def footprint(a, b, thresh=2):
    """a,b: RGB int16 数组，需同尺寸。返回 (面积占比, 质心xy, bbox)。

    thresh 默认 2 而不是 0：**实机画面有传感器噪声**，两帧之间满屏都有 1 灰阶的
    随机差，阈值设 0 会让 footprint 恒等于 100%（这一版第一次跑就踩到了）。
    静态 PNG 素材没有这个问题，那边沿用 2.10.0 的 >0 口径。
    """
    import numpy as np
    if a.shape != b.shape:
        return None
    d = np.abs(a - b).max(axis=2) > thresh
    n = int(d.sum())
    if n == 0:
        return 0.0, (0.0, 0.0), (0, 0, 0, 0)
    ys, xs = np.nonzero(d)
    return (100.0 * n / d.size, (float(xs.mean()), float(ys.mean())),
            (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())))


# ------------------------------------------------------------------ 主流程
def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default="beauty_real_ab")
    ap.add_argument("--source", default="视频采集设备")
    ap.add_argument("--filter", default="OBS美颜")
    ap.add_argument("--arms", nargs="*", default=None,
                    help="只跑指定臂，如 ir0 ir90")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--attach", action="store_true",
                    help="OBS 已运行且已挂滤镜，不再起停（也不归零 frame 计数）")
    ap.add_argument("--settle", type=float, default=1.0,
                    help="改完参数后、抓图前的额外等待秒数")
    args = ap.parse_args(argv)

    tag = args.tag or time.strftime("live_%Y%m%d_%H%M%S")
    dst = os.path.join(OUT, tag)
    os.makedirs(dst, exist_ok=True)

    arms = ARMS
    if args.arms:
        want = set(args.arms)
        arms = [a for a in ARMS if a[0] in want]
        if not arms:
            sys.exit("--arms 一个都没匹配上：%s" % args.arms)

    proc = None
    if args.attach:
        if not V.obs_running():
            sys.exit("--attach 需要 OBS 已在运行")
        print("attach 模式：复用正在运行的 OBS")
    else:
        if V.obs_running():
            sys.exit("obs64 已在运行，先关掉（或加 --attach）")
        env_extra = {"BEAUTY_GPU": "1", "BEAUTY_PROFILE": "1",
                     "BEAUTY_GPU_REGION": "0"}
        # 2.14.0：GPU_REGION 已翻默认开。旧写法是「不设」（当时不设=关）；
        # 现在不设=开，同一语义必须显式写成 "0"，否则这一臂悄悄换了条件。
        proc, _ = V.start_obs(args.collection, extra_env=env_extra)
        print("已启动 OBS（collection=%s, BEAUTY_PROFILE=1, BEAUTY_GPU_REGION=0）"
              % args.collection)

    try:
        obsctl.wait_ready(wait=90.0, quiet=True)
        c = obsctl.connect()
        ensure_filter(c, args.source, args.filter)

        lp = log_path()
        if not lp:
            sys.exit("找不到 OBS 日志，perf 行读不到")
        print("日志：%s" % lp)

        shots, perfs = {}, {}
        for name, rot, desc in arms:
            print("\n[arm %s] %s" % (name, desc))
            apply_shape(c, args.source, args.filter, rot)
            cur = c.filter_settings(args.source, args.filter).get("input_rot")
            print("  input_rot=%r（读回）" % cur)

            # 丢掉切换所在的那个窗口（它一半属于上一臂），取下一个完整窗口
            line = len(read_lines(lp))
            line2, _ = wait_perf(lp, line, timeout=30.0)
            _, perf = wait_perf(lp, line2, timeout=30.0)
            if perf is None:
                print("  ⚠ 没等到 perf 行（BEAUTY_PROFILE 没生效？）")
            else:
                perfs[name] = perf
                print("  掩码 %.2f%%  有脸 %d%%  thumb=%d"
                      % (perf[0], perf[1], perf[2]))

            p = os.path.join(dst, "%s_on.png" % name)
            time.sleep(args.settle)
            # 用**源截图**而不是程序输出截图：源截图不经过场景项变换，两臂几何
            # 完全一致，做差只反映滤镜本身。代价是按需渲染下掩码可能不新鲜
            # （perf 行 anom=nofresh），五官几何会滞后一两帧 —— 对「效果落在哪」
            # 这个定性判断无影响，但**不能**用它的面积做定量结论。
            c.shot(args.source, p)
            shots[name + "_on"] = p
            print("  截图 %s" % os.path.basename(p))

            # 随手补一张关滤镜的参考帧，和 on 帧只隔 0.5 s：真人移动带来的
            # 差异被压到最小，footprint 才有点参考价值。
            c.set_filter_enabled(args.source, args.filter, False)
            time.sleep(0.5)
            q = os.path.join(dst, "%s_off.png" % name)
            c.shot(args.source, q)
            shots[name + "_off"] = q
            c.set_filter_enabled(args.source, args.filter, True)
            print("  参考帧 %s" % os.path.basename(q))

        # ---- 汇总 ----
        print("\n" + "=" * 74)
        print("%-6s %-22s %8s %8s %8s %10s" %
              ("臂", "说明", "掩码%", "有脸%", "thumb", "footprint"))
        print("-" * 74)
        for name, rot, desc in arms:
            pf = perfs.get(name)
            fp = ""
            if name + "_on" in shots and name + "_off" in shots:
                r = footprint(load_rgb(shots[name + "_on"]),
                              load_rgb(shots[name + "_off"]))
                if r:
                    fp = "%.2f%%" % r[0]
            print("%-6s %-22s %8s %8s %8s %10s" % (
                name, desc,
                "%.2f" % pf[0] if pf else "--",
                "%d" % pf[1] if pf else "--",
                "%d" % pf[2] if pf else "--",
                fp))

        # 判定：不预设人朝哪边，只看「四臂是否分得开」。
        # 主判据是**有脸 %**。注意「掩码 %」在这组配置下恒为 0 —— 日志里
        # gonly=180 / masknull=180 说明走的是纯几何掩码路径，皮肤掩码不产生，
        # 那个数不能当判据（2.13.1 实机第一次跑就在这里误判过）。
        # 2.13.0 及以前全帧路径不读 input_rot，四臂会给出同一个「有脸%」；
        # 只要 ir0 与其余三臂分开，就证明旋转确实进了全帧检测路径。
        print("-" * 74)
        verdict = []
        def face(a):
            pf = perfs.get(a)
            return None if pf is None else pf[1]
        f0, f9, f18, f27 = face("ir0"), face("ir90"), face("ir180"), face("ir270")
        if None not in (f0, f9, f18, f27):
            side_ok = (f9 <= 10 and f18 <= 10 and f27 <= 10)
            if f0 >= 80 and side_ok:
                verdict.append(("正放检出、三个旋转臂都检不到", "PASS",
                                "有脸 %d%% / %d%% / %d%% / %d%%"
                                % (f0, f9, f18, f27)))
                verdict.append(("结论：input_rot 已进全帧检测路径", "PASS",
                                "2.13.0 及以前四臂应同为 %d%%" % f0))
            elif f0 < 80:
                verdict.append(("正放臂本身就没检到脸", "FAIL",
                                "有脸 %d%% —— 人被挡住/太小/低头，先修机位" % f0))
            else:
                verdict.append(("某个旋转臂仍然检出", "FAIL",
                                "有脸 %d%% / %d%% / %d%% / %d%%（旋转没生效）"
                                % (f0, f9, f18, f27)))
        else:
            print("  （没跑够四臂，无法判定）")
        for what, v, why in verdict:
            print("  %-34s %-5s %s" % (what, v, why))

        meta = {"tag": tag, "collection": args.collection, "source": args.source,
                "filter": args.filter, "shape": SHAPE, "perf": perfs,
                "shots": {k: os.path.basename(v) for k, v in shots.items()},
                "verdict": verdict, "log": lp,
                "time": time.strftime("%Y-%m-%d %H:%M:%S")}
        with open(os.path.join(dst, "_meta.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
        print("\n产物：%s" % dst)
        c.close()
    finally:
        if proc is not None:
            print("\n关闭 OBS …")
            V.close_obs()


if __name__ == "__main__":
    main()
