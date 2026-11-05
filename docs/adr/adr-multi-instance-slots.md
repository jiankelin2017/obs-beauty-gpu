---
doc_type: adr
title: ADR · 每滤镜实例独占一个引擎槽
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: bb4dfba
maintainer: Jian
status: Active
supersedes: 无（新增决策记录）
---

# ADR · 每滤镜实例独占一个引擎槽

> **结论**：多个 `obs_beauty_filter` 实例必须各自独占一份引擎上下文，不能共享进程级单例。
> **决策状态**：已接受（3.0.0 实现，3.0.1 加固）。
> **适用条件**：3.0.1、最多 8 个并发滤镜实例、**一源一滤镜**。
> **失效说明**：`BEAUTY_SEG=1` 分割影子层仍是进程级全局，只支持单实例，默认关；该边界不因本决策改变。

## 上下文

3.0.0 之前 `beautyEngine()` 是进程级单例，帧级状态（帧号、掩码轨迹、肤色 EMA、诊断计数）全是文件级全局。
OBS 对每个 `obs_beauty_filter` 实例分别回调 `render()`，多机位时多个实例交替写同一份上下文，
表现为：A 镜头人物不停闪烁、侧机位磨皮 / 美白 / 润色清晰全无效果，而主机位正常。

## 决策

- 按槽隔离：`SlotState` 按槽存帧号 / 掩码轨迹 / 肤色 EMA / hook user / 诊断计数；
  `thread_local int t_slot` 选择当前线程正在处理的槽，`slotIndex()` 做范围 clamp；
  `g_engineAtomic[kMaxEngineSlots]` 是每槽引擎指针的原子快路径；
  `g_engineMutex` 保护惰性建引擎，`g_slotMutex` 保护槽位分配 / 释放 / 清空。
- 对外 API（`01_源码/beauty_core.h`）：`beautyCoreAcquireSlot()` / `beautyCoreBindSlot()` /
  `beautyCoreReleaseSlot()` / `beautyCoreActiveSlots()` / `beautyCoreReleaseAllEngines()`。
- 异步 worker 绑定创建它的槽：`std::thread([this, mySlot] { t_slot = mySlot; asyncWorker(); })`。
- 上限 **8 个实例/进程**，超出时 `beautyCoreAcquireSlot()` 返回失败槽位。
- 3.0.1 加固：槽位获取失败时 `create()` 立即直通，不留悬空 hook user；`render()` 早退判断提到
  `beautyCoreBindSlot()` 之前；`destroy()` 只在有效槽摘钩子；`beautyCoreReleaseSlot()` 在同一把
  `g_slotMutex` 内调用 `stopAsync()` 并清空 `SlotState`，消除 worker 释放竞态。

## 后果 / 边界

- 单实例 `slot=0` 行为与 2.16.1 不变。
- 每多一实例多加载一份 YuNet 模型并多占一份帧环内存。
- **一源一滤镜是唯一受支持用法**；同一源串联多个美颜滤镜不受支持（OBS 滤镜链 + GPU 链尾直绘的固有表现），
  不算本决策要修的串扰。
- 槽可复用：删掉一路后新实例会拿到空出的槽。
- `BEAUTY_SEG=1` 的多实例边界**仍未解决**，默认关闭时不影响。

## 证据

- [`../CHANGELOG.md`](../CHANGELOG.md) 3.0.0 / 3.0.1；[`../README.md`](../README.md) §2。
- 4 机位自动化复验：4 个独立源各挂 1 滤镜 → `slot=0/1/2/3`、活跃实例 `1/2/3/4`；
  删一路后其余正常、槽可复用、无崩溃与黑屏；离线回归 4 条 golden 逐字节相同。
- 代码：`01_源码/beauty_core.cpp`、`01_源码/beauty_core.h`、`01_源码/obs-beauty.cpp`。
- **判读陷阱**：`render#N` 日志在 `obs-beauty.cpp` 内用函数级 `static int dbg` 限流，
  整个进程只打前 3 行；不能据此判断 slot=1/2/3 没有 render。

## 待人工确认

- 真实 2–4 路摄像头分别调磨皮 / 美白 / 润色清晰，并连续观察 A 镜头是否闪烁。
  自动化只证明上下文隔离与输出正常，画面观感需人眼判断。
- 侧机位 `faces=0` 检不到脸是独立问题，不属于本决策范围。
