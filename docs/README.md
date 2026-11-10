---
doc_type: README
title: OBS 美颜 GPU 版
version: 0.2.0
last_updated: 2026-11-10
applies_to: 3.2.4
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 04_文档/README.md（已归档）
---

# OBS 美颜 GPU 版

> 把五段人脸美颜（磨皮 / 美白 / 美型 / 清晰 / 润色）从 CPU 搬到 GPU 的 OBS 视频滤镜，
> 源 ID `obs_beauty_filter`。当前版本 **3.2.4**，每实例独占一个引擎槽，上限 8 路并发。

## 装 / 卸

双击 `07_安装包/Setup.exe` → 装完即用，**无需手工放 `onnxruntime.dll` / `yunet.onnx` / 菜单面板**。
再双击一次即卸载（插件目录、顶部「美颜」面板模块、注册表一并清除）。
装上后：源 → 滤镜 → `+` → Beauty (GPU)；顶部菜单栏「美颜」另有参数面板入口。

## 部署约束

- OBS 32.x 只从 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\` 加载；
  `Program Files\obs-studio\obs-plugins\64bit` 是历史布局，装那儿不会加载。
- 一源一滤镜是受支持用法；**同一源串联多个美颜滤镜**会因 GPU 链尾直绘互相覆盖。
- `BEAUTY_SEG=1`（分割影子层）是进程级全局，**只支持单实例**；默认关。
- 每多一个实例多加载一份 YuNet 模型 + 一份帧环内存；上限 **8 个实例/进程**。

## 能力

| 维度 | 结论 |
|---|---|
| 性能 | 4K 往返 ≈2.3–2.8 ms、720p ≈0.8–1.5 ms；全 0 参数直通 0.00 ms |
| 正确性 | 五段与 CPU `maxdiff ≤1`；六配置整体 `maxdiff=0` |
| 稳定性 | 4K 30 min / 54,180 帧零 D3D11 异常；画布切换 / 热重载 / 冷启动通过 |

> 以上为 **2.14.x 基线（RTX 5060 Laptop、单脸），3.x 未重测**，不可当作 3.2.4 实测值引用。
> 测量规程见 [`reference/reference-baseline.md`](reference/reference-baseline.md)。

## 构建与验证

```bat
powershell -ExecutionPolicy Bypass -File 02_构建\build_gpu.ps1   :: 滤镜 dll
powershell -ExecutionPolicy Bypass -File 02_构建\build_ui.ps1    :: 顶部菜单面板
powershell -ExecutionPolicy Bypass -File 03_装机\7_install_gpu.ps1
powershell -ExecutionPolicy Bypass -File 07_安装包\build_setup.ps1
py -3.12 05_验证\check_release.py --strict
```

- 装机前**必须关闭 OBS**；本机 `python` 是 Store 别名，验证脚本一律用 `py -3.12`。
- 发布前闸门须 **14 PASS / 0 FAIL**；离线回归 / 方向认定 / 槽位循环的跑法与判据见
  [`guides/guide-verification.md`](guides/guide-verification.md)、[`guides/guide-offline-regression.md`](guides/guide-offline-regression.md)。

## 文档

| 想知道 | 去哪 |
|---|---|
| 全库总表（唯一索引） | [`INDEX.md`](INDEX.md) |
| 每版改了什么 | [`CHANGELOG.md`](CHANGELOG.md) |
| 已知限制与回退 | [`reference/reference-limits.md`](reference/reference-limits.md) |
| 开关默认值与含义 | [`reference/reference-switches.md`](reference/reference-switches.md) |
| 开发流程 / 验证踩坑 | [`guides/`](guides/) |
| 设计稿与决策（ADR） | [`design/`](design/)、[`adr/`](adr/) |
| 逐轮证据总账（冻结） | [`archive/PROGRESS-20261105.md`](archive/PROGRESS-20261105.md) |

## 许可

**GPL-2.0 only**。可自由使用、修改、再分发（含商用）；**分发修改版必须同样以 GPL-2.0 开源**，
并保留版权与「无担保」声明 —— 与 OBS Studio 宿主同许可，无冲突。
`LICENSE` 是 FSF 原文逐字节副本（不可改，否则 GitHub/SPDX 会归为 "Other"）；版权行与版本口径见
[`COPYRIGHT.md`](../COPYRIGHT.md)，第三方组件（ONNX Runtime / YuNet 等，均 MIT）见
[`THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)。
本插件为独立第三方插件，与 OBS Project 无隶属或背书关系。
