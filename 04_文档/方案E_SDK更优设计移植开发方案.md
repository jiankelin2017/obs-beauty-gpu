# 方案 E：SDK 更优设计向 GPU 版的移植（复刻）开发方案

> 版本：E-0.1（设计稿，未接线）
> 日期：2026-11-05
> **适用范围：仅 GPU 版 `obs-beauty.dll`**（源码 `D:\插件\OBS美颜GPU版\01_源码`，着色器 `01_源码\shaders\*.hlsl`）。CPU 版 `D:\插件\OBS美颜源码` 冻结，本方案不涉及。
> 证据来源：`D:\插件\OBS美颜源码\参考文件\01_逆向分析_第三方SDK\`（`fuai.dll` 伪 C + GL 运行时 `glGetShaderSource` 逐字 dump + Frida uniform 真值）
> 前置文档：`方案D_遮挡感知掩码设计.md`、`开关总表.md`

> ## ⚠ 2026-11-05 P0-1 实测更新（以下三处推翻了 v0.1 的假设，以本节为准）
>
> **① 动态轴改造成功。** 把 `input` 的 dim_value `512/512` 改成 dim_param `h/w`，`onnx.checker` 通过，ORT 能跑 192×256 / 256×256 / 256×384 / 384×384 / 512×512 等任意 32 倍数尺寸。产物 `06_离线验证\models\bisenet_face_dyn.onnx`。
>
> **② 类别顺序不是文档原先写的「0=皮肤、18=背景」。** t=15s 竖屏帧的分割对照图显示：**ch16（浅青）覆盖脸 + 托腮的手 + 桌上的手**，ch18 覆盖背景/衣服区域，ch1 占次大。正面资产图上 ch1 又占 72%。这份权重的 19 类顺序**尚未确认**，代码里所有通道索引都做成可配置（`BEAUTY_SEG_CH_SKIN` 等），默认 skin=16。
>
> **③ 最重要：手也在皮肤类里。** 这意味着「用皮肤概率直接做掩码」**不能**解决手遮脸溢出——语义上「手就是皮肤」，这份模型把手和脸一起标了。P0 因此改为**影子层**：默认全关、只出诊断与纹理链路，上线前必须配合方案 D 的几何/亮度仲裁，或换一份只标面部皮肤的模型。
>
> **④ 耗时实测**（Python ORT 1.31.1，单线程，256×256，三个输出头全取）：**65.6 ms**，远超 v0.1 估计的 12 ms。线程数影响很大：2 线程 35.8 ms、4 线程 **19.3 ms**、8 线程 14.8 ms。512×512 为 266.5 ms。插件侧装的是 ORT **1.20.1**（API 20），与实测版本不同，实机耗时需重新量。
>
> 脚本：`onnx_probe\{p0_measure.py, p0_norm.py, p0_vis.py}`，对照图 `p0_vis_t15.png`。
>
> **⑤ 实施调整（已落地）**：P1 不把现有 `skin_tex` 从 R8 改成 RGBA8——那会牵动 `beautyCoreCopyMask`、`protect_buf` 与四处 effect 绑定。改为**新增一张 256×256 RGBA8 的 `segTex`** 承载四语义，现有 R8 链路一行不动。

> ## ⚠ 2026-11-05 P0-2 复核更新（推翻上面 ②③，以本节为准）
>
> **① 类别顺序不是"未确认"，是标准 CelebAMask-HQ 19 类，已逐类核对。**
> 方法是把 argmax 图按调色板逐类对回原画：皮肤=桃色、头发=棕、眼镜=橄榄黄、鼻=青绿、唇=粉、耳=橙、领口=蓝、背景=近黑。两张不同人物都逐类对上，其中戴眼镜那张的 `ch6` 正好套住镜片（`ch6 = eye_g`）。
>
> `0 background / 1 skin / 2 l_brow / 3 r_brow / 4 l_eye / 5 r_eye / 6 eye_g / 7 l_ear / 8 r_ear / 9 ear_r / 10 nose / 11 mouth / 12 u_lip / 13 l_lip / 14 neck / 15 neck_l / 16 cloth / 17 hair / 18 hat`
>
> **② 之前"ch16 覆盖脸+手"是误读。** ch16 是**衣服**。当时之所以看着像"脸+手"，是因为喂的是整帧 letterbox，模型退化成一团糊块——那团"脸+手"其实是衣服/背景类的大片区域。默认值 skin=16 因此等于把"磨皮权重"算到了衣领上。
>
> **③ 真问题不是类别映射，是输入口径：这份模型吃「人脸裁切」。**
> 同一张脸、同一个模型，只改输入方式：
>
> | 输入 | conf | 熵 | 峰值 | 结果 |
> |---|---|---|---|---|
> | 整帧 letterbox 256（现状） | 0.455 | 1.561 | 0.80 | 两类吃 84%，器官形状全无 |
> | 整帧 letterbox 512 | 0.743 | 0.760 | 0.99 | 一类吃 85%，退化 |
> | **紧脸裁切 256** | **0.665** | 1.064 | 0.99 | 19 类逐类可对 |
> | **紧脸裁切 512** | **0.757** | 0.695 | 0.995 | 最干净 |
> | 上半身宽裁切 512 | 0.682 | — | — | 退化：hat 61%、eye_g 10%，脸只剩 4% |
>
> 结论：**必须先裁脸，再换句话说 256 就够用**（256 与 512 的分类排序一致，512 边界更准）。宽裁切（脸小）会直接崩，所以"整帧通吃"这条路不通。
>
> **④ "手也判进皮肤类"这条结论作废，但不是被推翻、而是"没测过"。** 它是在糊块输出上读 ch16 得出的。现在要重测手，得先有"托腮/遮脸手落在人脸裁切框内"的素材——现有 `occ_1080p_86-120s.mp4` 里手都在胸前举物，不在脸框内，测不了。**在重测之前，P0 仍按影子层处理，`BEAUTY_SEG_MIX` 默认 0 不动。**
>
> **⑤ 已落地**：`beauty_skinseg.cpp` 通道默认值改 `skin=1`、非肤归并 `{0,16,17,18}`、五官归并 `{2,3,4,5,6,10,11,12,13}`；顺手修掉旧代码把 `ch1(皮肤)` 与 `ch15(项链)` 累加进"非肤"的自抵 bug。编译通过（12 文件、607,232 B）。
>
> **⑥ P0-2 已落地（2026-11-05 深夜）：整帧 letterbox → 人脸裁切。**
> · 有人脸框时按 `max(fw,fh) × BEAUTY_SEG_MARGIN(1.8)` 取正方形，平移进帧内后**直接拉伸到 S×S，不补黑边**（黑边本身会点亮 ch1/ch18，等于喂了不存在的背景/皮肤）。
> · `SegFrame.uv` 改为**帧归一化的裁切矩形**，着色器 `suv = (uvPix - uv.xy)/uv.zw` 反查；`mask.hlsl` 增加 4 像素软边，框外把 `g` 拉回 1.0（`g=1` 即恒等，与改动前同义）。`segUv.zw == (1,1)`（整帧直填）时这段完全不生效。
> · `beautyCoreSegSeq()` 让渲染线程先判有无新结果，避免每帧白拷 1 MB；`beauty_e_update()` 从**只在自检路径调用**改为每帧调用 —— 原来那条路只在启动时跑一次，那时分割还没出结果，`segTex` 永远是 1×1 白纹理，`segMix` 拨多少都没效果（隐蔽性极强）。
>
> **⑦ 两条硬约束（实测得出，改之前必读）**：
> 1. **输入只能是 512**。这份"动态"模型只改了输入声明，图内部写死 512；插件自带的 onnxruntime 1.20.1 喂 256/288/320/384 会在 `Run` 里**整数除零、整进程崩溃**（0xC0000094），不是返回错误。早前"256 就够用"的结论来自 pip 版 ORT 1.31.1，**对插件不成立，已作废**。
> 2. **坐标有两套**。GPU 链开着时 worker 拿到的是宿主 640×640 缩略图，人脸框却是全帧几何坐标（`src = geom×mapScale + (ox,oy)`）。漏传映射的症状是 `crop=1` 但 `uv=(0,0,1,1)`——看着裁了，其实喂整帧。
>
> **⑧ 性能现实**：512 输入、插件自带 ORT 1.20.1、4 线程 = **95 ms/次**（1 线程 276 ms、8 线程 57 ms）。因此 `BEAUTY_SEG_EVERY` 默认改为 **12**（30 fps ≈ 400 ms 一次，约占 1/4 核），`BEAUTY_SEG_NONSKIN` 默认改为 **1.0**（R 单独用会磨头发：头发 R=0.60、背景 R=0.01，而 B 在头发 0.64/背景 0.98 上很准）。
>
> **⑨ 验收（已做）**：720p 真脸静图端到端跑通，日志三帧 `seg#1..3 102~114ms crop=1 uv=(0.230,0.000,0.562,1.000)`，OBS 不崩；离线复现三种口径统计一致 —— 脸 R=0.87 / 背景 R=0.01 / 衣服 R=0.005 / 头发 R=0.60。证据图与复跑方法见 `06_离线验证/segtest/README.md`。
>
> 复核脚本：`06_离线验证/onnx_probe/seg_class_probe.py`（四组口径对照）、`seg_class_verify.py`（单图逐类核对，可带 `--box`）。
>
> **⑩ 2026-11-05：手遮脸定量结果 —— P0 对痛点无解，`BEAUTY_SEG_MIX` 保持 0。**
> 用同一帧的真手像素贴回同一张脸造出"手在脸框内"，按本文口径（裁脸 1.8 / 512 / 最近邻 / `(v/255)*2-1`）实测：
> 手区 `R=0.916`、`B=0.065`、**`g=R×(1−B)=0.869`**，而脸核心区 `g=0.587`。手区 argmax `ch1(皮肤)=80%`。
> 即"打开分割"会让手部的磨皮/美白权重**高于脸**。原因是这份权重是 CelebAMask-HQ 19 类，**没有"手"这一类**，
> 手只能落到 `skin`。详见 `结论_分割对手遮脸无效_20261105.md`（含三个候选方向与手部检测成本重测：掌心 192 仅 6.5 ms）。
> **P0 的定位从此固定为"排除头发/衣服/背景"，不再承诺解决手遮脸。**

