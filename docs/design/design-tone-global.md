---
doc_type: design
title: 复刻 SDK：美白（色彩三段）改成整帧色彩变换，不再乘皮肤掩码
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 04_文档/复刻SDK_美白整帧色彩变换_20261105.md
---

# 复刻 SDK：美白（色彩三段）改成整帧色彩变换，不再乘皮肤掩码

> **结论**：美白（色彩三段）改为**整帧色彩变换、不再乘皮肤掩码**；
> 默认 `BEAUTY_TONE_GLOBAL=1`（= SDK 行为），`=0` 回退旧的「掩码内施加」。
> 离线回归 4 条 golden 重采基准后全绿；GPU / CPU 等价 444,528 采样 0 偏差。
> **适用条件**：仅 GPU 版；当前插件版本 3.0.1。
> **失效说明**：§四 / §五 的性能数字与文末装机 dll md5 是 **2026-11-05 当时实测 / 当时装机记录**，
> **3.0.1 未重测**；当前版本与 md5 一律以 [`../README.md`](../README.md) 与发布闸门为准。
> 正文引用的 SDK 逆向原文路径按 [`../INDEX.md`](../INDEX.md) §3 / §4 映射检索。

> 日期：2026-11-05
> 起因：用户指出「歪头时变的是**美白**边界，不是磨皮边界」，要求直接复刻 SDK 的逻辑
> 改动：`beauty_freq.cpp`（CPU `toneColor`）、`shaders/tone.hlsl`（GPU）、`obs-beauty.cpp`（绑定开关）
> 默认：`BEAUTY_TONE_GLOBAL=1`（= SDK 行为）。`=0` 可退回旧的「掩码内施加」
> 验证：离线回归 4 条重采基准后全绿；GPU/CPU 等价 444,528 采样 0 偏差；4K 8 线程 +6.7 ms

## 一、SDK 到底怎么做的（两条独立证据）

### 1. 着色器原文：美白不读掩码

`参考文件/01_逆向分析_第三方SDK/.../shaders_merged/` 里是从 GL 运行时抓下来的原文：

| 函数 | 是否读 `tex_mask` | 作用范围 |
|---|---|---|
| `shader_main_s_surfaceblur_mix`（磨皮） | **读**：`mask.a` 当皮肤软掩码、`mask.g` 当眼部掩码 | 按皮肤掩码 |
| `shader_main_s_color_custom_shader_whitten`（美白） | **不读** | 逐像素全图 |
| `shader_main_s_color_custom_shader_clarity`（清晰） | **不读** | 逐像素全图 |
| `shader_main_s_color_shader_red`（红润） | **不读** | 逐像素全图 |

`p58/p139` 的 `real_main` 顺序是：`surfaceblur_mix → tooth_whiten → red → whitten →
clarity`，这四步都作用在同一个整帧 `C` 上。美白是标准 3D-LUT（`tex_combine` 的
1600 高四波段打包，`dim/raw/col` 决定网格），**只按颜色查表，与位置无关**。

### 2. 真值帧：美白开关的差值铺满整条画面

`02_原始帧数据/out_white_off.raw` 与 `out_white_on.raw`（640×394，同场景开关切换）：

- 差值区域 `x172–466 / y0–393` —— 正好是那条 9:16 视频带，**头发、衣服、背景全在里面**；
- 29% 像素差值 >1，P95 只有 0 附近（不是少数边缘像素）；
- 按亮度分桶：暗部与高光≈0，中调最大（0.4–0.6 区间达 −0.033，约 8 灰阶），
  是典型的中调压缩型 LUT，不是"某个区域被点亮"。

两条证据方向一致：**SDK 的美白没有空间边界**，边界感来自颜色本身。

## 二、我们的边界是怎么来的

`toneColor()` 原来的写法是「LUT 结果 × 掩码」：

```cpp
const unsigned char mv = mask[rowOff + x];
if (mv == 0) continue;                 // 非皮肤：一个字节都不动
const float sk = mv * (1.0f / 255.0f);
r += (lut(r) - r) * sk;
```

再叠加 `roi` 是**掩码包围盒**，于是有两条硬边：掩码边缘（软，几十像素羽化）和
包围盒边缘（硬）。歪头时椭圆半轴变大（上一轮修的就是那个 bug），这两条边跟着外扩
—— 用户看到的正是「美白范围」在动，而不是磨皮（磨皮本身是渐变，边界被盖住了）。

## 三、改法

### CPU（`beauty_freq.cpp`）

