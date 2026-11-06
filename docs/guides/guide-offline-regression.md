---
doc_type: guides
title: 离线 CPU 引擎回归台
version: 0.1.1
last_updated: 2026-11-06
applies_to: 3.2.2
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 06_离线验证/README.md
---

# 06_离线验证 —— 不启动 OBS 的 CPU 引擎回归台

> **结论**：`core_probe.dll` + `offline_regress.py` + **4 条 golden 基准**
> （`blur100 / mixed / sharp / region`），喂一张裸 RGBA8 帧跑真正的 `CoreEngine` 并逐字节对拍。
> 改 CPU 引擎前后必跑 `py -3.12 06_离线验证/offline_regress.py check`；本机 `python` 是
> Microsoft Store 别名，命令一律用 `py -3.12`。

## 这是什么

`core_probe.dll` + `offline_regress.py` + **4 条黄金基准**（`blur100 / mixed / sharp / region`，见 `manifest.json`）。喂一张裸 RGBA8 帧、给一组
面板参数，跑**真正的 CoreEngine**（和 OBS 里跑的是同一份 `beauty_core.cpp`），
把输出写到文件，再逐字节对拍。

## 为什么要有它

在此之前，任何一次验证都是「装机 → 重开 OBS → 跑 15 秒 → 读日志」，一轮二十分钟
起，用户直播期间完全做不了，而且改动「算得对不对」这件事根本没法细看——日志只有
maxdiff 一个数。

CPU 侧引擎（`beauty_core` / `beauty_smooth` / `beauty_warp` / `beauty_region` /
`beauty_freq` / `beauty_guide`）**不依赖 libobs**，本来就能脱离 OBS 跑。把这一半
搬到桌面上，改动可以本地收敛，装机只用来验 GPU 那一段。

## 能验什么 / 不能验什么

| 能 | 不能 |
|---|---|
| CPU 引擎输出的逐字节变化 | GPU 着色器算出来的像素（需 gs_* 设备上下文） |
| 掩码 / 五官 / 美形 / 细节回填的 CPU 分支 | `smooth.hlsl` / `tone.hlsl` 的实际渲染结果 |
| 参数为 0 时是否原样返回 | 真机耗时（下面那个 ms 含同步检测，不是生产耗时） |
| 同参数两次是否完全一致（确定性） | 装机、插件加载、日志埋点 |

## 用法

```bash
# 编译（双击 build_offline.bat 也行）
powershell -ExecutionPolicy Bypass -File build_offline.ps1

# 回归门禁：把所有黄金基准跑一遍
py -3.12 offline_regress.py check

# 自检：确定性 + 零参数直通
py -3.12 offline_regress.py selftest --frame goldens/frame_1280x720.raw --w 1280 --h 720 --blur 100

# 手动跑一组参数并看结果
py -3.12 offline_regress.py run --frame goldens/frame_1280x720.raw --w 1280 --h 720 --blur 100 --save out.raw

# 改了 CPU 代码之后，确认到底动了哪些像素
py -3.12 offline_regress.py run --frame goldens/frame_1280x720.raw --w 1280 --h 720 --blur 100 --check goldens/g_blur100.raw

# PNG 转裸 RGBA8（换素材时用）
py -3.12 offline_regress.py from-png my.png in.raw --w 1280 --h 720

# 3.2.0 自动方向认定三判据（动检测 / 几何 / 输入方向相关代码必跑）
py -3.12 _autorot_probe.py
```

`--update` 会重采基准。**只在确认「CPU 输出变化正是这次想改的」之后才用**，
否则等于把回归门禁关掉。

## 方向认定判据（`_autorot_probe.py`，3.2.0 新增）

这个脚本验的是 3.2.0 的「自动输入方向认定」：源被场景项转过 90°/270° 时，引擎能不能自己认出方向。
它必须在本机的 `core_probe.dll` 上跑（同一份引擎代码），三条用例共用 720×1280 与 1280×720 两张素材：

| 用例 | 素材 | 期望 |
|---|---|---|
| 躺脸 + 方向未声明 | `05_验证/assets/face_front_720p_r90.png` 缩放到 720×1280 | 120 帧内自动命中，之后 `faces=1` |
| 正立脸 | `05_验证/assets/face_front_720p.png` 缩放在 1280×720 | 40 帧内始终 `effRot=0` 且 `faces=1`（不得误判） |
| 纯色空画面 | 程序生成 | 120 帧内始终 `effRot=0`（没人时不得锁方向） |

