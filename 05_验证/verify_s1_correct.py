#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
S1 正确性验收主脚本（2026-11-02）。

补齐 PROGRESS.md 9.2.1 里「脚本做不了、需人工」的三项：

  第 1 项  像素对拍
      * 全 0 直通 vs 滤镜禁用          -> maxdiff 应为 0
      * 四组合（磨皮/清晰/美白/五官）  -> 2.3.0 vs 2.2.1 应 maxdiff=0
      * 每个组合还要「动了没有」自检   -> 非全 0 必须与全 0 有差异，
        否则说明该组合根本没生效（脸没检到），对拍会变成假通过。
  第 2 项  滤镜开关来回 20 次：无闪退、无黑帧。
  第 3 项  4K 画面：抓整幅场景图供目视 + 亮度/直方图统计。

做法：
  用 make_static_collection.py 生成的 beauty_static 集合（image_source 喂固定
  PNG），这样输入完全确定，两版 dll 的像素差只可能来自代码。
  通过 obs-websocket 改滤镜参数/开关，用 GetSourceScreenshot 取回插件输出。

子命令：
    python verify_s1_correct.py capture v230        # 采集一套（当前装的 dll）
    python verify_s1_correct.py compare v221 v230   # 逐图对比
    python verify_s1_correct.py toggle              # 开关 20 次
    python verify_s1_correct.py visual4k            # 4K 目视素材
    python verify_s1_correct.py swaplib 2.2.1       # 换运行时 dll（2.2.1/2.3.0）

产物：
    05_验证/out/<tag>/<config>.png      每次采集的图
    05_验证/out/<tag>/_meta.json        采集元信息（版本、日志路径、耗时）
    05_验证/out/compare_<A>_<B>.json    对比结果
