---
doc_type: guides
title: 验证工具与 21 条坑
version: 0.1.1
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 05_验证/README.md
---

# 05_验证 —— 门禁与采集脚本

> **结论**：`05_验证/` 的脚本分两类 —— **离线对拍**（不启 OBS）与**采集 / 长跑**（启 OBS）；
> 跑之前先看 §三 的 21 条坑，否则很容易把工具误判当成产品回归。脚本仍留在仓库 `05_验证/`，
> 本文只是文档归位，命令里的路径不变。本机 `python` 是 Microsoft Store 别名，一律用 `py -3.12`。
> 文中 `PROGRESS §9.x` 按 [`../INDEX.md`](../INDEX.md) §3 统一映射检索（B1）。

> 本目录的脚本用于**离线对拍**与**装机后验收**。依赖见同目录 `requirements.txt`。

## 一、三条命令起环境

```bat
py -3 -m pip install -r 05_验证\requirements.txt
py -3 -c "import websocket, PIL, numpy; print('deps ok')"
py -3 05_验证\verify_smooth_compiles.py
```

第一条装依赖，第二条自检（能打印 `deps ok: <版本>` 即为成功），第三条是不需要 OBS 的着色器编译对拍，跑通即说明环境可用。

`py -3` 指 Python 3.9+ 的启动器形式（Windows 官方安装包自带）。**本机 `python` 是 Microsoft Store 别名，不可用**；发布闸门与离线回归一律用 `py -3.12`（见 [`../README.md`](../README.md) §4）。本文其余 `py -3` 命令保持原样，换机时按本机解释器调整。

`websocket-client` 是 `obsctl.py` 访问 obs-websocket v5 的客户端；只有需要**起停 OBS、改滤镜、截图**的脚本才会用到它。纯离线对拍（`verify_*_compiles.py`）不需要它，也不需要 OBS。

**`tools/` 目录不进仓库**（`.gitignore` 已挡）。人眼 A/B 那轮用过的 `tools/ffmpeg.exe`（约 64 MB）与 `tools/ffpkg.tgz`
（约 22 MB）是第三方二进制，与本仓库「二进制不入库」的规则（`docs/README.md` §3「工程」；旧编号 N12 见 `docs/INDEX.md` §4）同口径。需要重跑 A/B 时自备 ffmpeg
放到 `05_验证/tools/`；**当前没有任何脚本硬编码这个路径**（`ab_*.py` 全部走 Python 库与 OBS 录制，不外调 ffmpeg），
所以删掉它不影响任何门禁脚本，只影响手工造片。

> 这些脚本不是 argparse 写法，**没有 `--help`**。`gate.py --help` 只会把 `--help` 当选项滤掉、然后去扫日志；`probe_one.py --help` 会把它当浮点数并报错。参数说明看本文，或者直接看脚本头部。
>
> `gate.py` 无参运行时自动选用最近一份 OBS 日志：`py -3 05_验证\gate.py`；要指定日志就直接把路径传在后面。加 `--skip-transfer` 切到「A+C 合成口径」。

## 二、按用途分类

