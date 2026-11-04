# 05_验证 —— 门禁与采集脚本

> 本目录的脚本用于**离线对拍**与**装机后验收**。依赖见同目录 `requirements.txt`。

## 一、三条命令起环境

```bat
py -3 -m pip install -r 05_验证\requirements.txt
py -3 -c "import websocket, PIL, numpy; print('deps ok')"
py -3 05_验证\verify_smooth_compiles.py
```

第一条装依赖，第二条自检（能打印 `deps ok: <版本>` 即为成功），第三条是不需要 OBS 的着色器编译对拍，跑通即说明环境可用。

`py -3` 指 Python 3.9+ 的启动器形式（Windows 官方安装包自带）。若机器上只有 `python` 没有 `py`，把命令里的 `py -3` 换成 `python`，其余不变。

`websocket-client` 是 `obsctl.py` 访问 obs-websocket v5 的客户端；只有需要**起停 OBS、改滤镜、截图**的脚本才会用到它。纯离线对拍（`verify_*_compiles.py`）不需要它，也不需要 OBS。

**`tools/` 目录不进仓库**（`.gitignore` 已挡）。人眼 A/B 那轮用过的 `tools/ffmpeg.exe`（约 64 MB）与 `tools/ffpkg.tgz`
（约 22 MB）是第三方二进制，与本仓库「二进制不入库」的规则（根 `README.md` N12）同口径。需要重跑 A/B 时自备 ffmpeg
放到 `05_验证/tools/`；**当前没有任何脚本硬编码这个路径**（`ab_*.py` 全部走 Python 库与 OBS 录制，不外调 ffmpeg），
所以删掉它不影响任何门禁脚本，只影响手工造片。

> 这些脚本不是 argparse 写法，**没有 `--help`**。`gate.py --help` 只会把 `--help` 当选项滤掉、然后去扫日志；`probe_one.py --help` 会把它当浮点数并报错。参数说明看本文，或者直接看脚本头部。
>
> `gate.py` 无参运行时自动选用最近一份 OBS 日志：`py -3 05_验证\gate.py`；要指定日志就直接把路径传在后面。加 `--skip-transfer` 切到「A+C 合成口径」。

## 二、按用途分三类