---

## 0. 结论摘要

**SDK 值得抄的不是"算法更高级"，而是三件结构性的事：掩码与关键点解耦、掩码带多个语义通道、色彩走真三维查找表。** 其余（25-tap 核、半分辨率、非肤衰减）要么我们已有等价物，要么必须按本项目的分辨率和朝向重算，照抄常数会直接踩 SDK 自己的坑。

按"对'手遮脸时美白/磨皮溢出'这一痛点的缓解程度"排序：

| 优先级 | 抄什么 | 直接收益 | 落点文件 |
|---|---|---|---|
| **P0** | 掩码与关键点解耦（整帧分割，不经人脸框） | 手遮脸时掩码的来源不再是被污染的人脸框 | 新增 `beauty_skinseg.{h,cpp}` + `mask.hlsl` |
| **P1** | 掩码多语义通道（RGBA 而非单通道 R） | 皮肤/眼睛/非肤分开，可只关掉被遮的那一块；顺带省掉一趟眼睛 draw | `mask.hlsl` R8 → RGBA8 |
| **P2** | 保边核（25-tap 值域加权，非纯 box） | 磨皮不糊五官边界、不把毛孔糊成蜡 | `smooth.hlsl` |
| **P3** | 3D-LUT 色彩（1600 高四波段打包） | 美白不再偏色（一维 LUT 的固有缺陷：色相随亮度漂移） | `tone.hlsl` |
| **P4** | 关闭即早退 + 非肤软衰减 | 省 GPU；掩码边界更自然 | `tone.hlsl` / `smooth.hlsl` |

