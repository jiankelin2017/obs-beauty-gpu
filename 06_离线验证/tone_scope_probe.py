# -*- coding: utf-8 -*-
# tone_scope_probe.py  v0.1.1
#
# 一次性离线实验脚本（默认关）：对照「美白作用域」的三种取法在真实手掌遮眼
# 素材上的观感与数值。不参与产品构建、不被任何脚本引用、不修改任何产品源码 /
# DLL / 着色器 / 版本号 / 装机目录；只读源码 LUT，只读视频，只写本目录下的 PNG。
#
#   A 现状 2.16.1 : sk = mask/255          局部遮罩美白（手也在 mask 内 -> 叠到手上）
#   B G1 全局    : sk = 1                 全帧无差别美白（无遮罩边界）
#   C D3 淡出    : sk = mask/255 * gain    遮挡期 gain=0 的理想上界 -> 完全原色
#
# toneColor() 的 LUT 与插值逐行复刻自 01_源码/beauty_freq.cpp:481-578；
# mask 由 2.16.1 的 core_probe.dll (BeautyCoreGetMask) 提供真实软边值。
# 自校验：复刻的 A 必须与探针 BeautyCoreRun 的真实输出逐像素吻合。
#
# 运行： py -3.12 tone_scope_probe.py
#        （本机 python 是 Store 别名，验证脚本一律用 py -3.12）
#
import os
import re
import shutil
import ctypes
import numpy as np
import cv2

ROOT = r"D:\插件\OBS美颜GPU版"
VIDEO = os.path.join(ROOT, "05_验证", "out", "live_edge", "2026-11-04 22-17-08.mp4")
DLL = os.path.join(ROOT, "06_离线验证", "core_probe.dll")
OUT_PNG = os.path.join(ROOT, "06_离线验证", "_cmp_tone_scope.png")
TMP = os.environ.get("TEMP", ".")
ASCII_VIDEO = os.path.join(TMP, "wb_live_edge.mp4")
FRAME_IDX = 150          # 5.0 s：手掌稳定遮住右眼/半张脸
W, H = 960, 540          # 缩小是为了少占 CPU（后台有直播）
WHITEN_PANEL = 100.0     # 面板值；wv = panel/100 -> toneColor 的 [0,1] 参数
WHITEN = WHITEN_PANEL / 100.0
CLARITY = 0.0
ROSY = 0.0

# ---- beauty_freq.cpp:481-507  清晰 LUT（本实验 clarity=0，保留仅为完整性）----
CLAR = np.array([
    [[0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255]] * 3,
    [[0,16,32,47,62,78,95,112,129,146,162,179,195,210,225,241,255],
     [0,16,31,46,62,79,95,112,129,146,163,179,194,209,224,240,255],
     [0,16,31,46,62,79,95,112,129,146,163,179,195,209,225,240,255]],
    [[0,16,32,46,60,77,94,112,129,147,165,182,198,212,227,241,255],
     [0,16,29,44,60,77,95,113,131,149,166,182,195,211,225,240,255],
     [0,16,29,44,60,77,95,112,131,149,166,182,197,211,226,240,255]],
    [[0,16,32,45,58,75,93,111,130,149,167,185,200,215,229,241,255],
     [0,16,28,42,58,76,94,113,133,151,169,185,197,212,225,241,255],
     [0,16,28,42,58,76,94,113,132,151,169,185,200,212,227,241,255]],
    [[0,16,32,43,56,74,92,111,131,151,170,188,203,217,230,242,255],
     [0,16,26,40,56,74,94,114,134,154,172,188,199,214,226,241,255],
     [0,16,26,41,56,74,93,113,133,153,171,188,203,214,228,241,255]],
], dtype=np.float32)

