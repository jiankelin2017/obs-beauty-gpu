> **已归档（2026-11-05）**：本文件是验收过程日志的历史快照，内容冻结，**正文一字未改**，不作为当前结论依据。
> 当前入口见 [`docs/README.md`](../README.md)，全库总表见 [`docs/INDEX.md`](../INDEX.md)；
> 结论已转录到 [`docs/guides/guide-verification.md`](../guides/guide-verification.md) 与 [`docs/CHANGELOG.md`](../CHANGELOG.md)。
> **原路径** `05_验证/out/logs/P0-7_samemask_20261103.md` 已失效；`out/logs/` 仍是脚本输出目录，新报告**不入文档体系**。
> 旧名 → 新名映射见 [`docs/INDEX.md`](../INDEX.md) §2。
> 正文的版本号、性能数字与判据是 2.8.x–2.10.0 当时快照，**不代表 3.0.1 实测**；当前行为以 [`docs/README.md`](../README.md) 与 `01_源码` 为准。

---

# P0-7 定案：同一份掩码下，五官 GPU 与 CPU 算法一致（2.8.3）

2026-11-03 20:05–20:15。结论先说，数据在后。

## 结论

**① P0-7 结案：不是算法缺陷，是验收工具的取样条件不成立。**
把「引擎的 CPU 掩码」换成「GPU 本帧刚画好的掩码读回」，在同一帧上跑两遍算法：
720p / 4K 各 5 个样本，**maxdiff 全 ≤1**（差异像素 2~3 个，占 0.0003%）。
2.8.2 时那个 `maxdiff=8 / 1.56%` 是**两份不同掩码 + 两次独立启动**量出来的，
不是 GPU 与 CPU 的算法差。

**② 原来的自检为什么几乎不触发。** 它第 5 行写着 `if (!job.mask) return;`，而 S7 之后
绝大多数帧是 geomOnly（引擎只给几何、不给 CPU 掩码）。真机上三次采集只碰巧中一次，
所以 §9.19.3 那个 `maxdiff=0` 只有 1 个样本、还是 1% 强度下跑的。

**③ 顺带查出一个比 P0-7 严重的问题**（新记 P0-8）：祛斑痘在 geomOnly 帧整族不执行。**
几何快照里 `spot=0`，把 `BEAUTY_MASK_GEOM_ONLY=0` 关掉就变 `spot=1`。
根因是 `bounds_` 在 geomOnly 帧被清空（`beauty_core.cpp:3025`），而祛斑的开关条件
正是 `!bounds_.empty()`（`3942` / `3953`）—— GPU 与 CPU 两条路径同时被挡住。
默认配置下几乎每帧都是 geomOnly，等于**祛斑一直没生效**。

**④ `BEAUTY_GPU_REGION` 仍 opt-in**：结论不变，但理由再改一次 —— 从「无可靠判据」
改为「判据已具备（本工具），但覆盖面不足（多脸/祛斑未覆盖），且未做长跑」。
翻默认需要单独立项，不在本轮。

---

## 1. 做法：把「同一份掩码」这一条补上

2.8.3 改的是自检的**取样来源**，不是算法：

```c
// 旧（≤2.8.2）：没有 CPU 掩码就直接放弃 —— geomOnly 帧永远不走
if (!in_tex || !job.mask || job.w <= 0 || job.h <= 0) return;

// 新（2.8.3）：
if (!mask) {                       // geomOnly 帧
    if (!f->mask_skin_use) return;
    beauty_mask_readback8(f, f->mask_skin_use, mask_gpu.data(), cx, cy);
    mask = mask_gpu.data();        // CPU 基准也用 GPU 那份掩码
    mask_src = "GPU";
}
```

`beauty_mask_readback8()` 读 `mask_rt_a` 的 **R 通道**（`mask.hlsl` 把权重写在这里）。
采样口径与 shader 一致：`region.hlsl` 的 `skinMaskTex` 是 `Filter = Point`、uv 在像素
中心，所以读回的字节就是 shader 眼里那个值（UNORM8 → /255，与 CPU 的 `mrow[x]/255` 同）。

另外三处小改（都是为了让结论可信，不是为了让结论好看）：

| 改动 | 为什么 |
|---|---|
| 掩码覆盖率统计（非零像素%、均值） | 空掩码下两条算法都不碰像素、`maxdiff` 恒 0 —— 最典型的假通过（§9.18 的 `--nospot` 就是这么废的）。覆盖率 0 的帧直接不算样本 |
| `BEAUTY_REGION_CMP_N`（默认 1，上限 20） | 原来一个进程只出 1 个样本，样本量不足以说「稳定」 |
| `BEAUTY_REGION_CMP_DELAY`（可对拍帧数） | OBS 刚起来那几帧掩码/几何没稳，取样太早量的是启动态 |

## 2. 数据

命令：`py -3 05_验证/verify_region_samemask.py --canvas WxH --samples 5 --delay 120`

| 画布 | 样本 | 掩码来源 | maxdiff | 差异像素 | bbox | 掩码覆盖 |
|---|---|---|---|---|---|---|
| 1280×720 | 5/5 | GPU（5/5） | 1 | 3（0.0003%） | (655,399)-(792,497) | 12.8% / 均值 27.9 |
| 3840×2160 | 5/5 | GPU（5/5） | 1 | 2（0.0000%） | (1821,1237)-(2295,1349) | 12.7% / 均值 27.7 |

- 判据是 `maxdiff ≤ 1`：`applyEllipses` 里 CPU 用 `lroundf`、shader 用 `floor(x+0.5)`，
  smoothstep 与 `sqrt` 的浮点路径不同，边界上差 1 个灰阶是预期内的（不是"接近通过"，
  是同一算法的两种实现能给出的最小值）。
