---
doc_type: CHANGELOG
title: 用户可见变更
version: 0.1.8
last_updated: 2026-11-10
applies_to: 3.2.4
repo_baseline: 914f388
maintainer: Jian
status: Active
---

# 用户可见变更

> 只记**影响读者使用的变更**：装到哪、行为变没变、怎么回退。
> 逐轮实测、踩坑经过、内部调参一律在 [`archive/PROGRESS-20261105.md`](archive/PROGRESS-20261105.md)。
> 版本号唯一出处是 `01_源码/beauty_params.h` 的 `BEAUTY_BUILD_STR`。

## 3.2.4（2026-11-10 · 修复：装完顶部「美颜」菜单入口消失）

**滤镜行为与 3.2.3 完全相同；这一轮只改安装器载荷与面板分发方式，滤镜 dll 里只有版本字符串变了。**

| 改动 | 对使用者的影响 |
|---|---|
| 3.2.3 把「装不上」换成了「装完没有」 | 3.2.3 为修「卸载不干净」，把 `purgeMenuModule()` 同时挂到了**安装**路径上，而安装包载荷里并没有 `obs-beauty-ui.dll` —— 于是每次安装 = 删掉菜单模块 + 不补装，用户装完顶部菜单栏就看不到「美颜」入口。这是本次要修的 bug |
| 面板随安装包分发 | `obs-beauty-ui.dll` 作为第 108 号载荷（RCDATA）随包发出，装到 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty-ui\bin\64bit\`。装完即有入口，重装/覆盖安装也不会再消失 |
| 安装只清旧布局，卸载仍全清 | 安装路径只清 `Program Files` 下的旧副本（`includePluginData=false`），ProgramData 那份由写盘覆盖；卸载时三处布局照旧全清 —— 3.2.3「卸干净」的语义完整保留 |
| 面板源码进入本仓库 | `01_源码/obs-beauty-ui.cpp` + `02_构建/build_ui.ps1`（不定义 `BEAUTY_UI_CPU_EDITION`，菜单标题「美颜(&A)」）。此前本仓库无法自行构建面板，只能手工部署 CPU 分支的产物 |
| 版本号同源 | 面板的 `BEAUTY_UI_VERSION` 直接取 `BEAUTY_BUILD_STR`，与滤镜一处升、两处同步，不再出现面板角标与滤镜版本对不上 |

**装到哪**：滤镜 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll`；
面板 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty-ui\bin\64bit\obs-beauty-ui.dll`

**交付**：`obs-beauty.dll 645,632 B / MD5 9338367B318C75CAAD1C84F3D709AC07`（体积与 3.2.3
相同，差异只是内嵌版本串 `3.2.3` → `3.2.4`）；`obs-beauty-ui.dll 218,112 B /
MD5 B83C752563DE6A69C2201845D1D986A9`；`Setup.exe 13,316,096 B /
MD5 19A623A124C804C06ADEAB08AAB43CBC`（13,096,960 → 13,316,096，增量即面板 dll 与
新增写盘/自检逻辑）。`check_release.py --strict` **14 PASS / 1 WARN / 0 FAIL**
（唯一 WARN 是本机没有 `D:\obs_beauty_gpu\shaders` 这个 solo 开发目录，与本版无关）。

**怎么验**：装 3.2.4 → 打开 OBS → 顶部菜单栏应有「美颜」，点开「参数面板...」能连上
滤镜实例；OBS 日志应有两行 —— `[obs-beauty-ui] 模块已加载 v3.2.4` 与
`[obs-beauty-ui] 顶层菜单已添加`。卸载后该入口与
`plugins\obs-beauty-ui` 目录应一并消失。

**本轮真机实测（2026-11-10）**：装 3.2.4 → 启动 OBS，日志出现
`[obs-beauty] filter registered as 'obs_beauty_filter' (v3.2.4)`、
`[obs-beauty-ui] 模块已加载 v3.2.4`、`[obs-beauty-ui] 顶层菜单已添加`，
截图确认菜单栏最右侧有「美颜(A)」；随后跑一次卸载 —— `plugins` 下
`obs-beauty` 与 `obs-beauty-ui` 一并消失，`obs-multi-rtmp`、`obsplus` 完好
（3.2.3 的卸载语义未回退）；再装一次，两个目录与两行日志原样复现 ——
**「重装即丢入口」这条已闭环**。
离线侧另有两项：`02_构建/verify-ui-imports.py` 对面板 dll 的全部 623 个导入符号
逐个在 OBS 自带 DLL 里解析（`obs.dll 51 / obs-frontend-api.dll 1 / qt6widgets 470 /
qt6core 88 / qt6gui 13` 全部 OK）；`check_release.py` 新增「面板版本串 / 面板 /
面板装机」三项比对，专防「滤镜 bump 了、面板忘了重编」。

**回退**：把 `obs-beauty_3.2.3.dll` 覆盖回 `obs-beauty.dll` 即可回到 3.2.3 的滤镜行为；
若只想去掉顶部菜单入口，删掉 `plugins\obs-beauty-ui` 目录后重启 OBS。

## 3.2.3（2026-11-06 · 修复：卸载后 OBS 顶部「美颜」菜单仍在）

**滤镜行为与 3.2.2 完全相同；这一轮只改安装器的卸载路径，dll 里只有版本字符串变了。**

| 改动 | 对使用者的影响 |
|---|---|
| 卸载与安装都会清掉「美颜」菜单模块 | 此前插件已卸载、`%PROGRAMDATA%\obs-studio\plugins\obs-beauty` 目录已空，但 OBS 顶部菜单栏的「美颜」入口还在，看起来像没卸干净。根因是那个菜单来自**另一个模块** `obs-beauty-ui.dll`：它自带 `OBS_DECLARE_MODULE()`，与本滤镜并列、互不依赖，且从未被打进安装包（历史遗留的手工部署）。安装器不认识它，卸载自然不删它 |
| 清理范围只按固定文件名，三处都覆盖 | `plugins\obs-beauty-ui\`（正常子目录布局）、`plugins\obs-beauty-ui.dll`（dll 直接铺平的变体）、`Program Files\obs-studio\obs-plugins\{64,32}bit\obs-beauty-ui.dll` 与 `data\obs-plugins\obs-beauty-ui\`（3.0.x 旧布局）。不扫目录、不用通配符，因此不会误伤同目录下其它插件 |
| 安装时也清一次 | 菜单模块与本滤镜是两套独立实现。并存时，用户点顶部「美颜」看到的是旧模块的界面，而滤镜参数在滤镜属性页里 —— 两个入口指向不同实现，比「只有一个入口」更难解释 |
| 删不掉会明说 | 若有文件被占用，弹窗会点名 `obs-beauty-ui` 并给出处置办法，而不是照旧显示「已卸载」 |

**刻意不碰** `%APPDATA%\obs-studio\plugin_config\obs-beauty-ui`：本程序以管理员提权
运行，`%APPDATA%` 在这种上下文里指向哪个账户并不确定，删配置有误伤风险；而菜单
模块既然没了，这份配置也不会再生效。

**装到哪**：`%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll`

**交付**：dll `645,632 B / MD5 828E919722CA5A4D7D7919DABBFFEBF6`（体积与 3.2.2 相同，
差异只是内嵌版本串 `3.2.2` → `3.2.3`）；`Setup.exe 13,091,840 → 13,096,960 B /
MD5 CB329D90EF19CE87DDE3A0DD749F369A`（+5,120 B 全部来自新增的清理逻辑）。
`check_release.py --strict` **12 PASS / 0 WARN / 0 FAIL**。

**怎么验**：装 3.2.3 → 卸载 → 打开 OBS 看顶部菜单栏应无「美颜」；
目录侧 `plugins` 下应查不到 `obs-beauty-ui`，而 `obs-multi-rtmp`、`obsplus`
等其它插件不受影响。本轮已做**构造式实测**：手工造出
`plugins\obs-beauty-ui\bin\64bit\obs-beauty-ui.dll` 与平铺的
`plugins\obs-beauty-ui.dll`，跑一期卸载，两者连同目录一并消失，同目录另两个
插件完好。

**回退**：把 `obs-beauty_3.2.2.dll` 覆盖回 `obs-beauty.dll`（菜单清理只发生在
安装器里，回退 dll 不改变卸载行为）。

**实机部署备注（2026-11-10，已被 3.2.4 推翻）**：下面这段记录的是 3.2.3 当时的实际
行为与手工补装口径。**3.2.4 起不再需要手工补装** —— 面板已是安装包载荷，且安装路径
不再删 ProgramData 那份。原文保留，用于解释 3.2.3 期间为什么会「装完就没入口」：

> 因为「安装时也清一次」这条，凡是手工补装过 `obs-beauty-ui.dll` 的机器，再运行一次
> 安装器就会被重新删掉。当时把它判为「既定行为」，是**误判**：载荷里没有这个 dll，
> 删除后没有任何东西把它补回来，用户侧看到的就是功能入口消失。
>
> | 项 | 值 |
> |---|---|
> | 装到 | `%PROGRAMDATA%\obs-studio\plugins\obs-beauty-ui\bin\64bit\obs-beauty-ui.dll` |
> | 用哪份 | GPU 分支编的面板（不带 `BEAUTY_UI_CPU_EDITION` 宏，菜单标题为「美颜(&A)」）。CPU 分支那份菜单叫「美颜·CPU(&A)」，配 GPU 滤镜会出现两个口径不一的入口，不要用 |
> | 来源 | 3.2.3 时代面板源码只在 CPU 分支仓库；3.2.4 起已并入本仓库 `01_源码/obs-beauty-ui.cpp` |
> | 校验 | `python 02_构建/verify-ui-imports.py <dll>` 须全 OK；面板靠遍历滤镜的 `obs_properties` 动态建控件，不含版本一致性校验，所以版本号与滤镜不一致不会导致面板失效，只影响标题显示 |

## 维护（2026-11-06 · LICENSE 改为 FSF 原文，版权说明拆到 COPYRIGHT.md）

**不涉及版本号，`BEAUTY_BUILD_STR` 仍为 3.2.2，dll 逐字节不变。** 这一轮只动许可文件与安装包。

| 改动 | 影响谁 |
|---|---|
| `LICENSE` 换成 **FSF GPL-2.0 原文**（17,984 B / 338 行 / SHA256 `edaef632…b233f6`，逐字节未改） | 此前 `LICENSE` 头部有 44 行自定义前言（版权行 + 中英双语范围说明）。GitHub 的许可识别是拿整个文件与官方原文做全文比对，混入自定义段落就匹配不上，项目被归为 **"Other"** —— 仓库页没有 GPL-2.0 徽章，别人按许可筛选也搜不到。**法律效力本来就没问题，改的是可识别性** |
| 新增 [`COPYRIGHT.md`](../COPYRIGHT.md)：版权行、版本口径（2.0 only）、适用范围（覆盖哪些、不覆盖哪些第三方）、改 "or later" 的具体写法 | 这些内容从 LICENSE 头部移出。以后要改授权声明，改这个文件即可，不必碰许可正文（GPL 正文本身要求原样分发） |
| 安装包新增内嵌资源 `id 107`，`COPYRIGHT.md` 落到 `<root>\COPYRIGHT.md`，卸载一并删除 | 装机目录现在同时有 `LICENSE` 与 `COPYRIGHT.md`。GPL 要求分发时随附许可文本，版权与范围说明跟着一起走才不会各说各话 |
| `check_release.py` 新增断言：`LICENSE` 的 SHA256 必须等于 FSF 原文 | 这类改动只动文本、代码评审几乎看不出来，必须由闸门兜住。已做反向对照：给 `LICENSE` 加 3 行头，闸门立刻 FAIL 并指名"版权行请放 COPYRIGHT.md" |
| `.gitattributes` 对 `LICENSE` 设 `-text` | 仓库全局是 `eol=crlf`，一旦 LF 被转成 CRLF，文件就不再是原文，SHA256 与上面那条断言一起失效 |

**交付**：dll 未变 `645,632 B / MD5 ABF4413811F00C2E6EE1B9E14D0B84F5`；
`Setup.exe 13,089,792 → 13,091,840 B / MD5 E81A99881257904B5B7E68D3AB0D2B60`
（+2,048 B：`LICENSE` 由 20,134 → 17,984 B 少 2,150 B，新增 `COPYRIGHT.md` 3,796 B，
`THIRD-PARTY-NOTICES.md` 因补一行落盘清单微增，其余是 PE 节对齐）。
`check_release.py --strict` **12 PASS / 0 WARN / 0 FAIL**（内嵌载荷项 12 → 13 项）。

**怎么验**：`sha256sum LICENSE` 应为 `edaef632…b233f6`；装完看 `<root>` 下
`LICENSE`（17,984 B）与 `COPYRIGHT.md`（3,796 B）都在；GitHub 仓库页右侧应出现
GPL-2.0 徽章（推送后由 GitHub 重新识别，通常几分钟内生效）。

## 维护（2026-11-06 · 仓库清理：个人数据出库、第三方声明补齐）

**不涉及版本号，`BEAUTY_BUILD_STR` 仍为 3.2.2，dll 与安装包逐字节不变。** 这一轮改的是仓库本身。

| 改动 | 影响谁 |
|---|---|
| 离线回归的输入帧与 4 条基准（`06_离线验证/goldens/*.raw`）**从仓库移除**，只保留在本机 | 新 clone 本仓库的人拿不到基准，需自备素材重采；老机器不受影响。原因：这些帧由真人照片派生，帧内是可辨认的人脸 |
| `05_验证/out/` 的实验日志与截图（19 份）**从仓库移除** | 同上。这些是整份 OBS 日志，含本机用户名、显卡型号、配置文件与整份第三方插件清单 |
| 源码去掉写死的个人路径 | `obs-beauty.cpp` 的 `BEAUTY_THUMB_DUMP` 落盘位置改为系统临时目录（宽字符，中文用户名可用）；10 个验证脚本的项目绝对路径改为相对仓库根；`make_assets.py` / `scan_frontal_faces.py` 不再预设个人目录 |
| 新增 [`THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md) | 登记 ONNX Runtime / YuNet / BiSeNet / MediaPipe / 手部模型的许可与分发状态；顺带纠正一处口径：YuNet 模型目录是 MIT，而 `opencv_zoo` **仓库根是 Apache-2.0**，两者不能混用 |
| `07_安装包/models/` 下 15.5 MB 手部模型移入 `07_安装包/backup/` | 打包脚本从未收录、生产路径也不加载，属死重。**该 backup 已于同日整体删除**（含 2.17.2 旧版 exe/dll），仓库树内不再保留任何手部模型 |

**怎么回退**：历史重写前的完整仓库镜像曾是 `D:\插件\_obs_beauty_history_20261106.bundle`，
**已于 2026-11-06 20:50 按用户要求删除**（它含人脸图像，留着本身就是风险）。
重写后的仓库只少了「个人数据文件」与一个只往 `05_验证/out/` 加文件的空提交，其余 80 个提交内容逐字节不变，
因此不需要那份镜像也能正常工作。

## 维护（2026-11-06 · 安装包一键化，重编 dll）

**`BEAUTY_BUILD_STR` 仍为 3.2.2**，画面行为与上一构建完全相同；这一轮改的是**怎么装**和**装完是否可用**。

| 改动 | 影响谁 |
|---|---|
| `Setup.exe` 补齐运行载荷：`onnxruntime.dll`（11.6 MB）与 `yunet.onnx`（232 KB）内嵌进包，安装时落到 `<root>\bin\64bit\` 与 `\models\` | **新装机器不用再手工拷文件**。此前装完能开滤镜，但人脸检测起不来 —— 不报错，只是安静地退化成全帧磨皮，最难查的那种 |
| 版本号改为构建时从 `beauty_params.h` 注入 | 旧包把 `3.0.1` 写死在三处（弹窗、注册表 `DisplayName` / `DisplayVersion`），dll 已到 3.2.2 而"应用和功能"里还显示 3.0.1。现在单一来源，dll 版本不一致时打包直接失败 |
| 安装包体积 1.5 MB → **12.7 MB**（再补 ORT 官方声明后 **13.1 MB**） | 全部来自那 11.8 MB 运行时、模型与 0.34 MB 第三方声明；这是"一键可用 + 合规"的必要代价 |
| 卸载会清 `models\` 与 `bin\`，并移除 `THIRD-PARTY-NOTICES.md` | 卸载后回到"没装过"的状态。**注意：手工放进 `models\` 的其它模型（如 `selfie_multiclass_256.onnx`）也会一起删除** |
| 卸载不再要求 OBS 存在 | 先卸 OBS 再卸插件，此前会直接退出、插件残留 |
| 安装后自检三个关键文件的存在与大小 | 写盘被杀软拦下时当场报错，不再"装完了但缺文件" |
| `obs-beauty.cpp` 的 `BEAUTY_THUMB_DUMP` 落盘改 `GetTempPathW` + `_wfopen` **随本次重编生效** | 上一轮只改了源码没重编，交付的 dll 里仍是旧路径；本次 dll 已重编，`Administrator` 字符串归零 |
| 安装包不再重复嵌同一份 dll（旧的无用 `id 1` 资源） | 省 645 KB |
| 新增根目录 `LICENSE`：**GPL-2.0**（与 OBS 宿主同一许可） | 此前仓库无任何许可文件。现在明确：可自由使用、修改、再分发（含商用），但**分发修改版必须同样以 GPL-2.0 开源**并保留版权声明。选 GPL-2.0 是因为插件动态链接 `libobs`（OBS 本身即 GPL-2.0），分发时许可层面不存在冲突。第三方组件（ONNX Runtime / YuNet 的 MIT）不受影响，仍在 `THIRD-PARTY-NOTICES.md` 单独声明 —— MIT 与 GPL 兼容。`LICENSE` 随安装包落到装机目录 |
| 安装包随附 ONNX Runtime 官方 `ThirdPartyNotices.txt`（6,508 行 / 338,538 B，逐字未改），落到 `<root>\onnxruntime-ThirdPartyNotices.txt` | 此前只声明了 ORT 本身的 MIT，**漏了 ORT 内部链接的 abseil / protobuf 等**。原文存 `third_party/onnxruntime-1.20.1-ThirdPartyNotices.txt`，取自上游 tag `v1.20.1`（与 dll 内唯一版本串 `1.20.1` 对应），SHA256 记在 `THIRD-PARTY-NOTICES.md`。换 ORT 版本须同步换这份文件 |
| `THIRD-PARTY-NOTICES.md` 第四节改写：新增「无论是否认定为衍生作品，两种认定导向同一套义务」的分支对照 | 原文写「需自行评估」，与已选定 GPL-2.0 的事实脱节。改写**不下法律结论**，只说明选定同许可后该判断不再产生分歧；并注明若基于本插件改闭源商业版，该问题会重新变成实质风险 |
| `THIRD-PARTY-NOTICES.md` 待办三条全部关闭 | 含一条早已完成却未勾选的（`07_安装包/models/` 已删）。同日另删 `07_安装包/backup/`（手部模型 + 2.17.2 旧版），原「对外拷贝文件夹会带出 Apache-2.0 模型」的残留风险随之消除 |

**交付**：dll `645,632 B / MD5 ABF4413811F00C2E6EE1B9E14D0B84F5`；
`Setup.exe 13,089,792 B / MD5 2E3537A10D644CC4F655381C01204A04`
（体积未变是 PE 节对齐吸收：本轮改的是内嵌的 `THIRD-PARTY-NOTICES.md` 文本，
10,355 → 10,485 B，md5 因此变化；以 md5 为准确认版本）。
`check_release.py --strict` **12 PASS / 0 WARN / 0 FAIL**（安装包载荷项已扩到 12 个内嵌资源：
dll + 运行时 + 模型 + 两份第三方声明 + 本项目 LICENSE + 6 shader）。

**怎么验**：双击 `Setup.exe` → 装完直接开 OBS 加滤镜，**不手工放任何文件** → 镜头前有人时，
日志里 `frame#N drawn … faces=` 应出现非 0，`px=` 不再是 `-1`。若恒为 `faces=0` 且 `px=-1`，
说明模型没到位（回到「全帧磨皮」的降级态）。
卸载：再双击一次 `Setup.exe` 选"是"。

## 3.2.2（2026-11-06 · 修复：删掉美颜滤镜再加回来，该槽的美颜会彻底失效）

**画面调参行为与 3.2.1 完全相同；修的是「删了再加」这条路径，顺带把 3.2.1 遗留的三处内部缺陷补完。**

| 改动 | 对使用者的影响 |
|---|---|
| 修「删滤镜再加回来 → 该路美颜彻底失效」 | 同一个 OBS 会话里，把美颜滤镜删掉再加回来（或复制源后再删原源），该槽的异步检测线程此前**永久死亡**：掩码不再更新，脸内磨皮/美白/红润全部失灵，只有重启 OBS 能恢复。3.2.2 起自动重启线程，删加任意次都正常 |
| 顺带清掉上一轮遗留的帧与结果 | 新实例不再继承旧实例未消费的检测结果。换过源之后那是**另一路画面**的坐标，继承它等于拿别人的掩码磨自己的脸 |
| 并发约定修正（内部） | 自动方向探测的四个计数器原本由渲染线程与 worker 线程同时写（换源那一帧），属数据竞争。改为单向信号：渲染线程只置一个原子标志，检测侧自己清。x86 上对齐写不会真撕裂，所以线上没炸过，属潜在缺陷 |
| 预热标记修正（内部） | `warmed_` 在分配内存之前就置真（分配失败则永久跳过预热），且 `release()` 不复位它（第二次启用会重新吃一次首帧卡顿）。两处都已改对 |
| 新增离线判据 `06_离线验证/_slot_cycle_probe.py` | 模拟「删掉再加回来」，再用无脸画面判定 worker 是否真的重启。**对照实验已做**：撤掉修复那一行，同一脚本跑出 60 帧 `faces=1` 全挂（正是「效果冻住」的样子）；加回修复后第 2 帧即归 0 |

**装到哪**：`%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll`
（3.2.2 首次交付 `635,904 B / MD5 95C390D2DEDD474E1471E00C9C70311E`；
同日重编后为 `645,632 B / MD5 ABF4413811F00C2E6EE1B9E14D0B84F5`，
差别仅来自 `BEAUTY_THUMB_DUMP` 落盘改宽字符路径，画面行为不变，详见上一节；
上一版备份为同目录 `obs-beauty_3.2.1.dll`。安装包已重打成 13.1 MB 的一键包，
`check_release.py --strict` **12 PASS / 0 WARN / 0 FAIL**）

**回退**：把 `obs-beauty_3.2.1.dll` 改名覆盖回 `obs-beauty.dll`。

**怎么验**：加滤镜 → 确认有美颜 → 删掉滤镜 → 再加回来 → 拉磨皮/美白，应当照旧响应。
（此前这一步之后滑块会完全没反应，且日志里 `frame#N drawn … faces=` 一直是 0。）

## 3.2.1（2026-11-06 · 补丁：诊断可读性，画面行为不变）

**没有任何效果参数或渲染路径被改动；只改了一行日志和版本号。**

| 改动 | 对使用者的影响 |
|---|---|
| 「仅色彩层」诊断补 `geomOnly` 字段 | 磨皮=0、只开美白/清晰/红润时，那行日志现在会写成 `… px=0 有脸=1 geomOnly=1 …｜px=0 正常：geomOnly 帧，掩码在 GPU 上重画`。此前 `px=0` 会被误读成「没有掩码」，实际是那一帧 CPU 一个像素都不落笔、掩码由显卡重画 |
| 三种情形分别给出结论 | `px=-1` = 引擎没有有效掩码（没检出脸/模型未就绪）；`px=0 且 geomOnly=1` = 正常；`px=0 且 geomOnly=0` = 真异常（该画却没落笔） |
| 3.2.0 真机验收结果 | 2026-11-06 18:02 直播中实测竖屏机位：命中 **270°**、38 ms 后 `count=1`、滑块恢复响应、美白可见。同场统计：渲染 5.8 ms、丢帧 0/23620、编码跳过 0/11407、CPU 4.9% —— 全帧回读的代价被完全吸收，**不需要**再做缩略图方向优化 |

**装到哪**：`%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll`
（本次交付 `635,904 B / MD5 5C9181C44B34201D756C3295ECDFF545`，上一版备份为同目录 `obs-beauty_3.2.0.dll`；
安装包 `Setup.exe 1,538,048 B / 41F8B7415699578AE322B25FBE15563A` 已同批重打，`check_release.py --strict` **12 PASS / 0 FAIL**）

**回退**：把 `obs-beauty_3.2.0.dll` 改名覆盖回 `obs-beauty.dll`。

## 3.2.0（2026-11-06 · 新增：竖屏机位自动补偿）

**默认开，不需要改任何设置；判定有误时 `BEAUTY_AUTO_ROT=0` 一键回退到 3.0.1 的行为。**

| 改动 | 对使用者的影响 |
|---|---|
| 新增「自动输入方向探测」 | 采集画面被场景项转过 ±90°（典型接法：采集卡输出 1920×1080 横躺、场景项 −90° 显示竖屏）时，插件会自己认出方向并补偿。此前这种接法下磨皮停在横屏坐标、拉参数不响应、美白失效 |
| 触发条件与代价 | 连续 20 次检测都检不到脸后，才在同一帧上多跑两次 640×640 推理去试 90°/270°；命中后改走全帧路径（每帧多一次全帧回读）。没人时会每约 3 秒重试一次，不锁死 |
| 修 `warmUp()` 不幂等 | 离线验证台每帧都会重置掩码与检测缓存（旧缺陷，线上不触发）；修后离线台与 OBS 行为一致 |
| 新增离线判据 `06_离线验证/_autorot_probe.py` | 三条：躺脸必须自动命中、正立脸不得误判、空画面不得误判 |

**装到哪**：`%PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll`
（本次交付 `635,904 B / MD5 4C145124EADB7189DEF8B12170F9F55C`）

**怎么验**：竖屏机位下开美颜，日志里出现
`[美颜] 连续 20 次检不到脸，自动方向探测命中 90°/270°…`，随后 `frame#N drawn … faces=1`。
拉磨皮/美白滑块应当立即恢复响应；改回横屏机位（撤销场景项旋转）后，插件会在下一次
换分辨率或重启时重新判定，无需手动改配置。
（2026-11-06 实测命中 270°：采集卡 1920×1080 横躺、场景项反向旋转那种接法。）

**回退**：设用户环境变量 `BEAUTY_AUTO_ROT=0` 并重启 OBS。

## 3.0.1（2026-11-05 · 普通升级：代码审查修复）

**不动多实例架构，不新增开关，不改默认参数。** 只修交付链与内存安全：

| 改动 | 对使用者的影响 |
|---|---|
| `Setup.exe` 安装路径改为 `%PROGRAMDATA%\obs-studio\plugins\obs-beauty` | **重要**：OBS 32.x 只从这里加载插件。此前装到 `Program Files\obs-studio\obs-plugins\64bit` 旧布局，装完滤镜根本不加载 |
| 槽位获取失败路径不再留悬空 hook user | 多实例创建失败时不会崩溃 / 误摘别的实例钩子 |
| `beautyCoreReleaseSlot()` 在同一把锁内停 worker 并清空槽状态 | 删滤镜 / 槽复用时无释放竞态 |
| 去掉命令注入面，改为 Win32 递归删除 + `.tmp` + `MoveFileExW` 原子写 | 安装不会留下半截 dll |
| 发布闸门补 3 条断言（安装路径 / Setup.exe 版本串 / RCDATA 载荷） | `--strict` 从 9 项变 12 项，装错目录会被当场拦下 |

**交付状态**：dll `633,856 B / MD5 068E03E5FBE2E21C1883A7F372828783`，构建 / 安装包 / 装机三处逐字节一致；
`Setup.exe 1,533,952 B / MD5 E945A0F86041050C6506A33B26F4A7FC`；
`check_release.py --strict` **12 PASS / 0 WARN / 0 FAIL**；离线回归 4 条 golden 逐字节相同。

**多机位自动化复验**：4 个独立源各挂 1 滤镜 → `slot=0/1/2/3 活跃实例=1/2/3/4`；删一路后其余正常、
槽可复用、全程无崩溃与黑屏。旧 dll 备份 `03_装机/backup/obs-beauty_20261105_204239.dll`（3.0.0）。

> **待人工确认**：用真实 2–4 路摄像头分别调磨皮 / 美白 / 润色清晰并连续观察 A 镜头是否闪烁。
> 自动化只证明上下文隔离与输出正常，画面观感需人眼判断。

## 2026-11-05 修复批次（2.16.1 → 3.0.0 之间，未单独发版）

> **适用条件 / 失效说明（文档治理）**：本节是 7 篇 `修复_*_20261105.md` 的结论合并，
> 记录**当时的修复与决策**，**不是 3.0.1 的变更清单**。原文未给出统一插件版本号
> （个别处写「3.1.65 (core)」「2.17.2bak」），**版本归属待人工确认**；
> 3.0.1 中这些项是否逐条仍按原文生效，亦**待人工确认**。
> 当前行为一律以 §3.0.1 / §3.0.0 与 `01_源码` 为准；原文全部在 `archive/`，正文冻结。

| 主题 | 当时的结论 / 决策 | 对使用者的影响 | 原文 |
|---|---|---|---|
| GPU 纯锐化掩码缺失 | 磨皮 = 0 时 GPU 链掩码为空，「只开锐化」与关滤镜逐字节相同；改法是在磨皮 = 0 且要面部细节时强制引擎画全帧 CPU 掩码 | 锐化从 `0 px` 变为 `36 958 px (4.01%) / max 16`；**视觉上仍偏弱**（该帧脸区 `|hf|` 中位 0.43 灰阶，素材本身糊），不是残留 bug | [`archive/changelog-src-sharpen-mask-20261105.md`](archive/changelog-src-sharpen-mask-20261105.md) |
| P2 值域加权核 | 修掉四处会让 P2 静默失效的错误 + 一条常红门禁；但真实摄像头画面上 99% 像素权重落在 0.75~1.0，整幅平均差仅 0.11 灰阶。**决策（用户定）「选 A：保持关闭」** | `BEAUTY_BILAT` 默认 0 不变，画面逐字节不变，4 条 golden `maxdiff=0`；代码与两趟参数保留，`=1` 仍可复现实验。本轮**没有 GPU 实测**（dll 被 OBS 占用） | [`archive/changelog-src-bilateral-p2-20261105.md`](archive/changelog-src-bilateral-p2-20261105.md) |
| 五官立体白圈 | 根因是羽化语义为「平台半径」而非「软边宽度」；新增椭圆 `dome` 形状开关，并把亮部改成互相重叠的连续光场 | 拉满「五官立体」时额头 / 下巴 / 两颊的四个白圈消失；`blur100 / mixed / sharp` 逐字节不变，含 threed 的 `region` 按预期变化并重采 | [`archive/changelog-src-threed-mach-band-20261105.md`](archive/changelog-src-threed-mach-band-20261105.md) |
| 分割同步路径与 segMix | 分割原先只在 `asyncWorker()` 里调用，`BEAUTY_ASYNC_DETECT=0` 同步路径完全不跑；且没有第一份结果时 `segMix>0` 会把整张掩码乘成 0。抽成 `runSegIfDue()` 两条路径共用，宿主侧按有无结果门控 | 默认路径不受影响（4 条 golden 逐字节相同）；`BEAUTY_SEG=1` 的模型未加载 / 首帧结果未回时，美颜不再整段消失 | [`archive/changelog-src-seg-sync-path-20261105.md`](archive/changelog-src-seg-sync-path-20261105.md) |
| 掩码椭圆双重旋转 | 歪头（`|θ|>2°`）时椭圆半轴与像素迭代 AABB 被旋转两次；把脸自身轴宽高、椭圆半轴、AABB 三组量分开 | 歪头时美白范围不再铺开；15° 倾斜帧覆盖率 7.82% → 7.47%，确认是有意变化后重采 4 条基准。顺带修好离线回归台编译 | [`archive/changelog-src-mask-double-rotation-20261105.md`](archive/changelog-src-mask-double-rotation-20261105.md) |
| 祛斑与下巴 | 祛斑痘 `k=0.25 → 0.75`（用户定方案 A）；下巴从「纵向压缩」改「中线拉长 + 两侧内收」（方案 C），幅度 10%/14% → 18%/20%，四处同步改动 | 面板 100 档的祛斑痘与下巴不再是「观感等于没有」。已知局限：侧脸时横向内收左右不对称、`chinSpan` 依赖 YuNet 框底 | [`archive/changelog-src-spot-chin-20261105.md`](archive/changelog-src-spot-chin-20261105.md) |
| 锐化阈值被覆盖 | 三种互斥模式的判定被无条件覆盖，`pureSharpen` 时细频阈值拿不到 `faceSharpThrFine`；改成一次判定 | 锐化（脸内）满档从「几乎无效果」变为数值上生效；**仍偏弱是素材本身糊**，要更扎眼需加 `BEAUTY_FACE_SHARP_GAIN`，代价是噪点同步放大 | [`archive/changelog-src-sharpen-threshold-20261105.md`](archive/changelog-src-sharpen-threshold-20261105.md) |

**同时形成的决策记录**（原文归档，结论见对应 ADR）：

- 人脸分割不用于解决「手遮脸溢出」 → [`adr/adr-seg-not-for-occlusion.md`](adr/adr-seg-not-for-occlusion.md)；
  原文 [`archive/conclusion-seg-occlusion-20261105.md`](archive/conclusion-seg-occlusion-20261105.md)。
  结论基于**合成素材**，真实手遮脸素材仍待提供。
- CPU 与 GPU 掩码共用同一份 `faceGeom` 的工程约束 →
  [`adr/adr-cpu-gpu-same-mask.md`](adr/adr-cpu-gpu-same-mask.md)（**归纳、待人工确认**）。

## 3.0.0（2026-11-05 · 大版本更新：多实例隔离）

**修双机位串扰**：A 镜头人物不停闪烁、侧机位磨皮 / 美白 / 润色清晰全无效果。

- **根因**：`beautyEngine()` 是进程级单例，帧级状态全是文件级全局；多个 `obs_beauty_filter`
  实例交替写同一份上下文。
- **改法**：每个滤镜实例独占一个引擎槽 —— `SlotState` 按槽存帧号 / 掩码轨迹 / 肤色 EMA /
  hook user 与诊断计数，引擎按槽惰性实例化，异步 worker 绑定创建它的槽。
- **行为边界**：单实例 `slot=0` 行为不变；上限 **8 个实例/进程**，每多一实例多加载一份 YuNet 模型
  并多占一份帧环内存；`BEAUTY_SEG=1`（分割影子层）仍是进程级全局，**只支持单实例**，默认关。
- **回退**：无开关，回退 = 还原 2.16.1 代码。

**历史交付快照**：dll `631,808 B / MD5 06E275360025169B66B26C09C70528F4`；
`Setup.exe 1,532,416 B / MD5 FC0BA21D3D62BB5DB1EE8E01513CB565`；`--strict` 9 PASS。
> 该版 Setup.exe 装到 OBS 32.x 不加载的旧目录且闸门不查此项 —— 由 3.0.1 修复，不要照此版部署。

## 2.16.1（2026-11-04 · R1 严格字面回档）

用户实测 2.16.2–2.17.3 的「手遮眼 → 美白遮罩叠到手上」修复链**整体无效**，撤回功能基线：

- 撤回：`FaceParser` / `HandDetect` / 21 点手形凸包抠除 / 手形缓存 / CPU 掩码落盘增量 /
  方案 B `faceRoll` 重写 / 方案 A 两个手部模型打包；相关全局状态恢复 2.16.1 形态。
- **唯一保留的后续增量**：2.16.9「检测选项只在变化时置脏」通用掉帧修复（与遮挡无关，删掉会退回每帧重下发）。
- 全量备份 `_回档备份_20261104_2249`（18 文件快照）：其中 **4 篇文档已归档到 [`archive/backup-20261104_2249/`](archive/backup-20261104_2249/)**，逐篇映射见 [`INDEX.md`](INDEX.md) §2.2；其余 14 个源码 / dll / `Setup.exe` 仍在原目录、未移动、未入库；完整还原 18 文件时需把 4 篇归档件复制回原目录对应位置。
- 实验数据与失败结论**不随代码删除**，仍在归档总账 `§9.52.x`。

回档后实测：dll `600,064 B / MD5 7D83E51D230C3076402BDAB7781787EB`；
发布闸门 9 PASS；四条黄金基准 `maxdiff=0`。

## 更早里程碑（压缩，只留影响使用的）

| 版本 | 用户可见变化 | 回退 |
|---|---|---|
| 2.15.0 | 掩码跟脸转（`MASK_ROLL` 默认开）、默认掩码形状 0→3；检不到脸时自动退回 0° 并打 WARN | `BEAUTY_MASK_ROLL=0` + `BEAUTY_MASK_SHAPE=0` 回到 2.14.1 逐字节行为 |
| 2.14.1 | 内部清理（去两趟 Copy，draw 23→21），**画面逐位不变** | 无开关，回退 = revert |
| 2.14.0 | **默认值变化**：`BEAUTY_GPU_REGION` 由「默认关、认 `=1`」翻成「默认开、只认 `=0`」 | `BEAUTY_GPU_REGION=0`，不必换 dll |
| 2.13.1 | 修 N1：输入旋转提到 `letterboxRgb` 之前，全帧回退路径也生效 | — |
| 2.12.0 | 新增 opt-in `BEAUTY_DET_ADAPT_AGG=rms`（**默认仍 max，不翻**） | 不设即默认 |
| 2.11.0 | 新增 opt-in 方案C（`BEAUTY_MOTION_GPU=1`），默认关 | 不设即逐字节回到 2.10.x |
| 2.10.0 | 面板新增「输入方向 0/90/180/270」；2.15.2 起面板项删除，只剩环境变量 | — |

> 逐轮实测数字、判据与踩坑经过按 `§9.x` 在归档总账检索，映射见 [`INDEX.md`](INDEX.md) §3。

---

> **已合并自**（2026-11-05，文档治理批 4）：
> `04_文档/修复_GPU纯锐化掩码缺失_20261105.md` → [`archive/changelog-src-sharpen-mask-20261105.md`](archive/changelog-src-sharpen-mask-20261105.md)；
> `04_文档/修复_P2值域加权核_20261105.md` → [`archive/changelog-src-bilateral-p2-20261105.md`](archive/changelog-src-bilateral-p2-20261105.md)；
> `04_文档/修复_五官立体白圈_20261105.md` → [`archive/changelog-src-threed-mach-band-20261105.md`](archive/changelog-src-threed-mach-band-20261105.md)；
> `04_文档/修复_分割同步路径与segMix地雷_20261105.md` → [`archive/changelog-src-seg-sync-path-20261105.md`](archive/changelog-src-seg-sync-path-20261105.md)；
> `04_文档/修复_掩码椭圆双重旋转_20261105.md` → [`archive/changelog-src-mask-double-rotation-20261105.md`](archive/changelog-src-mask-double-rotation-20261105.md)；
> `04_文档/修复_祛斑与下巴_20261105.md` → [`archive/changelog-src-spot-chin-20261105.md`](archive/changelog-src-spot-chin-20261105.md)；
> `04_文档/修复_锐化阈值被覆盖_20261105.md` → [`archive/changelog-src-sharpen-threshold-20261105.md`](archive/changelog-src-sharpen-threshold-20261105.md)；
> `04_文档/结论_分割对手遮脸无效_20261105.md` → [`archive/conclusion-seg-occlusion-20261105.md`](archive/conclusion-seg-occlusion-20261105.md)
> （结论改写为 [`adr/adr-seg-not-for-occlusion.md`](adr/adr-seg-not-for-occlusion.md)）。
> 原文正文冻结，不作为当前结论依据；逐篇归档原因见各文件废弃头。