# ---- beauty_freq.cpp:509-535  美白 LUT -------------------------------------
WHITE = np.array([
    [[0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240,255]] * 3,
    [[0,16,32,49,65,82,99,115,132,148,164,179,195,210,225,241,255],
     [0,16,33,49,66,82,99,116,132,148,163,179,194,209,225,241,255],
     [0,16,32,49,66,83,100,116,133,149,164,179,195,210,225,240,255]],
    [[0,16,32,50,67,85,102,119,136,152,167,182,197,212,227,241,255],
     [0,16,33,51,68,85,103,119,136,152,167,182,196,211,225,241,255],
     [0,16,32,51,68,86,104,121,137,153,168,182,198,211,227,241,255]],
    [[0,16,32,50,68,87,105,122,140,156,171,185,200,214,228,242,254],
     [0,16,34,52,69,87,106,123,140,156,170,185,197,212,226,242,255],
     [0,16,32,52,70,89,108,125,142,158,172,186,201,213,228,241,255]],
    [[0,16,32,51,70,89,108,126,144,160,174,188,203,216,230,242,254],
     [0,16,35,54,71,90,109,126,143,159,174,187,199,214,227,242,255],
     [0,16,32,53,72,92,112,129,146,162,176,189,204,215,230,242,255]],
], dtype=np.float32)

# ---- beauty_freq.cpp:538-539  红润（只压绿、蓝微升）------------------------
ROS_G = np.array([0,0,-4,-4,-5,-5,-5,-5,-4,-4,-3,-3,-1,-1,0,0,0], dtype=np.float32)
ROS_B = np.array([0,0,2,1,1,0,1,1,1,1,1,1,1,1,1,0,0], dtype=np.float32)


def lut17(t, v):
    """beauty_freq.cpp:544  lut17 —— 17 锚点(步长16, 末点255)线性插值。"""
    p = v * (1.0 / 16.0)
    i = np.clip(np.floor(p).astype(np.int32), 0, 15)
    f = np.clip(p - i, 0.0, 1.0)
    val = t[i] + (t[i + 1] - t[i]) * f
    val = np.where(p <= 0.0, t[0], val)
    val = np.where(p >= 16.0, t[16], val)
    return val


def lv5(a):
    """beauty_freq.cpp:563  Lv5 —— 档位 0..1 映射到表索引 0..4。"""
    x = float(np.clip(a * 4.0, 0.0, 3.999))
    i0 = int(x)
    return i0, x - i0


def at(t, i0, f, c, v):
    return lut17(t[i0][c], v) * (1.0 - f) + lut17(t[i0 + 1][c], v) * f


def tone_color(src, sk, clarity, whiten, rosy):
    """beauty_freq.cpp:582  toneColor 的逐像素等价实现（全帧 ROI）。"""
    r = src[:, :, 0].astype(np.float32)
    g = src[:, :, 1].astype(np.float32)
    b = src[:, :, 2].astype(np.float32)
    if clarity > 0.0:
        i0, f = lv5(clarity)
        r = r + (at(CLAR, i0, f, 0, r) - r) * sk
        g = g + (at(CLAR, i0, f, 1, g) - g) * sk
        b = b + (at(CLAR, i0, f, 2, b) - b) * sk
    if whiten > 0.0:
        i0, f = lv5(whiten)
        r = r + (at(WHITE, i0, f, 0, r) - r) * sk
        g = g + (at(WHITE, i0, f, 1, g) - g) * sk
        b = b + (at(WHITE, i0, f, 2, b) - b) * sk
    if rosy > 0.0:
        g = g + lut17(ROS_G, g) * rosy * sk
        b = b + lut17(ROS_B, b) * rosy * sk
    out = np.stack([np.clip(np.floor(x + 0.5), 0.0, 255.0) for x in (r, g, b)], axis=2)
    out = out.astype(np.uint8)
    # C++ 里 mv==0 的像素直接 continue，一个字节都不动
    return np.where((sk > 0.0)[:, :, None], out, src)


def load_probe():
    d = ctypes.WinDLL(DLL)
    d.BeautyCoreRun.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_double, ctypes.c_int, ctypes.c_float, ctypes.c_float,
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    d.BeautyCoreRun.restype = ctypes.c_int
    for name, args in [
        ("BeautyCoreSetPerf", [ctypes.c_int] * 2),
        ("BeautyCoreSetTone", [ctypes.c_double] * 2),
        ("BeautyCoreSetClarityPanel", [ctypes.c_double]),
        ("BeautyCoreSetDetail", [ctypes.c_double] * 2),
        ("BeautyCoreSetRegion", [ctypes.c_int, ctypes.c_double]),
        ("BeautyCoreSetShape", [ctypes.c_int, ctypes.c_double]),
    ]:
        getattr(d, name).argtypes = args
    d.BeautyCoreGetMask.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int, ctypes.c_int]
    d.BeautyCoreGetMask.restype = ctypes.c_int
    d.BeautyCoreSetPerf(1, 1)
    d.BeautyCoreSetTone(WHITEN_PANEL, 0.0)   # needMask 由它驱动，必须非 0
    d.BeautyCoreSetClarityPanel(0.0)
    d.BeautyCoreSetDetail(0.0, 0.0)
    for i in range(5):
        d.BeautyCoreSetRegion(i, 0.0)
    for i in range(3):
        d.BeautyCoreSetShape(i, 0.0)
    return d


