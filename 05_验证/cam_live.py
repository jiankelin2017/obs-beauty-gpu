#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""摄像头真人采样：一次 OBS 会话内按组切换面板参数，逐组落盘截图 + 量人脸尺度。

为什么要有这一支：
    静态素材（`still` / 4K 录屏）证明不了两件事 ——
    ① 用户面板上开着的参数，在真实摄像头链路上到底有没有生效（历史两次白跑都是
       「参数静默不生效」，见 cam_probe.py 顶部）；
    ② 人脸在画面里多大时，五官/祛斑的像素强度是否还看得见（近景 vs 中景）。
    这两件事只有真人 + 真摄像头能给答案。

和 cam_probe.py 的分工：
    · cam_probe 只读性能行（有脸%/掩码%/dper/dmot），不落图，参数固定基础五项；
    · cam_live  落图 + 量脸宽占比，参数按组切换，用来做「同一场景的 A/B 对照」。

判据：**不判 PASS/FAIL**（真人画面两次不可能相同）。它产出的是可直接看的图 +
可量的人脸尺度，以及每组参数的读回值。

组的写法（`--group 名字:键=值,键=值`）：
    --group raw:_off=1                      关掉滤镜（原始画面参照）
    --group spot40:delspot_level=40         只开祛斑 40
    --group all:blur_level=60,delspot_level=40
    `_off=1` 是特殊键：关滤镜开关，其余值按面板刻度 0..100 下发。

用法：
    py -3 05_验证/cam_live.py --tag near --shots 3 \
        --group raw:_off=1 --group spot40:delspot_level=40
    # geomOnly 是启动时读的环境变量，A/B 必须分两次进程
    BEAUTY_MASK_GEOM_ONLY=0 py -3 05_验证/cam_live.py --tag near_nogeo ...

输出：05_验证/out/cam/<时间戳>_<tag>/<序号>_<组名>.png，以及同名 _meta.json。
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                      # noqa: E402
import verify_s1_correct as V      # noqa: E402

import numpy as np                 # noqa: E402

PANEL_KEYS = [
    "blur_level", "clarity", "color_level", "red_level", "sharpen",
    "global_sharpen", "face_threed", "eye_bright", "remove_pouch_strength",
    "remove_nasolabial_folds_strength", "delspot_level", "thin_face",
    "big_eye", "chin",
]

OUT_ROOT = os.path.join(HERE, "out", "cam")


def load_detector():
    """复用 count_faces.py 的检测口径（640 letterbox / conf 0.6 / NMS 0.3）。"""
    try:
        import count_faces as CF
    except Exception as e:                      # noqa: BLE001
        print("  （人脸尺度不做，count_faces 导入失败：%s）" % e)
        return None
    try:
        return CF.make_detector(CF.find_model())
    except SystemExit as e:
        print("  （人脸尺度不做：%s）" % e)
        return None


def face_box(det, pil_img):
    """返回最大脸的 (x,y,w,h) 与宽占比；detect 失败返回 None。"""
    if det is None:
        return None
    arr = np.asarray(pil_img.convert("RGB"))[:, :, ::-1]     # RGB -> BGR
    h, w = arr.shape[:2]
    side = 640
    s = min(float(side) / w, float(side) / h)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    small = cv2_resize(arr, (nw, nh))
    pad = np.zeros((side, side, 3), dtype=arr.dtype)
    pad[:nh, :nw] = small
    det.setInputSize((side, side))
    _, faces = det.detect(pad)
    if faces is None or len(faces) == 0:
        return None
    best = max(faces, key=lambda r: r[2] * r[3])
    x, y, fw, fh = [float(v) / s for v in best[:4]]
    return {"x": x, "y": y, "w": fw, "h": fh,
            "w_pct": 100.0 * fw / w, "n": int(len(faces))}


def cv2_resize(arr, size):
    import cv2
    return cv2.resize(arr, size, interpolation=cv2.INTER_AREA)


def parse_group(spec):
    name, _, kv = spec.partition(":")
    g = {}
    for part in kv.split(","):
        part = part.strip()
        if not part:
            continue
        k, _, v = part.partition("=")
        g[k.strip()] = float(v)
    if not name:
        raise SystemExit("--group 缺名字：%s" % spec)
    return name, g


