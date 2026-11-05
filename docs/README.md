---
doc_type: README
title: OBS 美颜 GPU 版 · 现状入口
version: 0.1.1
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: bb4dfba
maintainer: Jian
status: Active
supersedes: 04_文档/README.md（已归档）
---

# OBS 美颜 GPU 版

> **结论**：把五段人脸美颜从 CPU 整体搬到 GPU 的 OBS 视频滤镜（`obs_beauty_filter`）。
> 当前可用版本 **3.0.1**，每个滤镜实例独占一个引擎槽，支持最多 8 路并发。
> 全库总表 [`INDEX.md`](INDEX.md)｜历史证据 [`archive/PROGRESS-20261105.md`](archive/PROGRESS-20261105.md)。

## 1. 当前状态（3.0.1 / 2026-11-05）

| 项 | 结论 |
|---|---|
| 版本 | **3.0.1**（代码审查修复 · 普通升级），唯一出处 `01_源码/beauty_params.h` 的 `BEAUTY_BUILD_STR` |
| 架构 | 3.0.0 起 `SlotState` 按槽隔离帧号 / 掩码轨迹 / 肤色 EMA / hook user / 异步 worker；修「A 镜头闪烁 + 侧机位无效果」的根因（进程单例引擎 + 文件级全局帧状态） |
| 3.0.1 修了什么 | 安装包路径改 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty`、槽位失败路径悬空钩子、worker 释放竞态、命令注入面、闸门补 3 条断言 → 见 [`CHANGELOG.md`](CHANGELOG.md) |
| 交付一致性 | 构建 / 安装包 / 装机三处 dll 逐字节一致（633,856 B / `068E03E5FBE2E21C1883A7F372828783`）；`Setup.exe 1,533,952 B / E945A0F86041050C6506A33B26F4A7FC`；`--strict` **12 PASS / 0 WARN / 0 FAIL** |
| 离线回归 | 4 条 golden 逐字节相同 |
| 多机位自动化 | 4 个独立源各挂 1 滤镜 → `slot=0/1/2/3 活跃实例=1/2/3/4`；删一路、槽复用、无黑屏 / 条纹 / 崩溃 |
| **待人工确认** | 用真实 2–4 路摄像头分别调磨皮 / 美白 / 润色清晰，连续观察 A 镜头是否闪烁。自动化只证明上下文隔离与输出正常，画面观感需人眼判断 |

**部署约束（必须记住）**

- OBS 32.x 只从 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll` 加载插件；
  旧的 `Program Files\obs-studio\obs-plugins\64bit` 是历史布局，装到那里 OBS 不会加载。
- 一源一滤镜是受支持用法；**同一源串联多个美颜滤镜**会因 GPU 链尾直绘互相覆盖，不是多实例串扰。
- `BEAUTY_SEG=1`（分割影子层）仍是进程级全局，**只支持单实例**；默认关，不影响多机位。
- 每多一个实例多加载一份 YuNet 模型并多占一份帧环内存；上限 **8 个实例/进程**。

## 2. 可用能力

| 维度 | 当前结论 | 适用条件 / 失效说明 |
|---|---|---|
| 性能 | 生产默认 4K 往返 ≈2.3–2.8 ms、720p ≈0.8–1.5 ms；全 0 参数直通 0.00 ms | **2.14.x 基线实测，3.0.1 未重测**（RTX 5060 Laptop、单脸）。3.0.1 只验证输出正常与多实例隔离，**不可当作 3.0.1 的实测值引用** |
| 正确性 | 五段与 CPU `maxdiff ≤1`；六配置整体 `maxdiff=0`；祛斑帧间 `diffpx` 极值比 1.00 | 2.14.x 及更早的逐像素判据，证据见归档总账 |
| 稳定性 | 4K 30 min / 54,180 帧零 D3D11 异常；画布切换 / 热重载 / 冷启动通过 | 同上，属 2.x 基线 |
| 工程 | 版本号单点维护、四份 shader 同源、装机逐文件 MD5 校验、发布前一致性闸门 | 见 §4 |

## 3. 已知限制（按主题分组，当前仍有效）