| 用途 | 脚本 | 要不要 OBS |
|---|---|---|
| 离线对拍（着色器编译 / 像素对齐） | `verify_tone_compiles.py`、`verify_smooth_compiles.py`、`verify_detail_compiles.py`、`verify_warp_compiles.py`、`verify_mask_compiles.py`、`verify_region_compiles.py` | 否 |
| 门禁判定与配对 A/B | `gate.py`、`ab_pair.py` | 门禁读日志：否；采集：是 |
| 采集 / 长跑 / 正确性 | `s0_lock.py`、`soak_s2.py`、`verify_s1_correct.py`、`slope_smooth.py` | 是 |
| 素材制作 | `make_assets.py`、`make_motion_assets.py`、`make_static_collection.py`、`make_front_assets.py`、`make_front_collection.py`、`make_motion_collection.py`、`scan_frontal_faces.py` | 否（建集合时 OBS 必须关） |
| **方案C 边界（三族素材 + 判定复算）** | **`motion_model.py`（2.11.1，先看这支）**：引擎「运动判定」的 CPU 复算，全库唯一实现 —— 亮度 `(54r+183g+19b)>>8`、**两段缩放**（整帧 letterbox 进 640×640 再缩 64×64，不是一步到位）、16 格取格内最大 → 4×4 块均值 → 取 16 块最大。输出「判动%」就是 OBS 里 `判动 hit/calls` 的离线预测（实测偏差 +7 个百分点，离线偏保守）。**`make_flash_assets.py`**：空场里每 60 帧闪出人脸 k 帧（k=0/1/2/4/8）；**`make_dropout_assets.py`**：反向 —— 脸持续在场、每 60 帧消失 k 帧（k=1/2/4），这族才压得到方案C；**`make_camera_assets.py`（2.11.1）**：从真实画面出发逐项叠加噪声/手抖/AGC，分离各扰动的判动贡献；**`flash_probe.py --family flash\|drop` 或 `--coll NAME --canvas WxH`** 跑 ON/OFF 两臂；**`calib_motion_stat.py`** 早期的三口径对照（已被 `motion_model.py` 取代，保留作历史） | 采集时是 |
| **块聚合口径（2.12.0 / P1-1c）** | **`calib_agg.py`**：一次解码同时算四种块聚合（`max` / `top4` / `rms` / `median`）的判动率，`--sweep` 追加阈值 5/6/7/8/10/12 下的判动率 —— 判断「换口径要不要跟着换阈值」就看它；**`face_box_drift.py`**：全片逐帧 YuNet，量「间隔 k 帧的两份检测框」位移、尺寸变化、IoU，并给出掩码羽化宽度（**0.08 × 脸宽**，2.13.0 据源码更正）作标尺；**`mask_lag_sim.py`**：复现 `detectPeriod`+`detectDue` 状态机 → 每帧掩码年龄 → 落到框序列 → 「掩码错多少像素」与「超羽化帧占比」，`--stills 8 6 4` 可一次扫多档周期、`--json` 落逐帧数据。三支都是离线，不启 OBS | 否（`face_box_drift` 要 onnx） |
| **掩码偏移值多少灰阶（2.13.0）** | **`make_still_real.py`**：从真实素材里挑一帧「单脸 + 离边 ≥90 px + 脸最大」存 PNG（`assets/real_1080x1920.png`，配集合 `beauty_still_real`，`make_static_collection.py real` 建）；**`mask_off_probe.py --cfg blur\|combo\|strong`**：用 `BEAUTY_DET_OFFSET=dx,dy` 把掩码整体平移 d 像素，静态抓图相减 → 「≥1/2/4/8 灰阶各占多少面积、改动区平均/最大灰阶」，`d=0` 重复跑做阴性对照；**`mask_cost.py --cfg X --lag out/masklag_curve.json`**：把逐帧偏移喂进上面的曲线 → 「平均每帧脸上多大面积差几个灰阶」，并按 `detAdaptStill` 分档 | 是（要 OBS + `BEAUTY_DET_OFFSET`） |
| **人眼 A/B（9.35，2.13.0）** | **`make_ab_clip.py`**：造 A/B 素材 —— 从 `assets/cam_real_1080x1920.mp4` 取最坏段 `[1421,1661)`，前置黑/红/绿三连同步锚，并在**每帧右下角烧 8 位帧号条码**（`stamp_code` / `read_code`，白=1 黑=0，灰=不可靠返回 −1）；**`ab_record.py --cfg combo --arms max rms rms4`**：起 OBS 逐臂录（无损 I444，见下）；**`ab_match.py --a A.mp4 --b B.mp4`**：**按帧号条码配对**（不是按时间，避开两臂起点差 2.5 帧与各自的重复/跳帧），输出 ≥4/≥8 灰阶面积、单帧最大差、均值差，并报告各臂时间线的重复/跳帧；**`ab_make.py`**：出 `ab_side`/`ab_face`/`ab_flicker`/`ab_slow`/`ab_brow`/`ab_diff_x20` 六支片，可直接给人眼判；**`ab_keys.py`**：抽最差/中位/最静关键帧图；**`ab_ladder.py`**：可见度标尺 —— 按实测 σ 在真帧上叠 0/半幅/实幅/2 倍/4 倍高斯鼓包，1:1 与 2 倍横排，把「差几个灰阶」换成「多大才看得见」。`ab_align*.py` 是三种失败的对齐尝试（**已废弃，只留作记录**：单帧匹配/序列互相关/DP，DP 会因匹配区静止给出残差 0.055 的假象） | 录制：是；其余：否 |
| 录制前要改的 profile（人眼 A/B 专用） | `basic.ini`：`ColorFormat=NV12→I444`、`ColorRange=Partial→Full`、`RecEncoder=none→obs_nvenc_h264_tex` + `RecQuality=Lossless`。备份 `basic.ini.bak_i444`。**同臂两次逐像素差为 0** 是这套设置的验收线，改回有损编码前先复测噪声底 | — |
| **帧节拍归因（9.35.7）** | **`pace_probe.py`**：量录制里源帧的真重复/跳帧。**两个口径必须分开**：真重复 = 码值相同 **且** 相邻帧像素差 < 0.2；码值相同但像素差 46–86 的是**条码误读**（`max` 臂 8 处全是这类）。归因臂在 `ab_record.py` 的 `ARMS` 里：`fix4`/`fix8`（`BEAUTY_MASK_EVERY=N` 会**自动关掉自适应**，把节奏钉死）、`rms30`（still=30）、`rms1`/`max64`（阈值对调，证明抖动跟 `dper` 走而不是跟 `agg` 走）。结论：重复率 ≈ 1/提交间隔，间隔为 1 时归零 | 录制：是 |
| **闪烁 JND 标尺（9.35.8）** | **`ab_jnd.py`**：把「看不看得出」换成「从第几档看得见」。同位置同形状的高斯鼓包（σ 由 FWHM 63 px 反推）按 0/6/9/12/15/18/24/36 灰阶逐档闪（5 Hz，每档 3 s），1:1 裁切不缩放。**第 0 档是阴性对照**（两臂完全相同），它若也「看见闪烁」则整片作废。段长 4.5 s，所以第 n 档的闪烁起点是 `4.5×(n−1)+0.5` 秒。**`ab_amp.py`**：把标尺档位与实测差异放到同一把尺上（校验标尺片 + 对三对录制逐帧算带幅度） | 否 |
| **幅度口径（务必统一，否则是两把尺子）** | 带幅度 = **逐像素取通道最大差 → 7×7 摊平 → 区域峰值**。同一对帧用灰度差会小 40%（12.76 vs 8.92），因为 `rms` 的掩码位移带动**色度**：一个通道涨、另一个跌，灰度加权后互相抵消。标尺上的鼓包是**三通道同加**的纯亮度变化，其通道最大 = 灰度差，所以只有通道最大口径能与标尺对齐。另注意：`ab_ladder`/`ab_jnd` 的「实幅」取的是**最差帧**，全序列中位数只有 7.04 | — |
| 单项度量 | `mask_probe.py`（掩码/有脸/往返）、`cpu_probe.py`（OBS 进程占用核数）、`count_faces.py`（素材多脸占比）、`cam_probe.py`（摄像头劣化输入观测） | 前三者按用途：`count_faces.py` 否，其余是 |
| **真实素材采集（2026-11-04）** | **`rec_material.py`**：ffmpeg 直接抓 dshow，**不走 OBS**（理由三条写在文件头：产品 profile 是无损 I444，1 分钟近 1 GB；采集卡 1080p 只有 MJPEG 通道，而 OBS 的 dshow 分辨率/格式是属性表**索引**，改集合 JSON 只能猜；滤镜必须关才能拿原始画面）。`--lead` 开场准备秒、`--sec1080/--sec720` 时长。两个参数是硬要求：`-pix_fmt yuvj420p` **原样透传**（不做全范围→有限范围转换，否则亮度被压一次），`-rtbufsize 256M`（默认 3 MB，1080p MJPEG 一帧就近 3 MB，会丢帧）。**连续长镜、不引导动作**；**`material_timeline.py`**：逐秒 YuNet 出「大脸/中脸/小脸/超小脸/多脸/遮挡」时间线 + ≥2 秒成段区间，用来按秒剪测试片段 | 采集时是（设备独占，OBS 必须关） |
| **遮挡格集合（2.14.0 后补）** | **`make_occ_collection.py`**：从 `beauty_rot_live`（1920×1080、源尺寸 = 画布、已挂 Beauty 滤镜）复制出 `beauty_real_cam_occ1080`，把 `image_source` 换成 `ffmpeg_source` 循环播放剪好的遮挡片段。**照抄会踩的坑**：模板里 scene 的 `custom_size` `cx/cy` 残留 **1080/1920**（它被 `V.start_obs(canvas=...)` 改过画布），不改就会把 1920×1080 的画面按竖屏裁切 —— 必须同步改成 1920/1080，并把 `pos_rel` 设 `(-1,-1)`。素材 `assets/capture/occ_1080p_86-120s.mp4`（原片 86–120 s，34 s，中脸 45.7% / 遮挡 54.3%），剪法与逐秒分布见 `PROGRESS §9.40` | 否 |

