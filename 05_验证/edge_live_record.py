#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""阶段 0：真摄像头（采集卡）取证的录制。

为什么要「一次录完三段」而不是分三次
------------------------------------
每次起停 OBS 都要重新初始化采集卡 + 滤镜 + 掩码（约 12 s），而真人的姿势
不可能复现。一次录完，段与段之间只有画面变化，没有设备重连，掩码也是连续演化的
—— 这正是真实使用时的样子。

节奏（脚本用蜂鸣提示，用户看 OBS 窗口的红点与计时即可）
------------------------------------------------------
    00–05 s  准备（两声短响）
    05–25 s  静止：端坐不动，眼神自然        （一声）
    25–30 s  过渡                            （两声）
    30–50 s  慢动：缓慢左右转头 / 轻微前后    （一声）
    50–55 s  过渡                            （两声）
    55–75 s  快动：快速左右摇头 / 大幅前后    （三声）
    75 s     结束（长响）

参数
----
磨皮 60 + 清晰 40 + 美白 40 + 红润 30（`combo`，与 §9.34/§9.42 同一档），
五官与美形全 0 —— 阶段 0 只测「掩码边界带」，不掺五官段。

用法
----
    py -3 05_验证/edge_live_record.py                  # CH1，75 s
    py -3 05_验证/edge_live_record.py --device CH2 --tag ch2
    py -3 05_验证/edge_live_record.py --plan 30        # 每段 30 s（总 105 s）
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import obsctl                       # noqa: E402
import verify_s1_correct as V       # noqa: E402
from mask_off_probe import CFGS     # noqa: E402

SRC = '视频采集设备'
FILT = 'OBS美颜'
COLL = 'beauty_live_edge'
OUT = os.path.join(HERE, 'out', 'live_edge')


def beep(kind):
    """一声/两声/三声/长响 —— 用时长与次数区分，不依赖音高。"""
    try:
        import winsound
        seq = {'one': [(900, 120)], 'two': [(900, 100), (900, 100)],
               'three': [(900, 90), (900, 90), (900, 90)],
               'long': [(700, 600)]}[kind]
        for f, d in seq:
            winsound.Beep(f, d)
    except Exception:
        pass


def newest_log():
    """OBS 运行中就在写日志，所以 preflight 不必等退出就能读。"""
    d = os.path.join(os.environ.get('APPDATA', ''), 'obs-studio', 'logs')
    fs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith('.txt')]
    return max(fs, key=os.path.getmtime) if fs else None


def read_perf(tail=4):
    """读最近几条 perf 行的 (有脸%, 边框%)。

    首轮用了「掩码覆盖率」当就位判据，是个**错判**：默认配置走缩略图检测，
    `gonly` 恒 180/180（源码 `beauty_core.cpp` 的注释写明「实测：默认 spot=0，
    gonly 恒 180/180」），geomOnly 帧一个像素都不落笔 → `掩码 0.00%` 是**正常
    口径**，不是没检测到。真正的脸部几何看行尾 `边框 x.x%`（ROI 包围盒占画面比）。
    """
    p = newest_log()
    if not p or not os.path.isfile(p):
        return []
    txt = open(p, encoding='utf-8', errors='replace').read()
    rows = []
    for ln in txt.splitlines()[-4000:]:
        if '性能 ' not in ln or '边框 ' not in ln:
            continue
        mv = re.search(r'有脸 ([\d.]+)%', ln)
        mvv = re.search(r'边框 ([\d.]+|--)%', ln)
        if mv and mvv and mvv.group(1) != '--':
            rows.append((float(mv.group(1)), float(mvv.group(1))))
    return rows[-tail:]