"""
import argparse
import ctypes
import glob
import io
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import obsctl  # noqa: E402

ROOT = os.path.dirname(HERE)
OBS_ROOT = r"C:\Program Files\obs-studio"
OBS_EXE = os.path.join(OBS_ROOT, "bin", "64bit", "obs64.exe")
OBS_BIN = os.path.join(OBS_ROOT, "bin", "64bit")
LOGDIR = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "logs")
OUT = os.path.join(HERE, "out")
BACKUP = None  # 装机脚本的 backup 目录，运行时定位
_CANVAS_BAK = None  # basic.ini 的临时备份（画布同步用）

COLLECTION = "beauty_static"
COLLECTION_4K = "beauty_static_4k"
SRC = "still"
FILT = "Beauty"

PANEL_KEYS = [
    "blur_level", "clarity", "color_level", "red_level", "sharpen",
    "global_sharpen", "face_threed", "eye_bright", "remove_pouch_strength",
    "remove_nasolabial_folds_strength", "delspot_level", "thin_face",
    "big_eye", "chin",
]

# 采集配置矩阵。None = 把 OBS 侧滤镜开关关掉（等价「滤镜禁用」基准）。
# 数值按面板语义取中高值，保证效果明显但不糊。
CONFIGS = [
    ("allzero", "全 0（直通）", {}),
    ("off", "滤镜禁用（基准）", None),
    ("blur", "磨皮 80", {"blur_level": 80}),
    ("clarity", "清晰 80", {"clarity": 80}),
    ("whiten", "美白 60 + 红润 50", {"color_level": 60, "red_level": 50}),
    ("shape", "五官 60（瘦脸/大眼/下巴/立体）",
     {"thin_face": 0.6, "big_eye": 0.6, "chin": 0.6, "face_threed": 0.5}),
]


# ------------------------------------------------------------------ 工具
def dll_runtime():
    return r"C:\ProgramData\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll"


def backup_dir():
    """backup 目录。用 ASCII 前缀 "03_" 探测，避免依赖中文目录字面量。"""
    for d in glob.glob(os.path.join(ROOT, "03_*")):
        b = os.path.join(d, "backup")
        if os.path.isdir(b):
            return b
    return os.path.join(ROOT, "03_backup")


def dll_version(path):
    """从 dll 里读 BEAUTY_BUILD_STR（形如 2.3.0）。"""
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except Exception:
        return "?"
    cands = []
    for v in (b"2.0.0", b"2.1.0", b"2.1.1", b"2.2.0", b"2.2.1", b"2.3.0", b"2.3.1"):
        if v in blob:
            cands.append(v.decode())
    # 引擎版本固定是 2.0.0，滤镜版本取最大的那个
    cands = [c for c in cands if c != "2.0.0"] or cands
    return max(cands) if cands else "?"


def obs_running():
    p = subprocess.run(["tasklist", "/FI", "IMAGENAME eq obs64.exe"],
                       capture_output=True, text=True, errors="ignore")
    return "obs64.exe" in (p.stdout or "")


def close_obs(timeout=20):
    """给 OBS 主窗口发 WM_CLOSE，等它自己退（拿干净退出日志）。"""
    global _CANVAS_BAK
    if not obs_running():
        canvas_restore(_CANVAS_BAK); _CANVAS_BAK = None
        return True
    user32 = ctypes.windll.user32
    hwnds = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def _cb(hwnd, _):
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        if buf.value and "OBS" in buf.value:
            hwnds.append((hwnd, pid.value, buf.value))
        return True

    user32.EnumWindows(_cb, 0)
    for hwnd, pid, title in hwnds:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not obs_running():
            return True
        time.sleep(0.5)
    subprocess.run(["taskkill", "/IM", "obs64.exe", "/F"], capture_output=True)
    time.sleep(2)
    canvas_restore(_CANVAS_BAK); _CANVAS_BAK = None
    return not obs_running()


def newest_log(after_names):
    files = [f for f in glob.glob(os.path.join(LOGDIR, "*.txt"))
             if os.path.basename(f) not in after_names]
    if not files:
        return None
    return max(files, key=os.path.getmtime)


def log_dir_snapshot():
    return set(os.path.basename(f) for f in glob.glob(os.path.join(LOGDIR, "*.txt")))


# ---------------------------------------------------------------- 画布同步
# 集合里的 resolution 字段不会改画布 —— 画布在 profile 的 basic.ini [Video]。
# 画布和素材尺寸不一致时，程序截图里素材是缩放/带黑边的，而且 OBS 会顺手把
# 源的 pos 改写掉，两次运行构图不同（实测「滤镜禁用」基准图差 44% 像素）。
# 所以采集前把画布钉成素材尺寸，退出后原样恢复。
PROFILE_INI = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "basic",
                           "profiles", "未命名", "basic.ini")
INI_KEYS = ("BaseCX", "BaseCY", "OutputCX", "OutputCY")


def _profile_ini():
    if os.path.isfile(PROFILE_INI):
        return PROFILE_INI
    # 中文 profile 名用 ASCII 探测兜底
    root = os.path.join(os.environ.get("APPDATA", ""), "obs-studio", "basic", "profiles")
    for d in glob.glob(os.path.join(root, "*")):
        f = os.path.join(d, "basic.ini")
        if os.path.isfile(f):
            return f
    return None


def canvas_set(w, h):
    """把画布改成 w×h，返回原文件备份路径（供 canvas_restore 用）。"""
    f = _profile_ini()
    if not f:
        print("!! 找不到 basic.ini，画布未同步")
        return None
    bak = f + ".bak_verify_canvas"
    if not os.path.isfile(bak):
        shutil.copy2(f, bak)
    with open(f, "rb") as fh:
        data = fh.read()
    import re
    for key, val in (("BaseCX", w), ("BaseCY", h), ("OutputCX", w), ("OutputCY", h)):
        data = re.sub((r"(?m)^" + key + r"=\d+").encode(),
                      ("%s=%d" % (key, val)).encode(), data)
    with open(f, "wb") as fh:
        fh.write(data)
    return bak


def canvas_restore(bak):
    if bak and os.path.isfile(bak):
        f = _profile_ini()
        if f:
            shutil.copy2(bak, f)
        os.remove(bak)


def start_obs(collection, extra_env=None, canvas=None):
    global _CANVAS_BAK
    if obs_running():
        sys.exit("obs64 已在运行，先关掉（或跑 close_obs）")
    env = dict(os.environ)
    env.pop("BEAUTY_ALLZERO_SKIP", None)
    env.pop("BEAUTY_ENGINE_IMPL", None)
    for k in ("BEAUTY_GPU_TONE", "BEAUTY_GPU_SMOOTH",
              "BEAUTY_GPU_DETAIL", "BEAUTY_GPU_WARP", "BEAUTY_GPU_MASK_CMP",
              "BEAUTY_CHAIN_AB", "BEAUTY_AB_TARGET"):
        env.pop(k, None)   # 只测生产行为，不开任何诊断开关
    # BEAUTY_GPU=1 走 GPU 引擎（engine=gpu/2.0.0），与 2.2.1 基线 perf 采集、
    # 以及用户日常 GPU 版一致。实测：不设它时 engine=core 且 faces 恒为 0
    # （人脸检测在 GPU 引擎路径上），静态图对拍会全部退化成「无脸」假通过。
    env["BEAUTY_GPU"] = "1"
    if extra_env:
        env.update(extra_env)
    if canvas:
        _CANVAS_BAK = canvas_set(*canvas)
        print("画布同步为 %dx%d（退出后恢复）" % canvas)
    snap = log_dir_snapshot()
    p = subprocess.Popen([OBS_EXE, "--collection", collection,
                          "--disable-shutdown-check"],
                         cwd=OBS_BIN, env=env,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    return p, snap


def set_config(c, cfg):
    """cfg=None -> 关掉滤镜；否则 14 项清零后叠加 cfg，并确保启用。"""
    if cfg is None:
        c.set_filter_enabled(SRC, FILT, False)
        return
    st = {k: 0.0 for k in PANEL_KEYS}
    st.update(cfg)
    st["enabled"] = True
    c.set_filter_settings(SRC, FILT, st, overlay=True)
    c.set_filter_enabled(SRC, FILT, True)


def grab(c, path, settle=1.35, rounds=3):
    """改完参数后要等掩码/几何重建完。

    实测：切到 blur=80 后 0.45s 抓图，与「滤镜禁用」逐字节相同 —— 那时自适应
    肤色掩码还在重建，磨皮等于没跑。1.3s 之后才稳定（perf 行显示掩码 17.7%、
    有脸 100%）。所以这里等久一点、抓多张，只留最后一张。
    """
    time.sleep(settle)
    im = None
    for _ in range(rounds):
        im = c.shot(SRC, path)
        time.sleep(0.25)
    return im


# ---------------------------------------------------------------- 程序截图
# 为什么要换路：GetSourceScreenshot 是**按需渲染**，那一次渲染里掩码不新鲜
# （perf 行 anom=nofresh 计数正好等于截图次数），磨皮直接跳过 —— 实测
# blur=80/100、双边/高斯/弱三档全为 maxdiff=0，而 清晰/美白/五官 都有效果。
# 也就是说按需截图对「掩码相关效果」系统性失真。
# OBS 自带的「截图输出」热键取的是**视频管线真实输出帧**，才是用户看到的画面。
SHOT_DIRS = [r"D:\c", os.path.join(os.environ.get("USERPROFILE", ""), "Videos"),
             os.path.join(os.environ.get("USERPROFILE", ""), "Pictures")]


def _existing_shots():
    s = set()
    for d in SHOT_DIRS:
        if os.path.isdir(d):
            s |= set(glob.glob(os.path.join(d, "Screenshot*.png")))
    return s


def _wait_image(path, timeout=8.0):
    """等 PNG 写完再返回。截图是异步写盘，搬早了会拿到截断文件。"""
    from PIL import Image
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        try:
            im = Image.open(path)
            im.load()
            return im
        except Exception as e:
            last = e
            time.sleep(0.3)
    raise RuntimeError("PNG 一直不完整: %s (%s)" % (path, last))


def prog_shot(c, dst, settle=1.2):
    """触发 OBSBasic.Screenshot，把新出的 PNG 搬到 dst。"""
    before = _existing_shots()
    time.sleep(settle)
    c.req("TriggerHotkeyByName", {"hotkeyName": "OBSBasic.Screenshot"})
    for _ in range(60):
        time.sleep(0.15)
        new = _existing_shots() - before
        if new:
            f = max(new, key=os.path.getmtime)
            # 先在原位置确认能完整解码再搬：4K 的 PNG 有 8MB 上下，落盘比
            # 720p 慢，搬早了 OBS 会继续往旧路径写 → 目标文件截断。
            _wait_image(f, timeout=15.0)
            d = os.path.dirname(os.path.abspath(dst))
            if d and not os.path.isdir(d):
                os.makedirs(d)
            # 用同盘 rename（os.replace）而不是 shutil.move：跨盘时 move 会退化成
            # 复制+删除，而本机 shell 的 safe-delete 会把删除转去回收站，4K 大图
            # 直接报 "Some operations were aborted"。rename 不碰回收站。
            try:
                os.replace(f, dst)
            except OSError:
                shutil.copy2(f, dst)   # 跨盘兜底：复制，源文件留着不动
            return _wait_image(dst)
    raise RuntimeError("OBS 截图输出没有产生文件（检查 Screenshot 热键是否被占用）")


# ------------------------------------------------------------------ capture
def cmd_capture(args):
    global SRC, COLLECTION, FILT
    tag = args.tag
    if args.source:
        SRC = args.source
    if args.filter:
        FILT = args.filter
    if args.collection:
        COLLECTION = args.collection
    coll = args.collection or (COLLECTION_4K if args.collection_4k else COLLECTION)
    outdir = os.path.join(OUT, tag)
    os.makedirs(outdir, exist_ok=True)
    if not close_obs():
        sys.exit("关不掉正在运行的 OBS")

    ver = dll_version(dll_runtime())
    print("=" * 62)
    print("采集 tag=%s  集合=%s  运行时 dll 版本=%s" % (tag, coll, ver))
    print("=" * 62)
    canvas = (3840, 2160) if coll == COLLECTION_4K else (1280, 720)

    # 预热启动：OBS 启动时会先按旧画布排一次源位置，再切到 basic.ini 里的新
    # 画布重排，这个竞态会让「前几张图」和「后几张图」构图不同（实测：同一次
    # 运行里 allzero/off 左侧有黑边、后面四张铺满）。先空跑一次让 OBS 按最终
    # 画布把集合归一化并回写，正式采集就不会再动。
    if not args.no_prepare:
        print("预热启动（让 OBS 按 %dx%d 归一化集合）..." % canvas)
        start_obs(coll, canvas=canvas)
        time.sleep(7)
        close_obs()

    envx = {}
    for kv in (args.env or []):
        k, _, v = kv.partition("=")
        envx[k.strip()] = v.strip()
    if envx:
        print("附加环境变量: %s" % envx)
    proc, snap = start_obs(coll, extra_env=envx or None, canvas=canvas)
    meta = {"tag": tag, "collection": coll, "dll_version": ver,
            "started": time.strftime("%Y-%m-%d %H:%M:%S"), "shots": []}
    try:
        c = obsctl.wait_ready(wait=90)
        print("websocket 已连接")
        # 等滤镜真正开始出帧（verified 预热完成），否则首张可能还是全路径
        time.sleep(2.5)

        # 清掉多余的美颜滤镜：实测源上会莫名多出一个 "OBS美颜"（与我的
        # "Beauty" 并存且都启用），两个滤镜串联处理 → 每次采集的基准整体漂移
        # （同一版本两次对拍 diff 到 16~26，而实际应该 0）。
        for where in (SRC, "Main"):
            try:
                for f in c.filters(where):
                    nm = f.get("filterName")
                    if nm != FILT:
                        c.req("RemoveFilter", {"sourceName": where, "filterName": nm})
                        print("已移除多余滤镜: %s.%s" % (where, nm))
            except Exception as e:
                print("!! 清理 %s 滤镜失败: %s" % (where, e))
        try:
            fs = c.filters(SRC)
            beauties = [f.get("filterName") for f in fs
                        if f.get("filterName") != FILT]
            for nm in beauties:
                # 只删插件自己的滤镜，别误删用户的其它滤镜
                try:
                    c.req("RemoveFilter", {"sourceName": SRC, "filterName": nm})
                    print("已移除多余滤镜: %r" % nm)
                except Exception:
                    pass
        except Exception as e:
            print("!! 滤镜清理失败: %s" % e)

        # 把源强制摆到画布原点、1:1。
        # 实测：滤镜「在处理中」时（非零参数）绘制会忽略源的位移，而直通/禁用
        # 时按位移走 —— 于同一次运行里，allzero/off 左侧有黑边、其余四张铺满，
        # 跨配置对拍直接失效（差异 100%，maxdiff 255）。归一化位置后两种状态
        # 重合，差异才只反映特效本身。
        try:
            sid = None
            for it in c.scene_items("Main"):
                if it.get("sourceName") == SRC:
                    sid = it.get("sceneItemId")
                    break
            if sid is None:
                print("!! 找不到源 %r 的场景项，跳过位置归一" % SRC)
            else:
                c.set_item_transform("Main", sid, {
                    "positionX": 0.0, "positionY": 0.0,
                    "scaleX": 1.0, "scaleY": 1.0,
                    "rotation": 0.0, "alignment": 5,
                    "boundsType": "OBS_BOUNDS_NONE",
                })
                time.sleep(0.6)
                print("源位置已归一: pos=(0,0) scale=1")
        except Exception as e:
            print("!! 位置归一失败: %s" % e)
        for key, label, cfg in CONFIGS:
            set_config(c, cfg)
            p = os.path.join(outdir, key + ".png")
            im = grab(c, p) if args.src_shot else prog_shot(c, p, settle=args.settle)
            print("  %-8s %-28s -> %s %s" % (key, label, os.path.basename(p), im.size))
            meta["shots"].append({"key": key, "label": label, "cfg": cfg,
                                  "file": os.path.basename(p),
                                  "size": list(im.size)})
        # 收尾：恢复全 0
        set_config(c, {})
        c.close()
    finally:
        close_obs()
        log = newest_log(snap)
        meta["log"] = log
        with open(os.path.join(outdir, "_meta.json"), "w", encoding="utf-8") as fh:
            json.dump(meta, fh, ensure_ascii=False, indent=2)
        print("日志: %s" % log)
        _log_digest(log)
    return 0


def _log_digest(log):
    if not log or not os.path.isfile(log):
        print("(没有新日志)")
        return
    keys = ["init OK", "filter registered", "skip_sdk", "error", "failed",
            "GPU 掩码对拍跳过"]
    print("---- 日志要点 ----")
    with open(log, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            if any(k in line for k in keys):
                print("  " + line.rstrip()[:160])


# ------------------------------------------------------------------ compare
def load(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


def diff2(a, b):
    import numpy as np
    if a.shape != b.shape:
        return {"shape_mismatch": [list(a.shape), list(b.shape)]}
    d = np.abs(a - b)
    per = d.reshape(-1, d.shape[-1])
    mx = int(d.max())
    changed = int((per.max(axis=1) > 0).sum())
    total = per.shape[0]
    return {
        "maxdiff": mx,
        "mean_abs": round(float(d.mean()), 4),
        "changed_pixels": changed,
        "changed_pct": round(100.0 * changed / total, 3),
        "max_channel": [int(d[..., i].max()) for i in range(d.shape[-1])],
    }


def cmd_compare(args):
    import numpy as np
    a_dir = os.path.join(OUT, args.a)
    b_dir = os.path.join(OUT, args.b)
    res = {"a": args.a, "b": args.b, "rows": []}
    print("%-8s %-26s %8s %9s %10s" % ("config", "说明", "maxdiff", "mean_abs", "变化像素%"))
    print("-" * 68)
    for key, label, _ in CONFIGS:
        fa = os.path.join(a_dir, key + ".png")
        fb = os.path.join(b_dir, key + ".png")
        if not (os.path.isfile(fa) and os.path.isfile(fb)):
            print("%-8s %-26s  (缺图)" % (key, label))
            continue
        r = diff2(load(fa), load(fb))
        r.update({"key": key, "label": label})
        res["rows"].append(r)
        print("%-8s %-26s %8s %9s %10s" % (
            key, label, r.get("maxdiff"), r.get("mean_abs"), r.get("changed_pct")))
    print("-" * 68)

    # 「动了没有」自检：非全 0 配置必须相对基准(off)有变化，否则组合没生效
    print("\n---- 效果自检（非全 0 必须比『滤镜禁用』有差异，否则等于没测到）----")
    res["effect"] = {}
    off = os.path.join(b_dir, "off.png")
    for key, label, cfg in CONFIGS:
        if not cfg:
            continue
        f = os.path.join(b_dir, key + ".png")
        if not (os.path.isfile(f) and os.path.isfile(off)):
            continue
        r = diff2(load(off), load(f))
        ok = r.get("maxdiff", 0) > 0
        res["effect"][key] = {"changed_pct": r.get("changed_pct"), "ok": ok}
        print("  %-8s %-26s 变化 %6s%%  %s" % (
            key, label, r.get("changed_pct"), "OK 生效" if ok else "!! 无变化"))
    dst = os.path.join(OUT, "compare_%s_%s.json" % (args.a, args.b))
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(res, fh, ensure_ascii=False, indent=2)
    print("\n结果写入: %s" % dst)
    return 0


# ------------------------------------------------------------------ toggle
def _session(coll, canvas, prepare=True):
    """建一个可用的采集会话：(proc, snap, client)。含画布同步、预热、清滤镜、归一位置。"""
    if not close_obs():
        sys.exit("关不掉正在运行的 OBS")
    if prepare:
        print("预热启动（让 OBS 按 %dx%d 归一化集合）..." % canvas)
        start_obs(coll, canvas=canvas)
        time.sleep(7)
        close_obs()
    proc, snap = start_obs(coll, canvas=canvas)
    c = obsctl.wait_ready(wait=90)
    time.sleep(2.5)
    for where in (SRC, "Main"):
        try:
            for f in c.filters(where):
                nm = f.get("filterName")
                if nm != FILT:
                    c.req("RemoveFilter", {"sourceName": where, "filterName": nm})
                    print("已移除多余滤镜: %s.%s" % (where, nm))
        except Exception as e:
            print("!! 清理 %s 滤镜失败: %s" % (where, e))
    try:
        sid = None
        for it in c.scene_items("Main"):
            if it.get("sourceName") == SRC:
                sid = it.get("sceneItemId")
                break
        if sid is not None:
            c.set_item_transform("Main", sid, {
                "positionX": 0.0, "positionY": 0.0,
                "scaleX": 1.0, "scaleY": 1.0,
                "rotation": 0.0, "alignment": 5,
                "boundsType": "OBS_BOUNDS_NONE"})
            time.sleep(0.6)
    except Exception as e:
        print("!! 位置归一失败: %s" % e)
    return proc, snap, c


def cmd_toggle(args):
    """第 2 项：滤镜开关来回 20 次，检查无黑帧、无闪退。

    采样用程序截图（真实输出帧），不是按需渲染 —— 后者对磨皮这类掩码相关
    效果系统性失真，黑帧判据会偏松。
    """
    outdir = os.path.join(OUT, "toggle")
    os.makedirs(outdir, exist_ok=True)
    proc, snap, c = _session(COLLECTION, (1280, 720))
    ok = True
    shots = []
    try:
        set_config(c, {})          # 全 0 + 启用 → 直通分支激活
        prog_shot(c, os.path.join(outdir, "_warm.png"), settle=2.0)
        print("\n开始来回切 20 次（每次切完取一帧真实输出）...")
        for i in range(1, 21):
            c.set_filter_enabled(SRC, FILT, False)
            im1 = prog_shot(c, os.path.join(outdir, "off_%02d.png" % i), settle=0.55)
            c.set_filter_enabled(SRC, FILT, True)
            im2 = prog_shot(c, os.path.join(outdir, "on_%02d.png" % i), settle=0.55)
            m1, m2 = _mean(im1), _mean(im2)
            black = (m1 < 8) or (m2 < 8)
            if black:
                ok = False
            shots.append((i, m1, m2, black))
            print("  #%02d off mean=%6.2f  on mean=%6.2f  %s" % (
                i, m1, m2, "!! 黑帧" if black else "ok"))
            if not obs_running():
                print("  !! OBS 已退出（闪退）")
                ok = False
                break
        alive = obs_running()
        print("\nOBS 仍存活: %s" % alive)
        ok = ok and alive
        c.close()
    finally:
        close_obs()
        log = newest_log(snap)
        print("日志: %s" % log)
        _log_digest(log)
        print("\n第 2 项结论: %s" % (
            "PASS（20 次开关无黑帧、无闪退）" if ok else "FAIL"))
    return 0 if ok else 1


def _mean(im):
    import numpy as np
    a = np.asarray(im.convert("RGB")).astype("float32")
    return float(a.mean())


# ------------------------------------------------------------------ 4K
def cmd_visual4k(args):
    """第 3 项：3840x2160 真实输出帧，供目视 + 亮度统计。"""
    outdir = os.path.join(OUT, "visual4k")
    os.makedirs(outdir, exist_ok=True)
    proc, snap, c = _session(COLLECTION_4K, (3840, 2160))
    rows = []
    try:
        cases = [("off", None),
                 ("allzero", {}),
                 ("combo", {"blur_level": 50, "clarity": 30,
                            "color_level": 40, "red_level": 40})]
        for key, cfg in cases:
            set_config(c, cfg)
            p = os.path.join(outdir, key + ".png")
            im = prog_shot(c, p, settle=4.0)
            m = _mean(im)
            rows.append((key, im.size, m))
            print("  %-8s %s mean=%.2f" % (key, im.size, m))
            # 另存一份 1080p 便于查看
            im.convert("RGB").resize((1920, 1080)).save(
                os.path.join(outdir, key + "_1080p.png"))
        c.close()
    finally:
        close_obs()
        log = newest_log(snap)
        print("日志: %s" % log)
        _log_digest(log)

    import numpy as np
    from PIL import Image as _Im

    def L(n):
        return np.asarray(_Im.open(os.path.join(outdir, n + ".png"))
                          .convert("RGB")).astype(np.int16)
    off, az, cb = L("off"), L("allzero"), L("combo")
    d1 = np.abs(az - off)
    d2 = np.abs(cb - off)
    print("\n---- 4K 判据 ----")
    print("  全 0 直通 vs 滤镜禁用 : maxdiff=%d 变化=%.2f%%" % (
        d1.max(), 100.0 * (d1.max(axis=2) > 0).mean()))
    print("  美颜组合 vs 滤镜禁用  : maxdiff=%d 变化=%.2f%% %s" % (
        d2.max(), 100.0 * (d2.max(axis=2) > 0).mean(),
        "OK 生效" if d2.max() > 0 else "!! 无变化"))
    return 0


# ------------------------------------------------------------------ swaplib
def cmd_swaplib(args):
    want = args.version
    rt = dll_runtime()
    if not close_obs():
        sys.exit("OBS 关不掉，不能换 dll")
    cur = dll_version(rt)
    print("当前运行时 dll: %s  (版本 %s)" % (rt, cur))
    if cur == want:
        print("已经是 %s，无需切换。" % want)
        return 0
    if os.path.isfile(want):          # 直接给 dll 路径（用自编译基线时）
        src = want
        print("直接使用指定 dll: %s (版本 %s)" % (src, dll_version(src)))
    else:
        cands = [f for f in glob.glob(os.path.join(backup_dir(), "*.dll"))
                 if dll_version(f) == want]
        if not cands:
            sys.exit("backup 里找不到版本 %s 的 dll" % want)
        src = max(cands, key=os.path.getmtime)
        print("选用备份: %s" % os.path.basename(src))
    # 先把当前 dll 也备一份，方便切回
    keep = os.path.join(backup_dir(), "swap_prev_%s.dll" % time.strftime("%Y%m%d_%H%M%S"))
    shutil.copy2(rt, keep)
    print("当前 dll 备份为: %s" % os.path.basename(keep))
    shutil.copy2(src, rt)
    print("已切换 -> 版本 %s" % dll_version(rt))
    return 0


# ------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description="S1 正确性验收")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("capture", help="采集一套截图")
    p.add_argument("tag")
    p.add_argument("--collection-4k", action="store_true")
    p.add_argument("--collection", help="覆盖集合名（如 beauty_s1 用摄像头）")
    p.add_argument("--source", help="覆盖源名")
    p.add_argument("--filter", help="覆盖滤镜名")
    p.add_argument("--env", action="append", metavar="K=V",
                   help="附加环境变量，可多次（如 BEAUTY_ALLZERO_SKIP=0）")
    p.add_argument("--no-prepare", action="store_true",
                   help="跳过预热启动（调试用）")
    p.add_argument("--settle", type=float, default=4.0,
                   help="每次切参数后等多久再截图（磨皮 EMA 收敛需要时间，默认 4s）")
    p.add_argument("--src-shot", action="store_true",
                   help="用 GetSourceScreenshot（按需渲染，磨皮会失真），默认用程序截图")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("compare", help="逐图对比两套采集")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("toggle", help="滤镜开关 20 次")
    p.set_defaults(func=cmd_toggle)

    p = sub.add_parser("visual4k", help="4K 目视素材")
    p.set_defaults(func=cmd_visual4k)

    p = sub.add_parser("swaplib", help="切换运行时 dll 版本")
    p.add_argument("version", help="2.2.1 / 2.3.0")
    p.set_defaults(func=cmd_swaplib)

    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
