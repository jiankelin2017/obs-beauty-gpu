# 方案 B：插件支持输入旋转（2.10.0）

> 状态：✅ **已完成并发布**（2.10.0），**2.13.1 补齐全帧路径**。
> 2.10.0 只在检测缩略图路径生效，全帧回退帧与「开了五官参数」时静默失效（当时记 P0-11）；
> 2.13.1 把旋转提到 `letterboxRgb` 之前做进重采样（`letterboxRgbRot`），**全帧路径同样生效**，
> 实机 `有脸` 100/1/2/0%（`ir=0/90/180/270`，`PROGRESS §9.36`）。**下文凡说「只在缩略图路径生效」
> 都是 2.10.0 的历史限制，2.13.1 起不成立。**
> 关联：每日记录 §9.20（真人竖屏摄像头「旋转在滤镜之后」缺陷，当时用子场景绕开）
> 版本依据：表 D —— 新增面板控制 + 旋转场景下几何正确性变化 → +0.1.0 → **2.10.0**
> 实现要点（与计划一致）：旋转只发生在**检测缩略图采样**与**检测出口几何反变换**；`g_thumbFullW/H` 仍传**源尺寸**（mask/protect 必须在源坐标系构建，渲染采样源纹理），旋转仅在坐标变换里消化。

---

## 1. 背景与问题

用户的真实竖屏摄像头链路：采集卡输出 **1920×1080 横屏**（脸在画面里是横躺的），用户在 OBS 场景项上设 `rot=-90` 让显示变竖屏。

缺陷（§9.20 已定位）：场景项变换只作用于**显示合成**，滤镜拿到的源纹理 `in_tex` 仍是未旋转的 1920×1080。于是：

- 检测（YuNet）在横躺的脸上跑，FaceBox / kps 全部错位；
- 五官几何（眼/眉/法令纹/嘴）、美形（瘦脸/大眼/下巴）、保护椭圆全部按错位置落笔；
- 当时用「插子场景 CamRot 把旋转移到滤镜之前」绕开，但那是临时工程 hack，不是产品能力。

`beauty_core.cpp:1434` 的 `setDetectOptions(int, ...)` 在 GpuEngine 里把 `rotate` 参数直接丢弃，也印证了旋转从未进入管线。

---

## 2. 目标与范围

**目标**：面板新增「输入方向 0/90/180/270」下拉；用户设为非零后，滤镜**在管线内部**把输入当作旋转后处理，使检测几何、五官、美形、保护全部对齐到正确人脸位置。**用户保留原有的场景项旋转做显示**（输出朝向不变）。

**范围内（做）**：
- 面板新增下拉 `input_rot`（0/90/180/270，默认 0）。
- 检测缩略图按 `input_rot` 旋转采样（letterbox/draw 步）。
- 检测出口的 FaceBox / kps 反变换回源坐标系（纯函数）。
- 验证：旋转场景下 mask/五官/美形落点正确。

**范围外（不做）**：
- 不旋转**输出像素**。滤镜输出仍是源朝向（横屏），由 OBS 场景项旋转负责显示竖屏。这样输出尺寸、23-draw 渲染目标、mask/protect 缓冲全部保持 `Ws×Hs`，改动面最小。
- 不做任意角度（只 90 的倍数），避免双线性重采样引入的几何精度问题。

---

## 3. 核心思路

只在「检测几何」这一条路上引入旋转感知，**渲染与输出完全不动**：

1. **旋转检测输入**：宿主把 `in_tex` 绕中心旋转 θ 后 letterbox 进 `thumb_rt`（S×S）。检测看到的因此是「正立的脸」。
2. **缩略图语义改成「旋转后全帧」**：`beautyCoreSetThumbSource(..., fullW, fullH, ...)` 的 `fullW/fullH` 传**旋转后的尺寸**（θ=90/270 时 `Wr=Hs, Hr=Ws`；θ=0/180 时 `Wr=Ws, Hr=Hs`）。`scale/ox/oy` 仍按 letterbox 正常算，只是针对 `Wr×Hr`。
3. **检测出口反变换**：引擎拿到 FaceBox/kps（旋转后全帧坐标）后，用一个**纯函数** `invRotateFace()` 把它们绕中心转回 `-θ`，映射到源坐标系 `(Ws×Hs)`。这一步之后，mask / protect / 五官椭圆 / 美形 warp 一切照旧在源坐标工作。
4. **渲染与输出**：`process(w,h)` 收到的仍是 `Ws×Hs` 源帧，mask/protect 仍在 `Ws×Hs` 上构建，23-draw 渲染、输出纹理尺寸全部不变。