| 用途 | 脚本 | 要不要 OBS |
|---|---|---|
| 离线对拍（着色器编译 / 像素对齐） | `verify_tone_compiles.py`、`verify_smooth_compiles.py`、`verify_detail_compiles.py`、`verify_warp_compiles.py`、`verify_mask_compiles.py`、`verify_region_compiles.py` | 否 |
| 门禁判定与配对 A/B | `gate.py`、`ab_pair.py` | 门禁读日志：否；采集：是 |
| 采集 / 长跑 / 正确性 | `s0_lock.py`、`soak_s2.py`、`verify_s1_correct.py`、`slope_smooth.py` | 是 |
| **改代码前后画面是否一致（9.45 / P1-2）** | **`smooth_selfcheck_probe.py --coll beauty_static --cfg blur --tag before`**：起 OBS（自带 `BEAUTY_GPU_SMOOTH=1`）→ 设参数 → 等 → 优雅退出 → 从日志抽 `GPU 磨皮对拍` 一行解析成八字段，落 `out/smooth_selfcheck_<tag>.json`（含 dll md5 / size / mtime）。**用法**：同一 dll 跑两次确认可复现 → 换 dll 再跑两次 → 八字段逐项比；要端到端证据再加 `ab_record.py --lossless` 逐像素比。**三个坑**：① 场景必须让掩码 **≥3%**（`beauty_static` 17.8% 可以；`beauty_real_ab720p60` 只有 2.49% → 走「弱效果兜底」→ `maxdiff=255` **假失败**，而且插件会当场卸掉磨皮钩子）；② `taskkill /F` 会在 `.sentinel/` 留 `run_*` 实例锁，下次启动 OBS 卡在弹窗、websocket 永不监听（脚本已自动清锁 + `CloseMainWindow` 收尾）；③ `BEAUTY_SMOOTH_PASSES` **不覆盖自检路径**（passes=1/3/6 在该行上数字完全相同），趟数边界只能走 `ab_record.py` 录屏。**`判定=通过` 只是宽门限（maxdiff≤2 且 平均偏差<0.3），不能当「画面没变」** | 是 |
| 素材制作 | `make_assets.py`、`make_motion_assets.py`、`make_static_collection.py`、`make_front_assets.py`、`make_front_collection.py`、`make_motion_collection.py`、`scan_frontal_faces.py` | 否（建集合时 OBS 必须关） |
| **方案C 边界（三族素材 + 判定复算）** | **`motion_model.py`（2.11.1，先看这支）**：引擎「运动判定」的 CPU 复算，全库唯一实现 —— 亮度 `(54r+183g+19b)>>8`、**两段缩放**（整帧 letterbox 进 640×640 再缩 64×64，不是一步到位）、16 格取格内最大 → 4×4 块均值 → 取 16 块最大。输出「判动%」就是 OBS 里 `判动 hit/calls` 的离线预测（实测偏差 +7 个百分点，离线偏保守）。**`make_flash_assets.py`**：空场里每 60 帧闪出人脸 k 帧（k=0/1/2/4/8）；**`make_dropout_assets.py`**：反向 —— 脸持续在场、每 60 帧消失 k 帧（k=1/2/4），这族才压得到方案C；**`make_camera_assets.py`（2.11.1）**：从真实画面出发逐项叠加噪声/手抖/AGC，分离各扰动的判动贡献；**`flash_probe.py --family flash\|drop` 或 `--coll NAME --canvas WxH`** 跑 ON/OFF 两臂；**`calib_motion_stat.py`** 早期的三口径对照（已被 `motion_model.py` 取代，保留作历史） | 采集时是 |
| **块聚合口径（2.12.0 / P1-1c）** | **`calib_agg.py`**：一次解码同时算四种块聚合（`max` / `top4` / `rms` / `median`）的判动率，`--sweep` 追加阈值 5/6/7/8/10/12 下的判动率 —— 判断「换口径要不要跟着换阈值」就看它；**`face_box_drift.py`**：全片逐帧 YuNet，量「间隔 k 帧的两份检测框」位移、尺寸变化、IoU，并给出掩码羽化宽度（**0.08 × 脸宽**，2.13.0 据源码更正）作标尺；**`mask_lag_sim.py`**：复现 `detectPeriod`+`detectDue` 状态机 → 每帧掩码年龄 → 落到框序列 → 「掩码错多少像素」与「超羽化帧占比」，`--stills 8 6 4` 可一次扫多档周期、`--json` 落逐帧数据。三支都是离线，不启 OBS | 否（`face_box_drift` 要 onnx） |
| **位移预算 / P2-6 路 A（9.44）** | **`mask_budget_sweep.py`**：离线评「把帧数预算换成位移预算」能不能省往返。复用 `mask_lag_sim` 的检测结果（缓存 `out/budget_cache.npz`，重扫是秒级），只换状态机规则，扫三族 —— **R1** 判动+预算都用 `max`、**R2** 判动看 `rms` 预算记 `max`、**R3** 都用 `rms`；输出「提交率 / 掩龄 / 偏移 px / 超羽化%」。**自检是硬要求**：`budget=∞` 时必须逐值复现 `§9.34` 的 `max`（52.5% / 9.05 px / 0.5%）与 `rms`（10.2% / 16.84 / 7.9%）两行，不一致就退出。**判读**：`提交率`＝真正发生检测提交的帧占比，才是成本货币。结论（`§9.44`）：R1 必涨提交（死路），R3 被 R2 支配，R2@7–9 只省 13–23% 提交 → **不立项** | 否（要 onnx，不启 OBS） |
| **掩码偏移值多少灰阶（2.13.0）** | **`make_still_real.py`**：从真实素材里挑一帧「单脸 + 离边 ≥90 px + 脸最大」存 PNG（`assets/real_1080x1920.png`，配集合 `beauty_still_real`，`make_static_collection.py real` 建）；**`mask_off_probe.py --cfg blur\|combo\|strong`**：用 `BEAUTY_DET_OFFSET=dx,dy` 把掩码整体平移 d 像素，静态抓图相减 → 「≥1/2/4/8 灰阶各占多少面积、改动区平均/最大灰阶」，`d=0` 重复跑做阴性对照；**`mask_cost.py --cfg X --lag out/masklag_curve.json`**：把逐帧偏移喂进上面的曲线 → 「平均每帧脸上多大面积差几个灰阶」，并按 `detAdaptStill` 分档 | 是（要 OBS + `BEAUTY_DET_OFFSET`） |
| **人眼 A/B（9.35，2.13.0）** | **`make_ab_clip.py`**：造 A/B 素材 —— 从 `assets/cam_real_1080x1920.mp4` 取最坏段 `[1421,1661)`，前置黑/红/绿三连同步锚，并在**每帧右下角烧 8 位帧号条码**（`stamp_code` / `read_code`，白=1 黑=0，灰=不可靠返回 −1）；**`ab_record.py --cfg combo --arms max rms rms4`**：起 OBS 逐臂录（无损 I444，见下）；**`ab_match.py --a A.mp4 --b B.mp4`**：**按帧号条码配对**（不是按时间，避开两臂起点差 2.5 帧与各自的重复/跳帧），输出 ≥4/≥8 灰阶面积、单帧最大差、均值差，并报告各臂时间线的重复/跳帧；**`ab_make.py`**：出 `ab_side`/`ab_face`/`ab_flicker`/`ab_slow`/`ab_brow`/`ab_diff_x20` 六支片，可直接给人眼判；**`ab_keys.py`**：抽最差/中位/最静关键帧图；**`ab_ladder.py`**：可见度标尺 —— 按实测 σ 在真帧上叠 0/半幅/实幅/2 倍/4 倍高斯鼓包，1:1 与 2 倍横排，把「差几个灰阶」换成「多大才看得见」。`ab_align*.py` 是三种失败的对齐尝试（**已废弃，只留作记录**：单帧匹配/序列互相关/DP，DP 会因匹配区静止给出残差 0.055 的假象） | 录制：是；其余：否 |
| 录制前要改的 profile（人眼 A/B 专用） | `basic.ini`：`ColorFormat=NV12→I444`、`ColorRange=Partial→Full`。备份 `basic.ini.bak_i444`。**编码器不能靠 `basic.ini`**（2026-11-04 更正，§9.42.1）：`RecEncoder` / `RecQuality=Lossless` / `RecBitrate` 对 NVENC **都不生效**，实际参数落在 `basic/profiles/未命名/recordEncoder.json`（改成 `{"rate_control":"CRF","crf":0}` 后底噪才从 p50 **6.6** 降到 **0.00**，备份 `.bak_cbr`）。现成做法是给 `ab_record.py` 加 `--lossless`（走 `obs_x264`），**不要手改 profile**。**同臂两次逐像素差为 0** 是这套设置的验收线，改回有损编码前先复测噪声底 | — |
| **帧率对照 / 60 fps 格（9.42）** | **`ab_pair_amp.py`**：通用「按帧号配对 + 量带幅度」（`--crop` 限区域、`--strict` 剔时序歧义、`amp_*.json` 同时落 `per_frame` 与 `codes`，后者用来把「超门限」换算成「看第几秒」）；**`make_ab_clip.py`** 已参数化（`--src/--start/--end/--fps/--canvas`），条码位宽按帧数自适应（480 帧内容 → **9 格**，8 格会溢出）；**`ab_record.py --coll/--canvas/--fps/--lossless`** 逐臂录；**`ab_flicker_60fps.py`**：出闪烁片 —— 60 fps **每 6 帧**切才与 30 fps 每 3 帧的 100 ms 等时，`--fmt ffv1`（mkv，写盘 0 灰阶差）/ `x264`（mp4，2 灰阶），`--from-amp/--thr/--pad/--loop` 只循环超门限片段，写盘后自动抽检编码保真；**`--blank-code` 判读必开**（涂黑帧号条码 —— 条码每帧跳 15–30 灰阶，比臂间信号大一个量级，不涂就会把条码当成闪烁）；**`--frozen max|zero|<帧号>`** 冻结帧交替（同一帧号两臂固定切，内容静止、条码不变，只剩臂间差异，是灵敏度上限的干净测法；`zero` 取零差异帧做阴性对照）。**判读定论（PROGRESS §9.42.10）**：60 fps 仍「看得见」，阴性对照看不见 → 闪烁来自真实脸部差异，**帧率不是逃生门**。**不要再试 `cv2.VideoWriter` + FFV1 + mp4**：fourcc 与容器分开传、错组合只在运行时失败 | 录制：是；其余：否 |
| **帧节拍归因（9.35.7）** | **`pace_probe.py`**：量录制里源帧的真重复/跳帧。**两个口径必须分开**：真重复 = 码值相同 **且** 相邻帧像素差 < 0.2；码值相同但像素差 46–86 的是**条码误读**（`max` 臂 8 处全是这类）。归因臂在 `ab_record.py` 的 `ARMS` 里：`fix4`/`fix8`（`BEAUTY_MASK_EVERY=N` 会**自动关掉自适应**，把节奏钉死）、`rms30`（still=30）、`rms1`/`max64`（阈值对调，证明抖动跟 `dper` 走而不是跟 `agg` 走）。结论：重复率 ≈ 1/提交间隔，间隔为 1 时归零 | 录制：是 |
| **闪烁门限标定（9.42.11）** | **`ab_jnd_scale.py`**：把两臂的**真实差异按比例缩放** `B' = A + α·(B−A)`（α = 1.0 / 0.5 / 0.25 / 0.1），位置形状全不变、只有幅度线性缩小 —— 「哪一档开始看得见」就直接等于「这种形态的门限是多少灰阶」，不用再靠合成鼓包类比。**1:1 裁切 440×600 不缩放**（缩放会抹平细边，而细边正是 `rms` 差异的载体），裁切顺带把帧号条码挤出画面。每档 4.5 s = 0.5 静止 + 3.0 闪烁（5 Hz）+ 1.0 黑场，第 n 段闪烁起点 = `4.5n + 0.5` s；第 0 段 α=0 是阴性对照。默认 **FFV1 mkv**（`bgr0`，抽 10 对帧最大差 **0**）—— 最低档只有 **1.20 灰阶**，x264 的 2 灰阶取整比信号还大，mp4 便利版只够粗看。**已判读（§9.42.11）**：第 3 段（**6.02 灰阶**）开始可见、第 2 段（3.02）不可见 → 门限 **`(3.02, 6.02]`**，与 `ab_jnd.py` 的 `+6` 一致 | 否 |
| **边界带闪烁真机取证（9.46，已判不成立）** | **`edge_flicker_measure.py`**（主测量）：两臂按帧号条码配对 → `D=\|open−raw\|` → `ΔD=\|D(t)−D(t−1)\|`，7×7 摊平后取脸内 / 边界带 / 背景三区峰值，与 **6.02 灰阶**门限比；`--skip` 剔预热帧（素材源 20 对 / 静态 30 对）、按帧间位移分档、算与位移的相关性；`--static` 切静态图模式（固定脸框 `560,540,240,300`、不做条码配对）。**`edge_live_record.py`**：真机录制 —— 起 OBS → 枚举并写入采集设备 → 下发 `combo` → **就位检查**（判据是「有脸% + 边框%」，不达标当场停并留 `need_adjust.png`）→ 按蜂鸣节奏录三段；`--check-only` 只查不录、`--still-only` 只录静止段。**`edge_live_measure.py`**：真机单臂统计（三区 ΔI + 按帧号 mod 7 分相），**结论是不适合当判据**，留在仓库只为记下这个边界（注释写明原因）。**判读定论（`PROGRESS §9.46`）**：静态图两臂 **359 对帧** ΔD 三区全 **0.00**、超门限 0%，而效果幅度 D 脸内 8.15 / 边界带 3.55 / 背景 0.24 非零 → 输入不动时掩码一帧不抖，仿真片的「人为整体平移掩码」在真机上不存在 → **边界带立项不成立，不写代码**。素材源两臂（118 对帧）ΔD 与位移相关性 0.70、`<1 px` 档 max 5.08 不越门限，是「真素材全程在动」的量化起点。**真人在录课做不到静止，真人 `--still-only` 已取消且不重启**，改用静态图两臂作等价物 | 录制：是；测量：否 |
| **闪烁 JND 标尺（9.35.8）** | **`ab_jnd.py`**：把「看不看得出」换成「从第几档看得见」。同位置同形状的高斯鼓包（σ 由 FWHM 63 px 反推）按 0/6/9/12/15/18/24/36 灰阶逐档闪（5 Hz，每档 3 s），1:1 裁切不缩放。**第 0 档是阴性对照**（两臂完全相同），它若也「看见闪烁」则整片作废。段长 4.5 s，所以第 n 档的闪烁起点是 `4.5×(n−1)+0.5` 秒。**`ab_amp.py`**：把标尺档位与实测差异放到同一把尺上（校验标尺片 + 对三对录制逐帧算带幅度） | 否 |
| **幅度口径（务必统一，否则是两把尺子）** | 带幅度 = **逐像素取通道最大差 → 7×7 摊平 → 区域峰值**。同一对帧用灰度差会小 40%（12.76 vs 8.92），因为 `rms` 的掩码位移带动**色度**：一个通道涨、另一个跌，灰度加权后互相抵消。标尺上的鼓包是**三通道同加**的纯亮度变化，其通道最大 = 灰度差，所以只有通道最大口径能与标尺对齐。另注意：`ab_ladder`/`ab_jnd` 的「实幅」取的是**最差帧**，全序列中位数只有 7.04 | — |
| 单项度量 | `mask_probe.py`（掩码/有脸/往返）、`cpu_probe.py`（OBS 进程占用核数）、`count_faces.py`（素材多脸占比）、`cam_probe.py`（摄像头劣化输入观测） | 前三者按用途：`count_faces.py` 否，其余是 |
| **真实素材采集（2026-11-04）** | **`rec_material.py`**：ffmpeg 直接抓 dshow，**不走 OBS**（理由三条写在文件头：产品 profile 是无损 I444，1 分钟近 1 GB；采集卡 1080p 只有 MJPEG 通道，而 OBS 的 dshow 分辨率/格式是属性表**索引**，改集合 JSON 只能猜；滤镜必须关才能拿原始画面）。`--lead` 开场准备秒、`--sec1080/--sec720` 时长。两个参数是硬要求：`-pix_fmt yuvj420p` **原样透传**（不做全范围→有限范围转换，否则亮度被压一次），`-rtbufsize 256M`（默认 3 MB，1080p MJPEG 一帧就近 3 MB，会丢帧）。**连续长镜、不引导动作**；**`material_timeline.py`**：逐秒 YuNet 出「大脸/中脸/小脸/超小脸/多脸/遮挡」时间线 + ≥2 秒成段区间，用来按秒剪测试片段 | 采集时是（设备独占，OBS 必须关） |
| **遮挡格集合（2.14.0 后补）** | **`make_occ_collection.py`**：从 `beauty_rot_live`（1920×1080、源尺寸 = 画布、已挂 Beauty 滤镜）复制出 `beauty_real_cam_occ1080`，把 `image_source` 换成 `ffmpeg_source` 循环播放剪好的遮挡片段。**照抄会踩的坑**：模板里 scene 的 `custom_size` `cx/cy` 残留 **1080/1920**（它被 `V.start_obs(canvas=...)` 改过画布），不改就会把 1920×1080 的画面按竖屏裁切 —— 必须同步改成 1920/1080，并把 `pos_rel` 设 `(-1,-1)`。素材 `assets/capture/occ_1080p_86-120s.mp4`（原片 86–120 s，34 s，中脸 45.7% / 遮挡 54.3%），剪法与逐秒分布见 `PROGRESS §9.40` | 否 |