判据的设计意图：**第一条证明能力存在，第二条与第三条证明它不会乱动**。只测第一条是不够的 ——
一个「永远返回 90°」的实现也能过第一条。

跑之前不要设 `BEAUTY_INPUT_ROT` / `BEAUTY_AUTO_ROT` / `BEAUTY_AUTO_ROT_MISS`，
否则测的是被环境变量改写过的行为。实测数据与真机验收见
[`../design/design-input-rotation.md`](../design/design-input-rotation.md) §10.4。

**已知边界**：只试 90° 和 270°，不试 180°（上下颠倒）。180° 的场合仍须手工设 `BEAUTY_INPUT_ROT=180`。

## 槽位循环判据（`_slot_cycle_probe.py`，3.2.2 新增）

验的是「删掉美颜滤镜再加回来，该槽的异步检测线程能不能重启」。缺陷本体在
`startAsyncIfNeeded()`：`stopAsync()` 只置 `async_.quit = true` 从不复位，而 `releaseSlot()`
刻意保留引擎对象（省模型预热），所以复用同一 `CoreEngine` 时新 worker 第一句就退出。
详见 [`../adr/adr-multi-instance-slots.md`](../adr/adr-multi-instance-slots.md)。

三步，**必须逐步按顺序**，因为判别位在第三步：

| 步骤 | 做什么 | 期望 |
|---|---|---|
| 1 | 有脸素材（1280×720）连喂 60 帧 | `faces=1`（worker 活着、掩码有效） |
| 2 | `BeautyCoreProbeSlotCycle()`：acquire → release → acquire | 返回 `slot=0`（引擎实例被复用，正是缺陷发生条件） |
| 3 | 换**纯色无脸**画面再喂 60 帧 | `faces` 归 0（worker 活着 → 新检测判无脸 → 掩码被 `onDetectFailed` 清掉） |

第三步为什么能当判别位：worker 死了的时候没人跑新检测，`maskFaces_` 会**停在上一轮**，
`faces` 恒为 1 —— 这正是线上「效果冻住、拉参数不响应」的样子。

两个必须知道的细节：

- 脚本会自己把 `BEAUTY_ASYNC_DETECT` 设成 `1`。`BeautyCoreRun` 默认强制同步（否则逐字节对拍
  会飘），而**同步路径根本没有这个线程**，不强制异步就等于没测。
- `BeautyCoreProbeSlotCycle` 是 3.2.2 新加的探针导出。老 dll 没有它，脚本会直接报错退出 ——
  先重跑 `build_offline.ps1`。

**反向对照已实跑**：把 `async_.quit = false;` 那一行撤掉重编，同一脚本第三步 60 帧全为 1
并报 FAIL。所以这个判据不是「跑了就算过」，它确实能抓到该缺陷。

## 素材与基准

> **这两样都不在仓库里（2026-11-06 起）。** `goldens/*.raw` 与输入帧由真人照片派生，
> 帧内是可辨认的人脸，属个人数据，已从 git 索引与历史中移除并写进 `.gitignore`；
> 本机文件仍在原处，回归台照常能跑。**新 clone 的仓库第一次跑会因缺文件失败，
> 这是预期行为**——按本节末尾「重采」操作，用你自己的素材重建。

`goldens/frame_1280x720.raw` 是一帧 1280×720 RGBA，里面有**能检出的人脸**
（实测 `faces=1` / 掩码 7.42% / roi 12.60%），所以掩码分支是被覆盖到的 —— 这一点
很重要：没有脸的话引擎会退化成全帧磨皮，掩码相关的改动就测不出来。

四个基准用例见 `manifest.json`：

| 用例 | 参数 | 覆盖什么 |
|---|---|---|
| `blur100` | 只开磨皮 | 双尺度带通 + 掩码 + 五官保护 |
| `mixed` | 磨皮 + 美白 + 红润 + 清晰 + 美形三项 | 色调 LUT + 美形位移场 |
| `sharp` | 磨皮 50 + 脸内锐化 60 + 全局锐化 30 | **细节回填的四档权重**（fwF/fwG/mwF/mwG） |
| `region` | 磨皮 100 + 五官五项各 30 | **五官三族**（椭圆增益 / 法令纹 / 祛斑） |