**不抄**（每条都有理由，见 §5）：半分辨率 192×256 上采样拖尾、`sx/sy` 各向异性核常数、RGB 硬阈值 `skin_detect`、`memset(0xFF)` 全帧兜底。

---

## 1. 差异基线（逐项，带证据行号）

### 1.1 掩码来源：几何驱动 vs 分割驱动

| | SDK | GPU 版现状 |
|---|---|---|
| 做法 | `FUAI_FaceProcessorGetSkinSegResult(proc, result, &w, &h, buf)` 直接出整帧概率图 | `mask.hlsl:148 PS_Skin()` 全靠 `faceRect / faceGeom / faceRot`，即"人脸框 + 椭圆 + CbCr" |
| 证据 | `皮肤分割_skinseg\_dec_skinsseg.c:164-176`：走 vtable `+0x1e8`，出参 `*param_3=w, *param_4=h`，返回像素指针。**入参只有 result 和缓冲区，没有 faceRect** | `mask.hlsl:50-68` 的 uniform 清单 |
| 后果 | 手遮脸 → 分割图照常给出"手上不是皮肤" | 手遮脸 → 框还在（YUnet 仍输出人脸），框内所有像素按 CbCr 判，手被算成皮肤 |

这是**唯一一条能真正解决用户痛点**的移植项。其余四项都是质量改善。

### 1.2 掩码语义：单通道 vs 四通道

| | SDK | GPU 版现状 |
|---|---|---|
| 皮肤 | `tex_mask.a`（另有独立 `tex_skin`） | `mask.hlsl:247` 写 R |
| 眼睛/五官 | `tex_mask.g`（另有 `tex_eye_mask`） | 第二张 R8 纹理，`mask.hlsl:263 PS_Eye()` 独立一趟 draw |
| 非肤检测 | `tex_mask.r` | 无（靠 CbCr 在 `PS_Skin` 里隐含） |
| 美白专用皮肤 | `tex_mask.b` | 无（与磨皮共用 R） |
| 证据 | `beauty_source_readable.md` §三① | `mask.hlsl` 全文 |

代价对比：GPU 版当前每帧要跑 `MaskSkinTech` + `MaskEyeTech` 两趟；SDK 一趟出四个语义，眼睛保护直接读 `.g`。

### 1.3 磨皮核：box vs 值域加权

| | SDK | GPU 版现状 |
|---|---|---|
| 形态 | 25-tap **单 pass**（8 方向 × 3 环 + 中心），环半径比 1:2:5 | 半分辨率 box **H/V 分离两 pass**（`smooth.hlsl:166 PS_BoxH` / `:173 PS_BoxV`） |
| 值域权重 | `wk = 1 − min((Δg)² · yFactor, 1)`，**只看绿通道**；`yFactor` 第 1 趟 40、第 2 趟 20 | **无**。改由 `PS_Amp`（`:186`）算 `|C.g − blur.g|` 做振幅门控，在 `PS_Comp`（`:209`）里转成 `gw` 权重 |
| 证据 | `surfaceblur_generation.md` L22-25、L135-136（GL program 67 逐字） | `smooth.hlsl` 全文 |

评价：**我们的 box + amp 门控不是"错的"，是"另一条路"**——先算全局模糊再按振幅决定信多少。SDK 是"边模糊边按值域丢弃异色邻居"。前者在皮肤这种大面积近似同色的区域表现接近，差异集中在**皮肤与非皮肤的边界**（发丝、眉梢、鼻翼侧影）：box 会把边界另一侧的颜色拖进来，amp 门控只能事后判断"这里振幅大、少磨"，不能阻止污染已经进入 blur。

### 1.4 色彩：一维 LUT vs 三维 LUT

| | SDK | GPU 版现状 |
|---|---|---|
| 结构 | 3D-LUT 打包进 1600 高 2D 纹理，跨蓝切片三线性 | 一维分段线性：`tone.hlsl:87 WHITEN[255]`（5 档 × 3 通道 × 17 锚点）、`:70 CLARITY[255]`、`:104 ROSE_G/ROSE_B[17]` |
| 波段 | 美白 0–512 / 美白备选 512–1024 / 清晰 1024–1536 / 红润 1536–1568 | 各自独立数组，无波段概念 |
| 证据 | `color_analysis.md` §2（`p34/p130/p136/p139` 逐字） | `tone.hlsl` |

一维 LUT 的本质缺陷：**每个通道独立映射**。`(r,g,b) → (f_r(r), f_g(g), f_b(b))`。真实美白是三维映射，肤色在不同亮度下需要的偏移方向不同；拆成逐通道后在中间调和高光接缝处会出现色相漂移（典型表现：额头高光偏青、脸颊偏橙）。3D-LUT 没有这个问题。

> 注意：一维 LUT 也是 GPU 版现在**能逐字节与 CPU 对齐**的原因——CPU 侧 `beauty_freq.cpp:582 toneColor()` 用的就是同样的表。换 3D-LUT 意味着主动放弃这条对齐链，必须靠新的验收手段（§4）。

### 1.5 早退与软衰减

| | SDK | GPU 版现状 |
|---|---|---|
| 早退 | 全部色彩 shader 顶部 `if (color_level < 0.001) return C;` | **待查**（`tone.hlsl` 有 `whiten/rosy/clarity` uniform，未见明确早退；接线时确认） |
| 非肤 | `nonskin_blur_scale` 乘在 `intensity` 上（软衰减） | `smooth.hlsl:220 se = skin * gE`，再在 `:234` 与 `:235` 用 `gI` / `keepEye` 调制——同样是软衰减，**这条我们已等价** |

---

## 2. 移植总蓝图