- 差异位置 5 个样本完全一致（bbox 相同），说明是**确定性的边界舍入**，不是随机噪声。
- `els=6+2+2`：主椭圆 6（立体 + 亮眼 + 眼袋）、法令纹 2、下瓣 2，三族都真跑到了。

## 3. 与 2.8.2 的 8 / 1.56% 怎么对上

| 项 | 2.8.2 截图对拍 | 2.8.3 同帧自检 |
|---|---|---|
| 掩码 | GPU 掩码 vs **CPU 残留掩码**（geomOnly 帧 `mask_` 是上一帧的） | **同一份**（GPU 那份，读回当基准） |
| 帧 | 两次独立启动各抓一张 | 同一帧 |
| 参数 | 50/50/50/40 | 50/50/50/40（写进集合，起 OBS 前生效） |
| 结果 | maxdiff=8 / 1.56% | maxdiff=1 / 0.0003% |

两次独立启动 + 两份不同掩码，量到 1.56% 的像素差是**必然**的：掩码本身就不是同一张。

## 4. 新发现 P0-8：祛斑痘在 geomOnly 帧不执行

### 证据

| 配置 | 几何快照里的 `spot` |
|---|---|
| 默认（`geomOnly` 生效） | **0**（`k=0.00 r=0 thr=[-0.0,0.0]`） |
| `BEAUTY_MASK_GEOM_ONLY=0` | **1**（`k=0.10 r=2 thr=[1.4,30.0]`） |

面板 `delspot_level=40` 两轮相同，链上椭圆数同为 0（只开祛斑），唯一变量是 geomOnly。

### 成因

```
beauty_core.cpp:2704   int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
beauty_core.cpp:2718   bx1 只在 paintSkin() 里更新（落笔时统计包围盒）
beauty_core.cpp:2816   if (geomOnly) continue;   ← 跳过全部逐像素落笔
beauty_core.cpp:3025   if (bx1 < 0) boundsOut = RoiRect();   ← geomOnly 帧恒走这条
beauty_core.cpp:3942   if (geoOut && spot > 0 && !bounds_.empty()) geoOut->spotOn = 1;
beauty_core.cpp:3953   if (dst && spot > 0 && !bounds_.empty()) applySpotFix(...);
```

即：geomOnly 帧不画掩码 → 包围盒算不出来 → `bounds_` 被清空 → 祛斑的**两条路径**
（GPU 段用 `geoOut->spotOn`、CPU 段用同一个条件）都不执行。

### 影响面

- 默认配置下 `gonly` 恒 176~180/180（S4b 缩略图检测使 `mapped=true`），所以
  **祛斑痘自 2.6.0（S4a）/ 2.8.0（S7）起基本没生效**，而不是"偶发失效"。
- 其它四项（立体/亮眼/眼袋/法令纹）不受影响：它们的几何来自 `faces_`，与 `bounds_` 无关。
- 不开祛斑的用户（默认面板值 0）无感。

### 建议修法（未实施，等定方向）

`if (bx1 < 0)` 分支在 geomOnly 帧**不清空** `boundsOut`，保留上一次真正画过掩码的包围盒：

```c
if (bx1 < 0) {
    if (!geomOnly) boundsOut = beautycore::RoiRect();   // 真空掩码才清
    // geomOnly：沿用上一份（与 mask_ 的"残留"语义一致，最多落后 8 帧）
}
```

理由：geomOnly 帧的 `mask_` 本来就是上一份，`bounds_` 跟着它走才自洽；祛斑 ROI 的
滞后量与掩码滞后量相同（≤ `BEAUTY_DET_ADAPT_STILL` 帧）。**这是行为变更**（祛斑会
开始生效），按表 D 属于「行为默认变化」，要 +0.1 位并重固化基线，所以没有擅自改。

## 5. 工具与三个坑

```
py -3 05_验证/verify_region_samemask.py                       # 4K 默认 5 样本
py -3 05_验证/verify_region_samemask.py --samples 5 --delay 120
py -3 05_验证/verify_region_samemask.py --only spot           # 只开一族
py -3 05_验证/verify_region_samemask.py --gpu-region 0        # 生产路径走 CPU 五官
```

1. **参数必须在起 OBS 之前写进集合文件**。自检是「第一个满足条件的帧就取样」，而
   OBS 从启动到 WebSocket 可用要 2~4 s。第一版用 `apply_load` 下发，等参数到达样本
   早取完了：`--only spot` 明明把另外三项置 0，日志里却是 `ellipses=6+2+2`。
   脚本现在改为「改集合 JSON → 起 OBS → 跑完原样写回」。
2. **`--zero` 拿不到样本**，不是工具坏了：五官参数全零时 `applyFaceRegions()` 在
   `beauty_core.cpp:3619` 直接返回，几何快照 `faceCount=0`，自检按设计不跑。
3. **`一次对拍 = 3 次全帧回读 + 一遍 CPU 五官`**，4K 下那一帧会明显变长。只在
   `BEAUTY_REGION_CMP=1` 时发生，默认路径零开销。

## 6. 复现

```
py -3 05_验证/verify_region_samemask.py --canvas 3840x2160 --samples 5 --delay 120
BEAUTY_REGION_GEOM=1 py -3 05_验证/verify_region_samemask.py --only spot --samples 1
BEAUTY_REGION_GEOM=1 BEAUTY_MASK_GEOM_ONLY=0 py -3 05_验证/verify_region_samemask.py --only spot --samples 1
```

装机体：2.8.3，MD5 `EE99F05013B85EA23E0331C36D55B7F0`，570368 B。
回滚点：`03_装机/backup/obs-beauty_20261103_200625.dll`（2.8.2）。
