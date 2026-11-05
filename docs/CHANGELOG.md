---
doc_type: CHANGELOG
title: 用户可见变更
version: 0.1.3
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: bb4dfba
maintainer: Jian
status: Active
---

# 用户可见变更

> 只记**影响读者使用的变更**：装到哪、行为变没变、怎么回退。
> 逐轮实测、踩坑经过、内部调参一律在 [`archive/PROGRESS-20261105.md`](archive/PROGRESS-20261105.md)。
> 版本号唯一出处是 `01_源码/beauty_params.h` 的 `BEAUTY_BUILD_STR`。

## 3.0.1（2026-11-05 · 普通升级：代码审查修复）

**不动多实例架构，不新增开关，不改默认参数。** 只修交付链与内存安全：

| 改动 | 对使用者的影响 |
|---|---|
| `Setup.exe` 安装路径改为 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty` | **重要**：OBS 32.x 只从这里加载插件。此前装到 `Program Files\obs-studio\obs-plugins\64bit` 旧布局，装完滤镜根本不加载 |
| 槽位获取失败路径不再留悬空 hook user | 多实例创建失败时不会崩溃 / 误摘别的实例钩子 |
| `beautyCoreReleaseSlot()` 在同一把锁内停 worker 并清空槽状态 | 删滤镜 / 槽复用时无释放竞态 |
| 去掉命令注入面，改为 Win32 递归删除 + `.tmp` + `MoveFileExW` 原子写 | 安装不会留下半截 dll |
| 发布闸门补 3 条断言（安装路径 / Setup.exe 版本串 / RCDATA 载荷） | `--strict` 从 9 项变 12 项，装错目录会被当场拦下 |

**交付状态**：dll `633,856 B / MD5 068E03E5FBE2E21C1883A7F372828783`，构建 / 安装包 / 装机三处逐字节一致；
`Setup.exe 1,533,952 B / MD5 E945A0F86041050C6506A33B26F4A7FC`；
`check_release.py --strict` **12 PASS / 0 WARN / 0 FAIL**；离线回归 4 条 golden 逐字节相同。

**多机位自动化复验**：4 个独立源各挂 1 滤镜 → `slot=0/1/2/3 活跃实例=1/2/3/4`；删一路后其余正常、
槽可复用、全程无崩溃与黑屏。旧 dll 备份 `03_装机/backup/obs-beauty_20261105_204239.dll`（3.0.0）。

> **待人工确认**：用真实 2–4 路摄像头分别调磨皮 / 美白 / 润色清晰并连续观察 A 镜头是否闪烁。
> 自动化只证明上下文隔离与输出正常，画面观感需人眼判断。

## 2026-11-05 修复批次（2.16.1 → 3.0.0 之间，未单独发版）

> **适用条件 / 失效说明（文档治理）**：本节是 7 篇 `修复_*_20261105.md` 的结论合并，
> 记录**当时的修复与决策**，**不是 3.0.1 的变更清单**。原文未给出统一插件版本号
> （个别处写「3.1.65 (core)」「2.17.2bak」），**版本归属待人工确认**；
> 3.0.1 中这些项是否逐条仍按原文生效，亦**待人工确认**。
> 当前行为一律以 §3.0.1 / §3.0.0 与 `01_源码` 为准；原文全部在 `archive/`，正文冻结。

| 主题 | 当时的结论 / 决策 | 对使用者的影响 | 原文 |
|---|---|---|---|
| GPU 纯锐化掩码缺失 | 磨皮 = 0 时 GPU 链掩码为空，「只开锐化」与关滤镜逐字节相同；改法是在磨皮 = 0 且要面部细节时强制引擎画全帧 CPU 掩码 | 锐化从 `0 px` 变为 `36 958 px (4.01%) / max 16`；**视觉上仍偏弱**（该帧脸区 `|hf|` 中位 0.43 灰阶，素材本身糊），不是残留 bug | [`archive/changelog-src-sharpen-mask-20261105.md`](archive/changelog-src-sharpen-mask-20261105.md) |
| P2 值域加权核 | 修掉四处会让 P2 静默失效的错误 + 一条常红门禁；但真实摄像头画面上 99% 像素权重落在 0.75~1.0，整幅平均差仅 0.11 灰阶。**决策（用户定）「选 A：保持关闭」** | `BEAUTY_BILAT` 默认 0 不变，画面逐字节不变，4 条 golden `maxdiff=0`；代码与两趟参数保留，`=1` 仍可复现实验。本轮**没有 GPU 实测**（dll 被 OBS 占用） | [`archive/changelog-src-bilateral-p2-20261105.md`](archive/changelog-src-bilateral-p2-20261105.md) |
| 五官立体白圈 | 根因是羽化语义为「平台半径」而非「软边宽度」；新增椭圆 `dome` 形状开关，并把亮部改成互相重叠的连续光场 | 拉满「五官立体」时额头 / 下巴 / 两颊的四个白圈消失；`blur100 / mixed / sharp` 逐字节不变，含 threed 的 `region` 按预期变化并重采 | [`archive/changelog-src-threed-mach-band-20261105.md`](archive/changelog-src-threed-mach-band-20261105.md) |
| 分割同步路径与 segMix | 分割原先只在 `asyncWorker()` 里调用，`BEAUTY_ASYNC_DETECT=0` 同步路径完全不跑；且没有第一份结果时 `segMix>0` 会把整张掩码乘成 0。抽成 `runSegIfDue()` 两条路径共用，宿主侧按有无结果门控 | 默认路径不受影响（4 条 golden 逐字节相同）；`BEAUTY_SEG=1` 的模型未加载 / 首帧结果未回时，美颜不再整段消失 | [`archive/changelog-src-seg-sync-path-20261105.md`](archive/changelog-src-seg-sync-path-20261105.md) |
| 掩码椭圆双重旋转 | 歪头（`|θ|>2°`）时椭圆半轴与像素迭代 AABB 被旋转两次；把脸自身轴宽高、椭圆半轴、AABB 三组量分开 | 歪头时美白范围不再铺开；15° 倾斜帧覆盖率 7.82% → 7.47%，确认是有意变化后重采 4 条基准。顺带修好离线回归台编译 | [`archive/changelog-src-mask-double-rotation-20261105.md`](archive/changelog-src-mask-double-rotation-20261105.md) |
| 祛斑与下巴 | 祛斑痘 `k=0.25 → 0.75`（用户定方案 A）；下巴从「纵向压缩」改「中线拉长 + 两侧内收」（方案 C），幅度 10%/14% → 18%/20%，四处同步改动 | 面板 100 档的祛斑痘与下巴不再是「观感等于没有」。已知局限：侧脸时横向内收左右不对称、`chinSpan` 依赖 YuNet 框底 | [`archive/changelog-src-spot-chin-20261105.md`](archive/changelog-src-spot-chin-20261105.md) |
| 锐化阈值被覆盖 | 三种互斥模式的判定被无条件覆盖，`pureSharpen` 时细频阈值拿不到 `faceSharpThrFine`；改成一次判定 | 锐化（脸内）满档从「几乎无效果」变为数值上生效；**仍偏弱是素材本身糊**，要更扎眼需加 `BEAUTY_FACE_SHARP_GAIN`，代价是噪点同步放大 | [`archive/changelog-src-sharpen-threshold-20261105.md`](archive/changelog-src-sharpen-threshold-20261105.md) |

**同时形成的决策记录**（原文归档，结论见对应 ADR）：

- 人脸分割不用于解决「手遮脸溢出」 → [`adr/adr-seg-not-for-occlusion.md`](adr/adr-seg-not-for-occlusion.md)；
  原文 [`archive/conclusion-seg-occlusion-20261105.md`](archive/conclusion-seg-occlusion-20261105.md)。
  结论基于**合成素材**，真实手遮脸素材仍待提供。
- CPU 与 GPU 掩码共用同一份 `faceGeom` 的工程约束 →
  [`adr/adr-cpu-gpu-same-mask.md`](adr/adr-cpu-gpu-same-mask.md)（**归纳、待人工确认**）。

## 3.0.0（2026-11-05 · 大版本更新：多实例隔离）

**修双机位串扰**：A 镜头人物不停闪烁、侧机位磨皮 / 美白 / 润色清晰全无效果。

- **根因**：`beautyEngine()` 是进程级单例，帧级状态全是文件级全局；多个 `obs_beauty_filter`
  实例交替写同一份上下文。
- **改法**：每个滤镜实例独占一个引擎槽 —— `SlotState` 按槽存帧号 / 掩码轨迹 / 肤色 EMA /
  hook user 与诊断计数，引擎按槽惰性实例化，异步 worker 绑定创建它的槽。
- **行为边界**：单实例 `slot=0` 行为不变；上限 **8 个实例/进程**，每多一实例多加载一份 YuNet 模型
  并多占一份帧环内存；`BEAUTY_SEG=1`（分割影子层）仍是进程级全局，**只支持单实例**，默认关。
- **回退**：无开关，回退 = 还原 2.16.1 代码。

**历史交付快照**：dll `631,808 B / MD5 06E275360025169B66B26C09C70528F4`；
`Setup.exe 1,532,416 B / MD5 FC0BA21D3D62BB5DB1EE8E01513CB565`；`--strict` 9 PASS。
> 该版 Setup.exe 装到 OBS 32.x 不加载的旧目录且闸门不查此项 —— 由 3.0.1 修复，不要照此版部署。

## 2.16.1（2026-11-04 · R1 严格字面回档）

用户实测 2.16.2–2.17.3 的「手遮眼 → 美白遮罩叠到手上」修复链**整体无效**，撤回功能基线：

- 撤回：`FaceParser` / `HandDetect` / 21 点手形凸包抠除 / 手形缓存 / CPU 掩码落盘增量 /
  方案 B `faceRoll` 重写 / 方案 A 两个手部模型打包；相关全局状态恢复 2.16.1 形态。
- **唯一保留的后续增量**：2.16.9「检测选项只在变化时置脏」通用掉帧修复（与遮挡无关，删掉会退回每帧重下发）。
- 全量备份 `_回档备份_20261104_2249`（18 文件快照）：其中 **4 篇文档已归档到 [`archive/backup-20261104_2249/`](archive/backup-20261104_2249/)**，逐篇映射见 [`INDEX.md`](INDEX.md) §2.2；其余 14 个源码 / dll / `Setup.exe` 仍在原目录、未移动、未入库；完整还原 18 文件时需把 4 篇归档件复制回原目录对应位置。
- 实验数据与失败结论**不随代码删除**，仍在归档总账 `§9.52.x`。

回档后实测：dll `600,064 B / MD5 7D83E51D230C3076402BDAB7781787EB`；
发布闸门 9 PASS；四条黄金基准 `maxdiff=0`。

## 更早里程碑（压缩，只留影响使用的）

| 版本 | 用户可见变化 | 回退 |
|---|---|---|
| 2.15.0 | 掩码跟脸转（`MASK_ROLL` 默认开）、默认掩码形状 0→3；检不到脸时自动退回 0° 并打 WARN | `BEAUTY_MASK_ROLL=0` + `BEAUTY_MASK_SHAPE=0` 回到 2.14.1 逐字节行为 |
| 2.14.1 | 内部清理（去两趟 Copy，draw 23→21），**画面逐位不变** | 无开关，回退 = revert |
| 2.14.0 | **默认值变化**：`BEAUTY_GPU_REGION` 由「默认关、认 `=1`」翻成「默认开、只认 `=0`」 | `BEAUTY_GPU_REGION=0`，不必换 dll |
| 2.13.1 | 修 N1：输入旋转提到 `letterboxRgb` 之前，全帧回退路径也生效 | — |
| 2.12.0 | 新增 opt-in `BEAUTY_DET_ADAPT_AGG=rms`（**默认仍 max，不翻**） | 不设即默认 |
| 2.11.0 | 新增 opt-in 方案C（`BEAUTY_MOTION_GPU=1`），默认关 | 不设即逐字节回到 2.10.x |
| 2.10.0 | 面板新增「输入方向 0/90/180/270」；2.15.2 起面板项删除，只剩环境变量 | — |

> 逐轮实测数字、判据与踩坑经过按 `§9.x` 在归档总账检索，映射见 [`INDEX.md`](INDEX.md) §3。

---

> **已合并自**（2026-11-05，文档治理批 4）：
> `04_文档/修复_GPU纯锐化掩码缺失_20261105.md` → [`archive/changelog-src-sharpen-mask-20261105.md`](archive/changelog-src-sharpen-mask-20261105.md)；
> `04_文档/修复_P2值域加权核_20261105.md` → [`archive/changelog-src-bilateral-p2-20261105.md`](archive/changelog-src-bilateral-p2-20261105.md)；
> `04_文档/修复_五官立体白圈_20261105.md` → [`archive/changelog-src-threed-mach-band-20261105.md`](archive/changelog-src-threed-mach-band-20261105.md)；
> `04_文档/修复_分割同步路径与segMix地雷_20261105.md` → [`archive/changelog-src-seg-sync-path-20261105.md`](archive/changelog-src-seg-sync-path-20261105.md)；
> `04_文档/修复_掩码椭圆双重旋转_20261105.md` → [`archive/changelog-src-mask-double-rotation-20261105.md`](archive/changelog-src-mask-double-rotation-20261105.md)；
> `04_文档/修复_祛斑与下巴_20261105.md` → [`archive/changelog-src-spot-chin-20261105.md`](archive/changelog-src-spot-chin-20261105.md)；
> `04_文档/修复_锐化阈值被覆盖_20261105.md` → [`archive/changelog-src-sharpen-threshold-20261105.md`](archive/changelog-src-sharpen-threshold-20261105.md)；
> `04_文档/结论_分割对手遮脸无效_20261105.md` → [`archive/conclusion-seg-occlusion-20261105.md`](archive/conclusion-seg-occlusion-20261105.md)
> （结论改写为 [`adr/adr-seg-not-for-occlusion.md`](adr/adr-seg-not-for-occlusion.md)）。
> 原文正文冻结，不作为当前结论依据；逐篇归档原因见各文件废弃头。