### 2.1 目标数据流

```text
                        ┌──────────────── 渲染线程（每帧）───────────────┐
源帧 (1080×1920) ──────►│ tone.hlsl ─► smooth.hlsl ─► detail.hlsl ─► out │
      │                 └───────┬───────────────────────────────────────┘
      │                         │ 采样
      │                    ┌────▼─────┐
      │                    │ mask.hlsl│  ← 几何先验 ∧ 分割概率 → RGBA
      │                    └────▲─────┘
      │                         │ 纹理（256×256 RGBA8）
      │  ┌──────────────────────┴──────────────────────┐
      └─►│ asyncWorker()（CPU，与渲染解耦）             │
         │  ① buildMaskInto（现有）                     │
         │  ② skinseg 推理（新增，节流 N 帧）           │
         │  ③ 结果与 heldFrame_ 一起 swap               │
         └─────────────────────────────────────────────┘
```

### 2.2 文件清单

| 动作 | 文件 | 规模 |
|---|---|---|
| 新增 | `beauty_skinseg.h` | ~60 行 |
| 新增 | `beauty_skinseg.cpp` | ~240 行 |
| 新增 | `shaders/skinseg.hlsl` | ~90 行（可选，见 P0-4） |
| 修改 | `beauty_core.h` | +40 行（`SegFrame` 结构 + 导出声明 + diag 字段） |
| 修改 | `beauty_core.cpp` | ~80 行（asyncWorker 挂推理、swap、节流） |
| 修改 | `obs-beauty.cpp` | ~120 行（建纹理、bind_all 绑定、开关、日志） |
| 修改 | `shaders/mask.hlsl` | ~50 行（采样合成 + 通道扩展） |
| 修改 | `shaders/smooth.hlsl` | ~70 行（bilateral 分支） |
| 修改 | `shaders/tone.hlsl` | ~80 行（3D-LUT 分支 + 早退） |

**总量约 830 行**，其中真正有风险的是 `mask.hlsl` 的合成处（涉及逐字节不变不变量）。

### 2.3 三条贯穿始终的不变量

1. **总开关关闭时逐字节不变。** 每个移植项都必须有一个默认为 `0` 的开关（`BEAUTY_SEG` / `BEAUTY_BILAT` / `BEAUTY_LUT3D`），关闭时输出与改动前**逐字节相同**，用 `verify_mask_compiles.py` 的哈希对拍验证。
2. **增益只在一个点相乘。** 掩码的最终值只在 `mask.hlsl` 的返回值处成型一次；`smooth/detail/tone` 只消费，不二次修正。
3. **参数每次 draw 前全量重绑。** `obs-beauty.cpp:3363 bind_all` 的老教训——漏绑一次，第二趟静默全 0，且不报错。

---

## 3. 分期详设

### P0 分割掩码接入（核心）

#### P0-1 模型准备

模型：`06_离线验证\models\bisenet_face.onnx`（= `Downloads\resnet18.onnx`，md5 `ed65fc1d...`，53,205,364 B）。

- 输入 `input`：`[batch,3,512,512]`，**H/W 是字面量不是动态轴**，ORT 会硬校验。
- 输出：`output` / `414` / `424`，各 19 类。取 `output`。
- 参数 13.29 M，50.71 MB。

三步准备：

1. **改动态轴**：把 `input` 的 dim_value `512/512` 改成 dim_param `h/w`。图里已有 14 个 `Shape` + 6 个 `Resize`，本就是"输出回贴输入尺寸"的设计，改轴应可直接生效（**推断，需实测**；backbone stride 32，H/W 必须是 32 的倍数）。
2. **定输入尺寸**：优先 **256×256**，不是 512。理由：耗时 ∝ 面积，512² 是 256² 的 4 倍；而掩码最终要 upscale 到 1080×1920，256 源已经够（8.4× 上采样靠 bicubic + 羽化）。
3. **定类别映射**（CelebAMask-HQ 19 类约定，**需实机验证顺序**）：

| 通道 | 语义 | 用途 |
|---|---|---|
| 0 | skin | 掩码 R（主） |
| 3,4 | 眼睛 | 掩码 G |
| 1,2,9,10,11,12 | 眉/鼻/嘴 | 并入 G（五官保护） |
| 13 | neck | 并入 R（脖子也算皮肤） |
| 15,16,17 | 衣服/头发/帽子 | 并入 B（非肤） |
| 18 | background | 并入 B |

预处理的 mean/std 用 ImageNet 还是 `[0.5,0.5,0.5]/[0.5,0.5,0.5]`，**不能靠猜**——必须跑一帧看皮肤通道的分布，用 §4.2 的探针确认。这是 P0 的第一个实测点。

#### P0-2 接口契约

```cpp
// beauty_core.h 追加
namespace beautycore {

struct SegFrame {
    enum { W = 256, H = 256 };
    alignas(16) uint8_t r[W * H];   // 皮肤概率（含脖子）
    alignas(16) uint8_t g[W * H];   // 五官保护（眼/眉/鼻/嘴）
    alignas(16) uint8_t b[W * H];   // 非肤（发/帽/衣/背景）
    alignas(16) uint8_t a[W * H];   // 美白专用皮肤（P1 用，P0 可先等于 r）
    uint32_t seq;                   // 递增序号，宿主据此判断是否是新帧
    uint32_t ms;                    // 本次推理耗时（毫秒）
    uint32_t ok;                    // 1 = 本帧有效
};

}  // namespace beautycore

// 导出（obs-beauty.def 同步加一行）
extern "C" __declspec(dllexport)
bool beautyCoreCopySeg(beautycore::SegFrame* out);
```

`beautyCoreCopySeg` 语义：**只读拷贝最近一次有效结果**，不阻塞渲染线程，永不分配内存。返回 `false` 表示"还没有结果"，宿主保持白纹理（等价于不介入）。

#### P0-3 CPU 侧接线

