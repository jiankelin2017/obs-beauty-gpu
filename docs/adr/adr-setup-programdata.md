---
doc_type: adr
title: ADR · 安装路径统一到 ProgramData 并由闸门强制校验
version: 0.1.1
last_updated: 2026-11-06
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

## 后续（3.2.3，2026-11-06 21:43）

3.2.2 的「卸载即回到没装过」有一处没兑现：**OBS 顶部菜单栏的「美颜」入口
在卸载后仍然存在**。

- **根因**：该入口由 `obs-beauty-ui.dll` 注册。它是一个**独立模块**——自带
  `OBS_DECLARE_MODULE()`，与本滤镜并列、互不依赖，链接产物也是单独的 dll
  （`build_gpu.ps1` 第 2 条注释已写明「obs-beauty-ui.cpp 不得链入本 dll，
  否则 LNK2005」）。它从未被收进安装包，历史上是手工部署到
  `%PROGRAMDATA%\obs-studio\plugins\obs-beauty-ui\bin\64bit\`。安装器既不知道
  它的存在，卸载自然不会动它。
- **修法**：安装与卸载两条路径都调用 `purgeMenuModule()`，按**固定文件名**
  （非通配符）清三处布局：`plugins\obs-beauty-ui\`、`plugins\obs-beauty-ui.dll`、
  `Program Files\obs-studio\obs-plugins\{64,32}bit\obs-beauty-ui.dll` 与
  `data\obs-plugins\obs-beauty-ui\`。父目录由 `pluginRoot()` 去掉固定后缀得到
  （`pluginsDir()`），不重复读环境变量，避免「校验过的字符串」与「实际使用的
  字符串」不是同一份。
- **刻意不碰** `%APPDATA%\obs-studio\plugin_config\obs-beauty-ui`：安装器以
  管理员提权运行，`%APPDATA%` 在提权上下文里指向哪个账户并不确定，删配置有
  误伤风险；菜单模块既已移除，这份配置也不会再生效。
- **实测**（构造式）：手工造出 `plugins\obs-beauty-ui\bin\64bit\obs-beauty-ui.dll`
  与平铺的 `plugins\obs-beauty-ui.dll`，跑 `uninstall.exe --uninstall --quiet`
  （退出码 0），两者连同目录一并消失；同目录的 `obs-multi-rtmp` 与 `obsplus`
  完好，证明固定名清理没有误伤。
- **同类缺陷的共性**：这是第三次遇到「组件不在安装器视野里」——3.0.0 是装到
  错目录、3.2.2 是漏带运行时、3.2.3 是漏管菜单模块。判据统一为一条：**凡本
  项目曾交付到用户磁盘上的文件，安装器必须知道自己管哪些**。目前这些路径集中
  在 `setup.cpp` 顶部的常量与 `purgeMenuModule()` 两处，新增交付物须同步。

## 后续（3.2.4，2026-11-10 · 3.2.3 的「装不上」）

3.2.3 修「卸不干净」时，把 `purgeMenuModule()` 同时挂到了**安装**路径上，而
安装包载荷里没有 `obs-beauty-ui.dll`。于是每次安装等于「删掉菜单模块 + 不补装」：
用户装完 3.2.3，OBS 顶部菜单栏就没有「美颜」入口了 —— 上一个缺陷（卸不掉）
被换成了更糟的一个（装不上）。

- **根因不是清理范围，是「删了却没有补」**。判据与 3.2.3 结尾那条同源：凡本
  项目交付到用户磁盘上的文件，安装器必须知道**自己会不会把它放回去**。只删
  不放，等于把用户已有的功能拿走。
- **修法**（三处，缺一不可）：
  1. 面板源码 `obs-beauty-ui.cpp` 并入本仓库 `01_源码/`，新增
     `02_构建/build_ui.ps1` 按 GPU 口径编译（不定义 `BEAUTY_UI_CPU_EDITION`，
     菜单标题「美颜(&A)」；`/MD` 而非 `/MT`，因为 Qt 是 `/MD`）。
  2. 面板成为安装包第 108 号载荷，落点
     `%PROGRAMDATA%\obs-studio\plugins\obs-beauty-ui\bin\64bit\`。
  3. `purgeMenuModule()` 增加 `includePluginData` 形参：卸载传 `true`（三处布局
     全清，3.2.3 的语义保留），安装传 `false`（只清 `Program Files` 旧副本，
     ProgramData 那份由写盘覆盖）。
- **为什么不把菜单注册并进 `obs-beauty.dll`**：那需要给滤镜链 Qt6Widgets/Gui/Core
  + `obs-frontend-api`，依赖会从「仅 `obs.dll`」变成一串 Qt dll，等于为修一个
  入口把已验证的核心产物重做一遍。收益不抵风险，故维持「两个模块、各自部署」。
- **闸门**：`check_release.py` 新增三项 —— 「面板版本串」（面板自报版本必须等于
  `BEAUTY_BUILD_STR`，专防「滤镜 bump 了、面板忘了重编」）、「面板」（内嵌 ==
  构建产物）、「面板装机」（装机 == 构建产物）。`SETUP_RCDATA` 增加 108，载荷
  项数从 13 变 14。
- **离线可证的部分**：`02_构建/verify-ui-imports.py` 把面板的 623 个导入符号逐个
  在 OBS 自带 DLL 里解析（`obs.dll 51 / obs-frontend-api.dll 1 / qt6widgets 470 /
  qt6core 88 / qt6gui 13` 全 OK）。这一条重要，因为 Qt ABI 不符的表现不是报错，
  而是菜单压根不出现 —— 和本缺陷的症状一模一样。
