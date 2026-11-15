"""五官立体：形状实验台（第 2 版 —— 直接写归一化椭圆表）。

为什么要重写几何而不是只换羽化：
  现状 8 个椭圆各自是「小平顶 + 短斜坡」的独立光斑，彼此不重叠，
  所以拉满时肉眼能数出来 4 个亮斑（额头 / 左右颧 / 下巴）。
  只换羽化只会把「白圈」变成「小白点」，斑点感还在。

这一版把亮部做成**互相重叠的连续光场**（额带→鼻梁→颧带→下巴连成一片），
把暗部做成宽而软的凹陷（眼窝 / 上唇 / 下颌）。重叠处的平台被邻居填平，
边界自然消失。

指标：
  · 脸内 dY 均值 / p95 / 峰 —— 效果总量
  · max|∇²Δ| —— 「看到圈」的直接来源（平台→斜坡的二阶突变，马赫带）
"""
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim_regions as S  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 1280, 720


def load_face():
    faces = S.parse_dump(os.path.join(HERE, "_els.txt"))
    return faces[0]


def load_src_mask():
    src = np.fromfile(os.path.join(HERE, "goldens/frame_1280x720.raw"),
                      np.uint8).reshape(H, W, 4)[:, :, :3]
    mb = np.fromfile(os.path.join(HERE, "_mask.raw"), np.uint8)
    return src, mb[8:].reshape(H, W)


def geom(face):
    """把关键点还原成归一化坐标（与 beauty_core.cpp 的 midX/midY 同源）。"""
    e0x, e0y = 0.321439, 0.390400
    e1x, e1y = 0.819133, 0.419200
    # 直接用现状表反推，避免再猜一遍关键点顺序
    els = face["els"]
    midX = els[1][0] + 0.030000
    midY = els[1][1] + 0.060000
    mouthMidX = els[6][0] + 0.020000
    mouthMidY = els[6][1] - 0.155000
    e0y = els[3][1] - 0.117500
    e1y = els[4][1] - 0.125000
    e0x = els[3][0] + 0.045000
    e1x = els[4][0] + 0.012500
    return dict(midX=midX, midY=midY, e0x=e0x, e0y=e0y, e1x=e1x, e1y=e1y,
                mouthMidX=mouthMidX, mouthMidY=mouthMidY)


def build(name, g):
    """返回 (els, dome, desc)。els 为 (cx,cy,rx,ry,rot,gain,shade,feather)。"""
    mx, my = g["midX"], g["midY"]
    if name == "A_current":
        return None
    if name == "F_continuous":
        els = [
            # 额头：宽、软，向发际线和眉弓两侧铺开
            (mx - 0.005, my - 0.200, 0.470, 0.200, 0.0, 6.20, 0.15, 0.0),
            # 眼窝 / 眉弓：宽软凹陷（原 -2.15 太弱、边界太硬）
            (mx - 0.020, my - 0.045, 0.340, 0.115, 0.0, -3.10, 0.15, 0.0),
            # 鼻梁 → 鼻头：竖条，穿过额带下沿，把上下连起来
            (mx - 0.005, my + 0.020, 0.090, 0.245, 0.0, 4.60, 0.15, 0.0),
            # 左 / 右颧骨：斜向宽带（沿颧弓外下方向），与鼻梁重叠
            (g["e0x"] - 0.030, g["e0y"] + 0.125, 0.235, 0.150, 0.14, 4.90, 0.15, 0.0),
            (g["e1x"] + 0.005, g["e1y"] + 0.135, 0.235, 0.150, -0.14, 4.90, 0.15, 0.0),
            # 鼻底 / 上唇：横向软阴影
            (mx - 0.005, my + 0.190, 0.265, 0.080, 0.0, -5.20, 0.15, 0.0),
            # 下巴
            (g["mouthMidX"] - 0.010, g["mouthMidY"] + 0.140, 0.160, 0.100, 0.0, 6.40, 0.15, 0.0),
            # 下颌 / 脖子：大面积压暗
            (mx + 0.050, my + 0.500, 0.505, 0.185, 0.0, -4.20, 0.15, 0.0),
        ]
        return els, 2.0
    if name == "G_bigger":
        els = [
            (mx - 0.005, my - 0.205, 0.560, 0.235, 0.0, 4.60, 0.15, 0.0),
            (mx - 0.020, my - 0.040, 0.390, 0.135, 0.0, -2.60, 0.15, 0.0),
            (mx - 0.005, my + 0.030, 0.105, 0.270, 0.0, 3.60, 0.15, 0.0),
            (g["e0x"] - 0.035, g["e0y"] + 0.130, 0.275, 0.175, 0.14, 3.70, 0.15, 0.0),
            (g["e1x"] + 0.010, g["e1y"] + 0.140, 0.275, 0.175, -0.14, 3.70, 0.15, 0.0),
            (mx - 0.005, my + 0.195, 0.290, 0.090, 0.0, -4.40, 0.15, 0.0),
            (g["mouthMidX"] - 0.010, g["mouthMidY"] + 0.140, 0.180, 0.115, 0.0, 5.20, 0.15, 0.0),
            (mx + 0.050, my + 0.500, 0.520, 0.195, 0.0, -3.60, 0.15, 0.0),
        ]
        return els, 2.0
    if name == "H_flat_dome":
        # 保守：几何不动，只把羽化换成钟形 + 亮部略放大补回总量
        els = []
        for i, e in enumerate(load_face()["els"]):
            e = list(e)
            if i in (0, 2, 3, 4, 6):
                e[2] *= 1.30
                e[3] *= 1.30
                e[5] *= 1.10
            els.append(tuple(e))
        return els, 2.0
    raise SystemExit("unknown " + name)