在 `asyncWorker()` 内、`buildMaskInto` 之后（约 `beauty_core.cpp:3810`）：

```cpp
// 节流：每 segEvery 帧跑一次。默认 3（30fps → 10 次/秒）。
if (segEnabled_ && ++segTick_ >= segEvery_) {
    segTick_ = 0;
    beautycore::SegFrame& dst = segPend_;      // 先写 pending
    if (skinseg_.run(thumbRGBA_, frameW, frameH, dst)) {
        dst.seq = ++segSeq_;
        dst.ok  = 1;
        std::lock_guard<std::mutex> lk(segMx_);
        segReady_ = segPend_;                  // 与 heldFrame_ 同一 swap 语义
    }
}
```

关键点：

- **必须与 `heldFrame_` 共用帧对齐语义。** 渲染线程拿到的是"上一帧的掩码纹理"，分割图若不跟着一起对齐，会出现"脸动了但掩码还是上一帧位置"的错位。做法：`segReady_` 只在 `maskFresh_` 立起的那一帧被宿主取走。
- **缩略图/letterbox 的坐标必须记下来。** 输入不是 16:9 而是 9:16（1080×1920），letterbox 的内容区在 `thumb` 里的位置要一并传给 shader（`segUv`），否则掩码会整体偏移。
- **线程数沿用 `load(path, err, threads=1)`。** 给 1 个线程，避免和 YUnet 抢核。

#### P0-4 GPU 侧合成

`mask.hlsl` 顶部加：

```hlsl
uniform texture2d segTex;      // 256×256 RGBA8；无结果时绑 1×1 白
sampler_state segTexS {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};
uniform float4 segUv;   // xy = 内容区左上（0..1），zw = 内容区尺寸
uniform float  segMix;  // 0 = 纯几何（默认，逐字节不变）；1 = 完全采用分割
uniform float  segGamma; // 把概率拉成权重，默认 1.0
```

`PS_Skin()` 返回值改为：

```hlsl
    int v = max(vFace, vNeck);          // 现有逻辑的结果

    // ---- P0：分割概率调制（segMix=0 时走严格恒等分支）----
    if (segMix > 0.0) {
        float2 uvPix = ((float2(x, y) + 0.5) / size);
        float2 suv   = saturate((uvPix - segUv.xy) / segUv.zw);
        float  s     = segTex.Sample(segTexS, suv).r;
        float  g     = pow(saturate(s), segGamma);
        // 只在"几何认为这里是皮肤"的前提下按分割概率打折，不做加法：
        // 几何漏掉的地方不凭空长出来（保持保守），几何多算的地方被压下去。
        v = (int)((float)v * lerp(1.0, g, segMix) + 0.5);
        v = min(v, 255);
    }
    return float4((float)v * (1.0 / 255.0), 0.0, 0.0, 1.0);
```

三个设计取舍，必须解释清楚：

1. **只做乘性折扣，不做加法。** 分割概率高但几何判 0 的地方（比如露出的脖子）保持 0，不用分割"救回来"。这样 P0 的最坏情况是"少磨一点"，不是"多磨一块"——方向安全。
2. **`segMix` 严格分支而非 lerp。** `if (segMix > 0.0)` 保证关闭时代码路径与现在完全一致，浮点运算一次都不多。
3. **`segGamma` 而不是直接乘。** 分割概率在边界处是 0.3–0.7 的斜坡，直接乘会让边缘变成"半磨"，观感上像脏。用 `pow` 压一压（实测起点 1.6）能让边界更干脆，再由 `featherRamp` 提供平滑。

#### P0-5 可选：GPU 侧 letterbox 与 softmax

如果 CPU 侧预处理（resize + 归一化）成为瓶颈，可把这两步搬到 `skinseg.hlsl`：

- `PS_Pre`：源帧 → 256×256 双线性缩放 + 归一化，写 RGBA16F
- 推理仍在 CPU（ORT 不认 D3D 纹理，除非上 DirectML，那是另一件事）
- `PS_Post`：19×256×256 logits → softmax → 通道归并 → RGBA8

**P0 阶段不做**，先用 CPU 版跑通，测出瓶颈再搬。

---

### P1 掩码通道扩展（RGBA）

#### 目标

一张 RGBA8 纹理替代现在的两张 R8（`mask` + `eyeMask`），并腾出通道给"遮挡/非肤"。

#### 通道分配

| 通道 | 语义 | 消费者 |
|---|---|---|
| R | 皮肤（磨皮/细节用） | `smooth.hlsl:213`、`detail.hlsl` |
| G | 五官保护（眼/眉/鼻/嘴） | `smooth.hlsl:214`、`detail.hlsl` |
| B | 非肤/遮挡（分割判为背景、或与几何冲突的区域） | P2/P4 新增 |
| A | 美白专用皮肤 | `tone.hlsl:45` |

#### 改动

- 纹理创建处 `GS_R8` → `GS_RGBA8`（`obs-beauty.cpp` 掩码纹理分配）
- `mask.hlsl`：`PS_Skin` 写 `.r` + `.b`，`PS_Eye` 的椭圆结果并入 `.g`（**可以先保留两趟 draw，只把输出并进 G**，减少一次性改动量）
- `PS_Comp` 读 `.g` 替代 `eyeMask.Sample().r`
- **兼容性**：R 通道语义不变，所以 `smooth/detail` 一行不用改，风险可控。

#### 收益量化

- 省一趟 draw（`MaskEyeTech` 若并入则省，不并入也省一张纹理与一次采样）
- 为 P2/P4 腾出"非肤"通道——这是 P0 分割结果落地的地方

---

### P2 保边核（值域加权，非照抄 25-tap 几何）

#### 移植的不是几何，是值域权重

这一点必须先说清楚，否则会误以为在照抄 SDK：

