---
doc_type: design
title: 方案 D：遮挡感知皮肤掩码（复刻思路 + 实现逻辑）
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 04_文档/方案D_遮挡感知掩码设计.md
---

# 方案 D：遮挡感知皮肤掩码（复刻思路 + 实现逻辑）

> **Draft · 未接线，不可作为当前实现依据**：本文是遮挡感知皮肤掩码的**设计稿**，
> 正文描述的是拟议实现，与 3.0.1 的实际代码不对应；当前实现一律以
> [`../README.md`](../README.md) 与 `01_源码` 为准。
> **结论**：提出用分割 / 遮挡信息改造皮肤掩码；截至 2026-11-05 **未接线、未验收**。
> **适用条件**：仅 GPU 版设计稿；CPU 版冻结。
> **失效说明**：正文引用的 SDK 逆向材料、素材路径与旧文档名均为写作当时路径，
> 按 [`../INDEX.md`](../INDEX.md) §3 / §4 映射检索。

> 版本：D-0.1（设计稿，未接线）
> 依据：obs-beauty v2.16.1 反编译结论 + 本机源码 `01_源码` + 实测素材
> `05_验证/out/live_edge/2026-11-05 05-45-51.mp4`（1080×1920 / 30fps / 67s）
> 前置文档：[`design-mask-gpu.md`](design-mask-gpu.md)、[`../reference/reference-switches.md`](../reference/reference-switches.md)

> **范围锁定（2026-11-05）**：本方案及此后所有优化**只作用于 GPU 版**——产物 `obs-beauty.dll`，
> 源码 `D:\插件\OBS美颜GPU版\01_源码`，着色器 `01_源码/shaders/*.hlsl`（mask/tone/smooth/detail/region/warp 六个）。
> CPU 版 `D:\插件\OBS美颜源码` **冻结**，待 GPU 正式版发布后再回头同步。
> 验收口径因此改为"GPU 渲染路径上逐帧生效"，**不以 CPU 参考实现的一致性作为通过条件**；
> 但"无遮挡时逐字节不变"这条不变量仍必须遵守（见 §不变量）。

---

## 0. 结论摘要

1. **`obsplus.dll` 与本问题无关。** 它是更新器兼加载器，不含任何像素处理。参考对象是 `obs-beauty`（也就是本项目自身）。
2. **obs-beauty 没有"遮挡"概念，只有"脸存在性"一个状态位。** 手遮脸时检测器照常输出人脸，`0xa19` 恒为 1，"holding mask"分支永不进入。直接抄它的策略解决不了本问题。
3. **本文实测否掉了两条常见判据**：掩码覆盖率 ρ 在双手遮脸段反而是全程最低（0.678 vs 无遮挡 0.72–0.78）；高频纹理在尺度归一后完全不可分（安静帧 0.0082 低于遮挡帧 0.0115）。**不要把触发器建在这两者上。**
4. **颜色路在本素材上数学不可分**：手背与脸颊的 (Cb,Cr) 距离只有 ≈0.16 个肤色椭圆半径。这意味着任何"收紧肤色椭圆"的做法都无效——这也是你试过的语义/换模型方案实测无效的根本原因。
5. **可行的只有三样**：几何自洽性（实测 aspect 0.94→1.40、roll 单帧跳 15°）、**局部相对亮度指纹**（手比脸亮 5–8 个 L 单位，且这个偏移持续十几秒）、**状态型持续性**（双阈值 + 最短保持 + 硬上限）。
6. 架构上复刻 obs-beauty 的四条结构（检测降频、状态位+计数+硬上限、确定性兜底、增益只乘掩码层），**不抄**它的"复用旧掩码"与"全帧 255 兜底"。

---

## 1. 从 obs-beauty 复刻什么 / 不抄什么

| # | 结构 | 位置 | 抄 | 理由 |
|---|---|---|---|---|
| 1 | 检测降频（每 4 帧跑一次推理，中间帧复用几何） | `param_1+0xa34`，`decomp_pass2.c:1461` | ✅ | 抑制关键点抖动最便宜的一招，直接减少污染源 |
| 2 | 状态位 + 计数器 + 硬上限 | `0xa19` / `0x1b48`，`FUN_18000e050` | ✅ | "状态型"正是你要求的：能持续保持，不靠单帧事件 |
| 3 | 无掩码时确定性兜底（不崩、不猜） | `FUN_18002c310` | ✅ | 任何降级路径都必须给确定的输出 |
| 4 | 增益只乘在掩码层，tone 通道不加分支 | `FUN_180039e70` 全文无人脸判定 | ✅ | 单一真相：改一处，磨皮/色调/细节/五官自动同步 |
| 5 | 丢失后"复用上一帧掩码" | `FUN_18000e050` 返回 1 | ❌ | 你的场景检测不丢失，这条永不触发；真触发时复用旧掩码会把旧污染一起留着 |
| 6 | 无掩码 → 整帧 255（全脸美白） | `memset(0xFF)` | ❌ | 效果优先的取向，与本项目"不牺牲正常脸"的取向相反 |
| 7 | 6 次上限后二选一（全零 / 跳过滤镜） | `BEAUTY_NOFACE_SKIP` | ❌ | 二值跳变必然闪；要的是连续增益场 |

**一句话**：抄骨架（降频、状态机、硬上限、单一施加点），换内脏（判据不能用"脸在不在"，要用"这块还像不像我记住的那张脸"）。

---

## 2. 实测证据（决定了判据取舍）

素材分段（人工标注 + 逐帧指标对齐）：

