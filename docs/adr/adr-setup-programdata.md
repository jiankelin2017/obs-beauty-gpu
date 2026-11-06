---
doc_type: adr
title: ADR · 安装路径统一到 ProgramData 并由闸门强制校验
version: 0.1.0
last_updated: 2026-11-05
applies_to: 3.0.1
repo_baseline: 914f388
maintainer: Jian
status: Active
supersedes: 无（新增决策记录）
---

# ADR · 安装路径统一到 ProgramData 并由闸门强制校验

> **结论**：OBS 32.x 只从 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty` 加载插件；
> 安装路径必须在 Setup.exe、装机脚本、发布闸门三处同口径，并由闸门当场拦下装错目录。
> **决策状态**：已接受（3.0.1 落地）。
> **适用条件**：3.0.1、OBS 32.x、Windows。
> **失效说明**：`Program Files\obs-studio\obs-plugins\64bit` 是历史布局，写入该目录等于没装；
> Setup.exe 端到端安装 / 卸载在 3.0.1 时点**未实跑**，已于 3.2.2 补齐（见文末「后续」）。

## 上下文

3.0.0 的 `Setup.exe` 仍按旧布局装到 `Program Files\obs-studio\obs-plugins\64bit`，
装完滤镜根本不加载；当时的 `check_release.py --strict` 只有 9 项判定，不检查安装路径，
所以构建、安装包、装机三处虽然逐字节一致，却一致地指向错误目录。

## 决策

- 统一安装根：`%PROGRAMDATA%\obs-studio\plugins\obs-beauty`，
  dll 落在 `bin\64bit\obs-beauty.dll`，shader 落在 `data\shaders`。
  `03_装机/7_install_gpu.ps1` 49–51 行同口径；
  `07_安装包/setup.cpp` 的 `pluginRoot()` 从 `GetEnvironmentVariableW(L"ProgramData", ...)` 派生，
  并对 `" % & | ^ < > ( ) !` 一类 cmd 元字符做白名单拒绝。
- 去掉 `_wsystem` 命令注入面，改用 Win32 递归删除 `removeTree()`。
- `writeFile()` 改为 `.tmp` + `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
  原子替换，失败删除临时文件，避免留下半截 dll。
- `05_验证/check_release.py` 补 3 条断言：安装路径、Setup.exe 版本串、PE RCDATA 载荷
  （`read_setup_resources()` 用 `LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE)` 只读读资源）。
  `--strict` 由 9 项变 **12 项 PASS**。
- 版本显示唯一出处仍是 `01_源码/beauty_params.h` 的 `BEAUTY_BUILD_STR = "3.0.1"`。

## 后果 / 边界

- 装错目录会被闸门当场拦下，不再依赖人工发现「装完没效果」。
- 卸载器会写 HKLM 注册表并创建卸载器副本 —— 因此端到端安装 / 卸载验证需单独授权后执行。
- 3.0.0 的 Setup.exe 快照仍留在归档证据中，**不要照该版部署**。

## 证据

- [`../CHANGELOG.md`](../CHANGELOG.md) 3.0.1；[`../README.md`](../README.md) §2 部署。
- `03_装机/7_install_gpu.ps1`: `$plugRoot = "C:\ProgramData\obs-studio\plugins\obs-beauty"`。
- `07_安装包/setup.cpp`；`05_验证/check_release.py --strict` → `PASS 12 / WARN 0 / FAIL 0`。
- 交付快照：dll `633,856 B / MD5 068E03E5FBE2E21C1883A7F372828783`；
  `Setup.exe 1,533,952 B / MD5 E945A0F86041050C6506A33B26F4A7FC`；构建 / 安装包 / 装机三处逐字节一致。

## 待人工确认

- ~~Setup.exe 端到端安装 / 卸载（会写 HKLM 并创建卸载器副本），本轮按边界未执行。~~
  **3.2.2 已实跑**，见下节。
- 不同 OBS 版本 / 便携版是否同样只认 ProgramData 布局，未在本仓库外验证。

## 后续（3.2.2，2026-11-06 20:52）

端到端安装 / 卸载已在真机执行，决策前提得到确认，并暴露了原包的一处缺陷：

- **安装**（`Setup.exe --quiet`，退出码 0）：写出 10 个文件 —— dll、
  `onnxruntime.dll`、`models\yunet.onnx`、`THIRD-PARTY-NOTICES.md`、6 支 shader，
  外加卸载器副本 `uninstall.exe`。注册表
  `HKLM\...\Uninstall\obs-beauty` 的 `DisplayVersion` = `3.2.2`，
  `InstallLocation` = `C:\ProgramData\obs-studio\plugins\obs-beauty`，
  `UninstallString` 指向 `uninstall.exe --uninstall`。
  dll 与 `02_构建/obs-beauty.dll` 的 MD5 均为 `ABF4413811F00C2E6EE1B9E14D0B84F5`。
- **卸载**（`uninstall.exe --uninstall --quiet`，退出码 0）：插件根目录在 2 秒内
  被自删的 cmd 完全移除，注册表项同时消失。
- **缺陷**：3.2.2 之前的包只带 dll + shader，`onnxruntime.dll` 与 `yunet.onnx` 要
  手工放。缺这两样时滤镜照样加载、不报错，只是人脸检测起不来、退化成全帧磨皮 ——
  与 3.0.0「装到错目录」属于同一类失败：**看起来装成功了**。现已把两者内嵌进包，
  并在安装末尾 stat 三个关键文件，缺一即报错退出。
- 卸载会连 `models\` 一起删。手工放进去的其它模型（如 `selfie_multiclass_256.onnx`）
  一并消失，这是有意为之：卸载的语义是回到没装过。
