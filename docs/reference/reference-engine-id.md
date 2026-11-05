---
doc_type: reference
title: 引擎 core 与 gpu 是什么关系
version: 0.1.1
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: bb4dfba
maintainer: Jian
status: Active
supersedes: 根 GPU_CPU_对比报告.md
---

# 引擎 `core` 与 `gpu` 是什么关系（现状）

> **结论**：`engine=gpu` 只是让宿主挂上 GPU 段链钩子；真正决定快慢的是六个 hlsl 有没有加载成功。
> **性能数字不在这里维护** —— 统一看 [`../README.md`](../README.md) §2，且该处数字已标注
> 「**2.14.x 基线实测，3.0.1 未重测**」，不要当作当前版本实测值。
> 本文只回答一件事：「`engine=core` / `engine=gpu` 两个启动日志值，与『GPU 管线到底有没有跑』是什么关系」。
> 历史版本（v3.1.76 时代）的对比数字与"切勿设 `BEAUTY_GPU_SMOOTH_PROD`"警告**已作废**，见文末。
> §2 的示例日志保留 2.14.1 原貌，仅演示日志格式；**当前版本号以 `../README.md` §1 为准**。

## 1. 结论先说

| 问题 | 答案 |
|---|---|
| `GpuEngine` 是另一套算法吗？ | **不是**。它只是 `CoreEngine` 的薄包装：检测、掩码、参数、历史帧都挂在内部 `CoreEngine` 上，`process()` 直转 `core_->process()` |
| 那 `engine=gpu` 有什么用？ | `supportsGpu()` 返回 **true**，这一位是宿主决定「要不要挂 GPU 段链钩子」的唯一依据 |
| 所以真正决定快慢的是什么？ | **不是 `engine=` 这个值，而是「六个 hlsl 有没有加载成功 + 段区间钩子有没有挂上」** |
| 加载失败会怎样？ | **静默回落 CPU**（画面正确、速度回到 CPU 水平），日志里只少了那几行 |

## 2. 怎么用日志一眼判断（装机后必看）

```
[obs-beauty] init OK (v2.14.1, engine=gpu/2.0.0, region=on, item=0, …)
[obs-beauty] tone.hlsl 已加载（…\data\shaders\tone.hlsl）
[obs-beauty] smooth.hlsl 已加载    [obs-beauty] detail.hlsl 已加载
[obs-beauty] warp.hlsl 已加载      [obs-beauty] mask.hlsl 已加载
[obs-beauty] region.hlsl 已加载
[obs-beauty] 引擎资源已预热 1280x720（开启美颜不再卡首帧）
```

六行 `已加载` + 一行 `已预热` 齐全，才算 GPU 管线真的在跑。少任何一行 → 回落 CPU，
此时 `性能` 行的 `S/T/R/D/W` 会出现 0，且引擎耗时会从 1–3 ms 跳到 10 ms 量级。

> 这类"看着在跑、其实回落"的坑踩过两次：一次是**shaders 从来没装进 `data/` 目录**（2.2.0 修复，
> 部署缺口），一次是**启动器没设引擎**（`6_launch_obs_test.bat` 漏了 `BEAUTY_ENGINE_IMPL=gpu`）。
> 两次都表现为「A/B 两臂测的都是 CPU 路径，数据无效」。

## 3. 已作废的历史说法（不要再引用）

| 旧说法 | 现状 |
|---|---|
| 「引擎 core 与 gpu 差异极小，真正差距是着色器有没有加载」 | **前半句过时**：那时的对比跑在 CPU 路径上。现在 GPU 管线默认生效，生产往返 0.8–2.8 ms（README §2.1） |
| 「测试对象 v3.1.76，引擎 4.6–5.7 ms」 | 版本早已重置为 2.x；那些数字是**未装 shaders / 未挂钩子**时代的快照 |
| 「**切勿设 `BEAUTY_GPU_SMOOTH_PROD=1`，会让 RTX 5060 硬崩**」 | **已作废**。硬崩的真因是 4 张中间纹理用 `GS_RGBA32F` 做线性采样（D3D11 不支持）→ 改 `RGBA16F` 后修复；该 23-draw 路径已在 3.1.76-p2 退役，其能力由**段区间钩子**接管（现在默认就走段链，不需要这个开关） |
| 「`core=0.0ms` 说明 core 更省 CPU」 | 那是 self-check 前 3 帧 `faces=0` 的读数，不能代表全时段 |

## 4. 想复现"引擎到底跑没跑"

```bat
:: 只看 GPU 管线是否生效：抓这 7 行
findstr /C:"hlsl 已加载" /C:"引擎资源已预热" /C:"段区间钩子已启用" ^
        "%APPDATA%\obs-studio\logs\*.txt"
```

判定标准、门禁口径与「哪些数字能横比」见
[`../guides/guide-verification.md`](../guides/guide-verification.md)
与 [`../README.md`](../README.md) §5；全库总表见 [`../INDEX.md`](../INDEX.md)。
