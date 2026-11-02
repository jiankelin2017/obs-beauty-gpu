# OBS 美颜插件（GPU 版 v3.1.76）· 引擎 core vs gpu 对比 + 性能实测

> 生成时间：2026-11-01 20:25
> 状态：**多次修正版**。早期"GPU 模式比 CPU 模式省 CPU / 两者大差不差"均不严谨，本版基于源码 + 三次日志复核给出最终结论。
> 测试对象：D:\插件\OBS美颜GPU版（obs-beauty v3.1.76）

## 一、两个引擎的相同点（源码确定）

| 相同点 | 说明 |
|---|---|
| 算法同源 | `GpuEngine` 是 `CoreEngine` 的薄包装壳，`process()` 在 Phase 1 直转 `core_->process()`（`beauty_core.cpp:3671`），磨皮算法逐字节等价 |
| 共享检测/掩码/参数 | 人脸检测(YuNet)、肤色掩码、磨皮参数、历史帧缓冲都挂在内部 `CoreEngine` 上，`GpuEngine` 仅透传（`copyMaskTo`/`maskStat`/`bandDiag` 等全是转发） |
| 统一渲染入口 | 渲染侧不论 core/gpu 都调用同一 `beautyEngine().process()`（`obs-beauty.cpp:4588`） |
| 输出画质 | 各自正常工作态下等价 |

## 二、两个引擎的不同点（源码确定）

| 维度 | `engine=core/1.6.0` | `engine=gpu/2.0.0` |
|---|---|---|
| 版本号 | `1.6.0` | `2.0.0`（仅标识） |
| `id()` | `core` | `gpu` |
| **`supportsGpu()`** | **false** | **true** ← 唯一真正起作用的开关 |
| 渲染侧 | CPU 路径：`process()` 内跑全帧 CPU 磨皮 | `supportsGpu()=true` 时挂 GPU 特效管线：段区间钩子认领磨皮段、`smooth/tone/mask.hlsl` 在显存算、链尾不回读省每帧 ~8MB 下载/上传 |

注意：`supportsGpu()=true` 只是"允许"走 GPU 管线，真正生效还依赖着色器成功加载；加载失败会回落 CPU 路径（与 core 表现一致）。

## 三、性能实测（三次运行，1280×720@30，每 180 帧均值）

「引擎」= `prof_proc` = `beautyEngine().process()` 耗时（`obs-beauty.cpp:4616/4747`）。

| 运行 | 引擎 | GPU 管线状态 | 有脸 | 引擎耗时 | 总耗时 |
|---|---|---|---|---|---|
| 19-50-44 | gpu | **生效**（hlsl 已加载 + 段链钩子启用） | 99–100% | **4.6–5.7 ms** | **7–9 ms** |
| 20-05-06 | gpu | **未生效**（日志无 hlsl 加载、无段链钩子） | 99–100% | 11.5–12.4 ms | 15.7–16.5 ms |
| 20-10-49 | core | CPU 路径 | 99–100% | 11.7–13.2 ms | 14.8–17.4 ms |

## 四、结论（最终）

1. **`engine=core` 与 `engine=gpu` 两个开关本身差异极小**：二者底层算法同源，渲染入口同一。
2. **真正拉开差距的是 GPU 渲染管线（段链钩子 + 着色器）是否真正生效**：
   - 生效时（19-50-44）：引擎耗时 **4.6ms vs 12ms ≈ 2.6× 更快**，整帧 ~8ms vs ~16ms。
   - 失效时（着色器未加载/回落，如 20-05-06）：退化成 CPU 路径，与 `engine=core` 等价（~12ms / ~16ms）。
3. **早期"大差不差"结论的误判来源**：之前对比的 GPU 组（20-05-06）那次 `engine=gpu` 但 GPU 管线并未真正加载（日志无 hlsl 已加载、无段链钩子），实际跑的是 CPU 路径，所以与 core 组（20-10-49）测得几乎一致。该对比不公平，不代表 GPU 引擎的真实上限。
4. **早期"GPU 更省 CPU / core=0.0ms"也是误读**：`core=0.0ms` 来自 self-check 的**前 3 帧**（`faces=0`，worker 异步检测未出结果），不能代表全时段；看「性能」行 `有脸 99–100%` 才知美颜全程在干活，且引擎耗时在 CPU 路径下为 ~12ms（非 0）。

## 五、待办 / 下一步

- 排查 `run_obs_gpu_test.bat`（20-05-06）为何 `engine=gpu` 却没加载着色器（段链钩子也没挂）——很可能是那次着色器懒加载未触发或回落。公平对比需确保日志出现 `tone/smooth/mask.hlsl 已加载` 与 `段区间钩子已启用` 再测。
- 数据来源日志：
  - `2026-11-01 19-50-44.txt`（engine=gpu，GPU 管线生效）
  - `2026-11-01 20-05-06.txt`（engine=gpu，GPU 管线未生效）
  - `2026-11-01 20-10-49.txt`（engine=core）

## 六、重要安全约束

- **切勿设 `BEAUTY_GPU_SMOOTH_PROD=1`**：触发 Phase 3.5（`beauty_gpu_smooth()` 23 次 ping-pong draw），在 RTX 5060（Blackwell）硬崩（无日志、无事件）。Phase 3.5 仅为兜底，主路径是 Phase 5a 段链钩子（已验证可用）。
