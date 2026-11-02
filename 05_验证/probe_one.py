#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""单配置探针：静态图源 + 磨皮 80，等够一个日志窗口再截图。

为什么要单独写：verify_s1_correct.py capture 每配置只等 4 s（120 帧），
而性能日志窗口是 180 帧 —— 参数生效期间的那一整窗日志永远打不出来，
于是「画面为什么等于原图」无从判断（tsm/tskip/gskip 全看不到）。

用法（第一个参数是等待秒数，默认 14）：
    BEAUTY_PROFILE=1 python -u 05_验证/probe_one.py 14
截图落在 05_验证/out/thumb_probe.png，日志按时间取最新的那个。
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import verify_s1_correct as V
import obsctl

SETTLE = float(sys.argv[1]) if len(sys.argv) > 1 else 14.0


def main():
    if not V.close_obs():
        sys.exit("关不掉正在运行的 OBS")
    V.start_obs(V.COLLECTION, canvas=(1280, 720))
    c = obsctl.wait_ready(wait=90)
    try:
        time.sleep(3.0)
        # 清掉多余滤镜（同 capture 的处理）
        try:
            for f in c.filters(V.SRC):
                if f.get("filterName") != V.FILT:
                    c.req("RemoveFilter", {"sourceName": V.SRC,
                                           "filterName": f.get("filterName")})
        except Exception as e:
            print("清理失败:", e)
        V.set_config(c, {"blur_level": 80.0})
        time.sleep(SETTLE)
        V.prog_shot(c, os.path.join(V.OUT, "thumb_probe.png"), settle=0.5)
        print("截图: %s" % os.path.join(V.OUT, "thumb_probe.png"))
    finally:
        c.close()
        V.close_obs()


if __name__ == "__main__":
    main()