| 时段 | 内容 |
|---|---|
| 0–16s | 趴桌，双手遮脸 |
| 16–24s | 抬头，手在脸侧 |
| 24–36s | 单手遮右眼 |
| 36–48s | 低头 |
| 48–67s | 低头作业，手离开 |

### 2.1 颜色不可分（关键否证）

26s 单手遮眼，LAB 均值：

| 区域 | L | a | b | 饱和度 |
|---|---|---|---|---|
| 左脸颊 | 74.8 | +11.8 | +14.9 | 27.2% |
| 左额 | 72.5 | +13.3 | +20.2 | 34.1% |
| 手掌（遮眼） | 83.2 | +8.3 | +15.8 | 24.1% |
| 手背 / 指节 | 79.9 | +10.7 | +16.2 | 26.6% |

手背 vs 脸颊：ΔL=5.1、Δa=1.1、Δb=1.3。把 Δa/Δb 折算到 CbCr 平面，距椭圆中心的变化约 **0.16 个半径**（Ru=0.075）。而 `skinWeight()` 的判定带是 `d ∈ [0.6, 1.3]`——手稳稳落在 0.3 以内。

**结论**：收紧椭圆到 `d<0.6` 也挡不住；`d<0.2` 会把脸砍掉一半。**颜色路到此为止。**

区分度只在**亮度**：手比脸亮 5–8 个 L 单位，而且是"整块平"的亮，不是脸的起伏式亮。

### 2.2 掩码覆盖率 ρ 不可用

| 时段 | ρ | aspect | \|roll\| | 高频 |
|---|---|---|---|---|
| 0–16s 双手遮脸 | **0.678** | 0.91 | 4.9° | 0.0073 |
| 16–24s 手在脸侧 | 0.871 | 0.81 | 9.7° | 0.0079 |
| 24–36s 单手遮眼 | **0.917** | 0.54 | 3.4° | 0.0084 |
| 36–48s 低头 | 0.760 | 0.93 | 15.9° | 0.0064 |
| 48–67s 手离开 | 0.742 | 0.96 | 17.0° | 0.0076 |

双手遮脸时 ρ 最低——因为手背偏暗、过不了亮度下门控 `smoothStep(0.12,0.30,y)`，掩码反而缩水。**ρ 只能单向使用**：`ρ` 显著高于基线可作为"掩码外溢"的辅证，但 ρ 低绝不代表干净。

### 2.3 高频纹理不可用（尺度归一后）

同一批帧，高斯差分（σ = k·脸宽）在脸椭圆内的均值：

| 帧 | 脸宽 | k=0.006 | k=0.010 | k=0.016 |
|---|---|---|---|---|
| 1s 双手遮脸 | 120 | 0.00946 | 0.01182 | 0.01937 |
| 10s 双手遮脸 | 246 | 0.00540 | 0.01044 | 0.01788 |
| 26s 单手遮眼 | 125 | 0.00911 | 0.01151 | 0.01737 |
| 40s 干净 | 125 | 0.00646 | 0.00818 | 0.01257 |
| 45s 干净 | 125 | 0.00813 | 0.01040 | 0.01630 |

10 Mbps 压缩 + 相机机内降噪把纹理全部抹平，**干净帧的低频能量甚至低于遮挡帧**。上一轮"手背高频比脸低一半"的观察是同帧内不同区域比较，被尺度效应污染，作为全局判据不成立。

### 2.4 几何自洽性成立

| 帧 | 框宽×高 | aspect | roll | 眼距/框宽 | 眼距/框高 |
|---|---|---|---|---|---|
| 26s 单手遮眼 | 125×175 | **1.40** | −2.5° | 0.371 | **0.265** |
| 30s | 126×177 | 1.40 | −2.4° | 0.364 | 0.259 |
| 34s | 134×187 | 1.40 | −1.9° | 0.392 | 0.281 |
| 40s 干净 | 125×117 | **0.94** | +11.3° | 0.417 | **0.444** |
| 45s 干净 | 125×124 | 0.99 | +13.4° | 0.425 | 0.429 |
| 55s 干净 | 126×124 | 0.98 | +9.4° | 0.427 | 0.436 |

手压上额头 → 检测框向上膨胀 45%（163→175 高），aspect 0.94→1.40。同时时间序列上 20s→22s 滚转角从 +11.5° 跳到 −3.6°，单帧 15°。

这两条都是**不依赖颜色、不依赖纹理**的纯几何信号，且在"手持续十几秒"期间**一直异常**——天然满足状态型要求。

---

## 3. 总体架构

```
                        ┌──────────────── 每帧一次 ────────────────┐
   输入帧 src ──────────▶│ ① 观测 observeCells()                    │
   (+ faces 几何)        │    8×8 格 × 4×4 点，固定 9216 次采样     │
                        │    输出 per-cell (Y, Cb, Cr) + 全脸中位  │
                        └───────────────┬──────────────────────────┘
                                        ▼
                        ┌──────────────────────────────────────────┐
                        │ ② 参考 trackReference()                   │
                        │    per-cell 相对亮度指纹 r0 = Y_cell−Y_med│
                        │    慢 EMA(τ≈8s)，只在 CLEAN 态更新        │
                        └───────────────┬──────────────────────────┘
                                        ▼
              几何自洽 ─────────▶┌──────────────────────────────────┐
              (aspect/roll/眼距)  │ ③ 决策 decideOcclusion()          │
              ρ 高侧辅证 ────────▶│    证据 → 评分 S → 四态机         │
                                  │    CLEAN / SUSPECT / OCCL / COOL  │
                                  └───────────────┬──────────────────┘
                                                  ▼ per-cell target gain
                                  ┌──────────────────────────────────┐
                                  │ ④ 施加 applyGain()                │
                                  │    3×3 两次盒滤波（空间）          │
                                  │    g += (t−g)(1−e^(−dt/τ))（时间） │
                                  └───────────────┬──────────────────┘
                                                  ▼
                               mask 生成处相乘（CPU paintSkin / GPU PS_Skin）
                                                  ▼
                        磨皮 / 色调 / 细节 / 五官 全部自动跟随（零改动）
```