> 为什么输出不动：用户已习惯用场景项旋转显示。若滤镜同时旋转输出，会和场景项旋转叠加成双重旋转。把「旋转」从场景项吸收进滤镜输入，是产品级正确解法；显示旋转继续留在场景项。

---

## 4. 坐标变换数学

设源帧 `Ws×Hs`，旋转角 `θ ∈ {0,90,180,270}`。旋转后全帧尺寸：
- θ=0 / 180：`Wr=Ws, Hr=Hs`
- θ=90 / 270：`Wr=Hs, Hr=Ws`

检测在「旋转后全帧」坐标系工作，FaceBox 中心 `(cx,cy)`、关键点 `kps[]` 均在该系。反变换把点映射回源系：

```
C_rot = ((Wr-1)/2, (Hr-1)/2)      // 旋转后全帧中心
C_src = ((Ws-1)/2, (Hs-1)/2)      // 源帧中心
P_src = R(-θ) · (P_rot - C_rot) + C_src
```

其中 `R(-θ)` 为标准 2D 旋转矩阵。box 的宽高在 θ=90/270 时互换（`w_src=h_rot, h_src=w_rot`），θ=0/180 时不变。

`invRotateFace(FaceBox& f, int theta, int Wr, int Hr, int Ws, int Hs)` 同时对 `f.cx/f.cy/f.w/f.h` 与全部 `f.kps[]` 做上述变换；`kpsSane()` 等后续判定在变换后执行即可。

---

## 5. 改动清单（按文件，行号为当前 ad430ad 快照）

### 5.1 `beauty_params.h`
- 在 `BeautySettings` 结构体新增成员 `int input_rot;`（默认 0），与 `smooth_mode` / `skin_detect` 同级，**不**插入 `P_` 面板枚举（避免扰动 14 项默认值与既有索引）。
- 构造函数初始化 `input_rot(0)`。
- 横幅 / 版本：立项时 `BEAUTY_BUILD_STR` 由 2.9.1 升 **2.10.0**（已完成；当前 2.14.1）。

### 5.2 `obs-beauty.cpp`（UI）
- 面板新增下拉（仿 `smooth_mode` 写法，obs-beauty.cpp:1018）：
  `obs_properties_add_list(ppts, "input_rot", "输入方向", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT)` + 四项 `0 原样 / 90 / 180 / 270`。
- `obs_data_set_default_int(settings, "input_rot", 0)`（obs-beauty.cpp:928 附近）。
- 在 `f->smooth_mode` 读取处（obs-beauty.cpp:523 附近）同步读 `f->input_rot = (int)obs_data_get_int(s, "input_rot");`。

### 5.3 `obs-beauty.cpp`（缩略图构建，约 5034–5110）
- 当前：`gs_draw_sprite(src, 0, nw, nh)` 直接把源画进 letterbox（obs-beauty.cpp:5103）；`fullW/fullH` 传 `cx,cy`（源尺寸，obs-beauty.cpp:5523）。
- 改为：据 `f->input_rot` 计算 `Wr,Hr`；letterbox 的 `scale/ox/oy` 针对 `Wr×Hr` 重算；在 draw 前用 `gs_matrix_*` 绕 thumb 中心叠加旋转 `θ`（90/270 时 letterbox 矩形本身宽高互换）；`beautyCoreSetThumbSource(..., Wr, Hr, scale, ox, oy)` 传旋转后尺寸。
- θ=0 时路径与现有一字不差（默认行为不变）。

### 5.4 `beauty_core.cpp`（检测出口反变换）
- 在 `buildMaskInto` 拿到 `faces` 之后、构建 mask/protect 之前（约 2680 附近，kps 已做 `(-ox)/scale` 变换成旋转后全帧坐标之后），对每张 `faces[i]` 调用 `invRotateFace(..., g_thumbFullW, g_thumbFullH, w, h, input_rot)`。
- `input_rot` 从 `BeautySettings` 经 `process()` 传入（新增线程/成员持有，或在 `buildMaskInto` 调用处透传）。
- `GpuEngine::setDetectOptions`（beauty_core.cpp:1434）保持现状即可（旋转在采样端做，引擎 `rotate` 参数不再需要）。

