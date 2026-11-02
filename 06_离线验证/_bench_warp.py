# -*- coding: utf-8 -*-
"""量美型（warp）段的 CPU 耗时 —— 直接读引擎的分项计时。

为什么不用差值法：「同帧改档位前后总耗时相减」在美型段只有几 ms、整帧却在
8~20 ms 之间抖时完全不可用（实测差值在 -12 ~ +0 之间乱走）。引擎的 markSeg
边界自己记的时间没有这个噪声。

用法：python _bench_warp.py [--repeat N]
"""
import ctypes
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "core_probe.dll")
FRAME = os.path.join(HERE, "goldens", "frame_1280x720.raw")
W, H = 1280, 720
SEG = ["掩码", "磨皮", "色调", "五官", "细节", "美型"]


def load():
    d = ctypes.WinDLL(DLL)
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
    d.BeautyCoreSeg.argtypes = [ctypes.POINTER(ctypes.c_double)]
    return d


def set_params(d, thin, eye, chin):
    """美型以外的参数固定为「代表负载」，两组完全一致。"""
    d.BeautyCoreSetPerf(0, 1)          # 0 = 内核数，1 = 启用 ROI
    d.BeautyCoreSetTone(40.0, 20.0)
    d.BeautyCoreSetClarityPanel(30.0)
    d.BeautyCoreSetDetail(0.0, 0.0)
    for i, v in enumerate((0.0, 0.0, 0.0, 0.0, 0.0)):
        d.BeautyCoreSetRegion(i, v)
    for i, v in enumerate((thin, eye, chin)):
        d.BeautyCoreSetShape(i, v)


def one(d, out, thin, eye, chin):
    set_params(d, thin, eye, chin)
    msg = ctypes.create_string_buffer(512)
    rc = d.BeautyCoreRun(FRAME.encode("utf-8"), out.encode("utf-8"),
                         W, H, 60.0, 0, 0.0, 0.0, 1, msg, 512)
    txt = msg.value.decode("gbk", "ignore")
    if rc != 0:
        sys.exit("BeautyCoreRun 失败 rc=%d : %s" % (rc, txt))
    total = None
    if "ms=" in txt:
        total = float(txt.split("ms=")[1].split()[0])
    buf = (ctypes.c_double * 6)()
    d.BeautyCoreSeg(buf)
    return total, [buf[i] for i in range(6)]


def bench(d, tag, warp, n):
    out = os.path.join(HERE, "_bench_%s.raw" % tag)
    thin, eye, chin = (10.0, 10.0, 10.0) if warp else (0.0, 0.0, 0.0)
    rows = []
    for i in range(n + 2):             # 前 2 次热身（模型加载 + 缓冲分配）
        total, seg = one(d, out, thin, eye, chin)
        if i >= 2:
            rows.append((total, seg))
    if os.path.exists(out):
        os.remove(out)
    return rows


def main():
    n = 10
    if "--repeat" in sys.argv:
        n = int(sys.argv[sys.argv.index("--repeat") + 1])

    if not os.path.isfile(DLL):
        sys.exit("找不到 core_probe.dll —— 先跑 build_offline.bat")
    if not os.path.isfile(FRAME):
        sys.exit("找不到 goldens/frame_1280x720.raw")

    d = load()
    print("素材 %dx%d  重复 %d 次（丢弃前 2 次热身）\n" % (W, H, n))

    base = bench(d, "nowarp", False, n)
    warped = bench(d, "warp", True, n)

    print("%-6s %14s %14s" % ("段", "美型关(中位)", "美型开(中位)"))
    print("-" * 40)
    for i, name in enumerate(SEG):
        mb = statistics.median([r[1][i] for r in base])
        mw = statistics.median([r[1][i] for r in warped])
        print("%-6s %12.3f ms %12.3f ms" % (name, mb, mw))

    tb = statistics.median([r[0] for r in base if r[0]])
    tw = statistics.median([r[0] for r in warped if r[0]])
    print("-" * 40)
    print("%-6s %12.2f ms %12.2f ms   （含同步检测，非生产耗时）"
          % ("总", tb, tw))
    print()
    print("=> 美型段 CPU 耗时 ≈ %.2f ms @%dx%d（thin/eye/chin = 10/10/10）"
          % (statistics.median([r[1][5] for r in warped]), W, H))
    print("   差值法（总耗时相减）会给出 %.2f ms —— 噪声大于信号，忽略"
          % (tw - tb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