**核心不变式**：增益只在"掩码生成"这一个点相乘。下游 `smooth.hlsl`、`tone.hlsl`、`detail.hlsl`、`region.hlsl` 全都只读 `skinMask` 纹理，一行都不用改——这正是 obs-beauty 第 4 条结构（`FUN_180039e70` 里没有任何人脸判定）的价值。

---

## 4. 数据结构

新增到 `beauty_core.h`（放在 `BeautyMaskFace` 附近，第 534 行前后）：

```cpp
// ---- 方案 D：遮挡感知（D-0.1）---------------------------------------------
//
// 为什么不用全局标量而用 8×8 格：手通常只遮一侧（单手遮眼、托腮）。
// 整脸淡出会把好的一半脸也一起丢掉，违背"不牺牲正常脸"的约束。
// 8×8 已经足够定位"手在哪一侧"，代价是 64 个 float 的状态。
enum {
    BEAUTY_OCC_GRID = 8,          // 8×8 格
    BEAUTY_OCC_SAMPLES = 4,       // 每格 4×4 采样点
};

struct BeautyOccGain {
    int   state;                  // 0=CLEAN 1=SUSPECT 2=OCCLUDED 3=COOLING
    int   holdFrames;             // 当前状态已持续帧数
    float score;                  // 本帧证据合成分 0..1（日志用）
    float geoScore;               // 几何分量
    float fpScore;                // 指纹分量
    // 8×8 的最终增益，行优先；1.0 = 全效，0.0 = 该格完全跳过
    float g[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID];
};
```

`BeautyMaskDiag` 追加（第 562 行结构体尾部）：

```cpp
    BeautyOccGain occ;            // 方案 D：宿主据此上传 GPU 增益纹理 + 打日志
```

状态（文件作用域，与 `g_maskTrack` 同风格）：

```cpp
// 每张脸一份（GPU 掩码链只支持单脸，多脸时只用 idx 0）
struct OccTrack {
    // 参考指纹：per-cell 相对亮度（Y_cell − Y_face_median）
    float r0[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID];
    float r0Var[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID];   // 参考自身的方差，做自适应容差
    float aspect0;      // 参考宽高比
    float eyeW0;        // 参考 眼距/框宽
    float roll0;        // 参考滚转（度）
    bool  refValid;
    int   refAge;       // 参考已连续多少帧没更新（超时强制重学）
    // 时序
    float g[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID];
    int   state, holdFrames;
    int   enterCnt, exitCnt;
    double lastMs;
};
OccTrack g_occ[BEAUTY_WARP_MAX_FACES];
```

---

## 5. 观测层：固定预算网格统计

### 5.1 为什么是"固定预算"而不是"扫全脸"

现有 `buildMaskInto` 的椭圆分支是全像素遍历。若在同一个循环里累加格统计，`geomOnly=1` 的帧（本机日志常见）根本不进那个循环——观测会断帧。所以观测必须**独立成趟**，并且成本与分辨率脱钩：`8×8 格 × 4×4 点 × 3×3 邻域 = 9216 次读取`，与脸多大无关，实测应在 0.05 ms 量级（`src` 在 mapped 模式下还是 640 缩略图）。

### 5.2 网格坐标系

用**脸局部系**（与 `mask.hlsl` 的 `u/v` 同式），这样转头、歪头时格子始终对着同一块解剖区域：

```
u = dx·rc + dy·rs      dx = x − cx
v = −dx·rs + dy·rc     dy = y − cy
格子索引： gi = clamp((u/a)·0.5 + 0.5, 0, 1) · 8
          gj = clamp((v/b)·0.5 + 0.5, 0, 1) · 8
采样点：   u_k = a·(−0.75 + 0.5·k)   k = 0..3    （格中心 ± 3/4 格宽）
```

### 5.3 代码

