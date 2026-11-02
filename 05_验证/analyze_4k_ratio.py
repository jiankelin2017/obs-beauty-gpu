# -*- coding: utf-8 -*-
"""对比 720p 与 4K 两趟的段耗时，算出各项随像素的缩放比，并推算 5b 的收益。

用法：
    python analyze_4k_ratio.py <720p日志> <4K日志>

目的只有一个：判断「数据搬运」是否随像素线性。若是，5b（消灭整帧往返）
的收益会随分辨率放大；若否则收益封顶。

判读要点：
    draw 不随像素增长 —— 它量的是命令提交时间（D3D11 延迟执行），合并 pass 无收益。
    map / copy / prep / 掩码 随像素线性 —— 它们才是真正的成本，5b 打的就是这些。
"""
import io
import re
import sys
import statistics

SEG_RE = re.compile(r'([^ ]+) ([\d.]+) [+=]')
TOT_RE = re.compile(r'= ([\d.]+) ms')
# 性能行：下载 X + 引擎 Y + 上传 Z = W ms/帧
PERF_RE = re.compile(r'([\d.]+) \+ [^ ]+ ([\d.]+) \+ [^ ]+ ([\d.]+) = ([\d.]+) ms/')
CHAIN_RE = re.compile(
    r'chain=(\d+)/(\d+) S/T/R/D/W=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)')
SM_RE = re.compile(
    r'sm=([\d.]+)/([\d.]+)/([\d.]+)\(n=(\d+)\) prep=([\d.]+)/([\d.]+)')

PIXELS = {'720p': 1280 * 720, '4K': 3840 * 2160}


def grab(path):
    seg_rows, perf_rows = [], []
    with io.open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if ' seg ' in line:
                segs = SEG_RE.findall(line)
                tot = TOT_RE.search(line)
                g = CHAIN_RE.search(line)
                s = SM_RE.search(line)
                if len(segs) == 6 and tot and g and s:
                    v = [float(x[1]) for x in segs]
                    seg_rows.append(dict(
                        mask=v[0], smooth=v[1], tone=v[2], reg=v[3],
                        det=v[4], warp=v[5], eng=float(tot.group(1)),
                        chain=int(g.group(1)),
                        S=int(g.group(3)), T=int(g.group(4)),
                        R=int(g.group(5)), D=int(g.group(6)), W=int(g.group(7)),
                        draw=float(s.group(1)), map=float(s.group(2)),
                        copy=float(s.group(3)), n=int(s.group(4)),
                        prep=float(s.group(5)), prepmask=float(s.group(6))))
            elif ' ms/' in line:
                p = PERF_RE.search(line)
                if p:
                    perf_rows.append(dict(dl=float(p.group(1)),
                                          eng=float(p.group(2)),
                                          ul=float(p.group(3)),
                                          total=float(p.group(4))))
    return seg_rows, perf_rows


def med(rows, k):
    return statistics.median([r[k] for r in rows])


def main():
    pa, pb = sys.argv[1], sys.argv[2]
    table = {}
    for name, path in (('720p', pa), ('4K', pb)):
        seg, perf = grab(path)
        table[name] = (seg, perf)
        print('%s  seg窗口=%d  性能窗口=%d  (%s)'
              % (name, len(seg), len(perf), path.split('/')[-1].split('\\')[-1]))
        if not seg:
            continue
        for k in ('mask', 'smooth', 'det', 'eng', 'draw', 'map', 'copy',
                  'prep', 'prepmask'):
            print('   %-9s median=%7.2f  min=%6.2f  max=%6.2f'
                  % (k, med(seg, k), min(r[k] for r in seg),
                     max(r[k] for r in seg)))
        if perf:
            for k in ('dl', 'ul', 'total'):
                print('   %-9s median=%7.2f  min=%6.2f  max=%6.2f'
                      % (k, med(perf, k), min(r[k] for r in perf),
                         max(r[k] for r in perf)))
        print('   D==chain ? %s   chain=%s  D=%s'
              % (all(r['D'] == r['chain'] for r in seg),
                 sorted({r['chain'] for r in seg}),
                 sorted({r['D'] for r in seg})))

    seg_a, perf_a = table['720p']
    seg_b, perf_b = table['4K']
    if not (seg_a and seg_b and perf_a and perf_b):
        print('\n[!] 有数据缺失，无法算比例')
        return

    print('\n=== 4K / 720p 中位数比（像素比 %.2f）==='
          % (PIXELS['4K'] / PIXELS['720p']))
    keys = ('mask', 'smooth', 'eng', 'draw', 'map', 'copy', 'prep',
            'prepmask')
    for k in keys:
        v1, v2 = med(seg_a, k), med(seg_b, k)
        if v1 > 0.005:
            r = v2 / v1
            # 下界取 6.0：map 实测 6.90×，略低于像素比 9.00 但明确随像素增长，
            # 与「完全不涨」的 draw(1.75×) / 亚线性的下载(4.81×) 要分开。
            tag = '线性' if 6.0 <= r <= 12.0 else '非线性（含亚线性/固定开销）'
            print('   %-9s %5.2fx  %s' % (k, r, tag))
        else:
            print('   %-9s    n/a（720p 该项≈0）' % k)
    for k, label in (('dl', '下载'), ('ul', '上传'), ('total', '整帧')):
        v1, v2 = med(perf_a, k), med(perf_b, k)
        print('   %-9s %5.2fx  %s' % (k, v2 / v1, label))

    print('\n=== 5b 收益推算（用中位数，单位 ms）===')
    for name in ('720p', '4K'):
        seg, perf = table[name]
        t = med(perf, 'total')
        m, cp, ul = med(seg, 'map'), med(seg, 'copy'), med(perf, 'ul')
        dl, mk = med(perf, 'dl'), med(seg, 'mask')
        pk = med(seg, 'prepmask')
        # 5b-1：链尾不回读 —— 结果直接进 OBS 输出纹理
        s1 = m + cp + ul
        # 5b-2：掩码上 GPU —— 连下载与掩码上传一起消灭
        s2 = s1 + dl + mk + pk
        print('   %s: 当前 %.2f' % (name, t))
        print('       5b-1 链尾不回读  省 %5.2f → %.2f ms（%+.0f%%）'
              % (s1, t - s1, -100.0 * s1 / t))
        print('       5b-2 掩码上 GPU  再省 %5.2f → %.2f ms（%+.0f%%）'
              % (s2, max(t - s2, 0.0), -100.0 * min(s2, t) / t))


if __name__ == '__main__':
    main()