def read_frame():
    # OpenCV 5.0 打不开中文路径，复制一份到纯 ASCII 临时路径（只读原视频）
    if not os.path.isfile(ASCII_VIDEO):
        shutil.copyfile(VIDEO, ASCII_VIDEO)
    cap = cv2.VideoCapture(ASCII_VIDEO)
    cap.set(cv2.CAP_PROP_POS_FRAMES, FRAME_IDX)
    ok, bgr = cap.read()
    cap.release()
    if not ok:
        raise RuntimeError("cannot read frame %d" % FRAME_IDX)
    bgr = cv2.resize(bgr, (W, H), interpolation=cv2.INTER_AREA)
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    return rgb, np.dstack([rgb, np.full((H, W), 255, np.uint8)])


def run_engine(d, rgba):
    fin = os.path.join(TMP, "wb_scope_run_in.raw")
    fout = os.path.join(TMP, "wb_scope_run_out.raw")
    rgba.tofile(fin)
    msg = ctypes.create_string_buffer(512)
    rc = d.BeautyCoreRun(fin.encode("utf-8"), fout.encode("utf-8"), W, H,
                         0.0, 0, 0.0, 0.0, 1, msg, 512)
    txt = msg.value.decode("gbk", "ignore").strip()
    if rc != 0:
        raise RuntimeError("BeautyCoreRun rc=%d %s" % (rc, txt))
    out = np.fromfile(fout, np.uint8).reshape(H, W, 4)[:, :, :3].copy()
    buf = (ctypes.c_ubyte * (W * H))()
    d.BeautyCoreGetMask(buf, W, H)
    mask = np.frombuffer(buf, np.uint8).reshape(H, W).copy()
    return out, mask, txt


def ymean(a):
    return a.astype(np.float32).mean(axis=2)


ROIS = {   # 960x540 坐标，由 _tone_scope_mask_check.png 读出
    "hand":      (470, 150, 620, 500),
    "face":      (270, 120, 470, 530),
    "bg_left":   (40, 100, 230, 500),
    "bg_right":  (650, 100, 900, 500),
}