| 五官正确性对拍 | **`verify_region_samemask.py`（首选，2.8.3）**：插件内自检，同一帧、同一份掩码（geomOnly 帧读回 GPU 生产掩码当基准），`--samples/--delay/--only/--gpu-region`；`--only` 自 T10-b（2026-11-04）起含 **`threed`**（立体），并新增 `--strength` 覆盖五官强度（不给 = 沿用历史口径 eye/pouch/naso 50、threed 60、spot 40）。**默认口径不变，否则与 §9.20/§9.26 的数据不可比**；`verify_region_geomonly.py`（旧的截图对拍，**已证伪，只留作记录**） | 是 |
| **五官等价判据（T10-b）** | **`verify_region_equiv.py`（T10-b，2026-11-04）**：把 `verify_region_samemask.py` 的自检跑成**矩阵**并逐格判 PASS/FAIL，一次跑完 **17 格**（720p/4K × 立体/亮眼/眼袋/法令纹/祛斑/五族全开 × 强度 50/100 + 真人竖屏 6 帧 + **遮挡段 6 帧** + **超小脸三档**），末尾自动验「多脸回退」（`face_4k.png` 4 脸素材，`GPU_REGION=0/1` 两臂程序截图必须逐字节相同）。判据 E1–E5 见脚本头部，**超小脸格的 E2 下限按 `MIN_COV` 单独放宽**（掩码覆盖本来就只有 0.4%，按 1% 判会误杀自己，`§9.41.4`）；`--quick` 只跑立体+全开，`--dry-run` 不起 OBS，**`--match 遮挡` / `--match 超小脸` 只补某一批**（此时不跑多脸回退，报告文件名带后缀）。结果写 `out/logs/T10b_region_equiv[_<匹配词>]_<时间戳>.md`（脚本输出命名；历史 7 篇同主题报告已归档，见 [`../INDEX.md`](../INDEX.md) §2.1）；配套 `make_occ_collection.py` 造遮挡格集合、`make_tiny_collection.py` 造超小脸集合（截图缩放 + pad，不需要真人） | 是 |
| **开关两臂验证（2.14.0）** | **`verify_switch_arm.py`**：只回答一个问题 ——「环境变量到底有没有把开关拨到预期那一侧」。起 OBS 两次（**不设** / **显式设关闭值**），只匹配启动日志里那条一次性状态行，**不抓画面**（画面等价属各自的等价矩阵）。默认参数就是 2.14.0 的 `GPU_REGION`：A 不设 → 「GPU 生产链」、B `=0` → 「CPU 老路」。换别的开关用 `--switch/--off-value/--expect-on/--expect-off`；`--attach` 复用已在跑的 OBS（只验当前一侧）。补上《开关总表》回退原则第 3 条「开关本身也是被验收对象」一直缺的那件工具 | 是 |
| 祛斑正确性对拍 | **`verify_spot_pixels.py`（2.9.0）**：同一帧内容取「滤镜开 / 关」两张源截图逐像素比，只开祛斑一项。`--level` 是面板刻度 0..100（**0 档必须 0 像素**，作阴性对照）；`--probe N` 额外做 N 次时序采样并打印极值比（>1.2 即判帧间不稳）；`--env K=V`（可重复）开对照臂，例如 `--env BEAUTY_DETECT_SMALL=0 --env BEAUTY_MASK_GEOM_ONLY=0` 走有掩码帧。720p/4K 双档判据见 `docs/archive/log-20261103-p0-8-4k-s0recure.md`，帧类型对比见 `docs/archive/log-20261103-p0-9-geomonly-gate.md` | 是 |
| GPU 祛斑边界差离线复算 | **`analyze_spot_edge.py`（2.9.1）**：P0-10 用。从 `BEAUTY_M1_DUMP_DIR` 落盘的原图 + 掩码离线复算 CPU（截断窗口）与 GPU（clamp 重复）两种盒均值实现，验证「差异不随 k 变化、贴在 ROI 底边 2 像素带」这一结构性特征。复算结果（177 px / maxdiff 4 / 平均偏差 2.23 / bbox 一致）与插件内实测逐项对上。属诊断工具，**不进装机** | 否 |
| 输入旋转验收 | **2.10.0 实测过的方法**：判据不是「像素差多少」而是**效果落在哪里** —— `footprint = |输出 − 输入源图| > 0`，用「躺下就检不到」的 **3/4 侧脸**做阴性对照（正放 vs 躺放 + `input_rot=0/90/270`），比 footprint 面积与 IoU。实测：正放 14.30%、躺放 `ir=0` **0.24%**、`ir=90` **12.83%**、`ir=270` 0.23%。回归仍用 `verify_s1_correct.py capture` 六配置（`input_rot=0` 必须与上一版 `maxdiff=0`）。记录见 `docs/archive/log-20261104-input-rot-2.10.0.md` | 是 |
| **输入旋转实机验收（2.13.1）** | **`rot_live_check.py`**：起 OBS、给采集源挂滤镜、自动切 `ir=0/90/180/270` 四臂，取 perf 行的「有脸 %」判「旋转有没有进全帧检测路径」。**判据是臂间差距，不是绝对值**：正放臂 ≈100%、三个旋转臂 ≈0 即 PASS（2.13.0 及以前四臂会给同一个数）。三个坑：① 「掩码 %」在只开五官时恒为 0（`gonly=180`，纯几何掩码），不能当判据；② 真人低头/抬头会让同一臂内摆动 35 个百分点；③ 实机 footprint 必须用 `>2` 阈值，否则传感器噪声让它恒为 96%。`--attach` 可复用已开的 OBS。记录见 `PROGRESS §9.36` | 是 |
| 采集前置检查 | `gpu_guard.py`（显卡是否干净，被 `start_obs()` 自动调用） | 否 |
| **发布一致性** | **`check_release.py`（2.11.0，3.0.1 起 12 条）**：不需要 OBS、不需要画面，**12 条 PASS**（3.0.0 及以前是 9 条）—— ① 版本单源（`BEAUTY_BUILD_STR` 只许一处定义）；② **在 dll 二进制里搜版本字符串**（ASCII + UTF-16LE，撞版本号当场暴露）；③ 装机 dll == 构建 dll；④ `07_安装包` 载荷 == 构建 dll；⑤ 源码 / 安装包 / 装机 / solo **四份 6 个 `.hlsl` 逐字节相同**（多一个文件也算 FAIL）；⑥ 开关表 ⊆ 源码 `getenv()`，且「源码有、表里没有、别的文档提到」= 0。**3.0.1 新增 3 条 Setup.exe 断言**：安装路径由 `%PROGRAMDATA%` 派生、Setup.exe 内含当前版本串、内嵌 dll + 6 shader 与磁盘 payload 逐字节相同（修「装了等于没装」）。`--strict` 把 WARN 也算失败（发布前用），`--no-installed` 只查仓库。**打完 `Setup.exe` 之后跑**，当前 `--strict` 为 **12 PASS / 0 WARN / 0 FAIL**；见 `PROGRESS §9.30` | 否 |