```cpp
// 方案 D：观测一趟。返回 false = 本帧几何不可用（框太小 / 越界），跳过决策。
//
// 只读 src，不写任何掩码缓冲；不依赖 buildMaskInto 的落笔循环，
// 所以 geomOnly 帧照常工作。
static bool observeCells(const unsigned char* src, int w, int h,
                         float cx, float cy, float a, float b,
                         float rc, float rs,
                         float* cellY, float* cellCb, float* cellCr,
                         float* faceMedY) {
    if (!src || a < 8.0f || b < 8.0f) return false;
    float accY[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID] = {0};
    float accB[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID] = {0};
    float accR[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID] = {0};
    int   cnt [BEAUTY_OCC_GRID * BEAUTY_OCC_GRID] = {0};

    const float cellU = 2.0f * a / BEAUTY_OCC_GRID;   // 一格在局部轴上的宽
    const float cellV = 2.0f * b / BEAUTY_OCC_GRID;
    const float halfU = cellU * 0.375f;              // 只取格内 3/4，避开格边界混样
    const float halfV = cellV * 0.375f;

    for (int gj = 0; gj < BEAUTY_OCC_GRID; gj++) {
        for (int gi = 0; gi < BEAUTY_OCC_GRID; gi++) {
            const float u0 = -a + cellU * (gi + 0.5f);
            const float v0 = -b + cellV * (gj + 0.5f);
            float sY = 0, sB = 0, sR = 0;
            int n = 0;
            for (int sj = 0; sj < BEAUTY_OCC_SAMPLES; sj++) {
                for (int si = 0; si < BEAUTY_OCC_SAMPLES; si++) {
                    const float t1 = (BEAUTY_OCC_SAMPLES == 1) ? 0.0f
                                    : (-1.0f + 2.0f * si / (BEAUTY_OCC_SAMPLES - 1));
                    const float t2 = (BEAUTY_OCC_SAMPLES == 1) ? 0.0f
                                    : (-1.0f + 2.0f * sj / (BEAUTY_OCC_SAMPLES - 1));
                    const float u = u0 + t1 * halfU;
                    const float v = v0 + t2 * halfV;
                    // 局部 → 画面（R^T）
                    const float xf = cx + u * rc - v * rs;
                    const float yf = cy + u * rs + v * rc;
                    const int x = (int)xf, y = (int)yf;
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    const unsigned char* p = src + ((size_t)y * w + x) * 4;
                    const float r = p[0] * (1.0f / 255.0f);
                    const float g = p[1] * (1.0f / 255.0f);
                    const float bl = p[2] * (1.0f / 255.0f);
                    sY += 0.299f * r + 0.587f * g + 0.114f * bl;
                    sB += -0.168736f * r - 0.331264f * g + 0.5f * bl;
                    sR += 0.5f * r - 0.418688f * g - 0.081312f * bl;
                    n++;
                }
            }
            const int k = gj * BEAUTY_OCC_GRID + gi;
            if (n) {
                const float inv = 1.0f / n;
                accY[k] = sY * inv; accB[k] = sB * inv; accR[k] = sR * inv;
                cnt[k] = n;
            }
        }
    }
    // 全脸亮度中位数：用直方图近似（64 格，直接排序也不贵）
    float med[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID];
    int m = 0;
    for (int k = 0; k < BEAUTY_OCC_GRID * BEAUTY_OCC_GRID; k++)
        if (cnt[k]) med[m++] = accY[k];
    if (m < 16) return false;
    std::nth_element(med, med + m / 2, med + m);
    *faceMedY = med[m / 2];

    for (int k = 0; k < BEAUTY_OCC_GRID * BEAUTY_OCC_GRID; k++) {
        cellY[k]  = cnt[k] ? accY[k] : *faceMedY;
        cellCb[k] = accB[k];
        cellCr[k] = accR[k];
    }
    return true;
}
```

### 5.4 为什么用"相对亮度"而不是绝对亮度

`r_cell = Y_cell − Y_face_median`。

- 光照整体变亮/变暗：`Y_cell` 与 `Y_face_median` 同向平移，**差值不变** → 对光照免疫。
- 手遮住某格：该格变亮 5–8 个单位，而中位数被其余 60 多格拉住不动 → **差值跳变**。

这一条是本方案对"光照突变误触发"的正面回答，不需要额外的光照检测器。

---

## 6. 参考层：纯脸指纹

### 6.1 学习策略

```cpp
static void updateRef(OccTrack& t, const float* cellY, float medY,
                      float aspect, float eyeW, float rollDeg, bool clean) {
    constexpr float kA = 0.02f;      // 每帧吸收 2%，τ ≈ 50 帧 ≈ 1.7s（30fps）
    if (!clean) {
        if (++t.refAge > 900) {      // 30s @30fps：超过就强制重学，防漂死
            t.refValid = false; t.refAge = 0;
        }
        return;
    }
    for (int k = 0; k < BEAUTY_OCC_GRID * BEAUTY_OCC_GRID; k++) {
        const float r = cellY[k] - medY;
        if (!t.refValid) { t.r0[k] = r; t.r0Var[k] = 0.0f; }
        else {
            const float d = r - t.r0[k];
            t.r0[k]    += d * kA;
            t.r0Var[k] += (d * d - t.r0Var[k]) * kA;   // 参考自身的抖动幅度
        }
    }
    if (!t.refValid) { t.aspect0 = aspect; t.eyeW0 = eyeW; t.roll0 = rollDeg; t.refValid = true; }
    else {
        t.aspect0 += (aspect - t.aspect0) * kA;
        t.eyeW0   += (eyeW   - t.eyeW0)   * kA;
        t.roll0   += (rollDeg - t.roll0)  * kA;
    }
    t.refAge = 0;
}
```

三个要点：

1. **只在 CLEAN 更新**。遮挡期间参考锁死，手在脸上十几秒也不会"学进去"——这是与现有 `g_skinAdaptEma=1.0`（每帧直接采纳肤色中心）最本质的区别。
2. **慢 EMA（τ≈1.7s）**，吸收缓慢光照漂移与轻微表情。
3. **硬上限 30s**：参考超过 30 秒没更新就作废重学。对应 obs-beauty 的"6 次上限"——上限不是为了效果，是为了**防止状态卡死**。

### 6.2 指纹偏差

```cpp
// 单格偏差，0 = 与参考一致，≥1 = 判定为污染
static float cellDrift(const OccTrack& t, int k, float rNow) {
    const float sig = sqrtf(t.r0Var[k]) + 0.012f;    // 下限 0.012 ≈ 1.2 个 L 单位
    const float d = fabsf(rNow - t.r0[k]);
    return d / (3.0f * sig);                          // 3σ 归一
}
```