```cpp
const bool global = toneGlobal();      // BEAUTY_TONE_GLOBAL，默认 1
const int ry0 = global ? 0 : roi.y0;   // 整帧 or 掩码包围盒
...
float sk;
if (global) sk = 1.0f;                 // 不看掩码
else { mv = mask[...]; if (!mv) continue; sk = mv/255.f; }
```

### GPU（`shaders/tone.hlsl`）

```hlsl
uniform float toneGlobal;
...
float sk = (toneGlobal > 0.5f) ? 1.0f : m * (1.0f / 255.0f);
```

宿主三处绑同一个开关（`g_toneGlobal`）：诊断 pass `beauty_gpu_tone`、GPU 链
`beauty_chain_tone`、CPU 侧读同一个环境变量 —— 否则 GPU/CPU 对拍会两边判据不同。

### 顺带：把 17 锚点表预展开成 256 项（这一步是必须的）

直接整帧跑原式，4K 单线程 **29.3 → 92.0 ms（+63 ms）**，付不起。原因是每像素要跑
2 次 `lut17` × 3 通道。输入本来就是 0..255 整数，于是每次调用先展开成
`float[3][256]` 三张表（768 次求值），循环里用「整数项 + 小数线性插值」。

**这不是近似**：17 锚点表在每 16 级内本来就线性，插值结果与 `lut17(v)` 逐位相同；
`check` 在 `BEAUTY_TONE_GLOBAL=0` 下 4 条基准 **maxdiff=0** 就是证明。

## 四、验证

| 项 | 结果 |
|---|---|
| 改动范围（720p 基准帧，美白 100） | 掩码模式改 7.0% 像素（bbox x541–758 / y131–604）；整帧模式 43.0%，最大 18 |
| 掩码模式逐字节回归 | 4 条基准 `maxdiff=0`（查表改写未动数值） |
| 整帧模式重采基准 | 只有 `mixed` 变（美白 40+红润 20+清晰 30）：35.9% 像素、maxdiff 10、均值 1.6；其余 3 条 `maxdiff=0`（不含色彩三段）→ 已 `--update` 重采，旧档 `goldens/g_mixed.raw.toneglobal_bak` |
| GPU/CPU 等价 | `verify_tone_alignment.py` 扩到两种模式：444,528 采样、偏差 0 |
| shader 编译/解析 | `verify_tone_compiles.py` 全 PASS（ps_4_0 175 条指令） |

### 性能（美白 100，含同步检测，只能同条件相减）

| 分辨率 | 掩码模式 | 整帧模式 | 差 |
|---|---|---|---|
| 720p / 1 线程 | 10.55 ms | 13.90 ms | +3.35 |
| 720p / 8 线程 | 9.56 ms | 10.39 ms | +0.83 |
| 4K / 1 线程 | 26.96 ms | 56.76 ms | +29.8 |
| 4K / 8 线程 | 23.31 ms | 30.03 ms | +6.7 |

4K 8 线程（生产默认 `g_cfg.threads = min(8, 核数-1)`）多 6.7 ms。这是复刻 SDK 的
代价：SDK 在 GPU 上做同一件事几乎免费，我们在 CPU 上做。

## 五、与 SDK 的差异（必须说清楚）

1. **只复刻了"结构"，没复刻"颜色"**。SDK 的美白是一张 3D-LUT 纹理（`tex_combine`
   的 0–512 波段），**没有 dump 到**（`texdump证据/` 里只有 mask/surfaceblur 四张）。
   我们用的是自研的 17 锚点逐通道表 —— 同样"不乘掩码、与位置无关"，但曲线形状与
   强度不可能与 SDK 逐像素相同。
2. **磨皮仍按掩码走**，这是 SDK 的行为（`surfaceblur_mix` 读 `mask.a`），没动。
3. **整帧美白会把背景一起提亮**（基准帧均值 +1.6 灰阶）。SDK 亦然，这是"没有边界"
   的必然代价。若观感不接受，`BEAUTY_TONE_GLOBAL=0` 一行回退。

## 六、复算命令

```powershell
cd 06_离线验证
$env:BEAUTY_TONE_GLOBAL="1"; py -3.12 offline_regress.py run --frame goldens/frame_1280x720.raw --w 1280 --h 720 --whiten 100 --threads 8 --save _a.raw
$env:BEAUTY_TONE_GLOBAL="0"; py -3.12 offline_regress.py run --frame goldens/frame_1280x720.raw --w 1280 --h 720 --whiten 100 --threads 8 --save _b.raw
py -3.12 offline_regress.py check            # 默认 = 整帧模式，四条应全绿
```

装机：`obs-beauty.dll` **614,912 B / md5 694043BF61D30DC920A0BF0F7E0511AA**（2026-11-05 09:0x 装机）。