| | SDK | 本方案 |
|---|---|---|
| 几何 | 8 方向 × 3 环（1:2:5）+ 中心 = 25 tap，**单 pass** | 沿用现有 **H/V 分离**（`PS_BoxH` + `PS_BoxV`），每维 5 tap |
| 值域权重 | `wk = 1 − min((Δg)²·yFactor, 1)` | **照抄**这个式子 |
| 采样数 | 25 次/像素/pass × 2 pass | 5 次/像素/pass × 2 pass |

只移植值域权重，是因为我们的 H/V 分离在性能上明显更优，而 bilateral 的保边能力**主要来自值域权重，不来自方向排布**。若 P2 上线后边界观感仍不达标，再考虑改成 8 方向 3 环（此时采样数 ×2.5，需要重新量耗时）。

#### 实现

两个硬约束（来自本仓库 `shaders/*.hlsl` 的既有惯例，不是猜测）：

- **不能把 `texture2d` / `sampler_state` 当函数形参。** 六个现有 shader 里零先例，`OBS effect-parser` 是自研文本解析器，不保证支持 HLSL 的 sampler 形参传递。一律直接引用全局 uniform，每个用途写专用函数。
- **不要用 `[unroll]` 属性。** 现有 shader 从未用过，解析器行为未知。用普通 `for`（`smooth.hlsl:168` 已在用）。

```hlsl
uniform float bilatK;    // 0 = 纯 box（默认，逐字节不变）；1 = 完全值域加权
uniform float bilatYF;   // yFactor，与 SDK 同量纲（0..1 图像值上的平方），第 1 趟 40 / 第 2 趟 20

// 只用绿通道算差，与 SDK 一致。中心权重恒 1。
// 全程 0..1 图像值量纲 —— 与 image 采样返回一致，也与 SDK 的 yFactor 标定一致。
float3 boxWeightedH(float2 uv, float3 c0, float yF)
{
    float3 acc  = c0;
    float  wsum = 1.0;
    for (int k = -2; k <= 2; ++k) {
        if (k == 0) continue;
        float3 ck = image.Sample(boxTex, uv + float2((float)k * texel.x, 0.0)).rgb;
        float  dg = ck.g - c0.g;
        float  w  = 1.0 - min(dg * dg * yF, 1.0);
        acc  += ck * w;
        wsum += w;
    }
    return acc / wsum;
}

float3 boxWeightedV(float2 uv, float3 c0, float yF)
{
    float3 acc  = c0;
    float  wsum = 1.0;
    for (int k = -2; k <= 2; ++k) {
        if (k == 0) continue;
        float3 ck = image.Sample(boxTex, uv + float2(0.0, (float)k * texel.y)).rgb;
        float  dg = ck.g - c0.g;
        float  w  = 1.0 - min(dg * dg * yF, 1.0);
        acc  += ck * w;
        wsum += w;
    }
    return acc / wsum;
}
```

调用处（`PS_BoxH` / `PS_BoxV` 各改一处，`bilatK <= 0` 走原路径）：

```hlsl
float4 PS_BoxH(vertex_data vert_in) : TARGET {
    float3 s = float3(0.0, 0.0, 0.0);
    for (int k = -rad; k <= rad; ++k)
        s += image.Sample(boxTex, vert_in.uv + float2((float)k * texel.x, 0.0)).rgb;
    float3 box = s * invBox;                       // 原式，量纲 0..1
    if (bilatK > 0.0) {
        float3 c0 = image.Sample(boxTex, vert_in.uv).rgb;
        box = lerp(box, boxWeightedH(vert_in.uv, c0, bilatYF), bilatK);
    }
    return float4(box, 1.0);
}
```

> 注意 `lerp` 的位置：**必须在乘过 `invBox` 之后**。`s` 是未归一化的累加和，`boxWeightedH` 返回的是已归一化均值，两者量纲不同，直接在 `s` 上 lerp 会让混合结果随 `rad` 漂移——`rad` 是随画幅变的（`smooth.hlsl:105`），这个 bug 会表现为"换个分辨率磨皮强度就变了"，很难查。

#### 三点必须注意

1. **`yFactor` 的标定尺度是 0..1，不是 0..255。** `smooth.hlsl` 内部混用两种尺度：`PS_Down` 末尾 `* 255.0`、`PS_Comp` 开头 `c = src.rgb * 255.0`，但 `PS_BoxH/V` 里 `image.Sample().rgb` 是原始 0..1。SDK 的 `(Δg)²·yFactor` 是在 0..1 上标定的：Δg=0.1 时权重 0.6、Δg=0.16 时归零。**如果顺手乘了 255**（因为文件里到处是 `*255.0`，很容易顺手），Δg 变成灰阶差，`(Δg)²·40` 在 0.16 灰阶就饱和到 1，整个核退化成"只信中心像素"——磨皮不会报错，只是悄悄变成原图。这是本项最容易踩的坑，且症状是"没效果"而不是"出错"。
2. **不要照抄 `sx=0.002640845 / sy=0.0046875`。** 这组 UV 步长是 SDK 在 1136×640 缓冲上标定的，`sy/sx=1.775` 直接导致竖屏下核变成 3.16:1 的竖椭圆（`color_analysis.md` 顶部更正块已坐实）。我们按"像素等比"自己算：`stepUV = float2(1.0/srcW, 0.0)` 与 `float2(0.0, 1.0/srcH)`，各向同性。**用户的 1080×1920 竖屏直播正好是 SDK 这个 bug 的受害者，抄常数等于把 bug 一起抄进来。**
3. **半分辨率上做。** 核在 960×540 上跑，5 tap 一维 × 2 趟 = 10 次采样/像素，比全分辨率省 4 倍。SDK 也是这么做的（540×960 缓冲）。

#### 与现有 amp 门控的关系

不冲突，是串联的两级：bilateral 让 blur 本身更干净（不跨边界取色），amp 门控再按"原图与 blur 的差"决定信多少。**先上 bilateral，`amp` 参数可能要重调**（原本 box 污染大，`gT` 必须设低；核干净后可以放宽）。这是 P2 的验收重点。

