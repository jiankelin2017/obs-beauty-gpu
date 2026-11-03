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
| 单项度量 | `mask_probe.py`（掩码/有脸/往返）、`cpu_probe.py`（OBS 进程占用核数）、`count_faces.py`（素材多脸占比）、`cam_probe.py`（摄像头劣化输入观测） | 前三者按用途：`count_faces.py` 否，其余是 |
| 五官正确性对拍 | **`verify_region_samemask.py`（首选，2.8.3）**：插件内自检，同一帧、同一份掩码（geomOnly 帧读回 GPU 生产掩码当基准），`--samples/--delay/--only/--gpu-region`；`verify_region_geomonly.py`（旧的截图对拍，**已证伪，只留作记录**） | 是 |
| 祛斑正确性对拍 | **`verify_spot_pixels.py`（2.9.0）**：同一帧内容取「滤镜开 / 关」两张源截图逐像素比，只开祛斑一项。`--level` 是面板刻度 0..100（**0 档必须 0 像素**，作阴性对照）；`--probe N` 额外做 N 次时序采样并打印极值比（>1.2 即判帧间不稳）；`--env K=V`（可重复）开对照臂，例如 `--env BEAUTY_DETECT_SMALL=0 --env BEAUTY_MASK_GEOM_ONLY=0` 走有掩码帧。720p/4K 双档判据见 `out/logs/P0-8_4k_S0recure_20261103.md`，帧类型对比见 `out/logs/P0-9_geomonly_gate_20261103.md` | 是 |
| 采集前置检查 | `gpu_guard.py`（显卡是否干净，被 `start_obs()` 自动调用） | 否 |

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
- `out/`：每次验收一个子目录（如 `v271s4k`、`v271f720`、`v270f4k`），内含截图与 `_meta.json`；`_obs_screenshots/` 放整屏截图。
- **运动素材的场景集合**：`beauty_motion720`（`motion_720p.mp4` 循环，1280×720），用
  `py -3 05_验证\make_motion_collection.py` 重建。它是 S7 这类「按运动自适应」改动的唯一
  验证手段 —— 静态素材看不出任何滞后，只能证明「不动时一致」。
- **长跑日志要自己留**：OBS 只保留最近 **10** 个日志，跑完十几轮 A/B 就会把长跑那份轮转掉。建议长跑一结束就把日志复制到 `out/logs/`（该目录当前不存在，首次使用先 `mkdir 05_验证\out\logs`），再启动下一个 OBS 实例。
