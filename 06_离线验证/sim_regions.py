"""把 BEAUTY_REGION_DUMP 导出的椭圆表在 CPU 上逐式重放（numpy）。

用途：改「五官立体」的形状 / 羽化 / 增益时，几秒钟一轮看差分场，
不用每次重编 core_probe.dll 再跑整条链。

与引擎的对应关系（beauty_region.cpp applyEllipses L112-169）：
    d   = sqrt(u^2 + t^2)，u/t 是椭圆局部坐标除以 rx/ry
    fall= 1 - smoothStep(feather, 1, d)
    g   = gain * fall * (1 - shade*Y/255)
    acc = Σg * mask/255，clamp ±48，逐通道 lroundf

用法：
    python sim_regions.py _els.txt goldens/frame_1280x720.raw _mask.raw out.raw
    python sim_regions.py _els.txt ... --dome 0.0     # 试「钟形」羽化
"""
import sys

import numpy as np


def smooth_step(a, b, x):
    if b <= a:
        return np.where(x < a, 0.0, 1.0)
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def parse_dump(path):
    faces = []
    with open(path, "r", encoding="utf-8") as fp:
        for ln in fp:
            p = ln.split()
            if not p:
                continue
            if p[0] == "FACE":
                fx, fy, fw, fh = (float(v) for v in p[1:5])
                faces.append({"fx": fx, "fy": fy, "fw": fw, "fh": fh,
                              "els": [], "dome": None})
            elif p[0] == "E":
                v = [float(x) for x in p[1:9]]
                faces[-1]["els"].append(v)
            elif p[0] == "D":          # 可选：每个椭圆的 dome 系数
                faces[-1]["dome"] = [float(x) for x in p[1:]]
    return faces


def falloff(d, feather, dome):
    """dome=0 → 引擎原式（feather 处满强度、d=1 归零的平顶）。
    dome=1 → 从中心一路衰减的钟形（无平台 → 没有可见的圆边），
             用 feather 控制曲线的「饱满度」：fall = smoothstep 的幂。"""
    if dome <= 0.0:
        return 1.0 - smooth_step(feather, 1.0, d)
    # 钟形：t=1 → 0，t=0 → 1；feather 越大，靠近中心越平（越饱满）
    t = np.clip(d, 0.0, 1.0)
    base = 1.0 - smooth_step(0.0, 1.0, t)
    if feather > 0.0:
        base = np.power(base, 1.0 / max(0.15, 1.0 - feather))
    return base


def apply(face, src, mask, dome_override=None):
    h, w = src.shape[:2]
    dst = src.astype(np.int16).copy()
    fx, fy, fw, fh = face["fx"], face["fy"], face["fw"], face["fh"]
    acc = np.zeros((h, w), np.float32)
    idx = 0
    for e in face["els"]:
        cx, cy, rx, ry, rot, gain, shade, feather = e
        if gain == 0.0:
            continue
        cx = fx + cx * fw
        cy = fy + cy * fh
        rx *= fw
        ry *= fh
        if rx < 0.5 or ry < 0.5:
            continue
        dome = 0.0 if dome_override is None else dome_override
        r = max(rx, ry)
        x0 = max(0, int(np.floor(cx - r)))
        x1 = min(w, int(np.ceil(cx + r)) + 1)
        y0 = max(0, int(np.floor(cy - r)))
        y1 = min(h, int(np.ceil(cy + r)) + 1)
        if x1 <= x0 or y1 <= y0:
            continue
        px = np.arange(x0, x1, dtype=np.float32) + 0.5
        py = np.arange(y0, y1, dtype=np.float32) + 0.5
        dx = px[None, :] - cx
        dy = py[:, None] - cy
        c, s = np.cos(rot), np.sin(rot)
        u = (dx * c + dy * s) / rx
        t2 = (-dx * s + dy * c) / ry
        d2 = u * u + t2 * t2
        m = d2 < 1.0
        if not m.any():
            continue
        d = np.sqrt(d2)
        fall = falloff(d, feather, dome)
        blk = src[y0:y1, x0:x1].astype(np.float32)
        Y = 0.299 * blk[:, :, 0] + 0.587 * blk[:, :, 1] + 0.114 * blk[:, :, 2]
        g = gain * fall
        if shade > 0.0:
            g = g * (1.0 - shade * (Y / 255.0))
        acc[y0:y1, x0:x1] += np.where(m, g, 0.0)
        idx += 1
    mw = mask.astype(np.float32) / 255.0
    acc *= mw
    np.clip(acc, -48.0, 48.0, out=acc)
    # lroundf 语义：半数远离零。np.round 是「半数取偶」，但 acc 极少正好 .5
    out = np.where(np.abs(acc) > 0,
                   np.sign(acc) * np.floor(np.abs(acc) + 0.5), 0.0)
    tgt = dst + out.astype(np.int16)[:, :, None]
    return np.clip(tgt, 0, 255).astype(np.uint8)


def main():
    ap = sys.argv
    dump, frame, maskp = ap[1], ap[2], ap[3]
    out = ap[4] if len(ap) > 4 else None
    dome = None
    if "--dome" in ap:
        dome = float(ap[ap.index("--dome") + 1])
    w, h = 1280, 720
    if "--size" in ap:
        i = ap.index("--size")
        w, h = int(ap[i + 1]), int(ap[i + 2])
    faces = parse_dump(dump)
    src = np.fromfile(frame, np.uint8).reshape(h, w, 4)[:, :, :3]
    mb = np.fromfile(maskp, np.uint8)
    mask = mb[8:].reshape(h, w) if mb.size == 8 + w * h else mb.reshape(h, w)
    res = apply(faces[0], src, mask, dome)
    if out:
        rgba = np.dstack([res, np.full((h, w), 255, np.uint8)])
        rgba.tofile(out)
        print("wrote", out)


if __name__ == "__main__":
    main()