手比脸亮 0.05–0.08（Y 域，对应 5–8 个 L 单位），典型 `sig≈0.02`，于是 `d/(3·0.02) ≈ 0.8–1.3` → 落在判定带内。脸的固有起伏（眼窝暗、鼻梁亮）已经被 `r0` 学进去了，所以不产生偏差。

---

## 7. 决策层：四态机

### 7.1 证据合成

```cpp
struct OccEvidence {
    float fp;      // 指纹：污染格占脸椭圆内格子的比例（面积加权）
    float geo;     // 几何：aspect / 眼距比 / roll 三者的归一偏差取最大
    float spill;   // 外溢：ρ 高于基线（仅高侧有效）
};
```

```cpp
// 指纹分量：污染格面积占比，再做一次软压缩
float fp = 0.0f;
int   total = 0;
for (int k = 0; k < 64; k++) {
    const float d = cellDrift(t, k, cellY[k] - medY);
    fp += smoothStep(0.6f, 1.4f, d);          // 0.6 以下不算，1.4 以上全算
    total++;
}
fp /= (float)total;

// 几何分量（三条取最大，都是"相对基线"的归一偏差）
const float dAspect = fabsf(logf(aspect / t.aspect0)) / 0.15f;   // 15% 记 1.0
const float dEye    = fabsf(eyeW - t.eyeW0) / 0.05f;             // 5% 记 1.0
float dRoll = fabsf(rollDeg - t.roll0);
if (dRoll > 180.0f) dRoll = 360.0f - dRoll;
dRoll /= 12.0f;                                                  // 12° 记 1.0
const float geo = fminf(1.5f, fmaxf(dAspect, fmaxf(dEye, dRoll)));

// 合成：指纹为主（0.65），几何为辅（0.35）
float S = 0.65f * fp + 0.35f * fminf(geo, 1.0f);
```

**为什么不给 ρ 权重**：2.2 节已证 ρ 在双手遮脸段最低。ρ 只作为日志字段与"高侧否决"参考。

### 7.2 状态机

```cpp
const float kSHi = 0.55f;     // 进入阈值
const float kSLo = 0.35f;     // 退出阈值（滞回宽度 0.20）
const int   kEnter = 3;       // 连续 3 帧确认进入（30fps ≈ 100ms）
const int   kExit  = 20;      // 连续 20 帧确认退出（≈ 670ms）
const int   kMinHold = 45;    // 最短保持 45 帧（≈ 1.5s）

switch (t.state) {
case 0: // CLEAN
    t.enterCnt = (S > kSHi) ? t.enterCnt + 1 : 0;
    if (t.enterCnt >= kEnter) { t.state = 2; t.holdFrames = 0; t.exitCnt = 0; }
    break;
case 2: // OCCLUDED
    t.holdFrames++;
    t.exitCnt = (S < kSLo) ? t.exitCnt + 1 : 0;
    if (t.exitCnt >= kExit && t.holdFrames >= kMinHold) { t.state = 3; }
    break;
case 3: // COOLING：增益正在淡入回 1，但仍不更新参考
    t.holdFrames++;
    if (S > kSHi) { t.state = 2; t.exitCnt = 0; }
    else if (t.holdFrames > kMinHold + kExit) { t.state = 0; t.holdFrames = 0; }
    break;
}
```

**否决条件**（在算完 S 之后、进状态机之前，任一命中就 `S *= 0.3f`）：

| 条件 | 判据 | 挡掉的误报 |
|---|---|---|
| 换人 / 快速进出画 | 框中心位移 > 20% 脸宽 且 尺度变化 > 15% | 人走近、突然坐下 |
| 全局亮度阶跃 | `|medY − medY_prev| > 0.08` 且 各格偏差**同号** | 开灯、转台灯 |
| 转头 | `|aspect/aspect0 − 1| > 0.25` 且 其变化与 eyeW 同向 | 侧脸（aspect 变、眼距比同向变） |

第三条是区分"手遮"与"转头"的关键：**转头时 aspect 与眼距比同向、成比例变化**（都是 cos(yaw) 的投影效应）；手遮眼时 aspect 变大而眼距比变小（实测 1.40 对 0.265，反向）。

### 7.3 每格目标增益

```cpp
for (int k = 0; k < 64; k++) {
    const float d = cellDrift(t, k, cellY[k] - medY);
    float occ = smoothStep(0.6f, 1.4f, d);           // 格级污染度
    if (t.state == 0) occ = 0.0f;                    // CLEAN：全 1，逐字节不变
    // S 低时整体削弱，避免单格噪声把一小块打成 0
    occ *= smoothStep(kSLo, kSHi, S);
    t.target[k] = 1.0f - occ;
}
// 空间平滑：3×3 两次（近似 σ=1 高斯），消除格边界方块感
boxBlur3(t.target, 2);
```

---

## 8. 施加层

### 8.1 时间斜坡（帧率无关）

```cpp
const double now = nowMsCore();
float dt = (float)((now - t.lastMs) * 0.001);
t.lastMs = now;
if (dt < 0.0f || dt > 0.5f) dt = 1.0f / 30.0f;      // 首帧 / 暂停后恢复

const float tau = (target < g) ? 0.12f : 0.70f;     // 淡出快、淡入慢
const float k = 1.0f - expf(-dt / tau);             // 必须用 exp，不能固定系数
g += (target - g) * k;
```

