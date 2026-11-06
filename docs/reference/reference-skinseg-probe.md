---
doc_type: reference
title: 分割探针（离线直调 skinsegRun）
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 06_离线验证/segtest/README.md
---

# segtest —— 离线直调 `skinsegRun` 的验证工程（P0-2）

> **结论**：不启 OBS 复现语义分割裁切，两条硬约束 —— **输入只能是 512**、**坐标有两套**
> （宿主缩略图像素系 vs 全帧几何系）。
> **适用条件**：`BEAUTY_SEG=1` 的分割影子层仍是进程级全局，**只支持单实例**
> （见 [`../README.md`](../README.md) §1）；本文实测数据均为单实例、512 输入口径，
> 不可外推到多机位。

不启动 OBS，直接编译 `beauty_skinseg.cpp` + `beauty_onnx.cpp`，喂一张图 + 一个人脸框，
把 512×512 的语义结果原样写盘，用 Python 还原成叠加图核对。

**为什么需要它**：OBS 里这条路只能在日志看到 `crop=1 uv=(...)`，看不到"裁到哪儿去了"。
渲染管线出问题时（比如 uv 退化成 `(0,0,1,1)`），离线复现是唯一能一眼看出来的办法。

## 用法

```powershell
# 1) 准备（ASCII 路径，cl 对中文路径不友好）
#    src\ 放 segtest.cpp / ortprobe.cpp + 两个真源码文件
#    inc\ 放 beauty_seg.h / beauty_face.h / beauty_onnx.h / onnxruntime_c_api.h
#    out\ 放 onnxruntime.dll + model.onnx（bisenet_face_dyn.onnx 改名）
.\build.ps1

# 2) 全分辨率路径（像素系 == 几何系）
.\out\segtest.exe in.rgba 1280 720 withface.bin 475 138.8 348.3 481.3 0.914

# 3) 缩略图路径（复现 OBS 真实链路：640×640 letterbox + 全帧人脸框）
.\out\segtest.exe thumb.rgba 640 640 thumb.bin 475 138.8 348.3 481.3 0.914 `
                  1280 720 0.5 0 140

# 4) 只测"某个输入尺寸会不会崩"（换模型/换 ORT 后必跑）
.\out\ortprobe.exe model.onnx 512 256
```

参数含义（3 之后的五个）：`x y w h score`（全帧几何坐标）。
再往后的五个（可省）：`geomW geomH mapScale mapOx mapOy`。

## 2026-11-05 实测结论（这两条是硬约束，别"优化"掉）

1. **输入只能是 512**。`bisenet_face_dyn.onnx` 只改了输入声明，图内部仍写死 512。
   用插件自带的 onnxruntime **1.20.1** 喂 256 / 288 / 320 / 384 会在 `Run` 内部
   **整数除零、整个进程崩掉**（`0xC0000094`），不是返回错误码。
   512 正常。Python 侧 ORT 1.31.1 喂 256 能跑，所以早前"256 也可以"的结论**只对
   pip 版成立**，对插件不成立。
2. **坐标有两套**。GPU 链开着时 worker 拿到的是宿主 640×640 缩略图，人脸框却是
   全帧几何坐标（`src = geom×mapScale + (ox,oy)`）。漏传映射的典型症状：
   `crop=1` 但 `uv=(0,0,1,1)` —— 看着"裁了"，其实喂的是整帧。

## 耗时（512，插件自带 ORT 1.20.1）

| 线程 | 单次推理 |
|---|---|
| 1 | 276 ms |
| 4 | 95 ms |
| 8 | 57 ms |

配合 `BEAUTY_SEG_EVERY=12`（30 fps ≈ 400 ms 一次）。

## 证据图

- `evidence_face_overlay.png` —— 全分辨率裁切：皮肤概率压在脸上，眉/眼/唇/鼻被排除。
- `evidence_thumbpath_overlay.png` —— 缩略图路径，与上图统计几乎一致
  （脸 R=0.87 / 背景 0.01 / 衣服 0.005 / 头发 0.60），说明缩略图分辨率不是瓶颈。

## 2026-11-05 新增：两个模型档位

| 档位 | 模型 | 输入 | 类数 | 体积 | 4 线程耗时 |
|---|---|---|---|---|---|
| 默认 | `bisenet_face_dyn.onnx` | 512 NCHW | 19 | 53.2 MB | 98.1 ms |
| `selfie6` | `selfie_multiclass_256.onnx` | 256 NHWC | 6 | 16.4 MB | **45.6 ms** |

切档位只改环境变量，不动代码（`BEAUTY_SEG_PROFILE=selfie6` +
`BEAUTY_SEG_MODEL` 指向 `selfie_multiclass_256.onnx`）。

`selfie6` 的类表是 `0 背景 / 1 头发 / 2 身体皮肤 / 3 脸部皮肤 / 4 衣服 / 5 其他`，
皮肤只认 `face_skin`，所以手（被标成 `body_skin` 时）会被自动排除；
`BEAUTY_SEG_BODY=0.5` 可以把脖子（也是 `body_skin`）捞回来。

### ortprobe 现在支持任意形状

```powershell
\out\ortprobe.exe model.onnx 512              # 老写法：NCHW [1,3,512,512]
\out\ortprobe.exe selfie.onnx 1x256x256x3     # 新写法：含 x 即按完整形状解析
```

### 语义图渲染

`render_seg.py` 把 segtest 写出的 512×512 RGBA（R 皮肤 / G 五官 / B 非肤）画成
「原图 | R 热图 | B 热图 | R|B 叠加」四联图，省得每次再手写一遍：

```
py -3.12 render_seg.py <seg.rgba> <thumb.rgba> <thumbW> <out.png> <uv0> <uv1> <uv2> <uv3>
```

证据图：`evidence_bisenet_512.png`（19 类，五官被扣掉）、`evidence_selfie6_256.png`
（6 类，皮肤只压在脸上、脖子与头发干净）。