> `cpu_probe.py` 按 `GetProcessTimes` 采 `obs64.exe` 的 CPU 时间换算出核数，用来量「省不省 CPU」这类
> 不体现在帧耗时里的改动（S7 的全部立论就在这里）。用法：
>
> ```bat
> py -3 05_验证\cpu_probe.py --seconds 45 --every auto,4,8
> ```
>
> `auto` = 不设 `BEAUTY_MASK_EVERY`，走 2.8.0 的运动自适应默认；整数 = 锁死固定周期（**这个动作本身会关掉自适应**）。
> 末尾固定多跑一档「关滤镜」作下限。静止 720p 参考值见 `PROGRESS §9.13.1`。
>
> **2.8.0 起新增两个日志字段**（性能行行尾）：`dper=` 窗口内最大提交周期（0 = 每帧）、
> `dmot=` 窗口内最大运动量（块平均亮度差）。判「自适应有没有生效」看这两个，不要看毫秒数——
> 检测在 worker 上，渲染线程的耗时不变是正常的。

> `mask_probe.py` 当前只能读「有脸 / 往返」两项：本构建的 `性能` 行里「掩码」字段恒为 `0.00%`（侧脸素材同样为 0），
> 所以它**不能**用来比较掩码覆盖率，别拿这个数下结论。判「素材有没有被认出」，看「有脸」与 `faces` 取值。