def field(face, src, els, dome):
    acc = np.zeros((H, W), np.float32)
    fx, fy, fw, fh = face["fx"], face["fy"], face["fw"], face["fh"]
    for e in els:
        cx, cy, rx, ry, rot, gain, shade, feather = e
        if gain == 0.0:
            continue
        cx = fx + cx * fw
        cy = fy + cy * fh
        rx *= fw
        ry *= fh
        if rx < 0.5 or ry < 0.5:
            continue
        r = max(rx, ry)
        x0 = max(0, int(np.floor(cx - r)))
        x1 = min(W, int(np.ceil(cx + r)) + 1)
        y0 = max(0, int(np.floor(cy - r)))
        y1 = min(H, int(np.ceil(cy + r)) + 1)
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
        fall = S.falloff(d, feather, dome)
        blk = src[y0:y1, x0:x1].astype(np.float32)
        Y = 0.299 * blk[:, :, 0] + 0.587 * blk[:, :, 1] + 0.114 * blk[:, :, 2]
        gg = gain * fall
        if shade > 0.0:
            gg = gg * (1.0 - shade * (Y / 255.0))
        acc[y0:y1, x0:x1] += np.where(m, gg, 0.0)
    return acc


def stats(acc, mask):
    m = mask > 0
    a = acc[m]
    gx = np.zeros_like(acc)
    gy = np.zeros_like(acc)
    gx[:, 1:-1] = (acc[:, 2:] - acc[:, :-2]) * 0.5
    gy[1:-1, :] = (acc[2:, :] - acc[:-2, :]) * 0.5
    grad = np.hypot(gx, gy)
    lap = np.zeros_like(acc)
    lap[1:-1, 1:-1] = (acc[2:, 1:-1] + acc[:-2, 1:-1] +
                       acc[1:-1, 2:] + acc[1:-1, :-2] - 4 * acc[1:-1, 1:-1])
    return dict(mean=float(a.mean()), p95=float(np.percentile(np.abs(a), 95)),
                mx=float(a.max()), mn=float(a.min()),
                gmax=float(grad[m].max()),
                pmax=float(np.percentile(np.abs(lap[m]), 99.9)))


def compose(src, acc, mask):
    a = acc * (mask.astype(np.float32) / 255.0)
    np.clip(a, -48, 48, out=a)
    out = src.astype(np.float32) + a[:, :, None]
    return np.clip(np.floor(out + 0.5), 0, 255).astype(np.uint8)


def main():
    face = load_face()
    src, mask = load_src_mask()
    g = geom(face)
    print("geom:", {k: round(v, 4) for k, v in g.items()})

    names = ["A_current", "H_flat_dome", "F_continuous", "G_bigger"]
    crop = (580, 150, 780, 420)
    deltas, comps = [], []
    for nm in names:
        if nm == "A_current":
            els, dome = face["els"], 0.0
        else:
            els, dome = build(nm, g)
        acc = field(face, src, els, dome)
        st = stats(acc, mask)
        print("%-14s dome=%.0f  均值 %+.3f p95 %.2f 峰 %+.2f/%+.2f  "
              "max|∇| %.3f  ∇²p99.9 %.3f"
              % (nm, dome, st["mean"], st["p95"], st["mx"], st["mn"],
                 st["gmax"], st["pmax"]))
        vis = np.clip(128 + acc * 8, 0, 255).astype(np.uint8)
        d = Image.fromarray(vis).crop(crop)
        deltas.append((nm, d))
        c = Image.fromarray(compose(src, acc, mask)).crop(crop)
        comps.append((nm, c))

    def sheet(items, path, scale=2):
        w0, h0 = items[0][1].size
        s = Image.new("RGB", (w0 * len(items) + 6 * (len(items) - 1), h0), (255, 255, 255))
        for i, (_, im) in enumerate(items):
            s.paste(im.convert("RGB"), (i * (w0 + 6), 0))
        s = s.resize((s.width * scale, s.height * scale), Image.NEAREST)
        s.save(path)
        print("->", path)

    sheet(deltas, os.path.join(HERE, "_td_delta_sheet.png"))
    sheet(comps, os.path.join(HERE, "_td_comp_sheet.png"), scale=4)


if __name__ == "__main__":
    main()