### 5.5 不改动
- 23-draw 渲染链、输出纹理、`mask_rt_*` 对拍、region 链、warp 链一律不动（它们工作在源坐标，反变换已把几何送回源系）。

---

## 6. 验收方案

1. **单元级几何对拍**：构造一张「横屏里横躺的正脸」素材，设 `input_rot=90` 跑检测，导出 `faces[0]` 的 `cx,cy,w,h,kps`；手工把同一张图在 Python/OpenCV 里 `rot90` 后独立跑 YuNet，比对两者在**源坐标系**下的 FaceBox/IoU 与 kps 距离（应 ≤1 px）。
2. **mask 落点可视化**：开 `BEAUTY_MASK_GEOM_ONLY=1` + `BEAUTY_SPOT_DEBUG`/mask dump，肉眼确认旋转 90 后眼睛/嘴保护椭圆落在横躺脸的正确位置（不再整体错位）。
3. **端到端真机**：复用 §9.20 的 `CamRot` 子场景拆除，回到「源设 rot=-90 + 滤镜 input_rot=90」组合，截图确认五官/美形对齐；关掉场景项旋转、仅 input_rot=90 也应得到正立输出（两种都验证一次）。
4. **回归**：`input_rot=0` 全量回归 2.9.1 基线（S0 基线、4K 祛斑像素量），确保默认路径零差异。
5. **判定门槛**：旋转场景下 IoU ≥ 0.95、kps 中位误差 ≤1 px；`input_rot=0` 与 2.9.1 逐字节一致（maxdiff 0）。

---

## 7. 风险与回滚

- **缩略图旋转绘制**：`gs_matrix_*` 绕中心旋转 + letterbox 矩形互换，是宿主端最易错的环节；先单独用 `BEAUTY_THUMB_DUMP` 落盘旋转后缩略图肉眼核。
- **反变换方向**：务必是 `-θ` 且 box 宽高互换正确；用 §6.1 的 OpenCV 独立复算兜底（不要拿注释当证据，见 §9.12.4 教训）。
- **90/270 尺寸互换**：任何 `Wr/Hr` 用反都会让几何系统性偏移一整帧；`process(w,h)` 的 `w,h` 始终是源尺寸，反变换的输入/输出尺寸必须严格区分。
- **回滚**：`input_rot` 默认 0，出问题只需把下拉设回 0 即回到 2.9.1 行为；开关本身不污染默认路径。

---

## 8. 版本与文档同步（**立项后执行 → 已全部执行**）

> 这四条都已在 2.10.0 / 2.13.1 落地：`BEAUTY_BUILD_STR` 现为 2.14.1；计划表 A/D 已有两行；
> 输入旋转走**面板**、无 env 旁路（故《开关总表》只记面板控件）；`05_验证/README.md` 已有
> `rot_live_check.py` 行与三个坑；`PROGRESS §9.27` / `§9.28` / `§9.36` 三节齐。
> 以下原文保留，作为「一次立项要同步哪几处」的清单样例。

- `beauty_params.h` `BEAUTY_BUILD_STR` → `2.10.0`。
- `04_文档/开发流程与优化计划.md`：表 A 升 2.10.0 + 新增「输入旋转」行（状态：进行中）；表 D 补 2.10.0。
- `04_文档/开关总表.md`：补 `BEAUTY_INPUT_ROT` 或说明该能力走面板（非 env）；若实现中确需 env 旁路再加。
- `05_验证/README.md`：补输入旋转验证工具行与坑。
- `PROGRESS.md`：新增 §9.27 立项记录。

---

## 9. 工作量估计

- 纯函数 + 面板 + 缩略图旋转绘制 + 反变换接入：约 0.5–1 天编码。
- 验收（几何对拍 + 真机 + 回归）：约 0.5–1 天（需 OBS 窗口，约 100 min 量级）。
- 合计：约 1–2 天，阻塞性低（默认 0 不影响既有用户）。
