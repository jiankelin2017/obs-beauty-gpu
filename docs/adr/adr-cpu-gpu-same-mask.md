---
doc_type: adr
title: ADR · CPU 与 GPU 掩码共用同一份 faceGeom
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: bb4dfba
maintainer: Jian
status: Active
supersedes: 无（归纳性决策记录）
---

# ADR · CPU 与 GPU 掩码共用同一份 faceGeom

> **结论**：CPU `buildMaskInto` 与 GPU `mask.hlsl` 消费同一份 `faceGeom`；
> 任何几何改动必须两边同步，并对拍、重采 4 条 golden，否则 CPU 离线台与 GPU 实机必然分叉。
> **决策状态**：**归纳、待人工确认** —— 本篇是从 2026-11-05 多篇修复记录中归纳出的工程约束，
> 不是某一次原始决策记录；请维护人复核后转为「已接受」。
> **适用条件**：3.0.1 的几何 / 椭圆表 / 位移场改动。
> **失效说明**：若未来把 CPU 与 GPU 几何拆成两套独立实现，本约束作废，需另立 ADR。

## 上下文

同一份 `faceGeom`（脸框、滚转角、半轴）同时喂给 CPU 掩码路径与 GPU mask shader。
历史上多次出现「只改一边」导致的分叉：歪头时美白范围变大、五官立体出现四个白圈、
祛斑与下巴位移场对不上。这些修复记录已并入 [`../CHANGELOG.md`](../CHANGELOG.md)
「2026-11-05 修复批次」，原文在 `../archive/changelog-src-*.md`。

## 决策（归纳）

1. 几何常量、椭圆表、位移场只在 `faceGeom` 一处定义；CPU 与 GPU 各自消费，不各写一份。
2. 改动顺序固定：两边同改 → `BEAUTY_REGION_CMP=1` GPU/CPU 对拍 →
   `py -3.12 offline_regress.py check`；若 golden 变化被确认为**有意**，才 `check --update` 重采并更新 MD5。
3. 只改一边视为 BUG，不得靠「GPU 上看着对」结案。

## 后果 / 证据

- `修复_掩码椭圆双重旋转`：歪头时半轴与 AABB 被旋转两次；改法把三组量分开 ——
  `lw/lh`（脸自身轴宽高）、`hw/hh`（椭圆半轴）、`hx/hy`（**只用于像素迭代**的 AABB）。
  15° 倾斜帧掩码覆盖率 7.82% → 7.47%；确认是有意变化后重采基准，4/4 绿。
- `修复_五官立体白圈`：`BEAUTY_REGION_CMP=1` 在 1920×1080 `blur=30 + threed=100` 下
  `maxdiff=1、差异像素 1 个`；新增的 `dome` 形状开关同时要同步参数纹理 `C[1] = e.dome`。
- `修复_祛斑与下巴`：位移场改动有**四处同步**，原文明确写「缺一处 GPU/CPU 就分叉」。
- 基准清单：`06_离线验证/manifest.json` 4 条 golden（`blur100 / mixed / sharp / region`）。
- 对拍踩坑：用「静态图源 + obs-websocket 截图」时，源必须是当前节目场景里启用的场景项，
  否则 `GetSourceScreenshot` 返回原图；走 `geomOnly` GPU 掩码链时 mask 纹理为空会让效果整个消失，
  同一次运行里加 `BEAUTY_GPU_MASK=0` 才对拍通过。详见
  [`../guides/guide-verification.md`](../guides/guide-verification.md) 与
  [`../guides/guide-offline-regression.md`](../guides/guide-offline-regression.md)。

## 待人工确认

- 本约束是否覆盖**全部**几何路径（例如 `BEAUTY_GPU_MASK=0` 回退路径、未来新增 shader 分支）。
- 归纳性结论需维护人复核；复核通过后把「决策状态」改为「已接受」并同步
  [`../INDEX.md`](../INDEX.md)。