> **历史证据（保留原貌，不代表 3.0.1 新增）**：`sharp` 是 3.1.52 才加的。此前两个用例都没开锐化，`detailRefill` 一直没有基准可对 ——
> 3.1.52 要改细节段（GPU 化），先把基准补上。**它的 golden 是 3.1.52 采的**，
> 也就是说它保护的是「3.1.52 之后不再变」，不能反过来证明 3.1.52 没改 CPU 输出；
> 后者由 `blur100` / `mixed` 两条（golden 早于改动、`maxdiff=0`）证明。

### 重采（新 clone 或换素材后）

```bat
REM 1) 准备一帧 1280x720 RGBA 裸数据：PNG 转 raw
REM    转法见本文档前面「PNG 转裸 RGBA8」
REM 2) 放进 goldens/，文件名必须叫 frame_1280x720.raw
REM 3) 确认里面有脸（否则掩码分支测不到）
python 05_验证/count_faces.py 06_离线验证/goldens
REM 4) 重采四条基准，并更新 manifest.json 的 md5
python 06_离线验证/offline_regress.py check --update
```

**素材请用你有权使用的照片**（自己的、或明确授权可再分发的）。任何能检出人脸的帧
都不要 `git add`——`.gitignore` 已经挡住 `goldens/*.raw` 与 `goldens/*.raw.*`，
但换文件名绕过规则时仍会被提交。

## 人脸模型放在哪（踩过一次，写死在这里）

引擎查找 YuNet 的顺序是 `BEAUTY_FACE_MODEL` 环境变量 → **`<dll 所在目录>\models\yunet.onnx`**。
两个要点：

- **放 `<dll 所在目录>\yunet.onnx` 不生效**，必须是 `models\` 子目录。放错位置的表现是
  引擎**不报错**、只回报 `faces=0`，然后退化成全帧磨皮。
- `offline_regress.py` 会自动用 `BEAUTY_FACE_MODEL` 指向兄弟工程的
  `05_验证\models\yunet.onnx`，所以走它永远有脸。**但直接调 DLL 的脚本没有这层保护**
  —— `_bench_warp.py` 就这么踩过：跑出来 `faces=0`，美型段恒为 0.000 ms，看起来像
  「美型几乎不耗时」的结论，实际是根本没执行到。
- 现在 `build_offline.ps1` 会在缺模型时自动往 `models\` 拷一份，台子自包含。

`offline_regress.py` 的 `run` / `check` 在检出 `faces=0` 时会打告警：这类「通过」只覆盖
全帧磨皮，掩码 / 五官 / 美型三个分支一概没跑到。

## 分项耗时测量（`_bench_warp.py`）

```bash
py -3.12 _bench_warp.py --repeat 12
```

用它量单段 CPU 耗时。**不要用「改档位前后总耗时相减」的差值法**：整帧在 8~23 ms 之间
跳，而被测段可能只有 2 ms，差值会落在 -12 ~ +0 之间乱走，中位数完全不可信（实测过一次，
同一批数据算出 -1.04 ms 这种负数结果）。正确做法是读引擎自己的 `markSeg` 边界——为此
`core_probe.cpp` 导出了 `BeautyCoreSeg(double* out6)`，索引含义同 `beautyCoreSegLabel()`。

实测（1280×720，单线程，thin/eye/chin = 10/10/10）：**美型段 2.06 ms**，
同时可读出掩码 11.65 / 磨皮 8.63 / 色调 0.50 / 五官 0 / 细节 0（含同步检测的绝对值不可
当生产耗时，段间比例可参考）。

## 已知限制

1. **ms 不是生产耗时。** `core_probe` 强制 `BEAUTY_ASYNC_DETECT=0`，每帧同步跑
   YuNet（约 15 ms），所以报出来的毫秒数里含检测，只能同条件相减比较。
2. **默认单线程**（`--threads 1`）以保证逐字节可复现。多线程版本在浮点求和顺序上
   可能飘，不适合当基准。
3. **`onnxruntime.dll` 是拷进来的**（约 11 MB，来自兄弟工程的 `05_验证/deps/runtime`）。
   缺了它人脸检测会加载失败，引擎降级成全帧磨皮 —— 台子照样跑，但掩码分支不再被覆盖。

> 注：曾经还有一个 `beauty_core_abtest.exe`（与 `LegacyEngine` 对比的引擎切换测试）。
> 本分支与上游都已没有 `legacy_engine.*`，该测试无法链接，源码与构建步骤已一并移除。