固定系数（`g += (t−g)*0.3`）在 30fps 调好，录 60fps 就快一倍、掉到 20fps 就慢一半——这一条 obs-beauty 里没有可抄的实现（它不做亮度斜坡），必须自己写对。

### 8.2 CPU 侧：乘进 paintSkin

`beauty_core.cpp:3164`，`paintSkin` 里改三行。注意 lambda 现在没有 `y`，需要把签名从
`paintSkin(mrow, row, x, feather)` 改成 `paintSkin(mrow, row, x, y, feather)`，
四个调用点（3356 / 3379 / 3483 / 3499）同步补一个参数——外层的 `y` 是现成的：

```cpp
auto paintSkin = [&](unsigned char* mrow, const unsigned char* row,
                     int x, int y, int feather) {
    const int wgt = skinWeight(row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]);
    if (wgt <= 0) return false;
    int v = (feather >= 256) ? wgt : (wgt * feather + 128) / 256;
    // 方案 D：遮挡增益（定点 0..256，g=1.0 时 gq=256，下面一行恒等于 v）
    const int gq = occGainAt(x, y, cx, cy, hw, hh, rc, rs);   // 0..256
    if (gq < 256) v = (v * gq + 128) >> 8;
    if (v <= 0) return false;
    ...
};
```

**关键性质**：`gq == 256` 时 `(v*256 + 128) >> 8 == v` 恒成立（v ≤ 255），所以 **CLEAN 态下掩码逐字节不变**，黄金基准不受影响。这条必须写进验收。

### 8.3 GPU 侧：mask.hlsl 增量

`shaders/mask.hlsl` 加两处（不改任何现有公式）：

```hlsl
// 方案 D：遮挡增益场（8×8，R8，双线性）。未启用时宿主绑 1×1 全白纹理，
// 于是 g≡1，输出与旧版逐字节相同。
uniform texture2d occGain;
sampler_state occGainTex {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

// 局部归一坐标 → 增益。u/v 由调用点算好传进来（与 faceGeom/faceRot 同一套）。
float occGainAt(float u, float v, float a, float b)
{
    float2 uv = float2(u / a * 0.5 + 0.5, v / b * 0.5 + 0.5);
    return occGain.Sample(occGainTex, uv).r;
}
```

`PS_Skin` 末尾：

```hlsl
    if (v > 0) {
        float g = occGainAt(uFace, vFace, faceGeom.z, faceGeom.w);
        // 与 CPU 的 (v*gq + 128) >> 8 对齐：先把 g 量化成 0..256 的整数
        int gq = (int)(g * 256.0 + 0.5);
        v = (v * gq + 128) >> 8;
    }
```

`uFace/vFace` 在椭圆分支里本来就算了（`mask.hlsl:185-186` 的 `u/v`），矩形分支补一份同样两行即可。

⚠ **命名陷阱**：`mask.hlsl` 里 `v` 有两个身份——`int v` 是掩码值（166 行），椭圆分支内部又有一层 `float v` 是局部纵坐标（186 行）。增益计算必须放在内层作用域之外，所以局部坐标要么**提到函数开头统一算**（推荐，顺手把矩形分支也覆盖了），要么改名成 `uFace/vFace`。写成 `occGainAt(u, v, ...)` 会静默取到那个 `int` 掩码值，而且不报错。脖子段的 `u/v` 用 `neckEll` 那套，`v` 会超出 [0,1]，Clamp 到最下一行——语义是"脖子沿用下颌那一行的增益"，符合直觉（手托腮时下颌和脖子一起衰减）。

### 8.4 宿主接线

`obs-beauty.cpp` 在 GPU 掩码 pass 之前：

```cpp
// 方案 D：8×8 增益 → R8 纹理（只在 diag.occ.state != CLEAN 或上一帧有残值时上传）
if (occTex && diag.occ.state != 0) {
    unsigned char g8[BEAUTY_OCC_GRID * BEAUTY_OCC_GRID];
    for (int i = 0; i < 64; i++)
        g8[i] = (unsigned char)(diag.occ.g[i] * 255.0f + 0.5f);
    gs_texture_set_image(occTex, g8, BEAUTY_OCC_GRID, 0);
}
gs_effect_set_texture(occParam, occTex);
```

未启用时绑 1×1 全 255 纹理（与 `obs-beauty.cpp:1796` 的常量掩码纹理同一做法）。

---

## 9. 几何层硬化（独立收益，先做）

这四条与遮挡检测**无关**，是消除"污染正反馈"本身的修正。改动小、风险低，建议第一批落地。

### L0-1 椭圆半轴不再用旋转 AABB

`beauty_core.cpp:3214-3216`：

```cpp
// 旧：拿旋转后 AABB 的宽当椭圆半轴 → θ=15° 且 w≈h 时半轴放大 22%，面积 ×1.5
const float lw = rolled ? (sf.w * ac + sf.h * as) : sf.w;
const float hw = lw * kFaceBoxGrow * 0.5f;

// 新：半轴用真实半轴；AABB 只用来定裁剪框与羽化
const float hw = sf.w * kFaceBoxGrow * 0.5f;
const float hh = sf.h * kFaceBoxGrow * 0.5f;
const float hx = hw * ac + hh * as, hy = hw * as + hh * ac;   // 仅 x0..y1 用
```

θ=0 时结果逐字节不变（`ac=1, as=0` → `lw=sf.w`），黄金基准安全。

### L0-2 滚转自洽性加"膨胀率"判据

