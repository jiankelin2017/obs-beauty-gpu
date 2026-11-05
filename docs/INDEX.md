---
doc_type: INDEX
title: 文档总表
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: 56dc178
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
| 现状入口 | [`README.md`](README.md) | README | Active | 3.0.1 | Jian | 2026-11-05 | 替代根 `README.md`、`04_文档/README.md` |
| 变更记录 | [`CHANGELOG.md`](CHANGELOG.md) | CHANGELOG | Active（骨架） | 3.0.1 | Jian | 2026-11-05 | 批 4 并入 7 篇 `修复_*` |
| 开发流程与计划 | [`guides/guide-development.md`](guides/guide-development.md) | guides | Active | 3.0.1 | Jian | 2026-11-05 | 原 `04_文档/开发流程与优化计划.md` |
| 引擎 id 与 GPU 管线判读 | [`reference/reference-engine-id.md`](reference/reference-engine-id.md) | reference | Active | 3.0.1 | Jian | 2026-11-05 | 原根 `GPU_CPU_对比报告.md` |
| 开关总表 | [`reference/reference-switches.md`](reference/reference-switches.md) | reference | Active（元数据待批 2 补） | 3.0.1 | Jian | 2026-11-05 | 原 `04_文档/开关总表.md` |
| 验证工具与坑 | `guides/guide-verification.md` | guides | 待迁移（批 2） | 3.0.1 | Jian | — | 当前 `05_验证/README.md` |
| 离线回归 | `guides/guide-offline-regression.md` | guides | 待迁移（批 2） | 3.0.1 | Jian | — | 当前 `06_离线验证/README.md` |
| 分割探针 | `reference/reference-skinseg-probe.md` | reference | 待迁移（批 2） | 3.0.1 | Jian | — | 当前 `06_离线验证/segtest/README.md` |
| 基线测量规程 | `reference/reference-baseline.md` | reference | 待迁移（批 2） | 3.0.1 | Jian | — | 当前 `04_文档/Phase0_基线测量规程.md` |

> 批 1 期间 `README.md` 中指向 `guides/guide-verification.md`、`guides/guide-offline-regression.md` 的
> 链接尚未生效（批 2 迁移后自动接通）；上表「当前」列给出过渡期的实际位置。

## 2. archive 索引

| 归档件 | 原路径 | 归档原因 | 替代文档 |
|---|---|---|---|
| [`archive/PROGRESS-20261105.md`](archive/PROGRESS-20261105.md) | 根 `PROGRESS.md` | 6263 行逐轮证据总账 / 流水账，正文冻结 | `README.md` / `CHANGELOG.md` |
| [`archive/index-20261105.md`](archive/index-20261105.md) | `04_文档/README.md` | 严重失实：写 2.14.1、47 条开关、25 项清单（实际 3.0.1 / 63 条） | 本表 |

归档区另有：`05_验证/out/logs/*.md` 25 篇过程日志、`_回档备份_20261104_2249/` 快照（批 5 迁入）。

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
| N18–N24 | §3.4 验证工具 7 条坑 | `README.md` §3「工具坑」；逐条展开见 `guides/guide-verification.md`（批 2） |

## 5. 治理批次进度

| 批 | 聚类 | 状态 |
|---|---|---|
| 1 | G1/G2 骨架 + 入口 | **已落地** |
| 2 | G3/G9 参考与工具 | 待执行 |
| 3 | G4/G5/G6 设计稿 | 待执行 |
| 4 | G7/G8 决策与变更 | 待执行 |
| 5 | G10 过程日志与回档快照归档 | 待执行 |
| 6 | 防腐化机制建议 | 待输出 |

## 6. 维护规则（摘要）

- 固定分类：`README` / `design` / `guides` / `reference` / `CHANGELOG` / `adr` / `archive`；
  主目录每类只留当前版，同一主题不许两份并存。
- 命名 `类型-主题.md`；禁止 `final_v3_副本` 一类命名。归档件命名 `主题-YYYYMMDD.md`。
- 每篇必须有 YAML 头：`version` / `last_updated` / `applies_to` / `maintainer` / `status` / `supersedes`。
- 归档件文首必须写「已废弃，见 xxx」+ 归档原因；正文冻结不改。
- 新文档只能放规定位置；改「当前结论」必须同步本表与 `CHANGELOG.md`。
- `05_验证/check_release.py` 第 6 项的语料扫描包含 `docs/` 但**排除 `docs/archive/`**
  （归档日志含未入表内部旋钮，纳入会制造假 FAIL）。