## 三、几条容易踩的坑

1. **收益只认配对**：`gate.py --skip-transfer` 判的是健康项与引擎项，跨会话的搬运项不可比；单次绝对值只能看趋势，收益结论必须来自 `ab_pair.py` 的同会话交替 A/B。
2. **采集前先确认数据有效**：日志里 `engine=gpu` 且 `S/T/R/D/W` 非 0，否则那一轮测的是 CPU 路径，数字不能用。
3. **`对齐%` 在 2.8.0 之后不能当静态采集的质量指标**：静止时检测每 8 帧才提交一次，
   对齐率会从 100% 掉到 13%，这是设计后果不是回归 —— 静态内容下新旧掩码逐像素相同，
   已用 `ADAPT=0` vs `ADAPT=1` 的五组 `maxdiff=0` 证明。要判质量用 `maxdiff`，不用对齐率。
4. **采集前显卡必须是干净的**。`gpu_guard.py` 已在 `start_obs()` 里自动调用：有重度 GPU
   程序在跑（DaVinci Resolve 等），或显存已用超过 3500 MiB，就直接拒绝启动，不再往下测。
   单独看一眼当前状态：

   ```bat
   py -3 05_验证\gpu_guard.py
   ```

   本机实测（RTX 5060 Laptop，8151 MiB，2026-11-03）：桌面空闲 1.3~2.4 GB 已用；
   DaVinci Resolve 一开就 5 GB 以上。问题不在「会不会卡」，而在**测出来的帧时会虚高**，
   `gate.py` 会把它判成「引擎超差 FAIL」—— 那时人去翻源码找的是一个不存在的问题。

   > 闸只在**启动前**查一次。长跑中途开 DaVinci 它看不见，只能靠人。
   > 放行开关：`BEAUTY_GPU_BUDGET_MB=6000`（改预算）或 `BEAUTY_NO_GPU_GATE=1`（完全跳过）。
   > 后者一旦用了，那一轮数据**不能进基线**。这两个变量只有验证脚本读，插件不读。
5. **门禁表下面那行「健康项(最坏窗口)」要一起看**。`gonly / gskip / tskip` 取的是
   **最坏窗口**而不是中位数 —— 中位数会把单窗的故障平均成 0。

   | 字段 | 正常值 | 非 0 意味着什么 |
   |---|---|---|
   | `gonly` | 180（S4a 起就是这个） | 本窗走 geomOnly：只建几何、整帧不回读，掩码由 GPU 重画 |
   | `gskip` | **0** | geomOnly 但 GPU 建图失败 → 整帧跳过。非 0 即 FAIL，掩码 RT/shader 出问题了 |
   | `tskip` | **0** | 缩略图帧认领不全 → 整帧跳过。阈值 3（≈1.5 帧）；**一帧记两次**，所以 `tskip=10` ≈ 5 帧 |

   这三项 2026-11-03 之前**根本没进 `gate.py` 的判定**（`gate_rows()` 没把字段传给
   `--cmp`），健康项恒 PASS。已修，见 `PROGRESS §9.15.3`。