---

### P3 3D-LUT 色彩

#### 目标

`tone.hlsl` 的美白/清晰/红润改走真 3D-LUT，替代一维 17 锚点分段线性。

#### 纹理布局（照 SDK 的波段打包，但可简化）

**建议不照抄 1600 高四波段**，理由：我们只有美白/清晰/红润三路，没有"美白备选"这一路（那是 SDK 的 `mode_tex` 双模式）。改为**独立纹理**或**纵向两段（1024 高）**，寻址简单一半，出错面小一半。

若坚持复刻波段打包，`color_analysis.md` §2 的偏移量是硬约束：美白 `y∈[0,512)`、备选 `[512,1024)`、清晰 `[1024,1536)`、红润 `[1536,1568)`，分母 1600，红润 16 切片压进 32px。

#### 采样（照抄这段数学，它是标准算法）

同样遵守 P2 的两条硬约束：**不用 sampler 形参、不用 `[unroll]`**。另外 `fmod` 不用——改用 `x − floor(x/y)*y`，避免解析器对 `fmod` 的处理与 D3D 不一致。

```hlsl
uniform texture2d lutTex;          // 波段打包的 2D 纹理
sampler_state lutTexS {
    Filter   = Linear;
    AddressU = Clamp;              // L6：必须 Clamp，Repeat 会在 R/G 越界处镜像出接缝
    AddressV = Clamp;
};
uniform float4 lutDim;             // x=dim(切片数) y=raw(行) z=col(列) w=预留
uniform float2 lutBand;            // xy = 该波段的 (y 起点, 总高度)，如美白 (0,1600)

float2 lutGrid(float b, float col)
{
    float q = floor(b / col);
    return float2(b - q * col, q);
}

float3 sampleLut3d(float3 c)
{
    float dim = lutDim.x, raw = lutDim.y, col = lutDim.z;
    float3 tc = clamp(c, 0.0, 1.0);
    float  b  = tc.b * (dim - 1.0);
    float  b0 = floor(b);
    float  f  = b - b0;

    float2 q0 = lutGrid(b0,        col);
    float2 q1 = lutGrid(b0 + 1.0,  col);

    float2 inv  = float2(1.0 / col, 1.0 / raw);
    float2 half1 = float2(0.5 / (col * dim), 0.5 / (raw * dim));
    float2 p0 = q0 * inv + half1
              + float2((1.0 / col - 1.0 / (col * dim)) * tc.r,
                       (1.0 / raw - 1.0 / (raw * dim)) * tc.g);
    float2 p1 = p0 + (q1 - q0) * inv;
    p0.y = (p0.y * lutBand.y + lutBand.x) / 1600.0;   // 波段偏移（不分波段时置 (0,1600)）
    p1.y = (p1.y * lutBand.y + lutBand.x) / 1600.0;
    return lerp(lutTex.Sample(lutTexS, p0).rgb,
                lutTex.Sample(lutTexS, p1).rgb, f);
}
```

#### 必须准备的 LUT 数据

这是 P3 的真正工作量：**3D-LUT 从哪来**。三个选项：

| 选项 | 做法 | 风险 |
|---|---|---|
| A | 从 SDK 的 `tex_combine` 里 dump 出实际 LUT（已有 texdump 基础设施，`gl_texture_dump.js`） | 版权/合规不明；且那是 SDK 的调色风格 |
| B | 用现有 `WHITEN[255]` 一维表**合成**一张 3D-LUT：对每个 (r,g,b) 格点套用一维映射，得到 `f_r(r),f_g(g),f_b(b)` | 合成结果 ≡ 一维表，**没有收益**，纯属浪费 |
| C | 自己做一张 17³ 或 33³ 的调色 LUT（Photoshop/达芬奇导出 .cube） | 需要调色判断，且要重新定"什么是好看的美白" |

**结论：P3 的优先级因此应排在最后。** 它的收益（消除色相漂移）真实，但成本是"要有一张可信的 LUT"，而这一项不属于工程问题。**建议 P3 暂缓，等 P0/P1/P2 落地且观感验收通过后再决策。**

#### 早退（这条与 P3 解耦，可以先做）

```hlsl
// PS_Main 开头
if (whiten < 0.001 && rosy < 0.001 && clarity < 0.001)
    return image.Sample(imageTex, vert_in.uv);
```

SDK 每一路色彩都这么写，代价为零、收益是"全关时省掉整条色彩链"。

---

### P4 非肤软衰减

现状已等价（`smooth.hlsl:220 se = skin * gE`），**P4 实际只剩"把 P1 的 B 通道用起来"**：

```hlsl
float nonSkin = mask.Sample(maskS, uv).b;   // P1 新增：分割判为非肤的置信
float intact  = 1.0 - nonSkin * nonskinK;   // nonskinK 默认 0，逐字节不变
float3 res    = c + (even - c) * gI * intact;
```

这条的意义：几何椭圆在"脸框边缘外扩区"会误判（下巴外、耳侧），分割能把这些地方标出来。属于 P0 的自然延伸，工作量很小。

---

## 4. 验收

### 4.1 三条硬性回归

| 编号 | 条件 | 判据 | 工具 |
|---|---|---|---|
| R1 | 所有新开关 = 0 | 输出与 v2.17.2 **逐字节相同** | `verify_mask_compiles.py` 现有哈希对拍 |
| R2 | `segMix=1` 且无分割结果（白纹理） | 与 `segMix=0` 逐字节相同 | 同上（白纹理 = 255 = 恒等） |
| R3 | 每次 draw 前参数全绑 | 第二趟不出现全 0 | `bind_all` 日志 + `掩码路径` 行 |

### 4.2 新增的定性验收