| 五官正确性对拍 | **`verify_region_samemask.py`（首选，2.8.3）**：插件内自检，同一帧、同一份掩码（geomOnly 帧读回 GPU 生产掩码当基准），`--samples/--delay/--only/--gpu-region`；`--only` 自 T10-b（2026-11-04）起含 **`threed`**（立体），并新增 `--strength` 覆盖五官强度（不给 = 沿用历史口径 eye/pouch/naso 50、threed 60、spot 40）。**默认口径不变，否则与 §9.20/§9.26 的数据不可比**；`verify_region_geomonly.py`（旧的截图对拍，**已证伪，只留作记录**） | 是 |
| **五官等价判据（T10-b）** | **`verify_region_equiv.py`（T10-b，2026-11-04）**：把 `verify_region_samemask.py` 的自检跑成**矩阵**并逐格判 PASS/FAIL，一次跑完 **17 格**（720p/4K × 立体/亮眼/眼袋/法令纹/祛斑/五族全开 × 强度 50/100 + 真人竖屏 6 帧 + **遮挡段 6 帧** + **超小脸三档**），末尾自动验「多脸回退」（`face_4k.png` 4 脸素材，`GPU_REGION=0/1` 两臂程序截图必须逐字节相同）。判据 E1–E5 见脚本头部，**超小脸格的 E2 下限按 `MIN_COV` 单独放宽**（掩码覆盖本来就只有 0.4%，按 1% 判会误杀自己，`§9.41.4`）；`--quick` 只跑立体+全开，`--dry-run` 不起 OBS，**`--match 遮挡` / `--match 超小脸` 只补某一批**（此时不跑多脸回退，报告文件名带后缀）。结果写 `out/logs/T10b_region_equiv[_<匹配词>]_<时间戳>.md`；配套 `make_occ_collection.py` 造遮挡格集合、`make_tiny_collection.py` 造超小脸集合（截图缩放 + pad，不需要真人） | 是 |
| **开关两臂验证（2.14.0）** | **`verify_switch_arm.py`**：只回答一个问题 ——「环境变量到底有没有把开关拨到预期那一侧」。起 OBS 两次（**不设** / **显式设关闭值**），只匹配启动日志里那条一次性状态行，**不抓画面**（画面等价属各自的等价矩阵）。默认参数就是 2.14.0 的 `GPU_REGION`：A 不设 → 「GPU 生产链」、B `=0` → 「CPU 老路」。换别的开关用 `--switch/--off-value/--expect-on/--expect-off`；`--attach` 复用已在跑的 OBS（只验当前一侧）。补上《开关总表》回退原则第 3 条「开关本身也是被验收对象」一直缺的那件工具 | 是 |
| 祛斑正确性对拍 | **`verify_spot_pixels.py`（2.9.0）**：同一帧内容取「滤镜开 / 关」两张源截图逐像素比，只开祛斑一项。`--level` 是面板刻度 0..100（**0 档必须 0 像素**，作阴性对照）；`--probe N` 额外做 N 次时序采样并打印极值比（>1.2 即判帧间不稳）；`--env K=V`（可重复）开对照臂，例如 `--env BEAUTY_DETECT_SMALL=0 --env BEAUTY_MASK_GEOM_ONLY=0` 走有掩码帧。720p/4K 双档判据见 `out/logs/P0-8_4k_S0recure_20261103.md`，帧类型对比见 `out/logs/P0-9_geomonly_gate_20261103.md` | 是 |
| GPU 祛斑边界差离线复算 | **`analyze_spot_edge.py`（2.9.1）**：P0-10 用。从 `BEAUTY_M1_DUMP_DIR` 落盘的原图 + 掩码离线复算 CPU（截断窗口）与 GPU（clamp 重复）两种盒均值实现，验证「差异不随 k 变化、贴在 ROI 底边 2 像素带」这一结构性特征。复算结果（177 px / maxdiff 4 / 平均偏差 2.23 / bbox 一致）与插件内实测逐项对上。属诊断工具，**不进装机** | 否 |
| 输入旋转验收 | **2.10.0 实测过的方法**：判据不是「像素差多少」而是**效果落在哪里** —— `footprint = |输出 − 输入源图| > 0`，用「躺下就检不到」的 **3/4 侧脸**做阴性对照（正放 vs 躺放 + `input_rot=0/90/270`），比 footprint 面积与 IoU。实测：正放 14.30%、躺放 `ir=0` **0.24%**、`ir=90` **12.83%**、`ir=270` 0.23%。回归仍用 `verify_s1_correct.py capture` 六配置（`input_rot=0` 必须与上一版 `maxdiff=0`）。记录见 `out/logs/input_rot_2.10.0_20261104.md` | 是 |
| **输入旋转实机验收（2.13.1）** | **`rot_live_check.py`**：起 OBS、给采集源挂滤镜、自动切 `ir=0/90/180/270` 四臂，取 perf 行的「有脸 %」判「旋转有没有进全帧检测路径」。**判据是臂间差距，不是绝对值**：正放臂 ≈100%、三个旋转臂 ≈0 即 PASS（2.13.0 及以前四臂会给同一个数）。三个坑：① 「掩码 %」在只开五官时恒为 0（`gonly=180`，纯几何掩码），不能当判据；② 真人低头/抬头会让同一臂内摆动 35 个百分点；③ 实机 footprint 必须用 `>2` 阈值，否则传感器噪声让它恒为 96%。`--attach` 可复用已开的 OBS。记录见 `PROGRESS §9.36` | 是 |
| 采集前置检查 | `gpu_guard.py`（显卡是否干净，被 `start_obs()` 自动调用） | 否 |
| **发布一致性** | **`check_release.py`（2.11.0）**：不需要 OBS、不需要画面，六项九条判定 —— ① 版本单源（`BEAUTY_BUILD_STR` 只许一处定义）；② **在 dll 二进制里搜版本字符串**（ASCII + UTF-16LE，撞版本号当场暴露）；③ 装机 dll == 构建 dll；④ `07_安装包` 载荷 == 构建 dll；⑤ 源码 / 安装包 / 装机 / solo **四份 6 个 `.hlsl` 逐字节相同**（多一个文件也算 FAIL）；⑥ 开关表 ⊆ 源码 `getenv()`，且「源码有、表里没有、别的文档提到」= 0。`--strict` 把 WARN 也算失败（发布前用），`--no-installed` 只查仓库。**打完 `Setup.exe` 之后跑**，见 `PROGRESS §9.30` | 否 |

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
   720p / 4K 各 5 样本 maxdiff 全 ≤1。详见 `out/logs/P0-7_samemask_20261103.md`。
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
  （1080p 那条小脸只占 0.5%）。两项后来都另走一条路收口（`PROGRESS §9.41`）：
  超小脸改**离线缩放静图**（`make_tiny_collection.py`，不需要真人），侧脸 90° **判为输入域外**
  （该姿态下 YuNet 先失效，到不了五官段）。
  > 与 `face_front_*.png` 的区别：那是合成的正面大脸（脸占画面高约 50%），这是**真实主持人**、
  > 脸宽只占画面 5–10%。所以它的掩码覆盖率天然只有 1–3%，**拿它跑 `verify_region_equiv.py` 时
  > E2 的 1% 阈值会贴边** —— 不是判据变严，而是素材里脸本来就小，读数要连着覆盖率一起看。
- `out/`：每次验收一个子目录（如 `v271s4k`、`v271f720`、`v270f4k`），内含截图与 `_meta.json`；`_obs_screenshots/` 放整屏截图。
- **运动素材的场景集合**：`beauty_motion720`（`motion_720p.mp4` 循环，1280×720），用
  `py -3 05_验证\make_motion_collection.py` 重建。它是 S7 这类「按运动自适应」改动的唯一
  验证手段 —— 静态素材看不出任何滞后，只能证明「不动时一致」。
- **长跑日志要自己留**：OBS 只保留最近 **10** 个日志，跑完十几轮 A/B 就会把长跑那份轮转掉。建议长跑一结束就把日志复制到 `out/logs/`（该目录当前不存在，首次使用先 `mkdir 05_验证\out\logs`），再启动下一个 OBS 实例。