6. **判 async 生效，别看段认领里的 `D`**。`S/T/R/D/W` 依次是
   磨皮/色调/五官/细节/美形（`BEAUTY_SEG_*`），`D` 是**细节段**，跟"延迟"无关。
   判 S3 只看三处：启动日志的「暂存面双缓冲回读生效」、S4b 判据行的 `async=1`、
   以及全窗 `tsm=0`（缩略图路径被关掉，这才是互斥的实证）。我自己在 §9.15.1 用
   `D=0` 推过一回，是错的，见 `PROGRESS §9.16.1`。
7. **「下载」在两套配置下不是同一种东西**。默认路径（S4b 缩略图）的 `下载` 约 1.6 ms，
   S3 路径的 `下载` 是全帧 4K memcpy，约 2.7 ms。跨配置横比这两个数没有意义，
   要比就比同一配置内的时间序列，或用 `ab_pair.py` 配对。
8. **源名/滤镜名写错不会报错，只会「什么都没发生」**。`soak_s2.py` 用的是静态集合那一套
   （`still` / `Beauty`）；拿它去驱动摄像头集合 `beauty_s1`（源 `视频采集设备`、滤镜 `OBS美颜`），
   `SetSourceFilterSettings` 会静默打偏，结果是 `blur=0`、`ALLZERO_SKIP` 生效、全程
   `有脸 0%`、`往返 0.00`，而脚本照样打印「存活」。**判别办法：看 `self-check` 那行的
   `blur=` 是不是你设的值**；不是就说明参数没下去，那一轮数据无效（`cam_probe.py` 已把这条做成读回断言）。
   另外刚连上 OBS 常回 `207 not ready`，必须重试 —— `soak_s2.apply_load` 重试 15 次就是这个原因。
9. **面板参数的单位是 0..100，不是 0..1**。`beauty_params.h` 的 `g_params` 注释写明
   「面板刻度统一 0..100，value = panel / 100 × max_val」，引擎侧一律 `panel / 100`。
   传 `eye_bright: 0.5` 等于把滑块拖到 0.5%，效果只剩 1%，取整后全被抹平。
   2026-11-03 之前 `soak_s2.LOAD_REGION` 就是 0..1 尺度，P0-7 的全部对拍都跑在 1% 强度上
   （见 `PROGRESS §9.19`）。`LOAD` 里 `thin_face / big_eye / chin` 仍是 0..1 的笔误，未修。
10. **五官段的截图对拍不能当判据**。它比的是两次独立启动 OBS 的画面；geomOnly 帧里
   CPU 侧的 `mask_` 是上一次非 geomOnly 帧的残留，GPU 侧用的是本帧几何重画的掩码，
   两者本来就不是同一份。证据：四个子项单独开，`maxdiff` 全是 8、bbox 完全相同、
   ≥8 的像素固定在同两点；关掉 geomOnly（`BEAUTY_MASK_GEOM_ONLY=0`）差异反而变大。
   要判五官算法，用 `verify_region_samemask.py`（同一个自检，2.8.3 起支持 geomOnly 帧）：
   它读回 GPU 本帧的生产掩码当 CPU 基准，比的是「同一份掩码 + 两种算法」。
   720p / 4K 各 5 样本 maxdiff 全 ≤1。详见 `docs/archive/log-20261103-p0-7-samemask.md`。
11. **参数要在起 OBS 之前写进集合 JSON，不能等起来再下发**。自检是「第一个满足条件的帧就
   取样」，而 OBS 从启动到 WebSocket 可用要 2~4 s —— 等 `apply_load` 到达，样本早取完了。
   症状是「参数明明改了，日志里却是旧值」：`--only spot` 把另外三项置 0，日志仍是
   `ellipses=6+2+2`。`verify_region_samemask.py` 已改为「改集合 → 起 OBS → 跑完原样写回」。
12. **`--zero`（五官全零）拿不到样本，不是工具坏**：`beauty_core.cpp:3619` 在五项全 0 时
   直接返回，几何快照 `faceCount=0`，自检按设计不跑。
13. **`spot=0` 曾经意味着祛斑这一族根本没进链**（P0-8，`PROGRESS §9.20.3`）。**2.9.0 已修**：
    geomOnly 帧沿用上次包围盒 / 用脸框兜底，且掩码全零时不做门控。现在默认配置下祛斑真的会改画面——
    判据用 `verify_spot_pixels.py`（0 档 0 像素、40/100 单调），不要再拿「拉动滑块看有没有变化」凭肉眼判。
14. **`--collection` 打偏是静默的，且坑在集合文件内部**。OBS 拿 `--collection` 去匹配集合 JSON 里的
    `name` 字段，**不是文件名**；`beauty_spot` / `beauty_spot4k` 是从 soak 集合复制出来的，内部
    `name` 还留着 `beauty_soak720` / `beauty_soak4k`，于是匹配不上、**回退到「上次用的集合」**。
    症状：画布日志是 `base resolution: 1280x720`，插件日志却是 `target=still 3840x2160`。
    这就是 4K 祛斑三轮「被上一个 OBS 会话接管」的真因（`PROGRESS §9.23.3`）。
    **判据只有一个**：插件日志的 `target=` 尺寸必须等于素材尺寸；`verify_spot_pixels.py` 已把它做成
    末行 WARN，并在打补丁时把内部 `name` 一并改成文件名（跑完原样写回）。
15. **想让祛斑走「有掩码帧」，必须同时关两个开关**：`BEAUTY_DETECT_SMALL=0`（不用 GPU 缩略图，
    令 `mapped=false`）**和** `BEAUTY_MASK_GEOM_ONLY=0`。只关后者无效 —— `beauty_core.cpp:2635`
    的判据是 `(mapped || g_maskGeomOnly)`，缩略图路径下 `mapped` 恒真。2.8.2 那轮「打开了却仍
    `gonly=180`」就是只关了后者（`PROGRESS §9.24.6`）。用法：
    `--env BEAUTY_DETECT_SMALL=0 --env BEAUTY_MASK_GEOM_ONLY=0`。
