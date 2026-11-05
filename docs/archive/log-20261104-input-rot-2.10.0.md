> **已归档（2026-11-05）**：本文件是验收过程日志的历史快照，内容冻结，**正文一字未改**，不作为当前结论依据。
> 当前入口见 [`docs/README.md`](../README.md)，全库总表见 [`docs/INDEX.md`](../INDEX.md)；
> 结论已转录到 [`docs/guides/guide-verification.md`](../guides/guide-verification.md) 与 [`docs/CHANGELOG.md`](../CHANGELOG.md)。
> **原路径** `05_验证/out/logs/input_rot_2.10.0_20261104.md` 已失效；`out/logs/` 仍是脚本输出目录，新报告**不入文档体系**。
> 旧名 → 新名映射见 [`docs/INDEX.md`](../INDEX.md) §2。
> 正文的版本号、性能数字与判据是 2.8.x–2.10.0 当时快照，**不代表 3.0.1 实测**；当前行为以 [`docs/README.md`](../README.md) 与 `01_源码` 为准。

---

# 2.10.0 输入旋转验收（2026-11-04 05:41–06:0x）

被测 DLL：`572,928 B / MD5 C040187A05D55BFEEC18753CF118DD6A`，自报 `v2.10.0, engine=gpu/2.0.0`，
装机体 MD5 一致（`7_install_gpu.ps1` 报 `match=True`，6 个 hlsl OK）。
素材：`assets/face_front_720p.png`（正脸，1280×720）、`assets/face_720p.png`（3/4 侧脸，1280×720），
旋转素材由 `np.rot90(img, k=-1)`（顺时针 90°）生成，映射 `(x',y') = (719-y, x)`。

## 1. 编译

`02_构建/build_gpu.ps1`：`compile OK (11 files)`。2.10.0 提交里唯一的编译错误是
`obs-beauty.cpp:5134` 的 `gs_matrix_rotate3d(GS_AXIS_Z, rad)` —— libobs 32.1.1 没有这两个符号，
改为 `gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, rad)`（`graphics.h:533`，`obs.lib` 有该导出）。

## 2. 回归：input_rot=0 与 2.9.1 逐字节一致

`verify_s1_correct.py capture v291b` / `capture v210b`（同一份 `beauty_static` 集合快照，
六种配置），`compare v291b v210b`：

| 配置 | maxdiff | 变化像素 |
|---|---|---|
| allzero / off / blur 80 / clarity 80 / whiten 60+50 / shape | **0** | **0** |

即默认路径零差异（`input_rot` 默认 0，不进旋转分支）。

## 3. 功能：旋转素材能不能把人脸找回来

判据不是「像素差多少」，而是**效果落在哪里**（footprint = 输出与输入源图的差异像素集合）。
用 3/4 侧脸（躺下后 YuNet 检不到）做决定性对照，只开磨皮 60（保证全段被链认领 → 走 S4b 缩略图路径）：

| 臂 | 素材 | input_rot | footprint | 占画面 | IoU vs A |
|---|---|---|---|---|---|
| A | 正放 | 0 | 131,808 px | 14.30% | — |
| C | 躺放 | 0 | 2,202 px | 0.24% | **0.000** |
| B | 躺放 | **90** | 118,224 px | 12.83% | **0.472** |
| D | 躺放 | **270** | 2,155 px | 0.23% | 0.000 |

结论：
- `input_rot=90` **确实**把躺着的脸转正后检出，几何再映射回源坐标 —— 掩码落在脸上（footprint 是 C 的 54 倍）。
- 方向正确：同一张**顺时针 90°**素材，`90` 有效、`270` 无效（`270` 把人脸转成倒置 → 检不到）。
  面板「顺时针 90°」标签与实现一致。
- IoU 0.47 而非 ~1：两次运行的检测框来自不同的缩略图渲染（一次是原图直缩，一次是矩阵旋转后重采样），
  掩码是阈值化的软边皮肤区域，边界差几像素就让 13% 面积的 IoU 掉一半。定性看 footprint 对比即可。

## 4. 发现的两个限制（都是设计边界，不是 bug，但必须写进文档）

1. **只在 S4b 缩略图路径生效。** 旋转发生在 `beauty_thumb_build()`（检测缩略图）与
   `buildMaskInto()` 的 `mapped` 分支（几何反变换）。一旦这一帧走全帧检测 ——
   `thumb_prev_ok=0`（上一帧没被链全认领）、多脸、`BEAUTY_DETECT_SMALL=0`、
   `BEAUTY_ASYNC_STAGE=1`、mirror —— `input_rot` 就**完全不起作用，且没有任何日志**。
2. **开五官参数时默认失效。** 五官段（REGION）在 `BEAUTY_GPU_REGION` 未开（默认）时落 CPU，
   本帧不是「全段认领」→ `chain_out_valid=0` → 缩略图路径不启用 → `input_rot` 失效。
   实测：同一张正脸、躺放，`input_rot=0/90/270` 三臂输出**逐字节相同**（`R0 vs R90: maxdiff=0`），
   直到把五官参数清零、只留磨皮才复现出上表的差异。

   这两条合起来意味着：**竖屏摄像头用户只要开了任一五官/美形参数，输入旋转就不生效**。
   回退路径（多脸/首帧）同理。后续要么把旋转做进全帧检测路径（`letterboxRgb` 之前转），
   要么在链认领不全时给出一次性 WARN。

## 5. 复现命令

```bat
py -3 05_验证\verify_s1_correct.py capture v291b        :: 先装 2.9.1
py -3 05_验证\verify_s1_correct.py capture v210b        :: 再装 2.10.0
py -3 05_验证\verify_s1_correct.py compare v291b v210b
```

旋转臂的集合（`beauty_rot0` / `beauty_rot90` / `beauty_rot90_270`）由「同一张素材 + `input_rot`
写进集合 JSON」生成 —— **参数必须在起 OBS 之前写进集合**（见 `05_验证/README.md` 第 11 条）。
