> **已废弃（2026-11-05）**：本文件为历史归档，内容冻结，**正文一字未改**，不作为当前结论依据。
> 当前入口见 [`docs/README.md`](../README.md)，全库总表见 [`docs/INDEX.md`](../INDEX.md)，
> 当前结论见 [`docs/CHANGELOG.md`](../CHANGELOG.md)「2026-11-05 修复批次」章节。
> **归档原因**：锐化满档无效（细频阈值被无条件覆盖）的修复记录；结论已并入 CHANGELOG。
> **正文中的旧相对路径已随迁移失效**，按 `docs/INDEX.md` §3 / §4 映射检索；按批 1/3 惯例不修归档件正文。
> 正文的版本号与状态是写作当时快照（含「3.1.65 (core)」「2.17.2bak」一类写法），**版本归属待人工确认**；
> 3.0.1 中是否逐条仍按原文生效亦**待人工确认**，当前行为一律以 [`docs/README.md`](../README.md) 与 `01_源码` 为准。
> 正文里的 `python ...` 是当时的历史写法；**本机 `python` 是 Microsoft Store 别名，不可用**，复跑一律用 `py -3.12`（见 [`docs/guides/guide-verification.md`](../guides/guide-verification.md)）。

---

# 修复：锐化（脸内）满档几乎无效果 —— 细频阈值被无条件覆盖

日期：2026-11-05
状态：**已修（CPU 引擎，离线台验证通过）**；GPU 侧不需要改（参数由引擎算，宿主读 job.detail）

## 1. 症状

面板「锐化」拉到 100，画面几乎不变。离线台实测（720p 黄金帧，blur=0 其余全 0）：

| | 变化像素 | 占比 | maxdiff | 平均偏差 |
|---|---|---|---|---|
| 修复前 | 3 724 | 0.40% | 15 | 0.0109 |
| 修复后 | 36 958 | 4.01% | 16 | 0.0638 |

变化像素全在掩码内（100%），bbox 543~771 × 138~605，即脸区 —— 说明路径本身是通的，
只是幅度被削掉了。

## 2. 根因

`beauty_core.cpp` 的 `buildDetailParams` 里，3.1.35 为「磨皮=0 的纯锐化」加了专属参数：

```cpp
if (t <= 0.0f) {
    dp.fine = shFace * g_cfg.faceSharpGain;
    dp.mid = 0.0f;
    dp.thrFine = g_cfg.faceSharpThrFine;   // 默认 0：不削小幅值细频
}
```

但同一个 lambda 的末尾有一行无条件覆盖：

```cpp
dp.thrFine = wantBlurRefill ? g_cfg.blurRefillThr : g_cfg.detailThrFine;  // 3.0
```

于是纯锐化想要的 `thrFine = 0` 被冲回 3.0。`detailRefill` 里
`softThrF(hf, 3.0)` 把 |hf| < 3 的细频全部削掉 —— 而这张 720p 脸区的
|hf| 中位数只有 0.43、P75 0.78、P90 1.56（实测），也就是**九成像素的细频
在阈值以下，加回量为 0**。

验证：不修代码、只把 `BEAUTY_DETAIL_THR=0`（覆盖全局阈值）跑一遍，变化像素
3 724 → 36 958，与修复后完全一致 —— 反证了「就是这一个阈值卡掉的」。

## 3. 修法

三种模式互斥，改成一次判定：

```cpp
const bool pureSharpen = (t <= 0.0f && wantFaceDetail && haveMask);
dp.thrFine = pureSharpen
                 ? g_cfg.faceSharpThrFine
                 : (wantBlurRefill ? g_cfg.blurRefillThr : g_cfg.detailThrFine);
```

- `t <= 0` 只可能走纯锐化；`wantBlurRefill` 本身要求磨皮强度超过阈值，两者不会同时成立。
- 磨皮 > 0 的两条路径（回填 / 基准回填）逐字节不变，所以 4 条黄金基准
  `maxdiff=0`（`offline_regress.py check` 全绿，2026-11-05 13:5x）。

## 4. 修复后仍偏弱的原因（不是 bug）

增益 `faceSharpGain` 默认 1.0，即「输出 = 原图 + 一倍细频」，数学上已是
unsharp mask amount=1.0。但**素材本身糊**：这张 720p 脸区 |hf| 中位 0.43
灰阶，乘 1.0 后大量像素落在 0.5 以下，取整后为 0。maxdiff 16 只出现在
睫毛/眉毛/发际线这类真高频处。

要更「扎眼」只有加增益（`BEAUTY_FACE_SHARP_GAIN`，1.5~2.0），代价是噪点
同时被放大 —— 这台摄像头在皮肤平滑区的噪声量级与 |hf| 同阶，倍数一上去
就是「锐化出颗粒」。

## 5. 复跑

```powershell
cd 06_离线验证
python offline_regress.py run --frame goldens/frame_1280x720.raw --w 1280 --h 720 `
    --sharpen 100 --save _t_sh_fix.raw
python offline_regress.py check      # 4 条基准应全部 maxdiff=0
```
