# 第三方组件与许可声明

本文件登记 `obs-beauty` 用到或曾经用到的全部第三方组件，以及它们各自的许可与
分发状态。**它只记录事实，不代替法律意见**；涉及再分发（对外发布 dll / 安装包）
时，请按各组件许可原文逐条核对。

最后核对日期：2026-11-06。许可以上游仓库当时的分支内容为准，见每行「依据」列。

## 一、运行时依赖

| 组件 | 版本 | 许可 | 用途 | 是否入库 | 是否随安装包分发 | 依据 |
|---|---|---|---|---|---|---|
| ONNX Runtime | 1.20.1（API 版本 20） | MIT | 模型推理（YuNet / 分割 / 手部） | 否（`06_离线验证/onnxruntime.dll`，已被 `.gitignore` 挡住） | **否**——当前 `Setup.exe` 不含，属已知分发缺口 | 上游 Microsoft/onnxruntime 仓库 LICENSE |
| YuNet 人脸检测模型 | 2023mar | MIT | 生产路径必需：人脸框 + 5 关键点 | **是**：`06_离线验证/models/yunet.onnx`<br>SHA256 `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4` | **否**——同属分发缺口 | `opencv/opencv_zoo` 下 `models/face_detection_yunet/LICENSE` 明确写「本目录所有文件为 MIT」；注意**仓库根目录是 Apache-2.0，与本目录不同**，引用时别混。上游训练方 `ShiqiYu/libfacedetection.train` |

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

## 待办

- [ ] 根目录缺自有 `LICENSE`：本仓库自身以何种许可对外，尚未确定。
- [ ] 安装包未分发 `onnxruntime.dll` 与 `yunet.onnx`，装机后需手工放置；若改为随包
      分发，则必须一并附上本文件与 ONNX Runtime 的 `ThirdPartyNotices.txt`。