16. **报「改了多少像素」时先报分母**。P0-8 结案时写「47% vs 0.76%、差 7 倍」是错的：前者是
    `hit/ROI`、后者是 `diffpx/整张画面`，混在一起就凭空造出 7 倍。同口径实测是 1.58 倍
    （`PROGRESS §9.24.5`）。凡是跨来源比较，先确认 ROI / 分辨率 / 分母是否一致。
17. **判「某参数有没有生效」，先确认它所在的那条路径这一帧到底有没有走**。`input_rot`（2.10.0 时期）
    当时只在 S4b 缩略图路径生效：`thumb_prev_ok=0`（上一帧没被链全认领）、多脸、`BEAUTY_DETECT_SMALL=0`、
    `BEAUTY_ASYNC_STAGE=1`、mirror —— 任意一条成立，`input_rot` 就**完全不起作用，而且一行日志都没有**。
    最容易踩的是**开了五官参数**：REGION 段在 `BEAUTY_GPU_REGION` 未开时落 CPU，
    本帧不是「全段认领」→ 缩略图路径不启用 → 旋转失效。
    **这段已在 2.13.1 解除**（旋转提到 `letterboxRgb` 之前，`PROGRESS §9.36`）；**2.14.0 起 `GPU_REGION`
    默认开**，后面这条触发条件更不常出现。但**方法本身仍然适用**：任何「参数不起作用」先查路径，再改算法。
    验证方法：运行中 `set_filter_settings`
    把 `input_rot` 从 0 改到 90 再截一张，`maxdiff=0` 就说明它根本没进管线，别去改算法。
18. **两个版本的截图能不能比，先看渲染的是不是同一个场景**。同一个集合 JSON 被 OBS 在退出时改写后
    （`pos_rel` / `pos` 被重算），下一轮会整体平移/缩放：实测同一张图 `maxdiff=255、99.994%`，
    而版本差异真实值是 **0**。做法：起 OBS 前把集合文件恢复成同一份快照（`beauty_static.good.json`
    那种），两边都从同一份开始。
19. **判「掩码有没有抖」必须两臂、必须无损、必须剔预热帧**。单臂只能测画面自身的时域差 ΔI，
    脸部内容一动就有 **7.4 灰阶（p50，额头区）/ 25（p90）**，比 6.02 门限还高，掩码贡献被完全淹没；
    有损编码的底噪 p50 6.6 灰阶会把本该是「0.00」的结果抬成几灰阶。两臂录制统一走
    `ab_record.py --lossless`（`obs_x264` / CRF 0），`edge_flicker_measure.py --skip` 剔掉开头
    初始化帧（素材源 20 对、静态 30 对）。**另外「ΔD 超门限」不等于「闪烁」**：素材源两臂
    24.5% 超门限帧与帧间位移相关性 **0.70**，是跟着脸动的差异，不是原地来回闪；
    两臂也要先剔掉运动项才能谈闪烁（`PROGRESS §9.46.3` / `§9.46.4` 坑 4）。
20. **OBS websocket 配采集源的四件套，一个都不能靠猜**（`edge_live_record.py`，§9.46.4 坑 1/5/6/7）：
    ① 想「让 OBS 重选设备」而清空 `video_device_id` 会直接**黑屏**，必须
    `GetInputPropertiesListPropertyItems` 枚举真实 `itemValue` 再 `SetInputSettings`
    （本机 `UGREEN 35867 CH1` / `CH2` 是两个独立设备项，不能凭名字拼）；② 源名
    `视频采集设备`、滤镜名 `OBS美颜` 都是中文常量，照抄旧脚本的 `still` / `Beauty`
    拿不到句柄，下发参数「看似成功」却没进滤镜，表现又像「掩码 0% = 没检测到」；
    ③ `boundsType` 要传字符串 **`OBS_BOUNDS_STRETCH`**，传数字 `2` 读回仍是旧值；
    ④ 复制旧集合可能带 `rot=90`，而 `c.shot(源)` 是正向、`c.shot(场景)` 才是转 90° 的 ——
    **判据必须看画布截图**，几何三件套（`rot=0` / bounds / pos）一次改全。
21. **复用别人的检测器先验一次 `create()`；实验前提不成立就别硬跑**（§9.46.4 坑 8/9）。
    本机 cv2 是 **5.0.0**，新 ONNX importer 读不了 `yunet.onnx`：`FaceDetectorYN.create()`
    抛 `ONNXImporter2::parseFile ... Can't read ONNX file`（不是「类被删了」，是模型解析失败），
    `hasattr` 验不出这个。绕法是 `onnxruntime` 直跑模型、自己解 `cls_8 / obj_8 / bbox_8`
    三个 6400 锚点输出头。同理：`--still-only` 要求真人端坐 20 s 不说话 / 不比手势 /
    不转头，老师在录课时做不到 —— 该次录制取消且**不再重启**；这是**实验设计前提不成立**，
    不是工具失败，改用可重复、不占用人的静态图两臂。凡「要真人分秒配合」的方案，
    先确认场景允许再排设备。

## 四、素材与输出

- `assets/`：静态人脸图（`face_720p.png`、`face_4k.png` 及 `*_single` 单脸版）与逐帧匀速移脸素材（`motion_720p.mp4` / `motion_4k.mp4` 及同名 `json`）。
  > **⚠ 这批人脸图是四分之三侧脸**（头部左转约 30–40°），不是正脸。侧脸的人脸框更窄、关键点偏移，因此**覆盖率类指标与五官段几何不能外推到正面主持人**；耗时类结论不受影响（磨皮/色调/细节走全帧、draw 数固定）。
- `assets/face_front_4k.png`、`face_front_720p.png`：**正脸**验收素材（合成，单脸，主脸占画面高约 50%），配套集合 `beauty_front4k` / `beauty_front720`。来源与检出指标见 `face_front_meta.json`，复现命令：

  ```bat
  py -3 05_验证\make_front_assets.py <人像路径> --frac 0.50 --crop-bottom 100
  py -3 05_验证\make_front_collection.py
  py -3 05_验证\soak_s2.py soak --minutes 1 --collection beauty_front4k --canvas 3840x2160
  ```

  > **合成素材必须过肤色这一关**：插件用 YCbCr 椭圆判肤色（中心 `cb=-0.065 / cr=+0.095`），
  > AI 人像常因色度过低落在椭圆外，表现为**掩码 0%、`faces` 恒 0**，而 cv2 直接测却是 `faces=1`。
  > `make_front_assets.py` 已内置「色度校正」（把主脸区推到 r²≈0.20），并会打印校正前后的 r²。
  > 另外：合成皮肤的毛孔级细节少于真人，**不能拿它评价磨皮/清晰的主观观感**，只用于耗时、检出、链路三类结论。