def apply_group(c, source, filt, g):
    """返回实际读回的设置（None = 滤镜已关）。"""
    if g.get("_off"):
        # 关滤镜也要重试：OBS 刚起时 websocket 已可用但源还没就绪，回 207。
        last = None
        for _ in range(15):
            try:
                c.set_filter_enabled(source, filt, False)
                return None
            except Exception as e:   # noqa: BLE001
                last = e
                time.sleep(2.0)
        raise RuntimeError("关滤镜失败：%s" % last)
    st = {k: 0.0 for k in PANEL_KEYS}
    st.update({k: v for k, v in g.items() if not k.startswith("_")})
    st["enabled"] = True
    last = None
    for _ in range(15):          # 刚连上常回 207 not ready，cam_probe 同款重试
        try:
            c.set_filter_settings(source, filt, st, overlay=True)
            c.set_filter_enabled(source, filt, True)
            last = None
            break
        except Exception as e:   # noqa: BLE001
            last = e
            time.sleep(2.0)
    if last is not None:
        raise RuntimeError("参数下发失败：%s" % last)
    return c.filter_settings(source, filt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--collection", default="beauty_s1")
    ap.add_argument("--source", default="视频采集设备")
    ap.add_argument("--filter", default="OBS美颜")
    ap.add_argument("--tag", required=True, help="场景名，如 near / mid")
    ap.add_argument("--shots", type=int, default=3, help="每组抓几张")
    ap.add_argument("--interval", type=float, default=0.6, help="组内张间隔（秒）")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="换组后等待秒数（参数生效 + 几何重建 + 曝光收敛）")
    ap.add_argument("--group", action="append", default=[],
                    help="见文件头：名字:键=值,键=值")
    ap.add_argument("--canvas", default=None, help="如 1280x720；不给则不动画布")
    ap.add_argument("--note", default="", help="写进 meta 的一句话")
    args = ap.parse_args()

    if not args.group:
        raise SystemExit("至少要一个 --group")
    groups = [parse_group(s) for s in args.group]

    stamp = time.strftime("%Y%m%d_%H%M%S")
    outdir = os.path.join(OUT_ROOT, "%s_%s" % (stamp, args.tag))
    os.makedirs(outdir, exist_ok=True)
    print("输出目录：%s" % outdir)

    canvas = None
    if args.canvas:
        canvas = tuple(int(x) for x in args.canvas.lower().split("x"))

    det = load_detector()
    env = {"BEAUTY_PROFILE": "1"}          # 没它不打印性能行（cam_probe 同款）
    print("环境补充：%s" % env)
    for k in ("BEAUTY_MASK_GEOM_ONLY", "BEAUTY_GPU_REGION", "BEAUTY_GPU"):
        if os.environ.get(k) is not None:
            print("  继承 %s=%s" % (k, os.environ[k]))

    attached = V.obs_running()
    snap = V.log_dir_snapshot()
    proc = None
    if attached:
        # 人可能正开着 OBS 看画面，重启会打断预览、还要重载 shader。接管即可。
        print("OBS 已在运行，接管当前会话（结束时不会关闭它）")
    else:
        proc, snap = V.start_obs(args.collection, extra_env=env, canvas=canvas)
    meta = {"tag": args.tag, "note": args.note, "stamp": stamp,            "source": args.source, "filter": args.filter, "groups": []}
    try:
        c = obsctl.wait_ready(wait=90)
        print("OBS 已连接")
        for name, g in groups:
            print("\n--- 组 %s：%s" % (name, "关滤镜" if g.get("_off") else g))
            got = apply_group(c, args.source, args.filter, g)
            if got is not None:
                key = next((k for k in PANEL_KEYS
                            if abs(float(got.get(k) or 0) - g.get(k, 0)) > 1e-6
                            and k in g), None)
                print("    读回：%s" % {k: got.get(k) for k in PANEL_KEYS
                                       if g.get(k)})
                if key:
                    print("    !! %s 读回 %s != 下发 %s —— 参数未生效，本组作废"
                          % (key, got.get(key), g[key]))
            time.sleep(args.settle)
            shots = []
            for i in range(args.shots):
                path = os.path.join(outdir, "%02d_%s.png" % (i + 1, name))
                img = c.shot(args.source, path)
                fb = face_box(det, img)
                shots.append({"file": os.path.basename(path),
                              "size": img.size, "face": fb})
                if fb:
                    print("    #%d %dx%d 脸宽 %.1f%%（%.0f px）"
                          % (i + 1, img.size[0], img.size[1], fb["w_pct"],
                             fb["w"]))
                else:
                    print("    #%d %dx%d 未检出人脸" % (i + 1, *img.size))
                time.sleep(args.interval)
            meta["groups"].append({"name": name, "req": g,
                                   "readback": got, "shots": shots})
        c.close()
    finally:
        with open(os.path.join(outdir, "_meta.json"), "w",
                  encoding="utf-8") as fh:
            json.dump(meta, fh, ensure_ascii=False, indent=1)
        if proc is not None:
            V.close_obs()

    log = V.newest_log(snap)
    print("\n日志：%s" % log)
    print("图：%s" % outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
