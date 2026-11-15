# -*- coding: utf-8 -*-
"""P2（值域加权核）离线对拍：把 smooth.hlsl 的整条链在 CPU 上逐式重放。

为什么需要它：`BEAUTY_BILAT` 只在 GPU 着色器里生效，`core_probe.dll`（离线回归台）
是纯 CPU 引擎、没有值域加权实现，所以 P2 无法用既有的离线台验收。这个脚本按
`shaders/smooth.hlsl` 的公式与 `beauty_gpu_smooth()` 的趟序逐行重放，只为回答一件事：
**同样参数下，box 与 bilateral 在发丝/皮肤边界上差多少。**

它不是替代品：GPU 的 16F 存储、双线性采样边界、draw 顺序都不在这里。凡是本脚本给出
的数字，都只能用于「两种核的相对比较」，不能当成 GPU 实测耗时或最终画面。

用法：
    python p2_bilat_probe.py --img ../../05_验证/assets/p2_hairline_20261105.jpg \
        --out _p2_hairline

输出：
    <out>.png          四联：原图 | box | bilateral(40/20) | bilateral(旧：两趟共用 40)
    <out>.txt          detail_keep / flat_smooth / 以及三者的耗时
"""
import argparse
import os
import time

import numpy as np


# ---------------------------------------------------------------- 基本算子
def box_axis(img, rad, axis, yF=None):
    """一趟可分离盒式。yF 非 None 时做值域加权（等价 shader 的 bilatK=1）。

    与 shader 一致：
      · 中心权重恒 1（未归一化时 acc 从 c0 起算）
      · 权重 wk = 1 - min(((ck.g - c0.g)/255)^2 * yF, 1)
      · 边界用 clamp（numpy 的 edge 补边 = shader 的 Clamp 寻址）
    """
    if rad <= 0:
        return img.astype(np.float32, copy=True)

    pad = ((rad, rad), (0, 0), (0, 0)) if axis == 0 else ((0, 0), (rad, rad), (0, 0))
    p = np.pad(img, pad, mode="edge")
    h, w = img.shape[:2]
    sl = lambda d: (slice(d, d + h), slice(None)) if axis == 0 else (slice(None), slice(d, d + w))

    if yF is None:
        acc = np.zeros_like(img, dtype=np.float32)
        for k in range(-rad, rad + 1):
            acc += p[sl(k + rad)]
        return acc / float(2 * rad + 1)

    c0 = img.astype(np.float32)
    acc = c0.copy()
    wsum = np.ones(img.shape[:2], np.float32)
    for k in range(-rad, rad + 1):
        if k == 0:
            continue
        ck = p[sl(k + rad)]
        dg = (ck[:, :, 1] - c0[:, :, 1]) * (1.0 / 255.0)   # 0..255 → 0..1
        wgt = 1.0 - np.minimum(dg * dg * yF, 1.0)
        acc += ck * wgt[:, :, None]
        wsum += wgt
    return acc / wsum[:, :, None]


def box_chain(img, rad, npass, yF):
    """B1/B2：首趟 H 读源 → 之后 (H,V) 交替，共 2·npass 趟。"""
    cur = img
    cur = box_axis(cur, rad, axis=1, yF=yF)          # H
    for i in range(npass):
        cur = box_axis(cur, rad, axis=0, yF=yF)      # V
        if i + 1 < npass:
            cur = box_axis(cur, rad, axis=1, yF=yF)  # H
    return cur


def down2(img):
    """PS_Down：2×2 求和 ×0.25 ×255（中间层是 0..255 浮点）。"""
    h, w = img.shape[:2]
    h2, w2 = h // 2, w // 2
    x = img[:h2 * 2, :w2 * 2].astype(np.float32)
    s = (x[0::2, 0::2] + x[0::2, 1::2] + x[1::2, 0::2] + x[1::2, 1::2]) * 0.25
    return s * 255.0


