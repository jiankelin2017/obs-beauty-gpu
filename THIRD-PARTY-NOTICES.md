# 第三方组件与许可声明

本文件登记 `obs-beauty` 用到或曾经用到的全部第三方组件，以及它们各自的许可与
分发状态。**它只记录事实，不代替法律意见**；涉及再分发（对外发布 dll / 安装包）
时，请按各组件许可原文逐条核对。

最后核对日期：2026-11-06。许可以上游仓库当时的分支内容为准，见每行「依据」列。

## 一、运行时依赖

| 组件 | 版本 | 许可 | 用途 | 是否入库 | 是否随安装包分发 | 依据 |
|---|---|---|---|---|---|---|
| ONNX Runtime | 1.20.1（API 版本 20） | MIT | 模型推理（YuNet / 分割 / 手部） | 否（`06_离线验证/onnxruntime.dll`，已被 `.gitignore` 挡住） | **是**：3.2.2 起内嵌进 `Setup.exe`，落到 `<root>\bin\64bit\` | 上游 `microsoft/onnxruntime` 仓库 LICENSE |
| YuNet 人脸检测模型 | 2023mar | MIT | 生产路径必需：人脸框 + 5 关键点 | **是**：`06_离线验证/models/yunet.onnx`<br>SHA256 `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4` | **是**：3.2.2 起内嵌，落到 `<root>\bin\64bit\models\` | `opencv/opencv_zoo` 下 `models/face_detection_yunet/LICENSE` 明确写「本目录所有文件为 MIT」，版权行 `Copyright (c) 2020 Shiqi Yu`；注意**仓库根目录是 Apache-2.0，与本目录不同**，引用时别混。上游训练方 `ShiqiYu/libfacedetection.train` |

## 二、仅离线实验使用（生产路径默认不加载）

| 组件 | 许可 | 用途 | 是否入库 | 依据 |
|---|---|---|---|---|
| BiSeNet 人脸解析（`bisenet_face.onnx` / `bisenet_face_dyn.onnx`，53 MB×2） | MIT | 皮肤/五官语义分割实验，`BEAUTY_SEG=1` 才加载 | 否 | `yakhyo/face-parsing` LICENSE（Copyright 2024 Yakhyokhuja Valikhujaev） |
| MediaPipe Selfie Multiclass 256（`selfie_multiclass_256.onnx`，16 MB） | Apache-2.0 | 同上，轻量替代方案 | 否 | MediaPipe 官方模型卡 |
| 手部检测（`hand.onnx` / `hand_landmark.onnx`，共 15.5 MB） | Apache-2.0（Copyright 2022 Katsuya Hyodo） | `BEAUTY_HAND_MASK` 遮挡实验，**默认关闭且打包脚本从未收录** | 否 | `PINTO0309/hand-gesture-recognition-using-onnx` LICENSE |

> 2026-11-06 清理：`07_安装包/models/` 下的两份手部模型已移到 `07_安装包/backup/models_hand_20261106/`。
> 它们既不在 `build_setup.ps1` 的打包清单里，也不被生产路径加载，留在安装包目录只会
> 让「装了但没用到」的第三方二进制混进交付物。

## 三、开发期工具（不入库、不入包）

| 组件 | 许可 | 用途 |
|---|---|---|
| websocket-client（Python） | Apache-2.0 | `05_验证/obsctl.py` 通过 obs-websocket 抓图 / 开关滤镜 |
| Pillow（Python） | MIT-CMU | 素材裁剪、掩码可视化 |
| OpenCV（Python，`cv2`） | Apache-2.0 | `count_faces.py` / `scan_frontal_faces.py` 用 `cv2.dnn` 跑 YuNet 做素材筛选 |
| ffmpeg | 各构建不同（LGPL / GPL） | A/B 素材转码，`05_验证/tools/` 已被忽略 |

## 四、宿主与集成边界

- **OBS Studio**（GPL-2.0）：本插件以 OBS 插件形式动态链接 `libobs`，自身源码不含
  OBS 代码。插件是否构成 GPL 意义上的衍生作品、以及对外分发时的义务，**需自行
  评估或咨询法律意见**，本文件不作结论。
- **obs-websocket / obs-multi-rtmp / obs-browser / 插件中心**：只是验证日志里出现过的
  同机第三方插件，与本插件无代码或分发关系，此处仅作说明。

## 五、源码内第三方名称的处理原则

`beauty_face.h`、`beauty_onnx.h`、`beauty_seg.h` 等处的 `YuNet` / `OpenCV` /
`ONNX Runtime` / `MediaPipe` / `BiSeNet` 字样是**技术选型说明与归属标注**，必须保留：
删掉它们不会减少任何风险，反而会让 MIT / Apache-2.0 要求的署名消失。

## 六、随安装包分发的第三方二进制（3.2.2 起）

`Setup.exe` 把下列文件作为 RCDATA 内嵌，安装时写出，卸载时删除：

| 落盘位置 | 源文件 | 许可 |
|---|---|---|
| `<root>\bin\64bit\onnxruntime.dll` | `06_离线验证/onnxruntime.dll` | MIT |
| `<root>\bin\64bit\models\yunet.onnx` | `06_离线验证/models/yunet.onnx` | MIT |
| `<root>\THIRD-PARTY-NOTICES.md` | 本文件 | —— |

`<root>` = `C:\ProgramData\obs-studio\plugins\obs-beauty`。

两个 MIT 组件都要求「在软件的所有副本或实质部分中保留上述版权声明与许可声明」，
所以本文件必须随装随附 —— 它不是可选说明，而是再分发的前提条件。

### ONNX Runtime

```
MIT License

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### YuNet（`models/face_detection_yunet/LICENSE`）

```
MIT License

Copyright (c) 2020 Shiqi Yu <shiqi.yu@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

> ONNX Runtime 官方发布包另带一份 `ThirdPartyNotices.txt`，覆盖它自身链接的
> 其他组件（如 abseil、protobuf 等）。本轮只分发 `onnxruntime.dll` 主库，
> 未随附该文件 —— 对外公开发布前应补上，见「待办」。

## 待办

- [ ] 根目录缺自有 `LICENSE`：本仓库自身以何种许可对外，尚未确定。
- [ ] 未随附 ONNX Runtime 官方 `ThirdPartyNotices.txt`（覆盖其内部依赖）。
      从上游 release 压缩包原样取出即可，不要手写。
- [ ] `07_安装包/models/` 已清空，`build_setup.ps1` 也不再引用该目录，
      可择机删除整个文件夹。
