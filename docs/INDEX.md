---
doc_type: INDEX
title: 文档总表
version: 0.1.7
last_updated: 2026-11-06
applies_to: 3.2.2
repo_baseline: 914f388
maintainer: Jian
status: Active
---

# 文档总表（唯一入口）

> **结论**：主目录每类只留当前版；旧版、过程日志、证据总账一律进 `archive/`。
> 任何「当前版本 / 当前性能 / 当前开关」以 [`README.md`](README.md) + 源码为准。
> 本表同时是 `PROGRESS §9.x` 与旧 N 编号的**唯一映射表**（B1 决策：源码 / 脚本注释里的 19 处引用不改）。

## 1. 当前版总表

| 主题 | 路径 | 类型 | 状态 | 适用版本 | 维护人 | 最后核对 | 替代 / 归档 |
|---|---|---|---|---|---|---|---|
| 现状入口 | [`README.md`](README.md) | README | Active | 3.2.2 | Jian | 2026-11-06 | 替代根 `README.md`、`04_文档/README.md` |
| 变更记录 | [`CHANGELOG.md`](CHANGELOG.md) | CHANGELOG | Active | 3.2.2 | Jian | 2026-11-06 | 批 4 已合并 7 篇修复记录 + 结论稿摘要 |
| 第三方组件与许可 | [`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md) | NOTICES | Active | 3.2.2 | Jian | 2026-11-06 | 新建：登记 ONNX Runtime / YuNet / BiSeNet / MediaPipe / 手部模型的许可、入库与分发状态 |
| 开发流程与计划 | [`guides/guide-development.md`](guides/guide-development.md) | guides | Active | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/开发流程与优化计划.md` |
| 引擎 id 与 GPU 管线判读 | [`reference/reference-engine-id.md`](reference/reference-engine-id.md) | reference | Active | 3.2.2 | Jian | 2026-11-05 | 原根 `GPU_CPU_对比报告.md` |
| 开关总表 | [`reference/reference-switches.md`](reference/reference-switches.md) | reference | Active | 3.2.2 | Jian | 2026-11-06 | 原 `04_文档/开关总表.md` |
| 验证工具与坑 | [`guides/guide-verification.md`](guides/guide-verification.md) | guides | Active | 3.2.2 | Jian | 2026-11-05 | 原 `05_验证/README.md` |
| 离线回归 | [`guides/guide-offline-regression.md`](guides/guide-offline-regression.md) | guides | Active | 3.2.2 | Jian | 2026-11-05 | 原 `06_离线验证/README.md` |
| 分割探针 | [`reference/reference-skinseg-probe.md`](reference/reference-skinseg-probe.md) | reference | Active | 3.2.2 | Jian | 2026-11-05 | 原 `06_离线验证/segtest/README.md`（原未跟踪，批 2 首次纳入） |
| 基线测量规程（历史） | [`reference/reference-baseline.md`](reference/reference-baseline.md) | reference | Active（历史参考） | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/Phase0_基线测量规程.md` |
| 掩码上 GPU 设计 | [`design/design-mask-gpu.md`](design/design-mask-gpu.md) | design | Active | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/方案A_5b2_掩码上GPU设计.md` |
| 五官段 GPU 设计 | [`design/design-region-gpu.md`](design/design-region-gpu.md) | design | Active | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/方案A_五官GPU设计.md` |
| 输入旋转 | [`design/design-input-rotation.md`](design/design-input-rotation.md) | design | Active | 3.2.2 | Jian | 2026-11-06 | 原 `04_文档/方案B_输入旋转开发计划.md`；§10 记 3.2.0 自动方向认定 |
| 运动判定搬 GPU | [`design/design-motion-gpu.md`](design/design-motion-gpu.md) | design | Active | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/方案C_运动判定搬GPU设计.md` |
| 遮挡感知掩码（Draft） | [`design/design-occlusion-mask.md`](design/design-occlusion-mask.md) | design | Active（Draft · 未接线） | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/方案D_遮挡感知掩码设计.md` |
| SDK 更优设计移植（Draft） | [`design/design-sdk-port.md`](design/design-sdk-port.md) | design | Active（Draft · 未接线） | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/方案E_SDK更优设计移植开发方案.md` |
| 美白整帧色彩变换 | [`design/design-tone-global.md`](design/design-tone-global.md) | design | Active | 3.2.2 | Jian | 2026-11-05 | 原 `04_文档/复刻SDK_美白整帧色彩变换_20261105.md` |
| 多实例引擎槽决策 | [`adr/adr-multi-instance-slots.md`](adr/adr-multi-instance-slots.md) | adr | Active | 3.2.2 | Jian | 2026-11-06 | 3.0.0 / 3.0.1 决策 + 3.2.2 worker 重启加固；证据见 `CHANGELOG.md` |
| 安装路径与闸门决策 | [`adr/adr-setup-programdata.md`](adr/adr-setup-programdata.md) | adr | Active | 3.2.2 | Jian | 2026-11-06 | 3.0.1 决策；3.2.2 补端到端安装 / 卸载实跑记录与「载荷缺失」同类缺陷 |
| 分割不用于遮挡决策 | [`adr/adr-seg-not-for-occlusion.md`](adr/adr-seg-not-for-occlusion.md) | adr | Active | 3.2.2 | Jian | 2026-11-05 | 替代 `04_文档/结论_分割对手遮脸无效_20261105.md`（原文已归档） |
| CPU/GPU 共用 faceGeom | [`adr/adr-cpu-gpu-same-mask.md`](adr/adr-cpu-gpu-same-mask.md) | adr | Active（归纳 · 待人工确认） | 3.2.2 | Jian | 2026-11-05 | 归纳自 3 篇修复记录，**非原始决策** |

> 批 2 起上表全部为 Active；批 3 起 `design/` 7 篇纳入总表，其中方案 D / E 是
> **Draft · 未接线**，不可作为当前实现依据。`05_验证/`、`06_离线验证/` 下的脚本与数据仍在原目录，文档只是归位。
>
> **2026-11-06 版本口径**：适用版本列统一改为 3.2.2（3.2.x 只新增检测方向认定、一行日志、
> 删滤镜再加回来的 worker 重启修复，未改动这些主题的主体实现）；`最后核对` 列保留各文档
> **实际复核**的日期 —— 仍写 2026-11-05 的表示本次只做了版本口径对齐、未逐字复核。
>
> **2026-11-06 新增两篇待登记**：`06_离线验证/_slot_cycle_probe.py`（3.2.2 槽位循环判据，
> 见 [`guides/guide-offline-regression.md`](guides/guide-offline-regression.md)）、
> `adr/adr-multi-instance-slots.md` §决策补 3.2.2 worker 重启条目。

## 2. archive 索引

| 归档件 | 原路径 | 归档原因 | 替代文档 |
|---|---|---|---|
| [`archive/PROGRESS-20261105.md`](archive/PROGRESS-20261105.md) | 根 `PROGRESS.md` | 6263 行逐轮证据总账 / 流水账，正文冻结 | `README.md` / `CHANGELOG.md` |
| [`archive/index-20261105.md`](archive/index-20261105.md) | `04_文档/README.md` | 严重失实：写 2.14.1、47 条开关、25 项清单（实际 3.0.1 / 63 条） | 本表 |
| [`archive/plan-gpu-port-20261105.md`](archive/plan-gpu-port-20261105.md) | `04_文档/方案A_GPU移植开发计划.md` | GPU 移植立项规划书，进度数字为写作当时快照；正文冻结、7 条旧链接失效 | `guides/guide-development.md` / 本表 |
| [`archive/changelog-src-sharpen-mask-20261105.md`](archive/changelog-src-sharpen-mask-20261105.md) | `04_文档/修复_GPU纯锐化掩码缺失_20261105.md` | 结论已并入 `CHANGELOG.md` 修复批次；正文冻结 | `CHANGELOG.md` |
| [`archive/changelog-src-bilateral-p2-20261105.md`](archive/changelog-src-bilateral-p2-20261105.md) | `04_文档/修复_P2值域加权核_20261105.md` | 结论与「选 A 保持关闭」决策已并入；正文冻结 | `CHANGELOG.md` |
| [`archive/changelog-src-threed-mach-band-20261105.md`](archive/changelog-src-threed-mach-band-20261105.md) | `04_文档/修复_五官立体白圈_20261105.md` | 结论已并入；正文冻结 | `CHANGELOG.md` |
| [`archive/changelog-src-seg-sync-path-20261105.md`](archive/changelog-src-seg-sync-path-20261105.md) | `04_文档/修复_分割同步路径与segMix地雷_20261105.md` | 结论已并入；正文冻结 | `CHANGELOG.md` |
| [`archive/changelog-src-mask-double-rotation-20261105.md`](archive/changelog-src-mask-double-rotation-20261105.md) | `04_文档/修复_掩码椭圆双重旋转_20261105.md` | 结论已并入；正文冻结 | `CHANGELOG.md` / `adr/adr-cpu-gpu-same-mask.md` |
| [`archive/changelog-src-spot-chin-20261105.md`](archive/changelog-src-spot-chin-20261105.md) | `04_文档/修复_祛斑与下巴_20261105.md` | 结论与方案 A / C 决策已并入；正文冻结 | `CHANGELOG.md` |
| [`archive/changelog-src-sharpen-threshold-20261105.md`](archive/changelog-src-sharpen-threshold-20261105.md) | `04_文档/修复_锐化阈值被覆盖_20261105.md` | 结论已并入；正文冻结 | `CHANGELOG.md` |
| [`archive/conclusion-seg-occlusion-20261105.md`](archive/conclusion-seg-occlusion-20261105.md) | `04_文档/结论_分割对手遮脸无效_20261105.md` | 实测结论稿，已改写为 ADR；原文保留合成素材数据与三个待决策方向 | `adr/adr-seg-not-for-occlusion.md` |

归档区另有：25 篇验收过程日志（下表）与 2.16.1 回档快照的 4 篇文档（下表）。

### 2.1 过程日志归档映射（25 篇 · 批 5）

`05_验证/out/logs/*.md` 已按 `log-YYYYMMDD-<主题>.md` 统一命名迁入 `archive/`，**正文冻结、仅加废弃头**。
其中 **20 篇原本已被 Git 跟踪**（迁移记为删除 + 归档新增），**5 篇为首次纳入仓库**；
7 篇 T10-b 同主题报告用时间戳区分。原路径 `out/logs/` 已失效，`out/logs/` 仍是脚本输出目录。

| 原文件名 | 归档件 |
|---|---|
| `P0-10_spot_edge_20261103.md` | [`log-20261103-p0-10-spot-edge.md`](archive/log-20261103-p0-10-spot-edge.md) |
| `P0-1_slope_20261103.md` | [`log-20261103-p0-1-slope.md`](archive/log-20261103-p0-1-slope.md) |
| `P0-2_region4k_20261103.md` | [`log-20261103-p0-2-region4k.md`](archive/log-20261103-p0-2-region4k.md) |
| `P0-7_region_20261103.md` | [`log-20261103-p0-7-region.md`](archive/log-20261103-p0-7-region.md) |
| `P0-7_samemask_20261103.md` | [`log-20261103-p0-7-samemask.md`](archive/log-20261103-p0-7-samemask.md) |
| `P0-8_4k_S0recure_20261103.md` | [`log-20261103-p0-8-4k-s0recure.md`](archive/log-20261103-p0-8-4k-s0recure.md) |
| `P0-8_fix_20261103.md` | [`log-20261103-p0-8-fix.md`](archive/log-20261103-p0-8-fix.md) |
| `P0-9_geomonly_gate_20261103.md` | [`log-20261103-p0-9-geomonly-gate.md`](archive/log-20261103-p0-9-geomonly-gate.md) |
| `S0_rebind_20261103.md` | [`log-20261103-s0-rebind.md`](archive/log-20261103-s0-rebind.md) |
| `S7_adaptive_20261103.md` | [`log-20261103-s7-adaptive.md`](archive/log-20261103-s7-adaptive.md) |
| `T10b_region_equiv_20261104_120733.md` | [`log-20261104-t10b-region-equiv-120733.md`](archive/log-20261104-t10b-region-equiv-120733.md) |
| `T10b_region_equiv_20261104_121203.md` | [`log-20261104-t10b-region-equiv-121203.md`](archive/log-20261104-t10b-region-equiv-121203.md) |
| `T10b_region_equiv_20261104_125849.md` | [`log-20261104-t10b-region-equiv-125849.md`](archive/log-20261104-t10b-region-equiv-125849.md) |
| `T10b_region_equiv_超小脸_20261104_125752.md` | [`log-20261104-t10b-region-equiv-tinyface-125752.md`](archive/log-20261104-t10b-region-equiv-tinyface-125752.md) |
| `T10b_region_equiv_超小脸_20261104_130509.md` | [`log-20261104-t10b-region-equiv-tinyface-130509.md`](archive/log-20261104-t10b-region-equiv-tinyface-130509.md) |
| `T10b_region_equiv_遮挡_20261104_124733.md` | [`log-20261104-t10b-region-equiv-occlusion-124733.md`](archive/log-20261104-t10b-region-equiv-occlusion-124733.md) |
| `T10b_region_equiv_遮挡_20261104_124750.md` | [`log-20261104-t10b-region-equiv-occlusion-124750.md`](archive/log-20261104-t10b-region-equiv-occlusion-124750.md) |
| `cam_live_20261103.md` | [`log-20261103-cam-live.md`](archive/log-20261103-cam-live.md) |
| `camera_degraded_20261103.md` | [`log-20261103-camera-degraded.md`](archive/log-20261103-camera-degraded.md) |
| `front_asset_20261103.md` | [`log-20261103-front-asset.md`](archive/log-20261103-front-asset.md) |
| `input_rot_2.10.0_20261104.md` | [`log-20261104-input-rot-2.10.0.md`](archive/log-20261104-input-rot-2.10.0.md) |
| `material_capture_20261104_122241.md` | [`log-20261104-material-capture-122241.md`](archive/log-20261104-material-capture-122241.md) |
| `region_geomonly_20261103.md` | [`log-20261103-region-geomonly.md`](archive/log-20261103-region-geomonly.md) |
| `soak4k_30min_20261103.md` | [`log-20261103-soak4k-30min.md`](archive/log-20261103-soak4k-30min.md) |
| `soak4k_s3_30min_20261103.md` | [`log-20261103-soak4k-s3-30min.md`](archive/log-20261103-soak4k-s3-30min.md) |

### 2.2 回档快照文档（4 篇 · 批 5）

`_回档备份_20261104_2249/` 的 4 篇 `.md` 已迁入 `archive/backup-20261104_2249/`，正文冻结。
**该快照其余 14 个源码 / dll / `Setup.exe` 文件仍在原目录、未移动、未纳入仓库**；
需完整还原 18 文件快照时，把 4 篇归档件复制回原目录对应位置。

| 原位置 | 归档件 |
|---|---|
| `_回档备份_20261104_2249/PROGRESS.md` | [`backup-20261104_2249/backup-progress-20261104.md`](archive/backup-20261104_2249/backup-progress-20261104.md) |
| `_回档备份_20261104_2249/README.md` | [`backup-20261104_2249/backup-readme-20261104.md`](archive/backup-20261104_2249/backup-readme-20261104.md) |
| `_回档备份_20261104_2249/04_文档/开关总表.md` | [`backup-20261104_2249/backup-switches-20261104.md`](archive/backup-20261104_2249/backup-switches-20261104.md) |
| `_回档备份_20261104_2249/04_文档/开发流程与优化计划.md` | [`backup-20261104_2249/backup-devplan-20261104.md`](archive/backup-20261104_2249/backup-devplan-20261104.md) |

### 2.3 不纳入文档体系的运行时产物

按 `.gitignore` 保持原位、不入库：`05_验证/out/logs/` 下 **27 篇 `.txt`、13 篇 `.json`、20 张 `.png`**，
以及回档快照里其余 14 个二进制 / 源码文件。脚本新产生的报告一律属运行时产物，**不进文档体系**；
归档件只是历史快照，结论以 `README.md` / `CHANGELOG.md` / `guides/` 为准。
`archive/` 整棵子树排除于 `check_release.py` 语料扫描（避免未入表内部旋钮制造假 FAIL）。

## 3. `PROGRESS §9.x` 统一映射（B1）

| 引用形态 | 去哪找 | 说明 |
|---|---|---|
| `PROGRESS §9.x` / `§9.x` | `archive/PROGRESS-20261105.md` 内按章号检索 | 正文未改，章号全部保留 |
| `§9.57` / `§9.57.1`（3.0.0 多实例隔离） | 同上；结论摘要见 `CHANGELOG.md` 3.0.0 |  |
| `§9.58`（3.0.1 代码审查修复） | 同上；结论摘要见 `CHANGELOG.md` 3.0.1 |  |
| `§9.52.x`（2.16.2–2.17.3 手遮眼实验） | 同上；结论摘要见 `CHANGELOG.md` 2.16.1 | 实验数据不随代码删除 |
| `README §2` / `§3` / `§5`（旧引用） | `README.md` §2 / §3 / §5 | 节号在重写稿中保持对齐 |
| 源码 / 脚本注释里 19 处 `§9.x` | **不改** | B1：避免触发代码链与重编译 |

## 4. 旧 N 编号 → 新章节映射

原根 `README.md` 的 N 编号有重复（N6 / N7 各出现两次）。重写后 §3 改为按主题分组，映射如下：

| 旧编号 | 原位置 | 去处 |
|---|---|---|
| N1 | §3.1 输入旋转静默失效 | **已修（2.13.1）** → `CHANGELOG.md` |
| N2 | §3.1 五官段默认拿不到 | **已解决（2.14.0）** → `CHANGELOG.md` |
| N3 | §3.1 多脸收益归零 | `README.md` §3「多脸」 |
| N4 | §3.1 开箱即改画面 | `README.md` §3「开箱行为」 |
| N5 | §3.1 竖屏观感像拉伸 | `README.md` §3「构图」 |
| N6a | §3.1 输入方向设置不当 | `README.md` §3「输入方向」 |
| N6b | §3.2 S3 双缓冲 opt-in | `archive/PROGRESS-20261105.md`（按 §9.16 检索） |
| N7a | §3.1 掩码跟脸转等价未对拍 | `README.md` §3「掩码」 |
| N7b | §3.2 4K 帧环 M1 1 灰阶差 | `archive/PROGRESS-20261105.md`（按 §9.5 检索） |
| N8 | §3.2 `tskip` 偶发 | `archive/PROGRESS-20261105.md`（按 §9.5.4 检索） |
| N9 | §3.2 S5 通道打包否决 | `archive/PROGRESS-20261105.md`（按 §9.12.2 检索）；决策在 `CHANGELOG.md` |
| N10 | §3.2 S6 去两趟 Copy | **已完成（2.14.1）** → `CHANGELOG.md` |
| N11 | §3.2 缩小检测输入已实现 | `archive/PROGRESS-20261105.md`（按 §9.5.3 / §9.29 检索） |
| N11b | §3.2 方案C 收益依赖场景 | `README.md` §3「性能妥协」 |
| N12 | §3.3 二进制不入库 | `README.md` §3「工程」 |
| N13 | §3.3 安装包与构建产物脱节 | **已修（闸门 + 3.0.1 路径修复）** → `CHANGELOG.md` |
| N14 | §3.3 边界带闪烁立项不成立 | `README.md` §3「立项否决」 |
| N15 | §3.3 工具不校验 dll 版本 | **已修（闸门）** → `CHANGELOG.md` |
| N16 / N17 | §3.3 UI 独立 dll / CPU 与 UI 源码不在本仓库 | `README.md` §3「工程」 |
| N18–N24 | §3.4 验证工具 7 条坑 | `README.md` §3「工具坑」；逐条展开见 [`guides/guide-verification.md`](guides/guide-verification.md) |

## 5. 治理批次进度

| 批 | 聚类 | 状态 |
|---|---|---|
| 1 | G1/G2 骨架 + 入口 | **已落地**（commit `914f388`） |
| 2 | G3/G9 参考与工具 | **已落地**（commit `f68d716`） |
| 3 | G4/G5/G6 设计稿 | **已落地**（单独 commit） |
| 4 | G7/G8 决策与变更 | **已落地**（单独 commit） |
| 5 | G10 过程日志与回档快照归档 | **已落地**（单独 commit） |
| 6 | 防腐化机制建议 | **已落地**：6.1 模板 + 6.2 元数据规范（commit `24aeaf6`）；6.3 提交闸门 + 6.4 定期巡检（单独 commit，见 §6） |

## 6. 维护规则（模板、元数据、提交闸门与巡检）

- 固定分类：`README` / `design` / `guides` / `reference` / `CHANGELOG` / `adr` / `archive`；
  主目录每类只留当前版，同一主题不许两份并存。
- 命名 `类型-主题.md`；禁止 `final_v3_副本` 一类命名。归档件命名 `主题-YYYYMMDD.md`。
- 每篇必须有 YAML 头：`version` / `last_updated` / `applies_to` / `maintainer` / `status` / `supersedes`。
- 归档件文首必须写「已废弃，见 xxx」+ 归档原因；正文冻结不改。
- 新文档只能放规定位置；改「当前结论」必须同步本表与 `CHANGELOG.md`。
- `05_验证/check_release.py` 第 6 项的语料扫描包含 `docs/` 但**排除 `docs/archive/`**
  （归档日志含未入表内部旋钮，纳入会制造假 FAIL）。
- `design/` 中的 Draft 稿不扩 `status` 枚举：仍写 `Active`，但文首必须醒目标注
  「**Draft · 未接线，不可作为当前实现依据**」，并写明适用条件与失效说明。
- `adr/` 命名 `adr-<主题>.md`，**不带日期**，一事一篇。归纳性而非原始决策的 ADR
  必须在头部标注「归纳、待人工确认」，不得冒充已接受决策。
- 过程日志按 `log-YYYYMMDD-<主题>.md` 归档，同主题多批次须带时间戳；
  `05_验证/out/logs/` 是脚本输出目录，新报告属运行时产物、不进 `docs/`。

### 6.1 文档模板（新建 / 重写时复制）

> **结论**：所有 Active 文档共用一套骨架，ADR 与归档件只做局部替换，不另起顶层文件。
> 骨架里的占位文字必须替换干净，不允许把「待补」「TODO」留在主目录。

通用骨架（适用于 `README` / `design` / `guides` / `reference` / `CHANGELOG`）：

```markdown
---
doc_type: <README|design|guides|reference|CHANGELOG|adr>
title: <一句话标题>
version: 0.1.0
last_updated: <YYYY-MM-DD>
applies_to: 3.0.1
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: <被替代的旧文档；无则写「无」>
---

# <标题>

> **结论**：<一句话，读者只看这句就能做决定>
> **适用条件**：<版本 / 场景 / 开关前提>
> **失效说明**：<何时不适用、历史数字是否代表当前实测>

## 当前有效结论
<只写当前成立的结论，逐条可执行；不确定处标「待人工确认」>

## 操作 / 参考细节
<步骤、命令、路径、判据；本机命令统一 py -3.12>

## 关键变更记录
<只留影响读者使用的里程碑，不写流水账>

## 失效内容与待人工确认
<已废弃做法、未验证项、原文归档位置>
```

ADR 骨架：沿用上述 YAML 头（`doc_type: adr`，文件名 `adr-<主题>.md` 不带日期），把
「当前有效结论」替换为「决策状态 / 备选方案 / 后果」三段；归纳性而非原始决策的 ADR
必须在头部标注「归纳、待人工确认」，不得冒充已接受决策。

归档件骨架：不套用通用骨架，只在原文最前面加 5–10 行废弃头，**正文一字不改**。
新归档件首行统一用「已废弃」；批 5 的 29 篇存量件写「已归档」，属等价写法，不回改。
关键是下列 5 项要素齐全：

```markdown
> **已废弃（<日期>）**：本文件已归档，内容冻结，不作为当前结论依据。
> **归档原因**：<过时 / 失效 / 结论已合并>
> **结论去哪找**：docs/README.md、docs/CHANGELOG.md、docs/INDEX.md
> **原路径**：<旧路径> 已失效
> **快照说明**：正文里的版本号、性能数字与判据是写作当时快照，不代表 3.0.1 实测。
```

### 6.2 元数据规范

| 字段 | 何时改 | 规则 |
|---|---|---|
| `version` | 该文档每次改动 | 普通修订 +0.0.1；与插件版本号无关 |
| `last_updated` | 该文档每次改动 | 实际改动日期 `YYYY-MM-DD` |
| `applies_to` | 产品发版时 | 全库统一，当前 `3.0.1` |
| `repo_baseline` | 产品发版时 | **文档治理 commit 不推进**；当前恒为 `914f388` |
| `status` | 归档时 | 只用 `Active` / `Deprecated`；Draft 走文首标注，不扩枚举 |
| `supersedes` | 归档 / 合并时 | 必填，形成可追溯链；无替代写「无」 |
| `maintainer` | — | 统一 `Jian` |

禁止事项（与上文各条规则同时生效）：

- 把历史性能数字润色成当前实测；须原样标「2.14.x 基线实测，3.0.1 未重测」或「当时快照，不代表 3.0.1 实测」。
- 臆造当前结论；不确定处必须标「待人工确认」。
- `final_v3` / `副本` / 带日期的 ADR 命名；同一主题在主目录出现第 2 份。
- 改归档件正文、改源码注释里的 `§9.x`（B1）、让文档治理触发版本升级或重编译 / 装机 / 实机复验（A1）。

### 6.3 提交前 4 项自检（提交闸门）

> **结论**：文档类提交前固定跑下面 4 项，任一项不过就先修再提交。目的是让不合规文档进不了仓，
> 而不是等巡检时再补救。4 项全为只读检查，不产生新文件、不改代码。

| # | 自检项 | 判据 / 命令 | 不通过时 |
|---|---|---|---|
| 1 | 元数据与版本 | 改过的每篇 Active 文档 YAML 头齐全；`version` 按 §6.2 递增、`last_updated` 为当天；`applies_to` / `repo_baseline` / `maintainer` 未被误改；归档件正文 `git diff` 为空 | 补齐字段 / 升修订位，只增不减、不跳号 |
| 2 | 骨架与命名 | 新建或重写件套 §6.1 骨架且无「待补 / TODO」残留；命名 `类型-主题.md`；ADR 不带日期；Draft 有文首标注；主目录同主题只有 1 份 | 回 §6.1 重套骨架或改名 |
| 3 | 总表与链接 | 新增 / 移动 / 归档的文档已登记进 §1 或 §2；§5 批次状态与实物一致；`docs/` 主动区相对链接断链 0 | 先补本表，再提交 |
| 4 | 发布闸门 | `py -3.12 05_验证/check_release.py --strict` 必须 **12 PASS / 0 WARN / 0 FAIL**；`docs/archive/` 仍在语料排除列表内；开关表 66 条与源码双向一致 | 定位 FAIL 来源；不得靠删断言过关 |

提交动作：用 `git add -u` + 显式 `git add -- docs/...`，**不用 `git add -A`**（仓库存在大量历史
未跟踪产物，全量暂存会把它们卷进来）。按 A1，改 `check_release.py` 属文档治理配套：不动插件版本、
不触发 dll / `Setup.exe` / 装机 / 实机复验链；按 B1，源码注释里的 19 处 `§9.x` 不改。

### 6.4 定期巡检（存量不漂移）

> **结论**：模板管「写时不跑偏」、闸门管「进不了仓」，巡检管「存量不漂移」。
> 固定两个人工触发点，结论只写一行到 `CHANGELOG.md`，**不新建巡检报告文件、不预设自动化任务**。

触发点：① 产品发版前必做；② 每月一次（人工）。维护人 `Jian`。

| 巡检项 | 判据 |
|---|---|
| 总表对账 | `docs/` 主动区 `.md` 数量与 §1 / §2 登记数一致，没有未登记的 Active 文档 |
| 元数据一致 | 所有 Active 文档 `repo_baseline` 同为 `914f388`（发版时统一推进）、`applies_to` 一致；无「改了内容却没升 `version`」 |
| 链接与归档区 | 主动区相对链接断链 0；`archive/` 正文无改动；运行时产物没被误放进 `docs/` |
| 闸门复跑 | `--strict` 12 PASS；`docs/archive/` 仍被排除；开关表 63 条仍与源码双向一致 |
| 未跟踪产物 | 只记录数量与位置变化，**不自动清理**；确需清理另行列清单确认 |

发现漂移：回到 §6.1 / §6.2 修正，在 `CHANGELOG.md` 记一行「巡检日期 + 结论 + 例外」，并在 §5 补一笔；
是否做成定时自动化另行决定，本表不预设。