def pick_device(c, want):
    r = c.req('GetInputPropertiesListPropertyItems',
              {'inputName': SRC, 'propertyName': 'video_device_id'})
    for it in (r.get('propertyItems') or []):
        if it.get('itemName', '').strip() == want:
            return it.get('itemValue')
    raise SystemExit('找不到设备 %r（可选：%s）'
                     % (want, [i.get('itemName') for i in (r.get('propertyItems') or [])]))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--device', default='UGREEN 35867 CH1')
    ap.add_argument('--tag', default='live')
    ap.add_argument('--plan', type=int, default=20, help='每段时长（秒）')
    ap.add_argument('--min-mask', type=float, default=1.5,
                    help='（已弃用，保留兼容）')
    ap.add_argument('--min-box', type=float, default=1.5,
                    help='就位检查的边框（ROI 占画面）下限 %%，低于它不录')
    ap.add_argument('--no-preflight', action='store_true', help='跳过就位检查')
    ap.add_argument('--check-only', action='store_true', help='只做就位检查，不录制')
    ap.add_argument('--still-only', action='store_true',
                    help='只录「静止」一段（判边界带闪烁的极限条件，省时）')
    a = ap.parse_args(argv)

    seg = a.plan
    if a.still_only:
        marks = [(0, 5, '准备', 'two'),
                 (5, 5 + seg, '静止：端坐不动、不要说话', 'one')]
        total = 5 + seg
    else:
        marks = [(0, 5, '准备', 'two'),
                 (5, 5 + seg, '静止：端坐不动，眼神自然', 'one'),
                 (5 + seg, 8 + seg, '过渡', 'two'),
                 (8 + seg, 8 + 2 * seg, '慢动：缓慢左右转头 / 轻微前后', 'one'),
                 (8 + 2 * seg, 11 + 2 * seg, '过渡', 'two'),
                 (11 + 2 * seg, 11 + 3 * seg, '快动：快速左右摇头 / 大幅前后', 'three')]
        total = 11 + 3 * seg

    os.makedirs(OUT, exist_ok=True)
    if V.obs_running():
        raise SystemExit('OBS 已在运行，先关掉')
    V.SRC, V.FILT = SRC, FILT
    # rec_encoder='obs_x264'：NVENC 没有无损模式（`RecQuality=Lossless` 对它是空操作，
    # 实测日志 rate_control: CBR / bitrate 10000），而有损录制两次同配置的差异能到
    # p50 6.6 灰阶 —— 与要测的 6.02 门限同量级，噪声底直接压在判据上。只有 x264
    # 的 Lossless 是真 CRF 0。
    proc, snap = V.start_obs(COLL, extra_env={'BEAUTY_PROFILE': '1', 'BEAUTY_GPU': '1'},
                             rec_encoder='obs_x264')
    log = None
    try:
        c = obsctl.wait_ready(wait=90)
        if c is None:
            raise SystemExit('websocket 没起来')
        time.sleep(4.0)
        dev = pick_device(c, a.device)
        c.req('SetInputSettings', {'inputName': SRC,
                                   'inputSettings': {'video_device_id': dev,
                                                     'audio_device_id': 'disabled',
                                                     'active': True}})
        print('设备：%s' % a.device, flush=True)
        time.sleep(2.0)
        for attempt in range(12):
            try:
                V.set_config(c, CFGS['combo'][1])
                break
            except RuntimeError as e:
                if attempt == 11:
                    raise
                time.sleep(1.0)
        print('参数：combo（blur60 / clarity40 / color40 / red30）', flush=True)
        c.req('SetRecordDirectory', {'recordDirectory': OUT.replace('\\', '/')})
        time.sleep(3.0)

        # ---- 就位检查：不达标就不录，省掉一次 71 s 的废片 ----
        pre = set(os.listdir(OUT))
        if not a.no_preflight:
            print('=== 就位检查（6 s，请保持最终姿势）===', flush=True)
            beep('one')
            c.req('StartRecord')
            time.sleep(6.0)
            c.req('StopRecord')
            time.sleep(2.5)
            cov = read_perf(4)
            for f in [f for f in os.listdir(OUT) if f not in pre]:
                try:
                    os.remove(os.path.join(OUT, f))
                except OSError:
                    pass
            face = max([x[0] for x in cov], default=-1.0)
            box = max([x[1] for x in cov], default=-1.0)
            if cov:
                print('  有脸%% 末 %d 条：%s   边框%%：%s'
                      % (len(cov), ['%.0f' % x[0] for x in cov],
                         ['%.1f' % x[1] for x in cov]), flush=True)
            if face < 80 or box < a.min_box:
                c.shot(SRC, os.path.join(OUT, 'need_adjust.png'))
                beep('three')
                print('!! 有脸 %.0f%% / 边框 %.1f%%，要求 有脸>=80%% 且 边框>=%.1f%% —— 先别录。'
                      % (face, box, a.min_box), flush=True)
                print('!! 当前画面已存 %s' % os.path.join(OUT, 'need_adjust.png'), flush=True)
                print('!! 调整：整张脸（含额头与下巴）进画面中部，'
                      '脸宽占画面宽度 1/4 以上，别贴边、别被裁', flush=True)
                raise SystemExit(2)
            print('  就位检查通过（有脸 %.0f%% / 边框 %.1f%%）' % (face, box), flush=True)
            # 源截图是"原始纹理"方向，画布截图才是录制会看到的样子 ——
            # 两者可能不一致（2026-11-04：场景项残留 rot=90，源正向、画布转歪）。
            # 所以两张都留，别只信源截图。
            c.shot(SRC, os.path.join(OUT, 'ready_src.png'))
            try:
                import json as _j
                coll_p = os.path.join(os.environ['APPDATA'], 'obs-studio', 'basic',
                                      'scenes', COLL + '.json')
                scen = _j.load(open(coll_p, encoding='utf-8-sig')).get('current_scene')
                if scen:
                    c.shot(scen, os.path.join(OUT, 'ready_scene.png'))
                    print('  画布截图 ready_scene.png（这才是录制看到的样子）', flush=True)
            except Exception as e:
                print('  画布截图失败：%s' % e, flush=True)
            print('  源截图 ready_src.png', flush=True)
            if a.check_only:
                print('（--check-only：只检查，不录制）', flush=True)
                return 0
            beep('two')
            time.sleep(2.0)

        print('=== 录制计划 %.0f s ===' % total, flush=True)
        for t0, t1, name, k in marks:
            print('  %5.0f–%-5.0f s  %s' % (t0, t1, name), flush=True)
        print('现在开始，请按蜂鸣提示表演', flush=True)

        c.req('StartRecord')
        t_start = time.time()
        fired = set()
        while True:
            el = time.time() - t_start
            for i, (t0, t1, name, k) in enumerate(marks):
                if el >= t0 and i not in fired:
                    fired.add(i)
                    print('  [%5.1fs] %s' % (el, name), flush=True)
                    beep(k)
            if el >= total:
                break
            time.sleep(0.2)
        c.req('StopRecord')
        time.sleep(2.0)
        print('结束', flush=True)
        beep('long')
        c.close()
    finally:
        log = V.close_obs()

    # 收尾：把最新录制搬成规范名 + 打印有脸率
    files = [os.path.join(OUT, f) for f in os.listdir(OUT)
             if f.lower().endswith(('.mp4', '.mkv'))]
    if files:
        newest = max(files, key=os.path.getmtime)
        print('录制：%s  %.1f MB' % (newest, os.path.getsize(newest) / 1048576), flush=True)
    if log and os.path.isfile(log):
        txt = open(log, encoding='utf-8', errors='replace').read()
        perfs = re.findall(r'掩码\s+([\d.]+)%\s+有脸\s+(\d+)%', txt)
        print('perf 行 %d 条，末五条（掩码%, 有脸%%）：%s' % (len(perfs), perfs[-5:]), flush=True)
        for ln in txt.splitlines():
            low = ln.lower()
            if 'dshow' in low and ('fail' in low or 'error' in low):
                print('!!', ln[:170], flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