def box_w(ww, frac, minw):
    """对应 smooth_box_w()：按半分辨率宽度取比例、取奇、夹下限。"""
    k = int(frac * ww + 0.5)
    k = max(k, minw)
    if k % 2 == 0:
        k += 1
    return k


# ---------------------------------------------------------------- 整链
def run_chain(rgb, mode, f1=5 / 960, f2=9 / 960, fa=5 / 960, passes=3,
              yF1=40.0, yF2=20.0, T=6.5, gwp=2.0, I=0.6, e=0.5):
    """mode: 'box' | 'bilat' | 'bilat_shared'（旧实现：两趟都 40）。"""
    h, w = rgb.shape[:2]
    d = down2(rgb)                       # 0..255
    hh, ww = d.shape[:2]

    w1 = box_w(ww, f1, 3)
    w2 = box_w(ww, f2, 5)
    if w2 <= w1 + 1:
        w2 = w1 + 2
    wa = box_w(ww, fa, 3)
    r1, r2, ra = (w1 - 1) // 2, (w2 - 1) // 2, (wa - 1) // 2

    ky1 = yF1 if mode != "box" else None
    ky2 = (yF1 if mode == "bilat_shared" else yF2) if mode != "box" else None

    b1 = box_chain(d, r1, passes, ky1)
    b2 = box_chain(d, r2, passes, ky2)

    # amp = |D.g - B2.g|，再做 ra 盒式（这一趟永远不加值域权重）
    amp = np.abs(d[:, :, 1] - b2[:, :, 1])
    amp = amp[:, :, None].repeat(3, axis=2)
    for i in range(passes):
        amp = box_axis(amp, ra, axis=1)
        amp = box_axis(amp, ra, axis=0)

    # Comp：全分辨率。skin=1、eye=0（只比较核，不掺掩码）
    c = rgb.astype(np.float32) * 255.0
    b1u = upsample(b1, h, w)
    b2u = upsample(b2, h, w)
    au = upsample(amp[:, :, :1], h, w)[:, :, 0]

    skin = 1.0
    se = skin * e
    at = au / T
    ap = at * at if 1.99 < gwp < 2.01 else np.power(np.maximum(at, 0.0), gwp)
    gw = 1.0 / (1.0 + ap)

    even = c - b1u + b2u
    even = c + (even - c) * skin
    even = even + (b2u - even) * se
    even = c + (even - c) * gw[:, :, None]
    res = c + (even - c) * I
    out = np.clip(np.floor(res + 0.5), 0, 255) / 255.0
    return out.astype(np.float32)