| 主题 | 限制 | 影响 / 回退 |
|---|---|---|
| 多脸 | ≥2 张脸时缩略图检测与帧环绕开，收益归零 | 设计边界；素材统计多脸 1.59%，不立项 |
| 开箱行为 | 磨皮出厂默认 **20**，装上就轻微改画面 | 要零影响把磨皮拖到 0 |
| 构图 | 竖屏 / 横竖混用时观感像被拉伸 | 与滤镜无关，是 OBS 场景项几何；滤镜侧无解 |
| 输入方向 | 设 90/270 而源其实正立 → YuNet 检不到脸 → 美颜全灭 | 2.15.0 起检不到脸自动退回 0° 并打 WARN；源侧躺而设 0/180 仍不覆盖 |
| 掩码 | 2.15.0「掩码跟脸转」的 CPU↔GPU 逐像素等价**尚未实机对拍** | 只过编译校验；待办 `GPU_MASK_CMP=1` + `MASK_GEOM_ONLY=0` |
| 性能妥协 | `DET_ADAPT_AGG=rms` 的代价越过人眼 JND（+6 灰阶档） | **默认 `max`，不翻**；要开就成套开 `rms + DET_ADAPT_STILL=4` |
| 立项否决 | 「边界带闪烁」立项不成立（仿真片高估了它） | 静态真机输入下掩码 ΔD 全 0.00；勿把「检测框时域残差」当成闪烁根因 |
| 工程 | 二进制不入库，当前装机是哪一版只能靠 MD5 与日志确认；UI 是独立 dll（版本轴不同步）；CPU 分支与 UI 源码不在本仓库部署 | 改 UI 要另建一份 dll |
| 工具坑 | `--collection` 静默打偏、面板单位 0..100、参数须起 OBS 前写、OBS 退出改写集合、收益只认配对、长跑日志轮转、合成人像色度过低 `faces=0` | 逐条展开见 [`guides/guide-verification.md`](guides/guide-verification.md) |

> 原根 `README.md` 的 N 编号有重复（N6 / N7 各出现两次），此处改为按主题分组；
> **旧 N1–N24 → 新章节的完整映射见 [`INDEX.md`](INDEX.md) §4**，避免历史引用断链。
> 已修复项（输入旋转、五官段默认、安装包脱节、dll 版本不校验）已移入 [`CHANGELOG.md`](CHANGELOG.md)。

## 4. 装机与运行（可执行）

```bat
powershell -ExecutionPolicy Bypass -File 02_构建\build_gpu.ps1
powershell -ExecutionPolicy Bypass -File 03_装机\7_install_gpu.ps1
powershell -ExecutionPolicy Bypass -File 07_安装包\build_setup.ps1
py -3.12 05_验证\check_release.py --strict
```

- 装机前**必须关闭 OBS**（脚本会拒绝运行中装机，并自动备份旧 dll）。
- 本机 `python` 是 Microsoft Store 别名，**验证脚本一律用 `py -3.12`**。
- 健康三行判据：`init OK (v3.0.1, engine=gpu/2.0.0, …)`、六行 `xxx.hlsl 已加载`、
  一行 `引擎资源已预热 WxH`；`engine=` 到底代表什么见
  [`reference/reference-engine-id.md`](reference/reference-engine-id.md)。
- 面板 22 项（1 总开关 + 14 滑块 + 7 选项），磨皮默认 **20**、其余 0；
  开关默认值与回退查 [`reference/reference-switches.md`](reference/reference-switches.md)。

## 5. 验证怎么跑

| 线 | 命令 | 管什么 |
|---|---|---|
| 发布闸门 | `py -3.12 05_验证/check_release.py --strict` | 版本单源 / 三处 dll / 四份 shader / 开关表 / Setup.exe 路径·版本串·载荷 |
| 离线引擎 | `py -3.12 06_离线验证/offline_regress.py check` | CPU golden 四例（动引擎接口 / S4 / S7 必跑） |
| 离线 shader | `05_验证/verify_{tone,smooth,region,detail,warp,mask}_compiles.py` | 编译 + 入口签名 |
| 实机 | `verify_s1_correct.py` / `verify_region_samemask.py` / `verify_spot_pixels.py` | 逐像素 / 同掩码 / 祛斑像素量 |
| 性能 | `s0_lock.py` → `gate.py --skip-transfer --cmp` → `ab_pair.py` | 收益只认同会话配对 A/B |

判据口径：**搬运项跨会话不可比**，跨会话只判引擎项 + 健康项（`gskip/tskip` 取最坏窗口）。

## 6. 文档地图

**唯一总表 [`INDEX.md`](INDEX.md)**（含 `PROGRESS §9.x` 与旧 N 编号的统一映射）。固定分类：
`design/` 每模块 1 篇、`guides/` 每场景 1 篇、`reference/` 每主题 1 篇、`adr/` 一事一篇、
`archive/` 隔离存放；主目录每类只留当前版，命名 `类型-主题.md`，禁止 `final_v3_副本` 一类命名。
新增文档只能放规定位置。