- **`assets/capture/`：真实采集卡素材（2026-11-04，已 gitignore 不入库）**。设备 `UGREEN 25173`，
  采集侧 MJPEG，`-pix_fmt yuvj420p` 原样透传，libx264 crf 12，**无音轨**，未开滤镜（原始画面）。
  连续长镜、**未引导动作**（出镜人不接受指挥，全凭自然发挥）。

  | 文件 | 画布/fps | 时长 | 帧数 | 体积 | 中脸 | 大脸 | 遮挡/无脸 | 多脸 |
  |---|---|---|---|---|---|---|---|---|
  | `real_1080p30_20261104_122241.mp4` | 1920×1080 / 30 | 182.0 s | 5,461 | 619.6 MB | 78.7% | 7.1% | **10.4%（约 19 s）** | 3.3% |
  | `real_720p60_20261104_122241.mp4` | 1280×720 / 60 | 92.0 s | 5,521 | 82.6 MB | 94.6% | 1.1% | 0% | 4.3% |

  两段全片解码 **0 错误**，实际帧率 30.00 / 60.00（`material_timeline.py --step 1` 统计）。
  **可用区间**：遮挡段 `85–87s`、`99–105s`、`107–115s`（1080p 那条，来自手持书本/包装盒真实挡脸）；
  720p60 那条几乎全程单脸、可直接当 60 fps 输入。
  **它填的是 §9.37.6 的「遮挡未验」与 P2-5 的「60 fps 缺口」**；**超小脸与真侧脸它补不了**
  （1080p 那条小脸只占 0.5%）。三项后来都另走一条路收口（`PROGRESS §9.41` / `§9.42`）：
  超小脸改**离线缩放静图**（`make_tiny_collection.py`，不需要真人），侧脸 90° **判为输入域外**
  （该姿态下 YuNet 先失效，到不了五官段），60 fps 格用这条 720p60 素材做成
  `beauty_real_ab720p60` 集合跑完对照（`§9.42`：**同一素材 30 fps 换 60 fps，代价 p50 8.86→0.00**）。
  > 与 `face_front_*.png` 的区别：那是合成的正面大脸（脸占画面高约 50%），这是**真实主持人**、
  > 脸宽只占画面 5–10%。所以它的掩码覆盖率天然只有 1–3%，**拿它跑 `verify_region_equiv.py` 时
  > E2 的 1% 阈值会贴边** —— 不是判据变严，而是素材里脸本来就小，读数要连着覆盖率一起看。
- `out/`：每次验收一个子目录（如 `v271s4k`、`v271f720`、`v270f4k`），内含截图与 `_meta.json`；`_obs_screenshots/` 放整屏截图。
- **`out/live_edge/`（9.46 边界带取证，不入库）**：`2026-11-04 16-30-00.mp4` 是唯一一份
  真机无损录课素材（**1335 MB** / 71 s / 1920×1080@30 / `obs_x264` CRF 0）；
  `static_open.mp4` + `static_raw.mp4` 是静态图两臂（各约 0.8 MB / 359、360 帧，**主证据**，
  可随时用 `edge_flicker_measure.py --static` 复算）；`probe/*.png` 含分区图，
  `ready_src.png` / `ready_scene.png` / `need_adjust.png` 是「源正向、画布转 90°」坑的实证；
  `box_still.npz` / `static_box.npz` 是检测框几何缓存。结果 JSON 在
  `out/logs/edge_flicker_edge0.json`（素材源两臂 118 对帧）与 `edge_live_live3.json`（真机单臂）；
  素材源两臂视频在 `out/ab/edge_open.mp4`（74 MB）/ `edge_raw.mp4`（83 MB）。
  **本轮 <500 MB 非静态废片已删**（失败的预录、有损编码片、重复探针截图），保留
  `out/live_edge/` **1.4 GB** + `out/ab/` **2.2 GB**，清理后 D 盘实测余量 **41 GB**。
  `out/` 与 `assets/` 都在 `.gitignore` 内，大素材与两个结果 JSON 均不入库 ——
  文档里的数字是**已转录的结论**，换机复查需按上表重新取数（`PROGRESS §9.46.7`）。
- **运动素材的场景集合**：`beauty_motion720`（`motion_720p.mp4` 循环，1280×720），用
  `py -3 05_验证\make_motion_collection.py` 重建。它是 S7 这类「按运动自适应」改动的唯一
  验证手段 —— 静态素材看不出任何滞后，只能证明「不动时一致」。
- **长跑日志要自己留**：OBS 只保留最近 **10** 个日志，跑完十几轮 A/B 就会把长跑那份轮转掉。长跑一结束就把日志复制到 `out/logs/`（该目录仍在，现只剩 `.txt` / `.json` / `.png` 运行时产物），再启动下一个 OBS 实例。**原存于该目录下的 25 篇过程报告已在治理批 5 迁入 `docs/archive/log-YYYYMMDD-<主题>.md`**，按 [`../INDEX.md`](../INDEX.md) §2.1 检索旧名 → 新名映射；`out/logs/` 仍是脚本输出目录，新报告不入库。

## 附：过程日志归档（治理批 5）

> **结论**：`05_验证/out/logs/*.md` 的 25 篇验收过程日志已于 2026-11-05 归档为
> `docs/archive/log-YYYYMMDD-<主题>.md`，旧名 → 新名映射见 [`../INDEX.md`](../INDEX.md) §2.1。
> 归档件**正文冻结**，只加废弃头，不作为当前结论依据；本文表格里的数字是已转录的结论。
>
> `out/logs/` 仍是脚本输出目录，但新报告与同目录下的 27 篇 `.txt` / 13 篇 `.json` / 20 张 `.png`
> 均属运行时产物，保持 `.gitignore`，**不进文档体系、不入库**。
> 回档快照 `_回档备份_20261104_2249/` 的 4 篇文档另见 [`../INDEX.md`](../INDEX.md) §2.2，
> 其余 14 个二进制 / 源码文件仍在原目录。
