# -*- coding: utf-8 -*-
"""把「掩码偏了多少像素」换算成「脸上有多大面积差了几个灰阶」（2.13.0 结账）。

两个输入都是实测，没有一个是估的：
    out/maskoff/res_<cfg>.json   `mask_off_probe.py`：偏移 d 像素 → 差多少灰阶/多少面积
    out/masklag_008.json         `mask_lag_sim.py`：逐帧的掩码位置偏差 d（含状态机复现）

做法：把前者当查找表（分段线性插值），对后者逐帧取值再求平均。
于是「align% 68.5%→23.5%」这种频率指标被换成一个画面指标：
**平均每帧，脸上有多大比例的面积，与「掩码没偏」时相差 ≥2 / ≥4 灰阶。**

口径说明（写进报告时必须带上）：
    · 面积分母 = 「磨皮开 vs 磨皮关」实际改变过的像素数（`effect_check.f1`），
      也就是效果真正落下去的地方，不是整幅画面；
    · 偏移取的是二维距离，而查找表只扫了 +x 一个方向 —— 实测 (24,24) 的代价
      对应 +x 的 ~44 px，说明横竖两个分量各自都会吃面积。用二维距离查表
      **系统性偏低**，所以下面的数是下界；
    · 静态单帧、参数固定、无运动模糊：这是「最容易被看见」的一档，
      真实视频里还有运动模糊与时间掩蔽把它压下去。

用法：
    py -3 05_验证/mask_cost.py
    py -3 05_验证/mask_cost.py --cfg combo
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")


def load_curve(cfg):
    p = os.path.join(OUT, "maskoff", "res_%s.json" % cfg)
    with open(p, encoding="utf-8") as fh:
        d = json.load(fh)
    rows = [r for r in d["rows"] if r["tag"] not in ("0b", "0c")]
    d_ = np.array([float(np.hypot(r["dx"], r["dy"])) for r in rows])
    o = np.argsort(d_)
    d_ = d_[o]
    f2 = np.array([rows[i]["f2"] for i in o])
    f4 = np.array([rows[i]["f4"] for i in o])
    mean_chg = np.array([rows[i]["mean_chg"] for i in o])
    return d_, f2, f4, mean_chg, d["effect_check"], d["label"]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cfg", default="blur", choices=("blur", "combo", "strong"))
    ap.add_argument("--lag", default=os.path.join(OUT, "masklag_008.json"))
    a = ap.parse_args(argv)

    d_, f2, f4, mchg, eff, label = load_curve(a.cfg)
    with open(a.lag, encoding="utf-8") as fh:
        lag = json.load(fh)
    aeff = eff["f1"]      # 效果真正落下去的面积（% 画面）

    print("=" * 78)
    print("掩码偏移 → 画面代价   参数：%s" % label)
    print("查找表（%s）：d=%s" % (a.cfg, " ".join("%.0f" % v for v in d_)))
    print("            ≥2 灰阶面积(%%画面) %s" % " ".join("%.3f" % v for v in f2))
    print("            ≥4 灰阶面积(%%画面) %s" % " ".join("%.3f" % v for v in f4))
    print("效果落点面积 %.2f%% 画面（磨皮开 vs 关，≥1 灰阶）" % aeff)
    print("=" * 78)
    print("%-4s %7s %8s %9s | %9s %9s %9s | %9s %9s"
          % ("臂", "判动%", "偏差p50", "超羽化%", "≥2均值", "≥2 p90", "≥2 p99",
             "≥4均值", "≥4 p90"))
    print("                                    （以下全部是「占脸/脖子效果区」的百分比）")
    print("-" * 96)
    for arm in lag["arms"]:
        v = lag["arms"][arm]
        d = np.array(v["dxy"], dtype=np.float64)
        g2 = np.interp(d, d_, f2)
        g4 = np.interp(d, d_, f4)
        mg = np.interp(d, d_, mchg)
        over = 100.0 * float((d > lag["feather"]).mean())
        print("%-8s %7.1f %8.2f %9.1f | %8.2f%% %8.2f%% %8.2f%% | %8.2f%% %8.2f%%"
              % (arm, v.get("hot", float("nan")),
                 float(np.percentile(d, 50)), over,
                 100.0 * g2.mean() / aeff, 100.0 * np.percentile(g2, 90) / aeff,
                 100.0 * np.percentile(g2, 99) / aeff,
                 100.0 * g4.mean() / aeff, 100.0 * np.percentile(g4, 90) / aeff))
        print("        最差帧 %.1f%% / %.1f%%；改动区平均 %.2f 灰阶；"
              "偏移 >16 px 的帧 %.1f%%"
              % (100.0 * g2.max() / aeff, 100.0 * g4.max() / aeff,
                 float(mg.mean()), 100.0 * float((d > 16).mean())))
    print()
    print("注：偏移用二维距离查 +x 的表，横竖分量本可叠加 → 上面的百分比是**下界**。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
