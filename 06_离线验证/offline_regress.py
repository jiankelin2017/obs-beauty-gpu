#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
离线回归台 —— 不启动 OBS 也能跑 CPU 引擎并逐字节对拍。

配套 `build_offline.ps1` 编出来的 `core_probe.dll`。CPU 侧引擎（beauty_core /
beauty_smooth / beauty_warp / beauty_region ...）**不依赖 libobs**，所以能脱离
OBS 跑；GPU 侧（gs_* / D3D11 effect）不能，那部分仍然必须装机验证。

用法:
    # 1) 把一张 PNG 转成引擎要的裸 RGBA8
    python offline_regress.py from-png face.png in.raw --w 1280 --h 720

    # 2) 跑一遍，存结果
    python offline_regress.py run --frame in.raw --w 1280 --h 720 --blur 100 \\
        --save out.raw

    # 3) 与基准逐字节比（改代码前后各跑一次，第 3 步就是回归门禁）
    python offline_regress.py run --frame in.raw --w 1280 --h 720 --blur 100 \\
        --check golden.raw

    # 4) 自检：同参数两次必须完全一致；全 0 参数必须原样返回
    python offline_regress.py selftest --frame in.raw --w 1280 --h 720

为什么需要它：此前每次验证都要「装机 → 重开 OBS → 跑 15 秒 → 读日志」，一轮
二十分钟起，而且用户直播时完全做不了。CPU 侧改动（美形几何、细节回填、掩码
逻辑）其实可以在桌面上先收干净，装机只用来验 GPU 那一段。

⚠ `core_probe` 强制 BEAUTY_ASYNC_DETECT=0（每帧同步跑 YuNet，约 15 ms），
  所以报出来的 ms 里**含检测**，只能在同条件之间相减比较，不能当生产耗时。