`beauty_core.cpp:2900` 现有判据只否 30° 以上（`|sinθ|>0.5`），实测 15° 的污染角度能整条穿过。补一条无前置条件的：

```cpp
// AABB 相对脸框的膨胀率：θ=0 时恒为 1，θ=15° 且 w≈h 时约 1.22
const float grow = fabsf(cosf(th)) + (f.h / f.w) * fabsf(sinf(th));
if (grow > 1.12f) return false;    // 这个角度只可能来自被污染的关键点
```

### L0-3 emaMaskFace 的 snap 加门

`beauty_core.cpp:2926-2929` 的四处 snap 会绕过 EMA（框变化 >15% 直接采纳）。手压上脸的瞬间正好触发。加一个入参：

```cpp
static void emaMaskFace(beautycore::FaceBox& sf, float& rc, float& rs,
                        size_t idx, bool geomTrusted) {
    bool snap = !t.valid;
    if (!geomTrusted) snap = false;   // 几何不可信时宁可沿用上一帧，不追新
    ...
```

`geomTrusted = kpsSane(f) && faceRoll 未被否决`。

### L0-4 肤色中心采样区上移 + 跳变拒绝

`beauty_core.cpp:1013` 采样 0.30–0.70 宽 × 0.40–0.80 高（鼻尖到下巴）——正是手最常遮的位置（捂嘴、托腮）。改到 **0.25–0.75 宽 × 0.30–0.60 高**（眉间到鼻梁），并在采纳前比较与当前中心的距离：

```cpp
if (fabsf(outCb - g_skinCb) + fabsf(outCr - g_skinCr) > 0.035f) return false;  // 跳变拒绝
```

---

## 10. 参数表（初值 + 标定方法）

| 参数 | 初值 | 依据 | 标定方法 |
|---|---|---|---|
| 网格 | 8×8 | 定位"手在哪一侧"够用 | 若手小（单手遮眼只占 2×3 格）改 12×12 |
| 每格采样 | 4×4 | 64×16=1024 点，成本恒定 | 若噪声大改 6×6 |
| `tol` 下限 | 0.012 (≈1.2 L) | 稳定光照下同格波动实测 <1.5 L | 用 48–67s 段量方差 |
| 3σ 归一 | 3.0 | 常规统计门限 | 看 `fp` 在安静段是否 <0.1 |
| `kSHi / kSLo` | 0.55 / 0.35 | 滞回 0.20 | 跑离线回放，画 S 的 ROC |
| 进入连续帧 | 3 | 30fps ≈ 100ms | 低于人手移动的起始感知 |
| 退出连续帧 | 20 | ≈670ms | 用户场景遮挡十几秒，退出慢无代价 |
| 最短保持 | 45 | ≈1.5s | 防手抖导致的进出抖动 |
| 淡出 τ | 0.12s | 手上白斑被察觉约 100ms | 观感调 |
| 淡入 τ | 0.70s | 快回显"闪" | 观感调 |
| 参考 EMA | 0.02 | τ≈1.7s | 太慢跟不上光照，太快会学进手 |
| 参考硬上限 | 30s | 防状态卡死 | 对应 obs-beauty 的 6 次上限 |
| 指纹权重 | 0.65 | 实测几何是"确认"不是"唯一" | 见 7.1 |

**所有阈值都必须在素材上标定，不要直接用上表。** 上表是"合理起点"，不是"验证过的最优值"。标定工具见第 12 节 P1。

---

## 11. 开关与日志

沿用现有 `BEAUTY_*` 环境变量风格（`beauty_core.cpp:1204` 附近）：

| 开关 | 值 | 行为 |
|---|---|---|
| `BEAUTY_OCC` | `0` | 完全关闭（等价于当前行为，黄金基准必须一致） |
| | `1` | **shadow**：跑观测+决策，只打日志，增益恒 1（默认，先跑一周） |
| | `2` | 生效：增益乘进掩码 |
| `BEAUTY_OCC_SHI / SLO` | float | 覆盖进入/退出阈值 |
| `BEAUTY_OCC_TAUOUT / TAUIN` | float | 覆盖淡出/淡入时间常数 |
| `BEAUTY_OCC_GRIDN` | 4/8/12 | 网格密度（需重编或用动态分配） |

日志（接在现有 `掩码路径：...` 那一行后面，`obs-beauty.cpp:4512`）：

```
[obs-beauty] occ: state=%d S=%.2f fp=%.2f geo=%.2f rho=%.2f
             aspect=%.2f(%.2f) eyeW=%.3f(%.3f) roll=%.1f(%.1f) gmin=%.2f gmean=%.2f
```

括号内是参考值。`gmin/gmean` 是最直接的观测量：遮挡时应该看到 `gmin→0` 而 `gmean` 仍 >0.7（说明是局部衰减，不是整脸淡出）。

---

## 12. 分阶段落地与验收

| 阶段 | 内容 | 改动量 | 验收 |
|---|---|---|---|
| **P0** | L0-1~L0-4 几何硬化 | ~20 行 | 黄金基准逐字节不变（θ=0 静止画面）；同素材重录，手部掩码面积下降 |
| **P1** | 观测层 + shadow 日志（`BEAUTY_OCC=1`） | ~150 行 | 跑完整 67s 素材，导出 `S/fp/geo` 时间序列；与人工标注对齐，算误报/漏报 |
| **P2** | 决策层 + 状态机（仍 shadow） | ~80 行 | 状态曲线无抖动（进出次数 ≤ 标注段数 + 1）；无 30s 卡死 |
| **P3** | 施加层 + GPU 接线（`BEAUTY_OCC=2`） | ~120 行 + hlsl 20 行 | 手区 mask 均值降 >80%，脸区保持 >90%；整脸淡出反例不出现 |
| **P4** | 参数标定 + 默认值固化 | 无代码 | 连续 2 小时正常直播，`gmin` 从未 <0.9（除真实遮挡） |