def upsample(img, h, w):
    """半分辨率 → 全分辨率。GPU 是双线性；这里用最近邻的两次平均近似，
    对"边界是否被糊"这类统计没有影响（只是不做插值平滑）。"""
    ih, iw = img.shape[:2]
    yi = np.minimum((np.arange(h) * ih) // h, ih - 1)
    xi = np.minimum((np.arange(w) * iw) // w, iw - 1)
    return img[yi][:, xi]


# ---------------------------------------------------------------- 指标
def lap_mag(g):
    """3×3 拉普拉斯的绝对值：高频能量。"""
    p = np.pad(g, 1, mode="edge")
    l = (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:]
         - 4.0 * p[1:-1, 1:-1])
    return np.abs(l)


def metrics(src, out, hf_thr=12.0):
    """把画面分成「边界像素」(原图高频) 与「平坦皮肤」(原图低频) 两组。

    detail_keep = 输出在边界像素上的高频能量 / 原图同处高频能量
                  → 越大 = 发丝、眉梢、鼻翼的锐度保得越多
    flat_smooth = 1 - 输出在平坦区的标准差 / 原图同处标准差
                  → 越大 = 皮肤上的噪点/毛孔糊得越干净
    """
    g_in = src[:, :, 1] * 255.0
    g_out = out[:, :, 1] * 255.0
    li, lo = lap_mag(g_in), lap_mag(g_out)

    hi = li > hf_thr
    lo_mask = li < 2.0
    # 腐蚀一圈，避开高频点到低频区的过渡带
    from numpy.lib.stride_tricks import sliding_window_view
    def erode(m, k=3):
        pad = np.pad(m, k // 2, mode="constant")
        win = sliding_window_view(pad, (k, k))
        return win.all(axis=(2, 3))
    # 边界像素是细线（发丝只有 1~2 px），一腐蚀就没了 —— 不腐蚀；
    # 平坦区反过来必须腐蚀，避开边缘过渡带。
    lo_mask = erode(lo_mask, 9)

    detail_keep = float(lo[hi].mean() / max(li[hi].mean(), 1e-6))
    flat_smooth = float(1.0 - g_out[lo_mask].std() / max(g_in[lo_mask].std(), 1e-6))
    # 兜底口径：整幅平均绝对改动（灰阶）。用来确认"确实磨了"，
    # 免得 flat_smooth 因为平坦区本来就极平滑而恒等于 0。
    mean_abs = float(np.abs(g_out - g_in).mean())
    return detail_keep, flat_smooth, mean_abs, float(hi.mean() * 100), float(lo_mask.mean() * 100)


def halo_metric(src, out, dark=50.0, skin_lo=60.0, skin_hi=210.0):
    """灰边（halo）口径：深色发丝旁边那圈皮肤，被糊暗了多少。

    做法（全部用**原图**分组，不看输出）：
      hair  = G < 50 灰阶（发丝很暗）
      skin  = 60 < G < 210（排除纯白墙面），且不在 hair 里
      近带  = 皮肤里「到发丝距离 1~3 px」的像素
      远带  = 皮肤里「距离 8~16 px」的像素
      halo  = 远带均值 − 近带均值；近带被深色邻居拖暗时 halo 变大。

    为什么不用拉普拉斯筛皮肤：紧贴发丝的皮肤本身就在发丝边缘上，
    高频判据会把整条近带排除掉（实测只剩 86 px），指标直接退化。
    只看 G 通道，与核用的通道一致。
    """
    import cv2
    g = src[:, :, 1] * 255.0
    hair = (g < dark).astype(np.uint8)
    skin_m = ((g > skin_lo) & (g < skin_hi)).astype(np.uint8)
    if hair.sum() < 5000 or skin_m.sum() < 5000:
        return float("nan"), float("nan")
    dist = cv2.distanceTransform((1 - hair).astype(np.uint8), cv2.DIST_L2, 5)
    near = (skin_m > 0) & (dist >= 1.0) & (dist <= 3.0)
    far = (skin_m > 0) & (dist >= 8.0) & (dist <= 16.0)
    if near.sum() < 200 or far.sum() < 200:
        return float("nan"), float("nan")
    go = out[:, :, 1] * 255.0
    return float(g[far].mean() - g[near].mean()), float(go[far].mean() - go[near].mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", required=True)
    ap.add_argument("--out", default="_p2")
    ap.add_argument("--crop", default="", help="x,y,w,h（可选，按原图坐标）")
    ap.add_argument("--I", type=float, default=0.6, help="磨皮强度 0..1")
    ap.add_argument("--e", type=float, default=0.5, help="evenfactor 0..1")
    ap.add_argument("--sweep", default="", help="逗号分隔的 gT 列表：扫「等灰边」曲线")
    ap.add_argument("--scale", type=float, default=1.0, help="先缩图再算（0..1），提速用")
    a = ap.parse_args()

    import cv2
    raw = np.fromfile(a.img, np.uint8)          # 中文路径安全
    bgr = cv2.imdecode(raw, cv2.IMREAD_COLOR)
    if bgr is None:
        raise SystemExit("读不到图：" + a.img)
    if a.crop:
        x, y, w, h = [int(v) for v in a.crop.split(",")]
        bgr = bgr[y:y + h, x:x + w]
    if a.scale != 1.0:
        bgr = cv2.resize(bgr, (int(bgr.shape[1] * a.scale), int(bgr.shape[0] * a.scale)),
                         interpolation=cv2.INTER_AREA)
    rgb = (bgr[:, :, ::-1].astype(np.float32) / 255.0)

    if a.sweep:
        Ts = [float(v) for v in a.sweep.split(",")]
        print("gT 扫描（同一张图、I=%.2f e=%.2f）：灰边列越小越好，mean|d| 列越大越接近\"磨过\""
              % (a.I, a.e))
        print("%-6s %-8s %-9s %-9s" % ("gT", "mode", "灰边(增加)", "mean|d|"))
        for T in Ts:
            for mode in ("box", "bilat"):
                o = run_chain(rgb, mode, T=T, I=a.I, e=a.e)
                h_in, h_out = halo_metric(rgb, o)
                ma = float(np.abs(o[:, :, 1] - rgb[:, :, 1]).mean() * 255.0)
                print("%-6.1f %-8s %+9.2f %9.2f"
                      % (T, mode, h_out - h_in, ma))
        return

    t0 = time.perf_counter()
    box = run_chain(rgb, "box", I=a.I, e=a.e)
    t1 = time.perf_counter()
    bil = run_chain(rgb, "bilat", I=a.I, e=a.e)
    t2 = time.perf_counter()
    old = run_chain(rgb, "bilat_shared", I=a.I, e=a.e)
    t3 = time.perf_counter()

    rows = []
    for name, o in (("box", box), ("bilat 40/20", bil), ("bilat 40/40", old)):
        dk, fs, ma, hp, lp = metrics(rgb, o)
        h_in, h_out = halo_metric(rgb, o)
        rows.append((name, dk, fs, ma, hp, lp, h_out))
        print("%-14s detail_keep=%.3f  灰边=%.2f 灰阶(输入 %.2f, 增加 %+.2f)  "
              "mean|d|=%.2f  (边界 %.1f%% / 平坦 %.1f%%)"
              % (name, dk, h_out, h_in, h_out - h_in, ma, hp, lp))
    print("CPU 重放耗时：box %.0f ms / bilat %.0f ms / 旧版 %.0f ms（不作为 GPU 耗时）"
          % ((t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3))

    # 三联 + 差值
    def to8(x):
        return np.clip(x * 255.0 + 0.5, 0, 255).astype(np.uint8)[:, :, ::-1]
    panels = [to8(rgb), to8(box), to8(bil), to8(old)]
    diff = np.clip(np.abs(bil - box)[:, :, ::-1].max(axis=2) * 8, 0, 255).astype(np.uint8)
    diff = cv2.applyColorMap(diff, cv2.COLORMAP_JET)
    sheet = np.vstack([np.hstack(panels[0:2]), np.hstack(panels[2:4])])
    # 缩到 1600 宽以内便于看
    if sheet.shape[1] > 1600:
        s = 1600.0 / sheet.shape[1]
        sheet = cv2.resize(sheet, (1600, int(sheet.shape[0] * s)))
    ok, buf = cv2.imencode(".png", sheet)
    open(a.out + ".png", "wb").write(buf.tobytes())

    with open(a.out + ".txt", "w", encoding="utf-8") as f:
        f.write("img=%s crop=%s I=%.2f e=%.2f\n" % (a.img, a.crop or "-", a.I, a.e))
        for name, dk, fs, ma, hp, lp, hout in rows:
            f.write("%-14s detail_keep=%.4f flat_smooth=%.4f halo=%.3f mean_abs=%.3f "
                    "hi=%.2f%% flat=%.2f%%\n"
                    % (name, dk, fs, hout, ma, hp, lp))
    print("写出：%s.png / %s.txt" % (a.out, a.out))


if __name__ == "__main__":
    main()