"""
import argparse
import ctypes
import glob
import os
import statistics
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DLL_PATH = os.path.join(HERE, "core_probe.dll")


# ---------------------------------------------------------------------------
# 依赖定位
# ---------------------------------------------------------------------------
def find_model():
    """找 YuNet 模型。引擎默认在 DLL 同目录找 models\\yunet.onnx，离线台没有，
    所以显式用 BEAUTY_FACE_MODEL 指过去（兄弟工程的 05_验证\\models 下）。"""
    env = os.environ.get("BEAUTY_FACE_MODEL")
    if env and os.path.isfile(env):
        return env
    root = os.path.dirname(os.path.dirname(HERE))          # ...\插件
    pats = [
        os.path.join(root, "OBS*", "05_*", "models", "yunet.onnx"),
        os.path.join(root, "OBS*", "07_*", "**", "yunet.onnx"),
        os.path.join(HERE, "models", "yunet.onnx"),
        os.path.join(HERE, "yunet.onnx"),
    ]
    for p in pats:
        hit = glob.glob(p, recursive=True)
        if hit:
            os.environ["BEAUTY_FACE_MODEL"] = hit[0]
            return hit[0]
    return None


def load_dll():
    if not os.path.isfile(DLL_PATH):
        sys.exit("找不到 core_probe.dll —— 先跑 build_offline.bat 编译。")
    # onnxruntime.dll 要在 DLL 同目录：引擎会用 selfDir() 指给 ORT。
    if not os.path.isfile(os.path.join(HERE, "onnxruntime.dll")):
        print("[warn] 本目录没有 onnxruntime.dll，人脸检测会加载失败"
              "（引擎会降级成全帧磨皮，仍是合法的回归目标，但不再覆盖掩码分支）",
              file=sys.stderr)

    d = ctypes.WinDLL(DLL_PATH)
    d.BeautyCoreRun.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_double, ctypes.c_int, ctypes.c_float, ctypes.c_float,
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
    ]
    d.BeautyCoreRun.restype = ctypes.c_int
    d.BeautyCoreSetPerf.argtypes = [ctypes.c_int, ctypes.c_int]
    d.BeautyCoreSetTone.argtypes = [ctypes.c_double, ctypes.c_double]
    d.BeautyCoreSetDetail.argtypes = [ctypes.c_double, ctypes.c_double]
    d.BeautyCoreSetClarityPanel.argtypes = [ctypes.c_double]
    d.BeautyCoreSetRegion.argtypes = [ctypes.c_int, ctypes.c_double]
    d.BeautyCoreSetShape.argtypes = [ctypes.c_int, ctypes.c_double]
    # 2.13.1（P0-11 阶段 2）：输入旋转。老 dll 没有这个导出，允许缺席 ——
    # 缺席时 apply_params 会跳过，其余用例照跑（只影响 --rot 用例）。
    if hasattr(d, "BeautyCoreSetInputRot"):
        d.BeautyCoreSetInputRot.argtypes = [ctypes.c_int]
    return d


# ---------------------------------------------------------------------------
# 参数下发
# ---------------------------------------------------------------------------
def apply_params(d, a):
    d.BeautyCoreSetPerf(int(a.threads), 1 if a.roi else 0)
    d.BeautyCoreSetTone(float(a.whiten), float(a.red))
    d.BeautyCoreSetClarityPanel(float(a.clarity))
    d.BeautyCoreSetDetail(float(a.sharpen), float(a.gsharpen))
    for i, v in enumerate((a.threed, a.eyebright, a.pouch,
                           a.nasolabial, a.delspot)):
        d.BeautyCoreSetRegion(i, float(v))
    for i, v in enumerate((a.thin, a.bigeye, a.chin)):
        d.BeautyCoreSetShape(i, float(v))
    rot = int(getattr(a, "rot", 0) or 0)
    if rot and hasattr(d, "BeautyCoreSetInputRot"):
        d.BeautyCoreSetInputRot(rot)


def run_once(d, fin, fout, w, h, blur, warm):
    """跑 warm+1 次（前 warm 次是热身：首帧要加载模型 + 分配缓冲），返回
    (输出数组, 日志文本, 最后一次的 ms)。"""
    msg = ctypes.create_string_buffer(512)
    ms = 0.0
    txt = ""
    out = None
    for i in range(warm + 1):
        rc = d.BeautyCoreRun(fin.encode("utf-8"), fout.encode("utf-8"),
                             w, h, float(blur), 0, 0.0, 0.0, 1, msg, 512)
        txt = msg.value.decode("gbk", "ignore")
        if rc != 0:
            sys.exit("BeautyCoreRun 失败 rc=%d : %s" % (rc, txt))
        if i == warm:
            out = np.fromfile(fout, np.uint8).reshape(h, w, 4)
        if "ms=" in txt:
            ms = float(txt.split("ms=")[1].split()[0])
    return out, txt, ms


# ---------------------------------------------------------------------------
# 比较
# ---------------------------------------------------------------------------
def compare(a, b, name_a="当前", name_b="基准"):
    """逐字节三通道差。口径与 OBS 里的 GPU/CPU 对拍一致（忽略 alpha）。"""
    if a.shape != b.shape:
        print("  形状不同：%s %s vs %s %s" % (name_a, a.shape, name_b, b.shape))
        return False
    ai = a[:, :, :3].astype(np.int16)
    bi = b[:, :, :3].astype(np.int16)
    diff = np.abs(ai - bi)
    maxd = int(diff.max())
    px = int((diff.max(axis=2) > 0).sum())
    total = a.shape[0] * a.shape[1]
    mean = float(diff.mean())
    print("  maxdiff=%d  差异像素=%d(%.6f%%)  平均偏差=%.6f"
          % (maxd, px, 100.0 * px / total, mean))
    if maxd == 0:
        print("  判定：逐字节相同")
        return True
    print("  判定：有差异（若预期相同，说明这次改动动了 CPU 输出）")
    return False


# ---------------------------------------------------------------------------
# 子命令
# ---------------------------------------------------------------------------
def cmd_from_png(a):
    from PIL import Image
    im = Image.open(a.png).convert("RGB").resize((a.w, a.h), Image.LANCZOS)
    rgba = np.dstack([np.asarray(im, np.uint8),
                      np.full((a.h, a.w, 1), 255, np.uint8)])
    rgba.tofile(a.out)
    print("写出 %s（%dx%d RGBA8，%d 字节）" % (a.out, a.w, a.h, rgba.size))


def face_coverage(txt):
    """从引擎回报里取 faces 数，取不到返回 None。

    为什么值得单独一个函数：缺 yunet.onnx 时引擎**不报错**，只是 faces=0 然后
    退回全帧磨皮。此时任何对拍都能干净地通过，但掩码 / 五官 / 美型三个分支
    根本没被执行到。那一轮采下来的黄金基准看起来完全正常，实际覆盖度是零。
    """
    if "faces=" not in txt:
        return None
    try:
        return int(txt.split("faces=")[1].split()[0])
    except (IndexError, ValueError):
        return None


def warn_if_no_face(txt):
    """faces=0 打印醒目告警；返回 True 表示覆盖度正常。"""
    n = face_coverage(txt)
    if n is None:
        print("  [warn] 引擎回报里没有 faces= 字段，无法确认覆盖度")
        return False
    if n == 0:
        print("  [warn] faces=0 —— 本帧没检出人脸，本次结果**只覆盖全帧磨皮**，")
        print("         掩码 / 五官 / 美型分支都没跑到。常见原因是 yunet.onnx 不在")
        print("         本目录（引擎会静默降级，不报错）。")
        return False
    return True


def cmd_run(a):
    d = load_dll()
    apply_params(d, a)
    out, txt, ms = run_once(d, a.frame, a.save or os.path.join(HERE, "_out.raw"),
                            a.w, a.h, a.blur, a.warm)
    if a.save:
        out.tofile(a.save)
        print("结果已写出: %s" % a.save)
    print("引擎: %s" % txt)
    print("耗时: %.2f ms（含同步人脸检测，只能同条件相减）" % ms)
    warn_if_no_face(txt)

    ok = True
    if a.check:
        ref = np.fromfile(a.check, np.uint8).reshape(a.h, a.w, 4)
        print("\n对拍 %s：" % a.check)
        ok = compare(out, ref)
    if a.zero_check:
        src = np.fromfile(a.frame, np.uint8).reshape(a.h, a.w, 4)
        print("\n零参数原样返回检查：")
        # 只有在所有档位都为 0 时才有意义
        vals = [a.blur, a.whiten, a.red, a.clarity, a.sharpen, a.gsharpen,
                a.threed, a.eyebright, a.pouch, a.nasolabial, a.delspot,
                a.thin, a.bigeye, a.chin]
        if any(v != 0 for v in vals):
            print("  跳过：本次参数非全 0，原样返回本就不成立")
        else:
            ok = compare(out, src, "输出", "输入") and ok
    return 0 if ok else 1


PARAM_KEYS = ("blur", "whiten", "red", "clarity", "sharpen", "gsharpen",
              "threed", "eyebright", "pouch", "nasolabial", "delspot",
              "thin", "bigeye", "chin")


def default_params(**over):
    """构造一份「全 0 参数 + threads=1」的 namespace，供 check 用。
    用 namespace 而不是 dict，是为了复用 apply_params 这一条下发路径 ——
    两条路径迟早会走岔，那时候对拍结果就不再代表生产行为。"""
    ns = argparse.Namespace(threads=1, roi=False)
    for k in PARAM_KEYS:
        setattr(ns, k, 0.0)
    for k, v in over.items():
        setattr(ns, k, v)
    return ns


def cmd_check(a):
    """读 manifest.json，把每个用例跑一遍并与黄金基准逐字节比。"""
    import json
    mpath = a.manifest if os.path.isabs(a.manifest) else os.path.join(HERE, a.manifest)
    if not os.path.isfile(mpath):
        sys.exit("找不到清单 %s" % mpath)
    with open(mpath, "r", encoding="utf-8") as fh:
        man = json.load(fh)

    fr = man["frame"]
    frame = fr["path"] if os.path.isabs(fr["path"]) else os.path.join(HERE, fr["path"])
    if not os.path.isfile(frame):
        sys.exit("找不到素材 %s" % frame)
    print("引擎: %s   素材: %dx%d %s"
          % (man.get("engine", "?"), fr["w"], fr["h"], os.path.basename(frame)))

    d = load_dll()
    tmp = os.path.join(HERE, "_check.raw")
    all_ok = True
    coverage_ok = True
    for c in man["cases"]:
        print("\n[%s] %s" % (c["name"], c.get("desc", "")))
        p = default_params(**c.get("args", {}))
        apply_params(d, p)
        out, txt, ms = run_once(d, frame, tmp, fr["w"], fr["h"],
                                float(p.blur), a.warm)
        if not warn_if_no_face(txt):
            coverage_ok = False
        gold_path = c["golden"] if os.path.isabs(c["golden"]) else os.path.join(HERE, c["golden"])
        if a.update:
            out.tofile(gold_path)
            import hashlib
            c["md5"] = hashlib.md5(out.tobytes()).hexdigest().upper()
            print("  已更新基准 %s  md5=%s" % (os.path.basename(gold_path), c["md5"]))
            continue
        ref = np.fromfile(gold_path, np.uint8).reshape(fr["h"], fr["w"], 4)
        ok = compare(out, ref, "当前", os.path.basename(gold_path))
        all_ok = all_ok and ok
        print("  %.2f ms（含同步检测）" % ms)

    if a.update:
        with open(mpath, "w", encoding="utf-8") as fh:
            json.dump(man, fh, ensure_ascii=False, indent=2)
        print("\n清单已更新: %s" % mpath)
        if not coverage_ok:
            print("[warn] 本次基准是在 faces=0 状态下采的 —— 只覆盖全帧磨皮。")
            print("       补上 yunet.onnx 后必须重跑 --update，否则这份基准")
            print("       会在掩码/美型有改动时表现成「全部通过」。")
        return 0

    print("\n合计：%s" % ("全部通过" if all_ok else "有差异 —— 见上"))
    if not coverage_ok:
        print("[warn] 本轮有 faces=0 的用例：这些用例的「通过」不含掩码/五官/美型覆盖。")
    return 0 if all_ok else 1


def cmd_selftest(a):
    """两件事：确定性（两次同参数必须一致）、零参数直通（输出 == 输入）。"""
    d = load_dll()
    tmp = os.path.join(HERE, "_selftest.raw")
    print("=== 1. 确定性：同参数连跑两次，输出必须逐字节相同 ===")
    apply_params(d, a)
    o1, t1, ms1 = run_once(d, a.frame, tmp, a.w, a.h, a.blur, a.warm)
    o2, t2, ms2 = run_once(d, a.frame, tmp, a.w, a.h, a.blur, a.warm)
    ok1 = compare(o1, o2, "第1次", "第2次")

    print("\n=== 2. 零参数直通：所有档位 0 时输出必须等于输入 ===")
    z = argparse.Namespace(**vars(a))
    for k in ("blur", "whiten", "red", "clarity", "sharpen", "gsharpen",
              "threed", "eyebright", "pouch", "nasolabial", "delspot",
              "thin", "bigeye", "chin"):
        setattr(z, k, 0)
    apply_params(d, z)
    o3, t3, ms3 = run_once(d, a.frame, tmp, a.w, a.h, 0.0, a.warm)
    src = np.fromfile(a.frame, np.uint8).reshape(a.h, a.w, 4)
    ok2 = compare(o3, src, "输出", "输入")

    print("\n=== 3. 计时（含同步检测，仅供同条件相减）===")
    print("  全参数: %.2f ms   零参数: %.2f ms" % (ms1, ms3))
    print("\n合计：%s" % ("通过" if (ok1 and ok2) else "未通过"))
    return 0 if (ok1 and ok2) else 1


def build_parser():
    p = argparse.ArgumentParser(
        description="离线回归台（CPU 引擎，不启动 OBS）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--frame", required=True, help="裸 RGBA8 输入")
        sp.add_argument("--w", type=int, required=True)
        sp.add_argument("--h", type=int, required=True)
        sp.add_argument("--warm", type=int, default=2, help="热身帧数（默认 2）")

    def add_params(sp):
        sp.add_argument("--blur", type=float, default=0.0, help="磨皮 0..100")
        sp.add_argument("--whiten", type=float, default=0.0, help="美白 0..100")
        sp.add_argument("--red", type=float, default=0.0, help="红润 0..100")
        sp.add_argument("--clarity", type=float, default=0.0, help="清晰 0..100")
        sp.add_argument("--sharpen", type=float, default=0.0, help="锐化 0..100")
        sp.add_argument("--gsharpen", type=float, default=0.0, help="全局锐化 0..100")
        sp.add_argument("--threed", type=float, default=0.0, help="五官立体")
        sp.add_argument("--eyebright", type=float, default=0.0, help="亮眼")
        sp.add_argument("--pouch", type=float, default=0.0, help="去黑眼圈")
        sp.add_argument("--nasolabial", type=float, default=0.0, help="去法令纹")
        sp.add_argument("--delspot", type=float, default=0.0, help="祛斑痘")
        sp.add_argument("--thin", type=float, default=0.0, help="瘦脸")
        sp.add_argument("--bigeye", type=float, default=0.0, help="大眼")
        sp.add_argument("--chin", type=float, default=0.0, help="下巴")
        sp.add_argument("--threads", type=int, default=1, help="线程数（默认 1，求可复现）")
        sp.add_argument("--roi", action="store_true", help="开启 ROI 裁剪（默认关）")
        sp.add_argument("--rot", type=int, default=0, choices=[0, 90, 180, 270],
                        help="输入旋转（2.13.1 起支持；离线默认 0，与旧基准一致）")

    sp = sub.add_parser("run", help="跑一遍并可对拍")
    add_common(sp)
    add_params(sp)
    sp.add_argument("--save", help="把结果写到这个 .raw")
    sp.add_argument("--check", help="与该 .raw 基准逐字节对比")
    sp.add_argument("--zero-check", action="store_true",
                    help="参数全 0 时校验输出等于输入")
    sp.set_defaults(func=cmd_run)

    sp = sub.add_parser("selftest", help="确定性 + 零参数直通自检")
    add_common(sp)
    add_params(sp)
    sp.set_defaults(func=cmd_selftest)

    sp = sub.add_parser("check", help="跑 manifest.json 里全部黄金基准（回归门禁）")
    sp.add_argument("--manifest", default="manifest.json")
    sp.add_argument("--update", action="store_true",
                    help="重新采集基准（仅在确认 CPU 输出变化是有意改动时用）")
    sp.add_argument("--warm", type=int, default=2)
    sp.set_defaults(func=cmd_check)

    sp = sub.add_parser("from-png", help="PNG → 裸 RGBA8")
    sp.add_argument("png")
    sp.add_argument("out")
    sp.add_argument("--w", type=int, default=1280)
    sp.add_argument("--h", type=int, default=720)
    sp.set_defaults(func=cmd_from_png)
    return p


def main():
    a = build_parser().parse_args()
    if a.cmd != "from-png":
        m = find_model()
        if m:
            print("模型: %s" % m)
        else:
            print("[warn] 没找到 yunet.onnx，将降级为「无掩码全帧磨皮」"
                  "（掩码相关分支不会被覆盖）", file=sys.stderr)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