**P1 的离线回放工具**（新增 `06_离线验证/occ_replay.py`）：读素材 → 逐帧跑 YuNet 拿几何 → 按第 5、6、7 节同一套公式算 `S` → 输出 CSV 与曲线图。作用有二：一是**不动机器就能标定阈值**，二是与 C++ 实现逐帧对拍（同样的公式，两条独立实现，对不上就是实现错了）。

### 必须守住的三条不变量

1. `BEAUTY_OCC=0` 时，输出与当前版本**逐字节相同**。
2. `state==CLEAN` 时，`gq==256`，掩码写入前的一行是恒等变换。
3. 增益只在掩码生成处相乘，`tone/smooth/detail/region` 四个 shader **一行不改**。

---

## 13. 已知失效场景（必须写进文档，不许假装没有）

| 场景 | 后果 | 缓解 |
|---|---|---|
| 手与脸**亮度也接近**（顺光、手背远离光源） | 指纹分量失效，只剩几何 | 几何分量兜底（权重 0.35）；接受部分失效 |
| 遮挡物是大面积白纸/口罩 | 亮度差 >0.2，指纹强烈触发 | 正常工作 |
| 手遮挡同时**转头** | 几何否决条件会压制 S，可能漏报 | 指纹分量独立于几何，仍会触发；但权重被 S 压缩 |
| 光照方向缓慢变化（夕阳） | 参考 EMA 跟随，无影响 | — |
| 光照快速变化（开灯） | 全局同号偏移 → 触发否决 | 3 秒后参考自动吸收 |
| 脸占比极小（<100px） | 8×8 格每格 <13px，统计不稳 | `observeCells` 的 `a<8` 早退；此时本来也不需要遮挡检测 |
| 多脸 | GPU 掩码链只支持单脸 | 多脸时 `BEAUTY_OCC` 自动降级为 shadow |
| 遮挡物与脸同色且同亮度**且**几何不变 | 本质不可检测 | 承认做不到，不做无用功 |

最后一行是这一整套方案的诚实边界：**当遮挡物在颜色、纹理、几何三个维度上都与脸无法区分时，单目 2D 信息里不存在可用的判别信号。** 这不是算法不够好，是信息量不够。

---

## 14. 事实 / 推断 / 未知

**可验证事实**
- `obs-beauty` 只有"脸存在性"状态位（`0xa19`），无遮挡概念；`FUN_18000e050` 的保持上限为 6 且只在检测丢失时进入。
- 无掩码时走 `memset(0xFF)` 或 `obs_source_skip_video_filter`，由 `BEAUTY_NOFACE_SKIP` 互斥选择（`FUN_18002c310`）。
- `FUN_180039e70`（tone 通道）全文无人脸存在性分支。
- `skinWeight()` 只含 CbCr 椭圆 + 亮度双门控，无邻域项、无时间项（`beauty_core.cpp:1099`，`mask.hlsl:123` 同式）。
- 椭圆半轴取自旋转 AABB（`beauty_core.cpp:3214-3216`）；`faceRoll` 只否 30° 以上（2900）；`emaMaskFace` 有 15% snap 旁路（2926-2929）；`estimateSkinCenter` 采样 0.30-0.70 × 0.40-0.80（1013）；`g_skinAdaptEma=1.0`（981）。
- 本文所有实测数字由 `vid_analysis/{analyze.py, cells.py, hfscale.py, eyes.py, regions.py}` 逐帧算出，可复跑。

**个人推测**
- 手的亮度整体高于脸 5–8 L，是"离光源更近"的结果，可能随打光位置改变；若换机位失效，指纹分量需要重新标定。
- `aspect 0.94→1.40` 的成因我判断是"手压额头把检测框向上撑大"，但 YuNet 内部如何回归框高我没有证据。
- 8×8 网格对"单手遮眼"（约占 2×3 格）够用，对"手指缝"这种细结构不够——但那种场景本来也不需要衰减。

**尚不可知**
- 你的 `OBS美颜摄像头 2`（`camera_beauty_source_v2`）是否与 `obs-beauty.dll` 共用同一份 `beauty_core.cpp`。日志显示 05:23 那条 `geomOnly=1` 属于另一个源；本文所有代码结论只对 `obs-beauty.dll` 这一路负责。
- 商业美颜 SDK 内部的遮挡策略我没有可公开验证的资料，本文不引用任何"行业标准"。
- 实机 30fps 下观测层的真实耗时我没有测（按 9216 次采样估 <0.1ms，属估算）。

---

## 附：最短路径建议

如果只想先看效果，按 **P0 + 8.1 的整脸版** 做：不做 8×8 网格，只用 `S` 驱动一个全局增益（淡出 0.12s / 淡入 0.7s）。

- 代价：手遮半脸时好的一半也一起淡出，与你"不牺牲正常脸"的目标冲突。
- 收益：约 200 行、两天量级，能立刻验证"状态机 + 斜坡"这套骨架是否成立，再决定要不要上网格。

**建议顺序**：P0（几何硬化，独立收益）→ P1（shadow 日志，拿真实数据）→ 再决定网格 vs 整脸。