| 项 | 怎么做 | 通过标准 |
|---|---|---|
| 类别顺序 | 用 `onnx_probe` 跑一段人脸视频，统计 19 通道的均值分布 | 皮肤通道在脸颊区 > 0.8；背景通道在画面四角 > 0.9 |
| 手部误判率 | 取 `05_验证/out/live_edge/2026-11-05 05-45-51.mp4` 的遮手段，统计手部矩形内通道 0 的均值 | **< 0.3**（这是 P0 成立与否的分水岭） |
| 耗时 | `BeautyMaskDiag` 加 `segMs` | 256×256 单帧 < 12 ms（节流 3 帧后摊薄 < 4 ms/帧） |
| 边界观感 | 侧脸、发丝、下颌线三处逐帧截图 | 无"半磨"灰边、无掩码跳动 |

### 4.3 diag 字段扩展

`beauty_core.h:562 BeautyMaskDiag` 追加：

```cpp
uint32_t segOk;      // 本帧是否有分割结果
uint32_t segMs;      // 推理耗时
uint32_t segSeq;     // 序号（判断是否卡住）
uint32_t segHandP;   // 手部区域平均概率（调试用，可选）
```

---

## 5. 不抄清单

| # | SDK 的做法 | 为什么不抄 |
|---|---|---|
| 1 | 192×256 半分辨率出掩码，直接双线性上采样到全分辨率 | 5.6× 上采样会在发丝、下颌线产生拖尾；我们掩码最终要贴到 1080×1920，用 256 已经是极限，再降就是画质事故 |
| 2 | `sx=0.002640845 / sy=0.0046875`（`sy/sx=1.775`） | 这是 1136×640 横屏标定的常数，竖屏下核变 3.16:1 竖椭圆。**SDK 自己的 bug，用户的竖屏直播正在受害** |
| 3 | RGB 硬阈值 `skin_detect`（`r>0.371 && g>0.156 && ...`） | 阈值硬编码，换光照/肤色直接误判（`color_analysis.md` L4 已标注）；我们已有 CbCr 椭圆 + 分割概率，没有理由退回硬阈值 |
| 4 | 无脸时 `memset(0xFF)` 整帧掩码 | 效果优先取向，与本项目"不牺牲正常脸"相反（方案 D §1 表第 6 行已判） |
| 5 | 跨蓝切片三线性里的 `dim/raw/col` 三组 uniform | 不抄"三组独立 uniform"这个设计，改成一个 `float4 lutDim` 结构体式传参，少两个出错点（`color_analysis.md` L2 就是栽在这上面） |
| 6 | 2 MB 共享内存 IPC | 我们是同进程插件，没有进程边界，抄这个是给自己加延迟 |

---

## 6. 风险表

| 风险 | 触发条件 | 影响 | 缓解 |
|---|---|---|---|
| BiSeNet 改动态轴失败 | ONNX 图里有依赖 512 的硬编码 Slice | P0 卡住 | 退回"固定 512 推理 + 输出 resize 到 256"，只损失耗时；或重导出 |
| 类别顺序与 CelebAMask-HQ 不符 | 权重来自非标准训练 | 皮肤/背景取反，掩码全错 | §4.2 第一项先验，不通过就换映射表 |
| 分割耗时超预算 | 256×256 实测 > 12 ms | 掉帧 | 降节流到 5 帧、或换 MobileNetV3-small 级模型、或上 DirectML |
| 掩码与脸框错位 | `segUv` 的 letterbox 内容区算错 | 掩码整体偏移 | §4.2 边界观感验收；先做单帧静态对拍 |
| bilateral 与 amp 参数打架 | P2 上线后 `gT` 未重调 | 磨皮变弱或过强 | P2 单独开关 + 与 v2.17.2 的 A/B 截图 |
| 3D-LUT 无数据可用 | P3 的三个选项都走不通 | P3 无法落地 | 已排最后，不影响 P0–P2 |

---

## 7. 实施顺序

```text
P0-1 模型准备（改轴 + 类别验证 + 耗时基线）   ← 纯离线，零风险，先做
   │
P0-2/3 接口 + CPU 接线（影子模式，segMix 恒 0）  ← 画面不变，只跑不生效
   │
P0-4 mask.hlsl 合成（segMix 从 0 拨到 1）        ← 第一个可见变化
   │
P1  RGBA 通道扩展                                 ← 依赖 P0 的 B 通道
   │
P2  bilateral 核                                  ← 与 P0 正交，可并行
   │
P4  B 通道软衰减                                  ← 依赖 P1
   │
P3  3D-LUT（暂缓，等 LUT 数据决策）
```

**第一步建议从 P0-1 开始**：它不碰任何生产代码，产出是三个数字（能否改轴、类别顺序对不对、单帧多少毫秒）。这三个数字决定后面全部工作的可行性。

---

## 8. 与方案 D 的关系

两套方案不冲突，是**互补的两条腿**：

| | 方案 D（遮挡感知） | 方案 E（SDK 移植） |
|---|---|---|
| 判据来源 | 几何自洽 + 局部亮度指纹（**无模型**） | 整帧语义分割（**有模型**） |
| 代价 | 零推理开销 | 3–12 ms/帧 |
| 强项 | 手遮脸十几秒这种"慢遮挡" | 任何形状的遮挡物、任意位置 |
| 弱项 | 分割能一眼看出的遮挡要绕一大圈推 | 模型没见过的手势/材质会失手 |
| 失效时的表现 | 增益慢慢降，画面渐变 | 掩码局部错，但不会全错 |

**建议的最终形态是两条腿并用**：分割给"哪里是皮肤"的先验，几何/指纹给"这个先验这一帧可不可信"的仲裁。若分割概率与几何判断冲突超过阈值（比如分割说 0.9 皮肤、几何说框外），就走方案 D 的状态机降增益。

这一条不写进 P0 的必做项——先把分割单独跑通、量化它的误判率，再决定要不要加仲裁层。