def panel(src_bgr, title):
    im = src_bgr.copy()
    cv2.putText(im, title, (10, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)
    return im


def main():
    d = load_probe()
    rgb, rgba = read_frame()
    engine_out, mask, logtxt = run_engine(d, rgba)

    sk_local = mask.astype(np.float32) / 255.0
    a_local = tone_color(rgb, sk_local, CLARITY, WHITEN, ROSY)          # 现状
    b_global = tone_color(rgb, np.ones((H, W), np.float32), CLARITY, WHITEN, ROSY)  # G1
    c_fade = tone_color(rgb, sk_local * 0.0, CLARITY, WHITEN, ROSY)      # D3 gain=0

    diff = np.abs(a_local.astype(np.int16) - engine_out.astype(np.int16))
    print("=== 自校验：复刻 A vs 探针真实 toneColor 输出 ===")
    print("  log       :", logtxt[:150])
    print("  maxdiff   :", int(diff.max()), " meandiff:", round(float(diff.mean()), 4),
          " px(diff>1):", int((diff > 1).sum()), "/", W * H)
    print("  mask>0    : %d (%.2f%%)  mask>128: %d" %
          (int((mask > 0).sum()), 100.0 * (mask > 0).mean(), int((mask > 128).sum())))

    print("\n=== 区域平均亮度变化 (out - src, 面板美白 100) ===")
    ys = ymean(rgb)
    rows = {}
    for name, out in [("A 现状", a_local), ("B G1 全局", b_global), ("C D3 淡出", c_fade)]:
        dy = ymean(out) - ys
        rows[name] = {k: float(dy[y0:y1, x0:x1].mean()) for k, (x0, y0, x1, y1) in ROIS.items()}
        print("  %-12s hand=%+6.2f face=%+6.2f bgL=%+6.2f bgR=%+6.2f 全帧=%+6.2f  改动像素=%6.2f%%" %
              (name, rows[name]["hand"], rows[name]["face"], rows[name]["bg_left"],
               rows[name]["bg_right"], float(dy.mean()),
               100.0 * float((np.abs(dy) > 0.5).mean())))
    for name, r in rows.items():
        halo = r["hand"] - 0.5 * (r["bg_left"] + r["bg_right"])
        print("  %-12s 手-背景亮度差(halo 指标) = %+6.2f" % (name, halo))

    # ---- v0.1.1：边界 / 对比关系 -----------------------------------------
    # 「遮罩边界是否还在」不能只看手变亮多少，要看改动量是否仍由 mask 决定。
    #  corr(dy,mask) 高 = 改动跟着遮罩走；边界梯度高 = mask 轮廓处有亮度跳变。
    print("\n=== 遮罩边界 / 手与紧邻背景的对比 (v0.1.1) ===")
    core = mask > 128
    k3 = np.ones((3, 3), np.uint8)
    band = (cv2.dilate(core.astype(np.uint8), k3) > 0) & \
           ~(cv2.erode(core.astype(np.uint8), k3) > 0)
    print("  boundary band px:", int(band.sum()))
    for name, out in [("A 现状", a_local), ("B G1 全局", b_global), ("C D3 淡出", c_fade)]:
        dy = ymean(out) - ys
        corr = 0.0 if float(dy.std()) < 1e-9 else float(
            np.corrcoef(dy.ravel(), mask.ravel().astype(np.float32))[0, 1])
        gy, gx = np.gradient(dy.astype(np.float32))
        edge = float(np.hypot(gx, gy)[band].mean())
        roi_core = core[150:500, 470:620]
        hand = float(dy[150:500, 470:620][roi_core].mean()) if roi_core.any() else 0.0
        wall = float(dy[100:500, 650:900].mean())
        print("  %-12s corr(dy,mask)=%+.3f 边界梯度=%5.2f 手(mask内)=%+6.2f "
              "紧邻白墙=%+6.2f 手-墙=%+6.2f" % (name, corr, edge, hand, wall, hand - wall))

    # ---- 拼图：上排 4 张画面，下排 4 张差异放大 / mask --------------------
    to_bgr = lambda a: cv2.cvtColor(a, cv2.COLOR_RGB2BGR)
    amp = lambda a: cv2.cvtColor(
        np.clip(128 + (ymean(a) - ys) * 8.0, 0, 255).astype(np.uint8), cv2.COLOR_GRAY2BGR)
    hot = to_bgr(rgb).copy()
    hot[:, :, 1] = np.maximum(hot[:, :, 1], (mask.astype(np.float32) * 0.75).astype(np.uint8))
    tiles = [
        panel(to_bgr(rgb), "src (frame 150, hand over eye)"),
        panel(to_bgr(a_local), "A: local mask (2.16.1)"),
        panel(to_bgr(b_global), "B: G1 full-frame"),
        panel(to_bgr(c_fade), "C: D3 gain=0 (original)"),
        panel(amp(a_local), "A-src  x8"),
        panel(amp(b_global), "B-src  x8"),
        panel(amp(c_fade), "C-src  x8"),
        panel(hot, "real skin mask (hand included)"),
    ]
    small = [cv2.resize(t, (480, 270)) for t in tiles]
    for t in small[:4]:
        x0, y0, x1, y1 = [v // 2 for v in ROIS["hand"]]
        cv2.rectangle(t, (x0, y0), (x1, y1), (0, 0, 255), 1)
    sheet = np.vstack([np.hstack(small[:4]), np.hstack(small[4:])])
    ok, buf = cv2.imencode(".png", sheet)
    if not ok:
        raise RuntimeError("png encode failed")
    with open(OUT_PNG, "wb") as f:
        f.write(buf.tobytes())
    print("\nsaved:", OUT_PNG, os.path.getsize(OUT_PNG), "B")
    print("NOTE: 本实验只复刻色彩层 toneColor；磨皮 / 五官 / 细节层未纳入（三方案共同盲区）。")


if __name__ == "__main__":
    main()
