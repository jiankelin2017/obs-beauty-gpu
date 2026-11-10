/*
 * obs-beauty-ui —— 把美颜滤镜的操作入口挂到 OBS 顶层菜单栏
 *
 * 机制（与商用「插件中心」同源，均经本机实测确认）：
 *   1. obs_frontend_get_main_window() 拿到 OBS 主窗口（QMainWindow*）
 *   2. mainWindow->menuBar()->addMenu() 往菜单栏插入一个顶层菜单
 *   3. 菜单项点开一个独立窗口，窗口内容由「滤镜自己的 obs_properties」自动生成
 *
 * 关键设计：面板不硬编码任何参数表，而是遍历滤镜的 obs_properties 动态建控件。
 * 因此滤镜那边新增/改名/改范围，这里自动跟随，不需要同步两份表。
 *
 * 依赖：Qt6Widgets / Qt6Gui / Qt6Core + obs.dll + obs-frontend-api.dll
 * 注意：OBS 安装包不提供 obs-frontend-api.h，这里只声明用到的那一个函数。
 *
 * 3.2.4：本文件由 CPU 分支仓库引入 GPU 分支仓库，并从「手工部署」改为
 *        「安装包载荷」——安装器把它装到
 *        %PROGRAMDATA%\obs-studio\plugins\obs-beauty-ui\bin\64bit\。
 *        起因：3.2.3 的安装路径也调用了 purgeMenuModule()，而载荷里没有
 *        这个 dll，于是「装完顶部菜单栏就没有美颜入口」。代码逻辑一行未改，
 *        只改了这行注释；编译脚本见 02_构建/build_ui.ps1（不定义
 *        BEAUTY_UI_CPU_EDITION，菜单标题因此是「美颜(&A)」）。
 */

#include "beauty_params.h"       /* 版本号唯一出处：与滤镜共用 BEAUTY_BUILD_STR */
#include <windows.h>              /* GetCurrentThreadId: same numeric ID as filter plugin logs */
#include <obs-module.h>
#include <obs.h>
#include <obs-properties.h>

#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMenu>
#include <QtGui/QAction>          /* Qt6：QAction 已从 QtWidgets 移到 QtGui */
#include <QtWidgets/QDialog>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>  /* 3.1.32：底部 2×2 区块（档位栏 + 两个按钮） */
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>   /* 3.1.31：fit_to_content 读 maximum() 量真实内容高 */
#include <QtWidgets/QTabWidget>   /* 3.1.26：顶部分类标签页 */
#include <QtWidgets/QTabBar>
#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QSlider>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QToolButton>
#include <QtCore/QTimer>
#include <QtCore/QString>
#include <QtCore/QSettings>     /* 3.1.37：窗口几何记忆（IniFormat，路径由 obs_module_config_path 给） */
#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QEvent>   /* QEvent::Show / WindowActivate，见 BeautyPanel::event() */
#include <QtGui/QScreen>
#include <QtGui/QGuiApplication>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()

/* 3.0.21：修「面板认不出新 id 的滤镜」。滤镜改名后 source id 变成
 *         obs_beauty_filter，面板原来的匹配只认旧的 legacy_beauty_filter，
 *         用户用新 id 加滤镜时面板报「没有找到美颜滤镜实例」。
 *         现在 isBeautyFilterId() 同时认两个 id（这是唯一的改动，界面不变）。
 * 3.0.20：界面减负——去掉「高级」标题里那串括注、删掉三个 preset 按钮、
 *         默认宽度收到 460 逻辑 px（此前会被长标题顶到 780+）。
 * 3.0.19：面板底部左下角常显版本号（常驻控件，随参数区滚动不消失）。
 *         版本号只引下面这一个宏 —— 菜单「关于」与面板角标共用它，
 *         升级时改这里一处即两处同步，不会出现"关于里 3.0.19、角标还是 3.0.18"。
 * 3.0.18：面板排版重做（自适应尺寸 / 紧凑行高 / 「高级」折叠 / 三个按钮真做出来）。
 * 只改本模块，滤镜 DLL 与 OBS 原生属性页完全不动。
 * 只改本模块，滤镜 DLL 与 OBS 原生属性页完全不动。
 * 3.1.11：**版本同号**（用户拍板）。此前面板走自己的 3.0.x 线，装机后角标
 *         v3.0.21 让用户以为滤镜还是旧版。从这版起面板与滤镜共用一个版本号
 *         （每次发布同步 bump），角标优先显示**滤镜 proc 报上来的实机版本**，
 *         面板宏只是回退与「与滤镜不符时」的对照标注 —— 安装漏换任何一边
 *         都能当场看出来，而不是默默显示两个不一致的号。
 * 3.1.13：版本同号补课 —— 滤镜 3.1.12/3.1.13 两版发的时候面板没跟着 bump，
 *         装机日志实锤 `模块已加载 v3.1.11` vs 滤镜 v3.1.13，角标会带
 *         「（面板 3.1.11）」括注。这次追平，无任何界面/行为改动。
 * 3.1.14：跟滤镜同步 bump（磨皮第 1 批结构对齐，见 legacy_engine.h 的 3.1.14 条）。
 *         面板本身无改动，仅为保持"版本同号"纪律，避免角标出现括注。
 * 3.1.15：跟滤镜同步 bump（磨皮第 2 批-A：25 tap 表面模糊，默认关闭）。
 *         面板本身无改动。
 * 3.1.16：跟滤镜同步 bump（磨皮第 2 批-B：双尺度带通 surface3RgbBand，
 *         默认关闭；核心引擎 0.9 → 1.0.0，因为磨皮核整体换掉了）。
 * 3.1.18：跟滤镜同步 bump（磨皮档位曲线对齐产品：强度改五锚点查表、
 *         evenfactor 固定 0.70，核心引擎 1.1.0 → 1.2.0）。面板本身无改动。
 * 3.1.19：跟滤镜同步 bump（磨皮选择性对齐产品：T 8.3→6.5、evenfactor
 *         0.70→0.95，核心引擎 1.2.0 → 1.3.0）。面板本身无改动。
 * 3.1.23：面板 UI 改版（总开关文案简化、磨皮算法/模式下拉名称简化、
 *         按类型与功能分成 磨皮/肤色/锐化/五官 四组），跟滤镜同步 bump。
 * 3.1.25：界面整体重设计。放弃 QGroupBox（OBS 主题给它留标题带，标题必压框，
 *         且组内上下留白极大 —— 真机截图里一组能空出半个屏），四组改成
 *         圆角「卡片」：中性半透明底色 + 组内左上角小标题，深浅两套主题
 *         都不用单独配色。同时修「恢复默认」点了没反应（见 reset_defaults）。
 *         所以比例是可比的）：行心距 82→50 物理 px、控件高 62→38.5、字号 12→11、
 *         标签**右对齐成一列**、滑块与数值框的右缘对齐到同一条竖线。
 *         结构上照参考的做法：4 张卡片全部拆掉，改成**顶部标签页 + 扁平参数表** ——
 *         磨皮/肤色/锐化/五官 各一页，页内不再有分组框、不再有多余留白。
 * 3.1.35：**撤回 3.1.32 的「行距撑满整页」**。用户原话：「原来选项直接的间距是
 *         正确的，只是调整新选项的位置」—— 这句话点明了前几轮一直搞错的地方：
 *         用户要的一直是「间距保持原样（固定 8 逻辑 px），只把新增的选项摆到对
 *         的位置」，而 3.1.32 为了填满固定 670×750 的窗口，写了个 spread_page()
 *         按视口高度反推行距（实测撑到 91，是原来的 11 倍），4 行参数之间全是
 *         大片空白，真机截图一眼就看得出「松」。
 *         现在：删掉 spread_page()，行距回到 make_tab() 里写死的 **8**；
 *         窗口高度改成**跟着当前页的真实内容走**（chrome + 内容高），
 *         不再钉死 611 —— 这样短页（锐化 2 行）不会被撑，长页（五官 5 行）
 *         也不会裁。新增选项（精准美肤 / 磨皮算法·磨皮模式移到底部 2×2）
 *         的位置保持不变，只调间距。
 * 3.1.36：**修「右下角少了两个按钮」+ 钉死 460×560**。
 *         用户 19:37 截图实锤两件事：
 *         a) 底部右侧空着，没有「恢复默认 / 关闭」——根因是**控件被误删**：
 *            构造函数把两个按钮加进 mode_grid_，紧接着 reload() → build_form()
 *            → clear_form() → purge_layout(mode_grid_)，把 grid 里**所有**控件
 *            连按钮一起 deleteLater 了，之后再没造回来。现在改成每次 build_form
 *            内重建按钮（与档位下拉同一生命周期），从此不会凭空消失。
 *         b) 内容溢出：5 行参数 + 两行档位把窗口顶到 567 还不够，底部一排被裁、
 *            右侧冒出滚动条。这版把行高 34→32、页内行距 16→16（像素不变但
 *            页边距 16/18→8/10）、根间距 12→10、上下边距收紧，实测 5 行页
 *            在 460×520 客户区里还剩 16~26 px 余量；尺寸改为 setFixedSize，
 *            fit_to_content 的「内容不够就加高窗口」整段删除 —— 它正是前几版
 *            窗口越量越高、比例越来越细长的原因。
 *         按钮尺寸按用户第二张图（110×55 竖排两个）取 108×44，间距 6。
 * 3.1.37：**去掉固定尺寸，改成「可自由拖动 + 记住上次大小/位置」**。
 *         用户原话：「给UI新增窗口大小记忆功能。去除固定大小！」
 *         3.1.36 的 setFixedSize 是我为了「一模一样复刻 460×560」加的，
 *         代价是窗口再也拖不动、每台机器的 DPI 不同就得重调一次。
 *         这版把「尺寸」从**常量**改回**状态**：
 *           a) setFixedSize → setMinimumSize(360, 420) + 初始 resize；
 *              下限是防「拖成 1px 碎片、滚都滚不出来」，不是审美。
 *           b) 三处强制设尺寸的调用点全部拆掉（构造函数 / Show 事件 /
 *              build_form 末尾）—— 只删 setFixedSize 是不够的，后两处每次
 *              刷新或失焦回来都会把用户刚拖好的尺寸打回 460×529。
 *           c) 几何用 saveGeometry()（含 大小+位置+最大化+所属屏）base64
 *              存到 obs_module_config_path("ui.ini")，即
 *              %APPDATA%\obs-studio\plugin_config\obs-beauty-ui\ui.ini ——
 *              OBS 标准插件配置位置，便携模式下跟着走，不碰注册表、
 *              不碰 OBS 自己的 global.ini。
 *           d) 落盘时机：拖动/缩放停下 800ms 防抖写一次 + 关闭窗口写一次，
 *              所以 OBS 被强杀也不会丢。
 *           e) 恢复时做**合理性校验**：拔掉副屏、换 DPI 后旧几何可能整个落到
 *              屏幕外，此时丢弃几何、退回 460×529 居中；尺寸过大也夹到
 *              当前屏可用区内，保证「窗口永远抓得回来」。
 *           f) 「恢复默认」按钮按用户要求**合并了重置窗口大小**：点一下既复位
 *              滤镜参数，也把窗口打回 460×529 居中并清掉 ini 里的几何记录。
 * 3.1.38：**统一所有控件尺寸**。
 *         用户原话：「UI大小都不一样，请统一。」（截图箭头指刷新按钮、红润数值框、
 *         右下两个按钮）
 *         实测（用户 175% 截图 ÷1.75）：下拉 28.6 / 标签页 31 / 行数值框 28.6 /
 *         底部按钮 51 —— 四种高度。根因**不在代码**，在 OBS 主题：
 *           Yami.obt 给每类控件各算一套高度：
 *             QComboBox  : height: var(--input_height)
 *             QSpinBox   : height + max-height: var(--input_height)
 *             QPushButton: height + max-height + padding(--input_padding)
 *                          + margin-top/bottom(--spacing_input)
 *         面板旧 QSS 只覆盖了 padding，height/margin 全放给主题算，于是各算各的。
 *         更麻烦的是代码里还有**第二套数字**（setFixedHeight(32)、
 *         setFixedSize(108,44)）：QSS 的 height 优先级高于 setFixedHeight，
 *         所以那套数字**部分生效** —— 宽度 74 生效（实测 74），
 *         高度 32 没生效（实测 28.6）。同一句 setFixedSize 只生效一半，
 *         这就是「怎么调都对不齐」的原因。
 *         修法：
 *           a) 高度只在样式表里定义一次（kCtrlH = 26 → 总高 28，含 1px 边框 ×2）；
 *              height / min-height / max-height / margin / padding 五件套写全，
 *              主题那套 height 和 margin 全部被压掉。
 *           b) 代码里**删光所有高度数字**，只留宽度（下拉 74、按钮 108）。
 *           c) 复选框无边框，单独用 %2 = kCtrlH+2，与其他控件同高。
 *           d) 标签页 tab 只能靠 padding 调（QTabBar::tab 不认 min-height），
 *              实测 27，比控件矮 1px。
 *         探针同步：它以前自造一套「假主题」，没有 margin/height，所以
 *         永远测不出真机高度（这正是 3.1.26~3.1.37 反复"探针 OK、装机不对"
 *         的原因）。现在探针按 Yami.obt 的真实写法模拟，并新增
 *         reportCtrlHeights() 直接打印每类控件的 height()，min==max 才算过。 */
// 3.1.40：面板「拖了美颜没反应」自愈修复（UI 源码唯一版本出处，装机件由 3.1.39 升来）。
//   ① 无滤镜实例时不再静默丢参数：限流日志 + 面板红字提示，明确告诉用户去点「刷新」；
//   ② 记住上次绑定的 (源, 滤镜)，实例失效后在窗口激活时自动重新枚举并重连；
//   ③ 滑块回调里只做「轻量重连」——绝不调用 build_form()，否则正在按着的控件会被
//      deleteLater 掉（见 sync_now 的 mouseButtons 判据，历史上为此崩过）。
/* ---- CPU 版显式标识（2026-11-06 立轴）--------------------------------
 * 只有 CPU 版构建脚本定义 BEAUTY_UI_CPU_EDITION=1。默认 0 —— GPU 版共用
 * 同一份 UI 源码时不定义该宏，窗口标题 / 菜单 / 角标 / 关于全部保持原样，
 * 因此本改动不会污染 GPU 版，也不会改变它正在使用的 dll 行为。 */
#ifndef BEAUTY_UI_CPU_EDITION
#define BEAUTY_UI_CPU_EDITION 0
#endif

#define BEAUTY_UI_VERSION BEAUTY_BUILD_STR   /* 2026-11-07：不再硬编码，直接取唯一出处 */

/* ---- 前端 API：OBS 安装包不带 obs-frontend-api.h，按 ABI 直接声明 ---- */
extern "C" {
void *obs_frontend_get_main_window(void);
}

/* 滤镜 source id：新 id 在前，旧 id（改名前的 "legacy_beauty_filter"）保留兼容 ——
 * 老场景里已挂好的滤镜存的是旧 id，面板必须两个都认，否则会找不到实例。 */
#define BEAUTY_FILTER_ID        "obs_beauty_filter"
#define BEAUTY_FILTER_ID_LEGACY "legacy_beauty_filter"
#if BEAUTY_UI_CPU_EDITION
#define BEAUTY_MENU_TITLE "\xe7\xbe\x8e\xe9\xa2\x9c" "\xc2\xb7" "CPU(&A)" /* 美颜·CPU(&A) */
#else
#define BEAUTY_MENU_TITLE "\xe7\xbe\x8e\xe9\xa2\x9c(&A)" /* 美颜(&A) */
#endif

static inline bool is_beauty_filter_id(const char *id)
{
	return id && (strcmp(id, BEAUTY_FILTER_ID) == 0 ||
		      strcmp(id, BEAUTY_FILTER_ID_LEGACY) == 0);
}

#ifdef BEAUTY_UI_AUTOTEST_BUILD
static bool beauty_ui_autotest_enabled();
static void ui_autotest_trace(const char *step)
{
	if (beauty_ui_autotest_enabled())
		blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: %s", step);
}
#else
static inline void ui_autotest_trace(const char *) {}
#endif

/* ==================================================================
 * 滤镜实例枚举
 * ================================================================== */
struct Inst {
	std::string source;
	std::string filter;
};

static void enum_filter_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	auto *v = static_cast<std::vector<Inst> *>(param);
	const char *id = obs_source_get_id(child);
	if (!is_beauty_filter_id(id))
		return;
	const char *sn = obs_source_get_name(parent);
	const char *fn = obs_source_get_name(child);
	v->push_back({sn ? sn : "?", fn ? fn : "?"});
}

static bool enum_source_cb(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, enum_filter_cb, param);
	return true;
}

static std::vector<Inst> collect_instances(void)
{
	std::vector<Inst> v;
	obs_enum_sources(enum_source_cb, &v);
	return v;
}

/* ==================================================================
 * 参数面板：遍历滤镜的 obs_properties 自动生成控件
 * ================================================================== */
class BeautyPanel : public QDialog {
public:
	explicit BeautyPanel(QWidget *parent) : QDialog(parent)
	{
#if BEAUTY_UI_CPU_EDITION
		setWindowTitle(QString::fromUtf8("OBS美颜 · CPU 版 v" BEAUTY_UI_VERSION));
#else
		setWindowTitle(QString::fromUtf8("OBS美颜参数面板"));
#endif
		/* 3.0.18：以前写死 resize(460, 620)，但表单有 22 行、每行约 52 逻辑 px，
		 * 实际高度被布局撑到 1150 px 以上 —— 1080p 屏在 150% 缩放下可用高度
		 * 只有约 680 px，一屏只露出 1/3 内容。现在改成：
		 *   1) 字号/行距/滑块高度整体收一档（一行约 28 px）
		 *   2) 尺寸按屏幕可用区域算，不再写死
		 *   3) 顶层散装属性自动折叠进「高级」，按钮横排成一行
		 * 收完 14 行美颜 ≈ 540 px，一屏放得下。 */
		/* 3.1.26：按对照插件的密度重写样式表。
		 * 上一版（3.1.25）做的「卡片」观感问题是：字号 12px + 行距 10px +
		 * 控件高 26px，一屏只放得下 4~5 行；同样的面板尺寸
		 * 能放 12 行以上。这版逐项按截图比例收：字号 12→11、控件高 26→20、
		 * 行距 7→2，并去掉卡片留白（结构改成标签页，见下）。 */
		/* ==============================================================
		 * 3.1.38：控件高度**只在这里定义一次**
		 *
		 * 为什么之前「大小都不一样」：OBS 主题（Yami.obt）给每类控件各自
		 * 算了一套高度，而且算法不同 ——
		 *   QComboBox  : height: var(--input_height)               → 约 28.6
		 *   QSpinBox   : height + max-height: var(--input_height)   → 约 28.6
		 *   QPushButton: height + max-height + padding(--input_padding)
		 *                + margin-top/bottom: var(--spacing_input)  → 实测 51
		 * 面板旧 QSS 只覆盖了 padding，height/margin 全放给主题算，于是
		 * 「下拉 28.6 / 标签页 31 / 按钮 51」三种高度并排出现。
		 *
		 * 更糟的是代码里还有第二套数字（setFixedHeight(32)、
		 * setFixedSize(108,44)）。样式表的 height 优先级高于控件的
		 * setFixedHeight，所以那套数字**部分生效、部分不生效** ——
		 * 宽度 74 生效了（实测 74），高度 32 没生效（实测 28.6）。
		 *
		 * 现在：高度、margin、padding 三件套一次写全，四类控件同一份；
		 * 代码里**不再有任何 setFixedHeight / 高度数字**，只留宽度。
		 *
		 * kCtrlH 是**内容盒**高度（Qt 的 QSS height 不含 border/padding），
		 * 加上 1px 边框 ×2 才是控件总高 = kCtrlH + 2。
		 * 改高度只改这一个数，QSS 里 %1 全部跟着变。
		 * ============================================================== */
		const int kCtrlH = 26;   /* → 总高 28 逻辑 px */
		setStyleSheet(QString::fromUtf8(
			"QDialog{font-size:14px;}"
			"QLabel{font-size:14px;}"
			/* 标签页做成「药丸」状：未选中是中性半透明灰（深浅主题自适应），
			 * 选中直接用主题的 highlight 色 —— 不写死蓝色，换主题不会错色。
			 * 3.1.38：padding 上下 5→1，tab 实测 27 高（比控件 28 矮 1px，肉眼不可见）。
			 * 注意 QTabBar::tab 不认 min-height/max-height（试过，实测无效），
			 * 想改 tab 高度只能调 padding。 */
			"QTabWidget::pane{border:none;background:transparent;}"
			"QTabBar::tab{background:rgba(128,128,128,40);"
			"color:palette(window-text);border:none;border-radius:5px;"
			"padding:1px 12px;margin:0px 6px 5px 0px;font-size:15px;"
			"min-width:32px;}"
			"QTabBar::tab:selected{background:palette(highlight);"
			"color:palette(highlighted-text);}"
			"QTabBar::tab:hover:!selected{background:rgba(128,128,128,72);}"
			"QLabel#beautyTitle{font-size:18px;font-weight:bold;}"
			"QLabel#beautyHint{color:#8a8a8a;font-size:12px;}"
#if BEAUTY_UI_CPU_EDITION
			"QLabel#beautyCpuTag{color:#185FA5;background:#E6F1FB;"
			"border:1px solid #85B7EB;border-radius:4px;padding:1px 6px;"
			"font-size:12px;font-weight:bold;}"
#endif
			/* ★ 统一尺寸的唯一出处：四类可交互控件同高、零外边距 */
			"QComboBox,QPushButton,QDoubleSpinBox,QSpinBox,QLineEdit{"
			"font-size:14px;"
			"height:%1px;min-height:%1px;max-height:%1px;"
			"margin:0px;"          /* 主题给了 margin-top/bottom，会让同排错位 */
			"padding:0px 8px;}"    /* 垂直 padding 必须 0，否则叠在 height 上 */
			/* 复选框无边框，要 +2 才和带 1px 边框的控件同高（%2 = kCtrlH+2） */
			"QCheckBox{font-size:14px;margin:0px;"
			"height:%2px;min-height:%2px;max-height:%2px;}"
			/* 数值框内容靠右，水平 padding 小一点；垂直仍为 0 */
			"QDoubleSpinBox,QSpinBox{padding:0px 4px;}"
			"QSlider{max-height:20px;}").arg(kCtrlH).arg(kCtrlH + 2));

		auto *root = new QVBoxLayout(this);
		/* 3.1.30：右边距 10 → 50 —— **右列内收 40 逻辑 px**，右侧留一条空白边。
		 * 用户要求「右边 UI 整体往左移」：只改这一个数字，刷新按钮、参数行
		 * 数值框、恢复默认/关闭、页内滑块区四条右缘会一起内收，天然对齐；
		 * 分散去改每个控件的宽度必然走偏（3.1.26 收 min-width 时踩过）。
		 * 代价：滑块区比原来短 40 px（400 宽下面板滑块仍有约 194 px 可拖）。 */
		/* 3.1.36：460×560 的纵向预算重排 —— 上 14 / 下 10，块间距 10。
		 * 右 14：数值框右缘落在 460−14−2(页内边距) 处，与参考图一致。 */
		root->setContentsMargins(12, 14, 14, 10);
		root->setSpacing(10);

		/* ---- 顶栏：作用对象 + 刷新 ---- */
		auto *top = new QHBoxLayout();
		top_row_ = top;
		top->setSpacing(6);
#if BEAUTY_UI_CPU_EDITION
		auto *cpuTag = new QLabel(QString::fromUtf8("CPU 版"), this);
		cpuTag->setObjectName("beautyCpuTag");
		top->addWidget(cpuTag);
#endif
		top->addWidget(new QLabel(QString::fromUtf8("作用对象")));
		combo_ = new QComboBox();
		/* 长源名不要撑爆窗口宽度：给个下限，超出部分由下拉自己省略 */
		combo_->setMinimumContentsLength(14);
		combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo_->setMinimumWidth(150);
		/* 3.1.38：不再 setFixedHeight —— 高度由样式表 kCtrlH 统一，
		 * 代码里写第二套数字正是「下拉 28.6 / 按钮 51」并排出现的根因。 */
		top->addWidget(combo_, 1);
		auto *refresh = new QPushButton(QString::fromUtf8("刷新"));
		refresh->setFixedWidth(74);   /* 只钉宽度；高度同 kCtrlH */
		refresh->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		top->addWidget(refresh);
		root->addLayout(top);

		/* ---- 总开关行：复选框 + 粗体名字 ----
		 * 3.1.25：原来它被塞进一个 QFormLayout，标签在左、复选框在右，
		 * 和下面那些参数行长得一模一样，看不出「这是一级总开关」。
		 * 现在单独一行、文字加粗放大一档，复选框紧贴文字。
		 * 控件由 build_form() 填（要 obs_property 才知道文案）。 */
		sw_row_ = new QHBoxLayout();
		sw_row_->setSpacing(6);
		sw_row_->setContentsMargins(2, 0, 2, 0);
		root->addLayout(sw_row_);

		/* 3.1.26：提示行改成「只在出错时出现」。
		 * 顶部没有任何说明文字 —— 这条信息对新用户只有第一次有用，
		 * 之后每次开面板都是一行灰噪音。现在正常状态隐藏，布局会把这行
		 * 连同行距一起收掉；找不到滤镜等错误仍然要显示（见 reload）。 */
		hint_ = new QLabel();
		hint_->setObjectName("beautyHint");
		hint_->setWordWrap(true);
		hint_->setVisible(false);
		root->addWidget(hint_);

		/* 3.1.26：卡片 + 单张滚动表 → 顶部标签页。
		 * 这一类的参数，页内是扁平表单（没有分组框、没有卡片）。
		 * 好处是「一屏看全本类参数」而且没有卡片留白；
		 * 代价是跨类对比需要切页 —— 这是参考图的做法，照做。
		 * 每页各自套一个 QScrollArea：小屏（或磨皮页以外内容超长）时页内滚动，
		 * 不把整个窗口撑出屏幕。 */
		tabs_ = new QTabWidget();
		/* 3.1.26：不要 documentMode —— 实测（离屏探针）documentMode 下
		 * QTabBar 走的是「整行均分的文档式标签」，5 个标签会平分整个宽度，
		 * 参考图里那种「窄标签靠左排一排」出不来。关掉之后 setExpanding(false)
		 * 才真正生效。 */
		tabs_->setDocumentMode(false);
		tabs_->setUsesScrollButtons(false);
		tabs_->setElideMode(Qt::ElideRight);
		tabs_->tabBar()->setExpanding(false);
		tabs_->tabBar()->setDrawBase(false);
		tabs_->setFocusPolicy(Qt::NoFocus);
		root->addWidget(tabs_, 1);

		/* 3.1.32：版本号从底栏挪到「启用美颜」行右侧（按用户草图）。
		 * 底栏那一行被底部 2×2 区块取代，见下。
		 * 注意：sw_row_ 每次 build_form 都会被 purge_layout 清空，
		 * 所以 clear_form() 里要先 removeWidget(ver_) 保住它，
		 * 再由 build_form 末尾重新挂回去（见那两处注释）。 */
#if BEAUTY_UI_CPU_EDITION
		ver_ = new QLabel(QString::fromUtf8("CPU v" BEAUTY_UI_VERSION), this);
		ver_->setObjectName("beautyCpuTag");
#else
		ver_ = new QLabel(QString::fromUtf8("v" BEAUTY_UI_VERSION), this);
		ver_->setObjectName("beautyHint");
#endif
		ver_->setToolTip(QString::fromUtf8(
			"选中滤镜实例后实时显示滤镜版本（面板与滤镜版本同号，不符即为装错"));
		ver_->setTextInteractionFlags(Qt::NoTextInteraction);
		sw_row_->addStretch(1);
		sw_row_->addWidget(ver_);

		/* 3.1.32：**底部 2×2 区块**（按用户草图重置）——
		 *     磨皮算法 [下拉]        恢复默认
		 *     磨皮模式 [下拉]        关闭
		 * 3.1.29/3.1.30 是「档位栏一行 + 底栏一行」共两行、按钮横排在底栏右端；
		 * 草图要求磨皮算法 / 磨皮模式**各占一行**，两个按钮分别落在对应行右侧。
		 *
		 * 3.1.36 修 BUG：两个按钮原来在构造函数里 addWidget 进来，紧接着
		 * reload() → build_form() → clear_form() → purge_layout(mode_grid_)
		 * 会把 grid 里**所有**控件（含这两个按钮）deleteLater 掉，之后再没重建
		 * → 真机上底部右侧永远是空的（用户 19:37 截图实锤）。
		 * 现在按钮与档位下拉同生命周期，每次 build_form 重建（见下方）。
		 * 这里只保留网格本身与列宽设定。 */
		mode_grid_ = new QGridLayout();
		/* 3.1.36：行距 10（原 14），列距 10；按钮贴右缘，中间空白列拉伸 */
		mode_grid_->setSpacing(10);
		mode_grid_->setContentsMargins(2, 0, 2, 0);
		mode_grid_->setColumnStretch(2, 1);      /* 空白列拉伸 */
		mode_grid_->setColumnMinimumWidth(0, 76); /* 与 make_label 的标签宽一致 */
		root->addLayout(mode_grid_);

		connect(refresh, &QPushButton::clicked, [this]() { reload(true); });
		connect(combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
			[this](int idx) { select_instance(idx); });

		reload(false);
		/* 3.1.37：构造末尾只做一次「恢复上次几何」，没有记忆时退回 460×529 居中。
		 * 这里不能再有 setFixedSize —— 窗口从这一刻起归用户管。 */
		restore_ui_state();
		ready_ = true;

		/* 3.1.36：切页不再重算窗口尺寸 —— 尺寸由用户钉死 460×560（客户区 520），
		 * 各页内容都按这个高度排好；哪页真装不下，页内滚动（滚轮/拖条），
		 * 窗口本身**绝不**跟着长高。3.1.31~3.1.35 的「量内容、加高窗口」
		 * 正是「比例越用越细长」的来源，这一版整段删除。 */
	}

	/* ==================================================================
	 * 3.1.37：窗口几何 —— **记忆**取代**固定**
	 *
	 * 三个常量各管一段，别混用：
	 *   kDefault*  —— 第一次打开（或记忆被判定失效）时的尺寸，即 3.1.36 的定稿值。
	 *                529 是**客户区**：用户要的 560 含 Windows 标题栏 31 逻辑 px。
	 *   kMin*      —— 拖动下限。不设下限用户能把窗口拖成 1px 碎片，连滚都滚不出来。
	 *   kRescue*   —— 恢复的几何落在屏幕外时，往屏幕里拉回多少的兜底余量。
	 *
	 * 存的是 saveGeometry()（大小 + 位置 + 是否最大化 + 所属屏，Qt 已做 DPI 修正），
	 * 另存逻辑 w/h 作降级：saveGeometry 解出来若比屏幕可用区还大（换小屏/拔副屏），
	 * 用它重新算一个能放下的尺寸。
	 * ================================================================== */
	/* 用 constexpr int 而不是 enum：未命名 enum 的类型推断会让 qBound 的
	 * 模板参数（T=int, U=<unnamed-enum>）对不上，MSVC 直接报 C2737。 */
	static constexpr int kDefaultW = 460;
	static constexpr int kDefaultH = 529;
	static constexpr int kMinW = 360;
	static constexpr int kMinH = 420;

	/* %APPDATA%\obs-studio\plugin_config\obs-beauty-ui\ui.ini
	 * obs_module_config_path 返回的是 malloc 出来的 UTF-8，必须 bfree。 */
	static QString config_ini_path()
	{
		char *p = obs_module_config_path("ui.ini");
		QString s;
		if (p) {
			s = QString::fromUtf8(p);
			bfree(p);
		}
		return s;
	}

	QScreen *target_screen() const
	{
		QScreen *scr = screen();
		return scr ? scr : QGuiApplication::primaryScreen();
	}

	/* 窗口「抓得回来」的判据：至少有一块屏的可用区与它相交 80×30 以上
	 * （80 是标题栏上还能点到「关闭」的宽度，30 是标题栏本身的高度）。
	 * 用 frameGeometry 而不是 geometry：用户抓的是外框。 */
	bool geometry_reachable() const
	{
		const QRect g = frameGeometry();
		for (QScreen *scr : QGuiApplication::screens()) {
			const QRect inter = scr->availableGeometry().intersected(g);
			if (inter.width() >= 80 && inter.height() >= 30)
				return true;
		}
		return false;
	}

	void center_on_screen()
	{
		QScreen *scr = target_screen();
		if (!scr)
			return;
		const QRect avail = scr->availableGeometry();
		move(avail.center().x() - width() / 2, avail.center().y() - height() / 2);
	}

	/* 把当前尺寸/位置夹进屏幕可用区。用于恢复后的兜底：
	 * 目标机的屏比记忆来源小（或 DPI 变了）时，别让窗口一半在屏幕外。 */
	void cap_to_screen()
	{
		QScreen *scr = target_screen();
		if (!scr)
			return;
		const QRect avail = scr->availableGeometry();
		const int maxW = qMax(kMinW, avail.width() - 40);
		const int maxH = qMax(kMinH, avail.height() - 80);
		const int w = qBound(kMinW, width(), maxW);
		const int h = qBound(kMinH, height(), maxH);
		if (w != width() || h != height())
			resize(w, h);
		if (!geometry_reachable()) {
			/* 完全在屏幕外 → 不再尝试「拉回来」，直接居中更省事也更可预期 */
			center_on_screen();
			return;
		}
		const int x = qBound(avail.left() - 8, pos().x(), avail.right() - 80);
		const int y = qBound(avail.top(), pos().y(), avail.bottom() - 40);
		if (x != pos().x() || y != pos().y())
			move(x, y);
	}

	/* 构造末尾调用一次。无记录 / 记录失效 → 460×529 居中。 */
	void restore_ui_state()
	{
		polish_tree();
		setMinimumSize(kMinW, kMinH);
		setSizeGripEnabled(true);   /* 右下角给个可拖的抓手，别只靠边框 */

		const QString path = config_ini_path();
		QSettings st(path, QSettings::IniFormat);
		bool restored = false;
		if (!path.isEmpty()) {
			const QByteArray geo = QByteArray::fromBase64(
				st.value(QStringLiteral("window/geometry")).toByteArray());
			if (!geo.isEmpty())
				restored = restoreGeometry(geo);
			if (restored) {
				const int w = st.value(QStringLiteral("window/w"), 0).toInt();
				const int h = st.value(QStringLiteral("window/h"), 0).toInt();
				/* saveGeometry 解出来的尺寸离谱（换屏/换 DPI）→ 用逻辑 w/h 重排 */
				if (w >= kMinW && h >= kMinH &&
				    (qAbs(w - width()) > 40 || qAbs(h - height()) > 40))
					resize(w, h);
			}
		}
		if (!restored) {
			resize(kDefaultW, kDefaultH);
			center_on_screen();
		}
		cap_to_screen();
		blog(LOG_INFO,
		     "[obs-beauty-ui] window geometry %s: %dx%d at (%d,%d)  ini=%s",
		     restored ? "restored" : "default", width(), height(), pos().x(),
		     pos().y(), path.isEmpty() ? "(none)" : path.toUtf8().constData());
	}

	void save_ui_state()
	{
		const QString path = config_ini_path();
		if (path.isEmpty())
			return;
		QDir().mkpath(QFileInfo(path).absolutePath());   /* 目录不存在时兜底建一次 */
		QSettings st(path, QSettings::IniFormat);
		st.setValue(QStringLiteral("window/geometry"),
			    QString::fromLatin1(saveGeometry().toBase64()));
		st.setValue(QStringLiteral("window/w"), width());
		st.setValue(QStringLiteral("window/h"), height());
		st.sync();
	}

	/* 拖动/缩放停下 800ms 才写盘 —— 拖动过程每帧都写就是几十次磁盘 IO。
	 * 单发定时器每次 start() 会重置计时，天然就是防抖。 */
	void schedule_save()
	{
		if (!ready_)
			return;   /* 构造期（restore_ui_state 里的 resize/move）不算用户操作 */
		if (!save_timer_) {
			save_timer_ = new QTimer(this);
			save_timer_->setSingleShot(true);
			save_timer_->setInterval(800);
			connect(save_timer_, &QTimer::timeout, this, [this]() { save_ui_state(); });
		}
		save_timer_->start();
	}

	/* 用户点「恢复默认」时**一并**执行（用户明确要求合并成一个开关）：
	 * 清掉 ini 里的几何记录 → 窗口回 460×529 居中。 */
	void reset_window_size()
	{
		const QString path = config_ini_path();
		if (!path.isEmpty()) {
			QSettings st(path, QSettings::IniFormat);
			st.remove(QStringLiteral("window/geometry"));
			st.remove(QStringLiteral("window/w"));
			st.remove(QStringLiteral("window/h"));
			st.sync();
		}
		resize(kDefaultW, kDefaultH);
		center_on_screen();
		/* 立刻把新几何写回去，免得关闭窗口时又把「重置前」的尺寸存下来 */
		if (save_timer_)
			save_timer_->stop();
		save_ui_state();
		blog(LOG_INFO, "[obs-beauty-ui] window geometry reset to %dx%d", kDefaultW,
		     kDefaultH);
	}

	/* ==================================================================
	 * 3.1.36：删掉 3.1.31~3.1.35 的整套「量内容 → 改窗口高」机器
	 *
	 * 被删掉的成员/函数：seed_page_need()、schedule_fit()、fit_to_content()、
	 * form_content_height()、pageNeed_/pageMeasured_/fitPass_/fitScheduled_。
	 * 它们的存在前提是「窗口高 = 内容高」，而这个前提已经被用户否掉：
	 * 窗口高是**用户指定的常量 560**，内容要迁就窗口，不是反过来。
	 * 留着这套代码只会让下一个人再去调「行距/高度」，重蹈覆辙。
	 * ================================================================== */

	/* 先把整棵树的 polish 跑完再量/再设尺寸。主题给控件写的 min-height
	 * 要 polish 之后才生效，不跑这一步，设尺寸那一刻看起来「还松」。 */
	void polish_tree()
	{
		ensurePolished();
		const QList<QWidget *> kids = findChildren<QWidget *>();
		for (QWidget *w : kids)
			w->ensurePolished();
		if (QLayout *l = layout())
			l->activate();
	}

	QVBoxLayout *root() const { return qobject_cast<QVBoxLayout *>(layout()); }

	~BeautyPanel() override
	{
		if (filter_)
			obs_source_release(filter_);
	}

#ifdef BEAUTY_UI_AUTOTEST_BUILD
	/* Only enabled in a disposable test build. This drives real Qt signals on
	 * the UI thread after obs_properties has been destroyed. */
	void run_stability_test()
	{
		blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: begin panel test");
		int sliders_driven = 0;
		for (QSlider *sl : findChildren<QSlider *>()) {
			const int old = sl->value();
			const int next = (old < sl->maximum()) ? old + 1 : old - 1;
			sl->setValue(next);
			sl->setValue(old);
			sliders_driven++;
		}
		blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: slider callbacks driven=%d", sliders_driven);

		QPushButton *reset = nullptr;
		for (QPushButton *button : findChildren<QPushButton *>()) {
			if (button->text() == QString::fromUtf8("恢复默认")) {
				reset = button;
				break;
			}
		}
		if (!reset || sliders_driven == 0) {
			blog(LOG_ERROR, "[obs-beauty-ui] ui-autotest: controls missing (reset=%d sliders=%d)",
			     reset != nullptr, sliders_driven);
			return;
		}
		for (int i = 0; i < 8; i++) {
			reset->click();
			QApplication::processEvents();
			blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: reset cycle %d/8", i + 1);
		}
		blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: complete");
	}
#endif

protected:
	/* 面板原本是「读一次 -> 建控件」的快照式，也不订阅滤镜的信号，所以
	 * 在滤镜属性页里改了参数，这边滑块不会跟着动（数据是同一份，只是
	 * 显示不刷新）。
	 *
	 * 考虑过 obs_source_add_signal 订阅参数变化，但那个回调是从 OBS 的
	 * 工作线程发出来的，在里面直接碰 Qt 控件就是跨线程操作 UI —— 会崩，
	 * 或者偶发地崩。所以不用信号。
	 *
	 * 改成一个笨但绝对安全的做法：面板每次被**显示**、或**重新被激活**
	 * （用户从别的窗口切回来）时，重新读一遍当前设置、重建控件。用户
	 * 切回来看到的就是最新值，而且整个过程都在 UI 线程里。 */
	bool event(QEvent *e) override
	{
		const QEvent::Type t = e->type();
		if (t == QEvent::Show) {
			sync_now();
			/* 3.1.37：Show 里**不再**碰尺寸。3.1.36 在这里调
			 * apply_adaptive_size(false)，用户拖大窗口后一失焦再点回来
			 * 就被打回 460×529 —— 那正是「固定尺寸」最难用的地方。 */
		} else if (t == QEvent::WindowActivate) {
			sync_now();
		}
		return QDialog::event(e);
	}

	/* 3.1.37：拖动/缩放 → 800ms 后落盘（防抖在 schedule_save 里）。
	 * 不在这里直接写：拖动时这些事件每帧都来。 */
	void resizeEvent(QResizeEvent *e) override
	{
		QDialog::resizeEvent(e);
		schedule_save();
	}

	void moveEvent(QMoveEvent *e) override
	{
		QDialog::moveEvent(e);
		schedule_save();
	}

	void closeEvent(QCloseEvent *e) override
	{
		/* 关窗是最可靠的一次落盘点：定时器可能还在等那 800ms，
		 * 用户一关就把待写的几何丢了，下次打开还是老尺寸。 */
		if (save_timer_)
			save_timer_->stop();
		save_ui_state();
		QDialog::closeEvent(e);
	}

private:
	/* ==================================================================
	 * 3.1.40：绑定自愈三件套
	 *
	 * 故障复盘（2026-11-06 实机）：源被移除重建 / 滤镜被删后重加时，
	 * 面板手里的 filter_ 要么被置空，要么还指向已不在 OBS 图中的旧实例。
	 * 旧版 apply_*() 开头一句 `if (!filter_) return;` 把参数**静默丢掉**，
	 * 用户拖磨皮毫无反应、OBS 日志里连一条「档位变化」都没有，
	 * 界面上却看不到任何错误 —— 这是最难自查的一类故障。
	 *
	 * 分工：
	 *   ensure_binding() —— 所有写回路径的统一守卫（滑块回调专用，零 Qt 操作）
	 *   try_light_rebind() —— 只按名字重新取引用，绝不 rebuild 控件
	 *   validate_binding() —— 窗口激活时做完整校验，允许 rebuild 控件
	 *   warn_no_filter()  —— 限流日志 + 面板红字，把「去点刷新」说清楚
	 *
	 * 铁律：**滑块回调里绝不调用 build_form() / reload()**。
	 * 用户正按着的那个 QSlider 会被 clear_form() 的 deleteLater 删掉，
	 * 历史上为此崩过（见原 sync_now() 的 mouseButtons 判据）。
	 * ================================================================== */

	/* 按记住的 (源, 滤镜) 名字重新取一次引用。不碰任何 Qt 控件，
	 * 因此可以在滑块 valueChanged 回调里安全调用。 */
	bool try_light_rebind()
	{
		if (bound_source_.empty() || bound_filter_.empty())
			return false;
		obs_source_t *src = obs_get_source_by_name(bound_source_.c_str());
		if (!src)
			return false;
		obs_source_t *f = obs_source_get_filter_by_name(src, bound_filter_.c_str());
		obs_source_release(src);
		if (!f)
			return false;
		/* 名字还在也可能拿到的是被标记 removed 的僵尸实例，必须再验一次 */
		if (obs_source_removed(f)) {
			obs_source_release(f);
			return false;
		}
		if (f == filter_) {
			obs_source_release(f);
			return false;   /* 同一个实例已被判失效，不算重连成功 */
		}
		if (filter_)
			obs_source_release(filter_);
		filter_ = f;
		blog(LOG_WARNING,
		     "[obs-beauty-ui] 面板自动重连成功：源=%s 滤镜=%s（上次绑定已失效）",
		     bound_source_.c_str(), bound_filter_.c_str());
		return true;
	}

	/* 拖一次滑块会连发几十次 valueChanged，2 秒只报一次，别把日志刷爆。 */
	void warn_no_filter(const char *key)
	{
		const unsigned long long now = GetTickCount64();
		if (last_no_filter_warn_ && now - last_no_filter_warn_ < 2000)
			return;
		last_no_filter_warn_ = now;
		blog(LOG_WARNING,
		     "[obs-beauty-ui] 参数 %s 未生效：面板未绑定美颜滤镜实例"
		     "（上次绑定：源=%s 滤镜=%s）。请给摄像头源添加「OBS美颜」"
		     "滤镜，然后点面板右上角「刷新」。",
		     key ? key : "?",
		     bound_source_.empty() ? "(无)" : bound_source_.c_str(),
		     bound_filter_.empty() ? "(无)" : bound_filter_.c_str());
		if (hint_) {
			hint_->setStyleSheet(QStringLiteral("color:#e5533d;font-size:12px;"));
			set_hint(QString::fromUtf8(
				"参数未生效：面板没连上美颜滤镜。请确认摄像头源上已添加"
				"「OBS美颜」滤镜，然后点右上角「刷新」。"));
		}
	}

	/* 所有 apply_* / reset_defaults 的唯一入口。
	 * 返回 true 表示 filter_ 可用；false 表示已明确告警，调用方直接 return。 */
	bool ensure_binding(const char *key)
	{
		if (filter_ && !obs_source_removed(filter_))
			return true;                 /* 正常快路径 */
		if (try_light_rebind())
			return true;                 /* 轻量自愈，控件一个都不动 */
		warn_no_filter(key);
		return false;
	}

	/* 窗口被激活时调用（此时没有鼠标按住，允许 rebuild 控件）。
	 * 旧版 filter_ 非空就只 build_form()，源/滤镜重建后面板会一直抱着
	 * 僵尸引用 —— 正是本次实机「拖了没反应」的根因。 */
	bool validate_binding()
	{
		if (!filter_ || obs_source_removed(filter_))
			return false;
		if (bound_source_.empty() || bound_filter_.empty())
			return true;    /* 无记忆信息（老场景）→ 退回旧行为，不误伤 */
		obs_source_t *src = obs_get_source_by_name(bound_source_.c_str());
		if (!src)
			return false;
		obs_source_t *cur = obs_source_get_filter_by_name(src, bound_filter_.c_str());
		obs_source_release(src);
		if (!cur || obs_source_removed(cur)) {
			if (cur)
				obs_source_release(cur);
			return false;
		}
		if (cur != filter_) {
			/* 同名但换了实例（源被移除重建 / 滤镜删后重加）。
			 * 换引用即可，表单由调用方 build_form() 重建。 */
			obs_source_release(filter_);
			filter_ = cur;
			blog(LOG_WARNING,
			     "[obs-beauty-ui] 窗口激活：检测到美颜滤镜实例已重建，"
			     "面板已自动重连（源=%s 滤镜=%s）",
			     bound_source_.c_str(), bound_filter_.c_str());
		} else {
			obs_source_release(cur);   /* 同一个实例，释放刚取的多余引用 */
		}
		return true;
	}

	void sync_now()
	{
		if (syncing_)   /* 重建控件本身不该再触发一次同步 */
			return;
		/* 正在拖滑块时绝不能重建 —— build_form() 会把旧控件 deleteLater，
		 * 手上正按着的那个滑块被删掉就是崩。虽然「拖到一半窗口被激活」
		 * 很少见，但代价太低，值得挡一下。 */
		if (QApplication::mouseButtons() != Qt::NoButton)
			return;
		if (!isVisible())
			return;
		syncing_ = true;
		if (validate_binding()) {
			/* 绑定仍然有效：只刷新显示值，不重新枚举源 */
			set_hint(QString());
			build_form();
		} else {
			/* 未绑定 / 绑定失效：重新枚举并自动接上。
			 * keep_selection=true 保证多路实例时不会跳回第一个。 */
			reload(true);
		}
		syncing_ = false;
	}

	QComboBox *combo_ = nullptr;
	QLabel *hint_ = nullptr;
	QLabel *ver_ = nullptr;   /* 3.0.22：左下角版本角标（含滤镜实例版本） */
	/* 3.1.26：原 host_ + form_（单张滚动表单）已被标签页取代。
	 * 每个标签页是「QScrollArea → QWidget → QFormLayout」，页控件由
	 * QTabWidget 持有，所以 clear_form() 只要 removeTab + deleteLater 即可。 */
	QTabWidget *tabs_ = nullptr;
	QHBoxLayout *sw_row_ = nullptr;   /* 3.1.25：总开关行（build_form 填控件） */
	QHBoxLayout *top_row_ = nullptr;    /* 3.1.26：顶栏，量高度用 */
	/* 3.1.32：底栏与档位栏合并成 2×2 网格 ——
	 * (0,0)磨皮算法标签 (0,1)下拉 (0,2)恢复默认
	 * (1,0)磨皮模式标签 (1,1)下拉 (1,2)关闭
	 * 原来的 bottom_row_（版本号 + 两按钮）已删除，版本号挪到 sw_row_ 右侧。 */
	QGridLayout *mode_grid_ = nullptr;
	/* 3.0.20：按钮条已整体移除（属性里的按钮不再渲染），
	 * 原来的 QHBoxLayout *btn_row_ 成员随之删除。 */
	obs_source_t *filter_ = nullptr;
	std::vector<Inst> insts_;
	/* 3.1.40：记住上次成功绑定的 (源, 滤镜)。源被移除重建后，
	 * reload() 会把 filter_ 置空但**不清这两个字符串**，这样滑块回调里
	 * 就能做一次不重建控件的轻量重连（try_light_rebind）。 */
	std::string bound_source_;
	std::string bound_filter_;
	unsigned long long last_no_filter_warn_ = 0; /* GetTickCount64，2 秒限流 */
	bool loading_ = false;
	/* 3.1.37：几何记忆。save_timer_ 是 800ms 防抖写盘器（resize/move 都重启它）；
	 * ready_ 之后才允许写 —— 构造期 restore_ui_state() 自己会 resize/move，
	 * 那时若触发 schedule_save，等于把「刚恢复的值」又写一遍（无害但无意义），
	 * 更要紧的是构造期窗口还没定位好，写下去的是错的位置。 */
	QTimer *save_timer_ = nullptr;
	bool ready_ = false;
	bool syncing_ = false;   /* sync_now() 重入保护，见上面的 event() */
	bool building_ = false;  /* 3.1.26：build_form 期间屏蔽切页重算高度 */

	/* 递归清空一个 layout。
	 * 注意：嵌套 layout 里控件的 parent 其实是页内那个 QWidget（不是 QFormLayout），
	 * 所以只 delete layout 删不掉控件，旧控件会堆在面板上。必须逐个摘掉再删。 */
	static void purge_layout(QLayout *lay)
	{
		if (!lay)
			return;
		while (lay->count() > 0) {
				QLayoutItem *it = lay->takeAt(0);
				if (!it)
					break;
				if (QWidget *w = it->widget()) {
					w->setParent(nullptr);
					w->deleteLater();
			} else if (QLayout *l = it->layout()) {
				/* A child layout is itself the QLayoutItem returned by takeAt().
				 * Delete its children here, then let delete it below exactly once.
				 * The old code did `delete l; delete it;` — l == it for a layout
				 * item, so every form rebuild double-freed nested QFormLayouts and
				 * corrupted the process heap (WER 0xC0000374). */
				purge_layout(l);
			}
			delete it;
			}
	}

	/* 3.1.25：总开关行的控件也是每次 build_form 重建的，必须一起清，
	 * 否则每重建一次就多一个复选框叠在同一行。
	 * 3.1.26：表单换成标签页后，页控件挂在 QTabWidget 上（不是某个 layout），
	 * purge_layout 碰不到它们 —— 必须先 removeTab 再删页，
	 * 否则每次 rebuild 都会多留一整页（旧页连同里面的信号连接全在）。 */
	void clear_form()
	{
		if (tabs_) {
			while (tabs_->count() > 0) {
				QWidget *pg = tabs_->widget(0);
				tabs_->removeTab(0);
				if (pg) {
					pg->setParent(nullptr);
					pg->deleteLater();
				}
			}
		}
		/* 3.1.32：ver_ 现在挂在 sw_row_ 里，而 sw_row_ 每次都要被清空 ——
		 * 必须先把它摘出来，否则 purge_layout 会把它 deleteLater 掉，
		 * 第二次 build_form 就再也没版本号了。 */
		if (sw_row_ && ver_)
			sw_row_->removeWidget(ver_);
		purge_layout(sw_row_);
		purge_layout(mode_grid_);
	}

	/* 3.1.26：提示行只有出错时才有内容。空字符串 = 隐藏（隐藏的控件
	 * 在布局里连行距一起消失，不像空 QLabel 那样还占一行高）。 */
	void set_hint(const QString &text)
	{
		if (!hint_)
			return;
		/* 3.1.40：空文本 = 恢复正常态，把 warn_no_filter() 设的红字
		 * 清掉，回到样式表里 QLabel#beautyHint 的灰色。 */
		if (text.isEmpty())
			hint_->setStyleSheet(QString());
		hint_->setText(text);
		hint_->setVisible(!text.isEmpty());
	}

	void reload(bool keep_selection)
	{
		ui_autotest_trace("reload: collect_instances begin");
		std::string keep;
		if (keep_selection && !insts_.empty() && combo_->currentIndex() >= 0 &&
		    combo_->currentIndex() < (int)insts_.size()) {
			keep = insts_[combo_->currentIndex()].source + "\x01" +
			       insts_[combo_->currentIndex()].filter;
		}

		insts_ = collect_instances();
		ui_autotest_trace("reload: collect_instances returned");
		loading_ = true;
		combo_->clear();
		for (auto &i : insts_)
			combo_->addItem(QString::fromUtf8((i.source + "  →  " + i.filter).c_str()));
		loading_ = false;

		if (insts_.empty()) {
			if (filter_) {
				obs_source_release(filter_);
				filter_ = nullptr;
			}
			/* 3.1.40：**故意不清 bound_source_ / bound_filter_**。
			 * 源被移除重建后名字往往不变，留着这两个名字，
			 * 滑块回调的 try_light_rebind() 和窗口激活的
			 * validate_binding() 才有重连的锚点。 */
			combo_->setEnabled(false);
			set_hint(QString::fromUtf8(
				"没有找到美颜滤镜实例。请先给摄像头源添加「OBS美颜」滤镜，"
				"然后点「刷新」。"));
			clear_form();
			return;
		}

		combo_->setEnabled(true);
		int idx = 0;
		if (!keep.empty()) {
			for (size_t i = 0; i < insts_.size(); i++) {
				if (insts_[i].source + "\x01" + insts_[i].filter == keep) {
					idx = (int)i;
					break;
				}
			}
		}
		loading_ = true;
		combo_->setCurrentIndex(idx);
		loading_ = false;
		select_instance(idx);
	}

	void select_instance(int idx)
	{
		if (loading_ || idx < 0 || idx >= (int)insts_.size())
			return;
		if (filter_) {
			obs_source_release(filter_);
			filter_ = nullptr;
		}
		obs_source_t *src = obs_get_source_by_name(insts_[idx].source.c_str());
		if (!src)
			return;
		filter_ = obs_source_get_filter_by_name(src, insts_[idx].filter.c_str());
		/* obs_source_get_filter_by_name 已经返回新引用，不要再 get_ref，否则泄漏 */
		obs_source_release(src);
		/* 3.1.40：只有真正拿到实例才覆盖记忆。拿不到时保留上一次的名字，
		 * 这样源/滤镜重建后仍有重连锚点；同时复位告警限流，
		 * 保证新绑定万一再次失效能立刻再报一次。 */
		if (filter_) {
			bound_source_ = insts_[idx].source;
			bound_filter_ = insts_[idx].filter;
			last_no_filter_warn_ = 0;
		}
		/* 3.0.20：原来这句还承诺「滑块双击可回到默认值」，但代码里从来没有
		 * mouseDoubleClickEvent —— 那是一条空承诺，删掉比留着误导读者好。
		 * 3.1.40：set_hint(空) 顺带清掉上一次的红字告警。 */
		set_hint(QString());
		update_version_label();
		build_form();
	}

	/* 3.1.11：角标 = **滤镜实机版本**（proc 现场报上来的一手数据）。
	 * 面板宏 `BEAUTY_UI_VERSION` 只用于：a) 滤镜太老没有该 proc 时兜底 ——
	 * 版本同号后兜底值与滤镜本该是的一致；b) 滤镜报上来的号与面板自身
	 * 不符时，追加括注标出真实滤镜版本 —— 这是「安装漏换某一边」的
	 * 现场证据，两个号默默不一致才最难查。
	 * 3.1.13 修订：修 3.1.11 引入的显示 BUG —— 带前缀的 "v3.1.13" 拿去和
	 * 不带前缀的宏 "3.1.13" 比较，永远不等，同号也打括注（真机 10:50 截图
	 * 实锤「v3.1.13（面板 3.1.13）」）。比较一律用不带 v 的裸号。 */
	void update_version_label()
	{
		if (!ver_)
			return;
		const QString panel = QString::fromUtf8(BEAUTY_UI_VERSION);
#if BEAUTY_UI_CPU_EDITION
		const QString prefix = QString::fromUtf8("CPU v");
#else
		const QString prefix = QString::fromUtf8("v");
#endif
		QString fv; /* 滤镜 proc 报上来的裸版本号（可能为空 = 老滤镜） */
		if (filter_) {
			if (proc_handler_t *ph = obs_source_get_proc_handler(filter_)) {
				calldata_t cd{};
				if (proc_handler_call(ph, "beauty_version", &cd)) {
					const char *v = calldata_string(&cd, "version");
					if (v && v[0])
						fv = QString::fromUtf8(v);
				}
				calldata_free(&cd);
			}
		}
		QString text;
		if (fv.isEmpty())
			text = prefix + panel;                     /* 兜底宏 */
		else if (fv == panel)
			text = prefix + fv;                        /* 同号：干净显示 */
		else
			text = prefix + fv +
			       QString::fromUtf8("（面板 %1）").arg(panel);
		ver_->setText(text);
	}

	/* ---- 写回单个参数：取全量设置 -> 改一个键 -> 更新滤镜 ----
	 * 3.1.40：四个入口统一走 ensure_binding()。旧版 `if (!filter_) return;`
	 * 会在绑定失效时静默丢掉整次拖动，用户看不到任何反馈；现在要么
	 * 自动重连后照常写回，要么打明确日志 + 面板红字提示去点「刷新」。
	 * 注意 ensure_binding() 全程不碰 Qt 控件，滑块回调里调用是安全的。 */
	void apply_double(const char *key, double v)
	{
		if (!ensure_binding(key))
			return;
		obs_data_t *s = obs_source_get_settings(filter_);
		obs_data_set_double(s, key, v);
		obs_source_update(filter_, s);
		obs_data_release(s);
	}
	void apply_int(const char *key, long long v)
	{
		if (!ensure_binding(key))
			return;
		obs_data_t *s = obs_source_get_settings(filter_);
		obs_data_set_int(s, key, v);
		obs_source_update(filter_, s);
		obs_data_release(s);
	}
	void apply_bool(const char *key, bool v)
	{
		if (!ensure_binding(key))
			return;
		obs_data_t *s = obs_source_get_settings(filter_);
		obs_data_set_bool(s, key, v);
		obs_source_update(filter_, s);
		obs_data_release(s);
	}
	void apply_string(const char *key, const char *v)
	{
		if (!ensure_binding(key))
			return;
		obs_data_t *s = obs_source_get_settings(filter_);
		obs_data_set_string(s, key, v);
		obs_source_update(filter_, s);
		obs_data_release(s);
	}

	void reset_defaults()
	{
		if (!ensure_binding("reset_defaults"))
			return;
		/* 3.1.25：修「点了恢复默认没反应」。
		 *
		 * 旧写法：
		 *     obs_data_t *def = obs_get_source_defaults(BEAUTY_FILTER_ID);
		 *     obs_source_update(filter_, def);
		 * 看起来天经地义，实际是 no-op。用 _api_probe.exe 直接对 obs.dll
		 * 实测（06_离线验证/全项对齐_20261030/_api_probe.cpp）拿到证据：
		 *   - obs_get_source_defaults() 返回的对象里**只有 default 条目**，
		 *     没有任何「实际值」—— obs_data_get_json() 打出来就是 {}；
		 *   - obs_data_get_double() 会回落到 default（读得出 42），
		 *     但 obs_source_update() 内部是把 settings 复制一份，
		 *     复制走的是 obs_data_apply()，而 apply **只拷实际值** → 拷出空对象。
		 * 于是滤镜要么收到全 0、要么源设置压根没动，界面一重建就回弹原值，
		 * 用户看到的就是「按钮无效」。
		 *
		 * 正确做法三步：
		 *   1) obs_data_get_defaults() —— 把 default 条目转成**实际值**
		 *      （同一探针实测：转换后 json = {"blur":42.0,...}，可被 apply 拷贝）；
		 *   2) 以当前设置为底、用默认值覆盖 —— 而不是直接拿默认值整份替换。
		 *      这样「有默认值的键」被复位，「没有默认值的键」（比如将来新增的
		 *      调试字段）保持原值，不会被悄悄清成 0；
		 *   3) 再整体回写。
		 * 顺带保留原来的 trace 日志，并多打一行「复位前/后」的实测值，
		 * 下次再怀疑它没生效，直接看 OBS 日志就能判断。 */
		blog(LOG_INFO, "[obs-beauty-ui] reset-trace: enter (thread %lu)",
		     (unsigned long)GetCurrentThreadId());

		obs_data_t *cur = obs_source_get_settings(filter_);
		obs_data_t *def = obs_get_source_defaults(BEAUTY_FILTER_ID);
		if (!cur || !def) {
			blog(LOG_WARNING,
			     "[obs-beauty-ui] reset-trace: settings/defaults NULL (cur=%p def=%p)",
			     (void *)cur, (void *)def);
			if (cur)
				obs_data_release(cur);
			if (def)
				obs_data_release(def);
			return;
		}

		obs_data_t *vals = obs_data_get_defaults(def);   /* default → 实际值 */
		obs_data_t *next = obs_data_create();
		obs_data_apply(next, cur);
		obs_data_apply(next, vals);

		const double before = obs_data_get_double(cur, "blur_level");
		const double after = obs_data_get_double(next, "blur_level");
		blog(LOG_INFO, "[obs-beauty-ui] reset-trace: blur_level %.1f -> %.1f",
		     before, after);

		obs_source_update(filter_, next);
		blog(LOG_INFO, "[obs-beauty-ui] reset-trace: obs_source_update returned");

		obs_data_release(vals);
		obs_data_release(next);
		obs_data_release(def);
		obs_data_release(cur);

		build_form();
		blog(LOG_INFO, "[obs-beauty-ui] reset-trace: done");
	}

	/* 3.0.18：QFormLayout::addRow(QString, QWidget*) 内部建的 QLabel 不支持换行，
	 * 长标题（「精准美肤（只磨皮肤，更弱更自然）」这类）会把表单的最小宽度
	 * 顶到 700px 以上 —— 这正是底部横向滚动条和「刷新」按钮被裁掉的原因。
	 * 统一改成自己建 QLabel：允许换行 + 限宽，长标题折成两行而不是横向撑开。
	 *
	 * 3.1.22：标签全部简化后，原来挂在括号里的说明（「最强·推荐」
	 * 「只美化皮肤」等）就只剩 tooltip 一条去处了。所以 add_labeled_row
	 * 现在接收属性说明，既挂到标签上、也挂到控件上，鼠标停住就能看。 */
	static QLabel *make_label(const QString &text, bool strong = false)
	{
		auto *l = new QLabel(text);
		l->setWordWrap(true);
		/* 3.1.26：标签列**右对齐**、宽 76。
		 * 控件左端成一条直线 —— 这才是「整齐」的真正来源。
		 * 上一版是左对齐固定 80 宽：短标签（磨皮）左端对齐、右端悬空，
		 * 长标签（去黑眼圈）右端顶满，两条边都参差不齐，视觉上显得散。
		 * 76 在 11px 字号下刚好放得下最长的 4 字标签（「去黑眼圈」≈44px）。 */
		l->setFixedWidth(76);
		l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
		if (strong) {
			QFont f = l->font();
			f.setBold(true);
			l->setFont(f);
		}
		return l;
	}
	/* 3.1.26：新建一个标签页，返回页内的 QFormLayout。
	 * 页结构：QScrollArea → QWidget → QFormLayout（扁平，无分组框/无卡片）。
	 * 行距 2、页内边距 (2,8,2,8) —— 参考图里控件之间几乎贴着，
	 * 分隔完全靠行距，不靠留白和边框。 */
	QFormLayout *make_tab(const QString &title)
	{
		auto *scroll = new QScrollArea();
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;}"));

		auto *inner = new QWidget();
		auto *fl = new QFormLayout(inner);
		/* 3.1.36：页内上下边距 16/18 → 8/10。5 行页在 460×520 客户区里
		 * 只剩 250 px 可用，光边距就吃掉 34；收到 18 后 5 行刚好排满。 */
		fl->setContentsMargins(2, 8, 2, 10);
		fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		/* 右对齐标签 + 左侧控件列：两条竖线都齐（见 make_label 注释） */
		fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
		/* 3.1.37：**改成顶对齐**。3.1.35 设 AlignVCenter 是为了让短页（锐化 2 行）
		 * 在固定高度里上下均分空白；现在窗口能随便拉高，居中就成了灾难 ——
		 * 用户拖高窗口想多看内容，结果整块参数被推到屏幕正中间，上方悬着一大片
		 * 空白，视觉上正是 3.1.32「四条空白带」的翻版。贴顶之后多出来的空间
		 * 全部落在底部，读起来才像正常设置页。 */
		fl->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
		/* 3.1.26：行距 5 是照参考图实测的 —— 我们的控件高 22，参考 ≈23，
		 * 参考行心距 31 逻辑 px（截图 54 物理 / 1.75），所以间距 = 31−22 ≈ 8。
		 * 先把间距写成 5 试过一版，实测行心距只有 27，比参考紧 15%，
		 * 五行连排会显得「糊成一片」。
		 * 3.1.36：控件高回到 32、行距 16 → 行心距 48，与用户 175% 截图
		 * 实测的 50 基本一致（差的 2 px 从页边距里省出来）。 */
		fl->setVerticalSpacing(16);
		fl->setHorizontalSpacing(8);
		scroll->setWidget(inner);
		tabs_->addTab(scroll, title);
		return fl;
	}

	void add_labeled_row(QFormLayout *form, const QString &label, QWidget *w,
			     const char *tip = nullptr, bool strong = false)
	{
		QLabel *l = make_label(label, strong);
		if (tip && *tip) {
			const QString t = QString::fromUtf8(tip);
			l->setToolTip(t);
			w->setToolTip(t);
		}
		form->addRow(l, w);
	}

	/* ---- 下拉框工厂 ----
	 * 3.1.29：从 add_row 的 LIST 分支里抽出来，因为「磨皮算法 / 磨皮模式」
	 * 现在渲染在底部档位栏（mode_row_）而不是表单里，两处都要同一套逻辑：
	 * 填充选项、按当前值定位、值变化时回写滤镜。抽成函数避免两份代码走偏。 */
	QComboBox *make_combo(obs_property_t *p, obs_data_t *cur)
	{
		const char *key = obs_property_name(p);
		if (!key)
			return nullptr;
		const std::string key_s = key;

		auto *cb = new QComboBox();
		/* 3.1.32：**不再让选项文字被截断**。
		 * 3.0.18 起用的是 setMinimumContentsLength(8) + setMinimumWidth(76)，
		 * 本质是按「8 个字宽」猜 —— 真机字体比探针大得多，「双边滤波」被截成
		 * 「双边滤i」（用户截图实锤）。现在改成 AdjustToContents：下拉的
		 * sizeHint 直接等于**最长选项的实测宽度**，布局只会给它更多、不会更少，
		 * 所以选项文字永远完整，而且换字体 / 换 DPI 都自动适配。 */
		cb->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		/* 3.1.38：高度不再写死 —— 由样式表 kCtrlH 统一定义，
		 * 这样下拉/数值框/按钮永远同高，不会再各算各的。 */
		cb->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

		enum obs_combo_format fmt = obs_property_list_format(p);
		size_t n = obs_property_list_item_count(p);
		for (size_t i = 0; i < n; i++) {
			const char *nm = obs_property_list_item_name(p, i);
			cb->addItem(QString::fromUtf8(nm ? nm : ""));
			if (fmt == OBS_COMBO_FORMAT_INT)
				cb->setItemData((int)i, (qulonglong)obs_property_list_item_int(p, i));
		}
		/* 定位当前值 */
		int curidx = -1;
		for (size_t i = 0; i < n; i++) {
			if (fmt == OBS_COMBO_FORMAT_INT) {
				if (obs_data_get_int(cur, key_s.c_str()) == obs_property_list_item_int(p, i)) {
					curidx = (int)i;
					break;
				}
			} else if (fmt == OBS_COMBO_FORMAT_STRING) {
				const char *sv = obs_data_get_string(cur, key_s.c_str());
				const char *iv = obs_property_list_item_string(p, i);
				if (sv && iv && strcmp(sv, iv) == 0) {
					curidx = (int)i;
					break;
				}
			}
		}
		if (curidx >= 0)
			cb->setCurrentIndex(curidx);

		connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
			[this, cb, fmt, key_s](int i) {
				if (i < 0)
					return;
				if (fmt == OBS_COMBO_FORMAT_INT)
					apply_int(key_s.c_str(),
						  (long long)cb->itemData(i).toULongLong());
				else
					apply_string(key_s.c_str(),
						     cb->itemText(i).toUtf8().constData());
			});
		return cb;
	}

	/* ---- 一行控件 ---- */
	void add_row(QFormLayout *form, obs_property_t *p, obs_data_t *cur,
		     bool strong = false)
	{
		/* obs_property_name() 返回的 key 属于 obs_properties_t；build_form() 末尾
		 * 会销毁 props，所以不能把原始 const char* 捕获到控件回调里。
		 * 之前 bool/int/float 分支都捕获了借用指针，销毁属性表后用户再拖滑块
		 * 就会读已释放内存，最终触发 Windows heap corruption (0xC0000374)。
		 * 一开始就复制成 std::string，所有回调都按值捕获自己的 key。 */
		const char *key = obs_property_name(p);
		const std::string key_s = key ? key : "";
		QString label = QString::fromUtf8(obs_property_description(p));
		enum obs_property_type t = obs_property_get_type(p);
		/* 3.1.22：标签简化后，说明文字只剩 tooltip 一条去处 */
		const char *tip = obs_property_long_description(p);

		/* 3.0.20：属性里的按钮（恢复默认 / 保存参数到 preset.json /
		 * 从 preset.json 读取参数）在面板上一律不渲染 —— 用户明确用不上，
		 * 而且三个长按钮横排会把窗口最小宽度顶到 700 px 以上。
		 * OBS 原生「滤镜属性」窗口里它们仍然存在、仍然可用。 */
		if (t == OBS_PROPERTY_BUTTON)
			return;

		switch (t) {
		case OBS_PROPERTY_BOOL: {
			auto *cb = new QCheckBox();
			/* 3.1.24：去掉焦点框。
			 * 实测（用户截图）：点一下「启用美颜」，复选框上会多出一圈
			 * **虚线焦点框**，而且和左侧的指示器错位叠在一起（右边多出一
			 * 截虚线），看着像双框乱码。
			 * 根因：OBS 主题把 focus 指示画在**整个 QCheckBox 控件矩形**
			 * 上，而这个复选框是「无文字」的（文字在左边的独立 QLabel 里），
			 * 指示器被画在控件矩形左侧，两者宽度对不上就错位了。
			 * 面板里的复选框只有鼠标点选这一个用法，焦点框没有信息量，
			 * 直接不让它拿焦点最干净（键盘 Tab 导航会少一个停靠点）。 */
			cb->setFocusPolicy(Qt::NoFocus);
			/* 3.1.26：行高统一 20 —— 参考图里复选框行和滑杆行一样高 */
			/* 3.1.38：高度交给样式表（kCtrlH） */
			cb->setChecked(obs_data_get_bool(cur, key_s.c_str()));
			connect(cb, &QCheckBox::toggled,
				[this, key_s](bool v) { apply_bool(key_s.c_str(), v); });
			add_labeled_row(form, label, cb, tip);
			break;
		}
		case OBS_PROPERTY_INT: {
			auto *sb = new QSpinBox();
			sb->setRange((int)obs_property_int_min(p), (int)obs_property_int_max(p));
			sb->setSingleStep((int)obs_property_int_step(p));
			sb->setValue((int)obs_data_get_int(cur, key_s.c_str()));
			/* 3.1.26：与浮点行的数值框同款尺寸/对齐，跨页看起来是同一套控件
			 * 3.1.38：只钉宽 74，高度交给样式表（kCtrlH） */
			sb->setFixedWidth(74);
			sb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			sb->setButtonSymbols(QAbstractSpinBox::NoButtons);
			connect(sb, QOverload<int>::of(&QSpinBox::valueChanged),
				[this, key_s](int v) { apply_int(key_s.c_str(), v); });
			add_labeled_row(form, label, sb, tip);
			break;
		}
		case OBS_PROPERTY_FLOAT: {
			double mn = obs_property_float_min(p);
			double mx = obs_property_float_max(p);
			double st = obs_property_float_step(p);
			if (st <= 0.0)
				st = (mx - mn) / 100.0;
			int steps = (int)std::lround((mx - mn) / st);
			if (steps <= 0)
				steps = 1;
			double v = obs_data_get_double(cur, key_s.c_str());

			auto *wrap = new QWidget();
			auto *h = new QHBoxLayout(wrap);
			h->setContentsMargins(0, 0, 0, 0);
			/* 3.1.26：滑杆与数值框间距 10 → 6；滑杆高度 20 → 14。
			 * 参考图的滑杆更细、更短，数值框更宽 —— 两者加起来占的行高
			 * 才是 20 左右，行距压到 2 也不会显得挤。 */
			h->setSpacing(6);
			auto *sl = new QSlider(Qt::Horizontal);
			sl->setRange(0, steps);
			sl->setValue((int)std::lround((v - mn) / st));
			sl->setMinimumWidth(110);
			sl->setFixedHeight(18);
			auto *sb = new QDoubleSpinBox();
			sb->setRange(mn, mx);
			sb->setSingleStep(st);
			sb->setDecimals(st < 1.0 ? 2 : 0);
			sb->setValue(v);
			/* 3.1.26：数值框 60x26 → 52x20。宽度收窄是因为字号降到 11px，
			 * 三位数（100）在 52 宽里仍有余量；同时让四页的右缘落在同一条线上。 */
			sb->setFixedWidth(74);   /* 3.1.38：只钉宽度，高度同 kCtrlH */
			sb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			sb->setButtonSymbols(QAbstractSpinBox::NoButtons);
			h->addWidget(sl, 1);
			h->addWidget(sb);

			connect(sl, &QSlider::valueChanged, [this, sb, mn, st, key_s](int sv) {
				double nv = mn + sv * st;
				if (std::fabs(sb->value() - nv) > 1e-9) {
					QSignalBlocker b(sb);
					sb->setValue(nv);
				}
				apply_double(key_s.c_str(), nv);
			});
			connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				[this, sl, mn, st, key_s](double dv) {
					int sv = (int)std::lround((dv - mn) / st);
					if (sl->value() != sv) {
						QSignalBlocker b(sl);
						sl->setValue(sv);
					}
					apply_double(key_s.c_str(), dv);
				});
			add_labeled_row(form, label, wrap, tip);
			break;
		}
		case OBS_PROPERTY_LIST: {
			/* 3.1.29：控件构造抽到 make_combo()，底部档位栏复用同一套 */
			if (QComboBox *cb = make_combo(p, cur))
				add_labeled_row(form, label, cb, tip);
			break;
		}
		case OBS_PROPERTY_TEXT: {
			auto *le = new QLineEdit(QString::fromUtf8(obs_data_get_string(cur, key_s.c_str())));
			/* 3.1.38：高度交给样式表（kCtrlH） */
			connect(le, &QLineEdit::editingFinished, [this, le, key_s]() {
				apply_string(key_s.c_str(), le->text().toUtf8().constData());
			});
			add_labeled_row(form, label, le, tip);
			break;
		}
		default: {
			auto *l = new QLabel(QString::fromUtf8("（此控件类型暂不支持，请在滤镜属性页设置）"));
			l->setStyleSheet("color:#888;");
			add_labeled_row(form, label, l, tip);
			break;
		}
		}
	}

	/* 3.1.26：make_collapsible()（3.0.18 引入的「高级」折叠区）已删除 ——
	 * 「高级」现在是最后一个标签页，不需要折叠按钮，QToolButton 也就用不上了。 */

	void build_form()
	{
		ui_autotest_trace("build_form: begin");
		clear_form();
		if (!filter_)
			return;

		/* 3.1.26：build 期间屏蔽「切页重算高度」。addTab() 会触发
		 * QTabWidget::currentChanged，那一刻页还没填完，量出来的高度是错的；
		 * RAII 保证任何一条 return 都能把标志复位。 */
		struct BuildGuard {
			bool &flag;
			explicit BuildGuard(bool &f) : flag(f) { flag = true; }
			~BuildGuard() { flag = false; }
		} build_guard(building_);

		obs_properties_t *props = obs_source_properties(filter_);
		ui_autotest_trace(props ? "build_form: obs_source_properties returned" : "build_form: properties NULL");
		/* 滤镜插件未加载（obs_module_load 失败/被停用）时 obs_source_properties 可能
		 * 返回 NULL；直接 obs_properties_first(NULL) 会崩。同样地 settings 也可能是 NULL。
		 * 两者都按「没有可显示的表单」处理，面板显示为空即可。 */
		if (!props) {
			ui_autotest_trace("build_form: abort (properties NULL)");
			return;
		}
		obs_data_t *cur = obs_source_get_settings(filter_);
		ui_autotest_trace(cur ? "build_form: obs_source_get_settings returned" : "build_form: settings NULL");
		if (!cur) {
			ui_autotest_trace("build_form: abort (settings NULL)");
			obs_properties_destroy(props);
			return;
		}

		/* 3.0.18 重排原因：原来所有顶层属性（启用/旋转/识别模式/人物个数/镜像）
		 * 和三个按钮都各占一整行，加上美颜组的 14 行一共 22 行、约 1150 逻辑 px，
		 * 1080p@150% 一屏只露出 1/3。现在改成：
		 *   第一遍 —— 分组（美颜）展开放最上面，这才是天天要调的
		 *   第二遍 —— 顶层散装属性收进默认收起的「高级」，按钮横排成一行
		 * 结果默认一屏 14 行 ≈ 540 px，不用滚动。 */

		/* ---- 第零遍：总开关 ----
		 * 「启用美颜」是滤镜的一级属性（不在任何分组里），必须置顶。
		 * 3.1.25：不再借用 add_row 的「标签在左、控件在右」布局 ——
		 * 那样它和下面的参数行长得一模一样，看不出这是整页唯一的一级开关。
		 * 现在填进构造函数里预留的 sw_row_：复选框紧贴文字、文字加粗放大。 */
		obs_property_t *sw = obs_properties_get(props, "enabled");
		if (sw && obs_property_visible(sw)) {
			auto *cb = new QCheckBox();
			cb->setFocusPolicy(Qt::NoFocus);
			cb->setChecked(obs_data_get_bool(cur, "enabled"));
			connect(cb, &QCheckBox::toggled,
				[this](bool v) { apply_bool("enabled", v); });
			const char *sd = obs_property_description(sw);
			auto *t = new QLabel(QString::fromUtf8(sd ? sd : ""));
			t->setObjectName("beautyTitle");
			const char *swtip = obs_property_long_description(sw);
			if (swtip && *swtip) {
				const QString qs = QString::fromUtf8(swtip);
				cb->setToolTip(qs);
				t->setToolTip(qs);
			}
			sw_row_->addWidget(cb);
			sw_row_->addWidget(t);
			sw_row_->addStretch(1);
		}
		/* 3.1.32：版本号挂在「启用美颜」行右侧（按草图）。无条件重新挂 ——
		 * 上面那个 if 找不到滤镜时不执行，ver_ 也得回到行里，不能飘着。 */
		if (ver_) {
			sw_row_->addWidget(ver_);
			ver_->setVisible(true);
		}

		/* ---- 第零点五遍：底部 2×2 区块 ----
		 * 左列：磨皮算法 / 磨皮模式（下拉），各占一行；
		 * 右列：恢复默认 / 关闭（竖排两个），与左列两行等高对齐。
		 *
		 * 3.1.36 关键修复：两个按钮**必须在这里造**。上一版是在构造函数里
		 * 加进 mode_grid_，而 build_form 的第一步 clear_form() 会
		 * purge_layout(mode_grid_)，把 grid 里所有控件（含这两个按钮）
		 * setParent(nullptr) + deleteLater() —— 构造函数跑完紧接着 reload()，
		 * 于是按钮在第一次 build_form 就没了，真机右下角永远是空的。
		 * 现在按钮与档位下拉同生命周期：每次 build_form 重新造、重新连信号。
		 * 标签仍用 make_label 的 76 宽，与页内表单的标签列对齐。 */
		if (mode_grid_) {
			static const struct {
				const char *key;
				const char *lbl;
			} kMode[] = {{"blur_type", "磨皮算法"}, {"smooth_mode", "磨皮模式"}};
			for (int i = 0; i < 2; i++) {
				obs_property_t *mp = obs_properties_get(props, kMode[i].key);
				if (!mp || !obs_property_visible(mp))
					continue;
				QLabel *lb = make_label(QString::fromUtf8(kMode[i].lbl));
				QComboBox *cw = make_combo(mp, cur);
				if (!cw)
					continue;
				cw->setMaximumWidth(150);
				mode_grid_->addWidget(lb, i, 0);
				mode_grid_->addWidget(cw, i, 1);
			}

			/* 3.1.39：**只剩一个按钮**，且与右上角「刷新」同尺寸（74×28）。
			 *
			 * 用户两条要求：
			 *   a) 「恢复默认」和右上角的「刷新」UI 大小应该一样
			 *      → 宽度 108→74（刷新也是 74），高度同 kCtrlH=28。
			 *        刷新按钮在构造函数里，恢复默认在 build_form 里，
			 *        两处**都只写 setFixedWidth(74)**，高度一律交给样式表 ——
			 *        只要有一处再写死高度，两个按钮就又会不一样大。
			 *   b) 「关闭」按钮直接去除 —— 和标题栏那个 × 功能重叠。
			 *      于是「恢复默认」移到原来「关闭」的第二行位置（第 1 行第 3 列），
			 *      第一行右侧留空（列 2 本来就在拉伸）。
			 *      关闭面板仍有两条路：标题栏 ×、Esc（QDialog 默认 reject）。
			 */
			auto *reset = new QPushButton(QString::fromUtf8("恢复默认"));
			reset->setFixedWidth(74);   /* 与构造函数里 refresh 的 74 一致 */
			reset->setCursor(Qt::PointingHandCursor);
			mode_grid_->addWidget(reset, 1, 3);   /* 原「关闭」的位置 */
			connect(reset, &QPushButton::clicked, this, [this]() {
				/* 3.1.37：按用户要求「把重置窗口大小也合并在一个开关」——
				 * 点一下既把滤镜参数复位，也把窗口打回 460×529 居中、
				 * 并清掉 ini 里的几何记录（下次打开也是默认尺寸）。
				 * 顺序：先复位窗口再复位参数。reset_defaults() 内部会
				 * build_form()，若反过来，重建表单时窗口正在改尺寸，
				 * 布局会按中间态算一次，视觉上闪一下。 */
				reset_window_size();
				reset_defaults();
			});
			/* 3.1.39：不再 setRowMinimumHeight(44) —— 那是给 44 高按钮用的；
			 * 现在两个控件都是 28 高，行高交给布局自然算。强行写 44 会在
			 * 两行之间留下 8px 的空隙，看着像「排错了」。 */
		}

		/* ---- 第一遍：分组 → 一个分组一个标签页 ----
		 * 扁平参数表」，页内既没有分组框也没有卡片留白，四组之间靠
		 * 标签切换而不是靠视觉分隔。这样一页的高度 = 本组行数 × 22，
		 * 磨皮页 4 行 ≈ 100 px，窗口自然就矮下来了。 */
		obs_property_t *p = obs_properties_first(props);
		while (p) {
			if (!obs_property_visible(p) ||
			    obs_property_get_type(p) != OBS_PROPERTY_GROUP) {
				obs_property_next(&p);
				continue;
			}
			obs_properties_t *inner = obs_property_group_content(p);
			const char *gt = obs_property_description(p);

			/* 先数一遍可见子项：空组不建页（否则会出现一个点进去啥都没有、
			 * 还占着标签栏的页）。 */
			int rows = 0;
			for (obs_property_t *q = obs_properties_first(inner); q;
			     obs_property_next(&q)) {
				if (obs_property_visible(q) &&
				    obs_property_get_type(q) != OBS_PROPERTY_BUTTON)
					rows++;
			}
			if (rows == 0) {
				obs_property_next(&p);
				continue;
			}

			QFormLayout *fl = make_tab(QString::fromUtf8(gt ? gt : ""));
			for (obs_property_t *q = obs_properties_first(inner); q;
			     obs_property_next(&q)) {
				if (obs_property_visible(q))
					add_row(fl, q, cur);
			}
			obs_property_next(&p);
		}

		/* ---- 第二遍：顶层散装属性 → 最后一页「高级」 ----
		 * 3.1.26：原来是折叠面板（默认收起），现在同样是标签页 ——
		 * 折叠面板的三角按钮本身就要占一整行高度，而且展开后会把这个
		 * 一屏放得下的窗口撑出滚动条；做成末页更干净，也和参考一致。 */
		QFormLayout *adv_fl = nullptr;

		/* 3.0.20：滤镜属性里那三个按钮（恢复默认 / 保存参数到 preset.json /
		 * 从 preset.json 读取参数）不再出现在面板上 —— 用户明确用不上，而且
		 * 三个长按钮横排会把窗口最小宽度顶到 700 px 以上，正是"UI 太宽"的成因之一。
		 * 这三个按钮的功能在 OBS 原生「滤镜属性」窗口里仍然照常可用，
		 * 面板自身的「恢复默认」也仍然在底部按钮行。 */
		p = obs_properties_first(props);
		while (p) {
			const bool vis = obs_property_visible(p);
			const enum obs_property_type t = obs_property_get_type(p);
			/* enabled 已在第零遍置顶渲染，这里跳过，否则会渲染两次；
			 * 3.1.29：blur_type / smooth_mode 已在档位栏渲染，同样跳过 */
			const char *nm = obs_property_name(p);
			const bool in_mode_row =
				nm && (strcmp(nm, "blur_type") == 0 ||
				       strcmp(nm, "smooth_mode") == 0);
			if (!vis || t == OBS_PROPERTY_GROUP || t == OBS_PROPERTY_BUTTON ||
			    (nm && strcmp(nm, "enabled") == 0) || in_mode_row) {
				obs_property_next(&p);
				continue;
			}
			if (!adv_fl)
				adv_fl = make_tab(QString::fromUtf8("高级"));
			add_row(adv_fl, p, cur);
			obs_property_next(&p);
		}

		/* 末页建了却没加进任何行（理论上不会，防御性处理）：把空页摘掉 */
		if (adv_fl && adv_fl->rowCount() == 0 && tabs_ && tabs_->count() > 0) {
			QWidget *last = tabs_->widget(tabs_->count() - 1);
			tabs_->removeTab(tabs_->count() - 1);
			if (last) {
				last->setParent(nullptr);
				last->deleteLater();
			}
		}

		obs_data_release(cur);
		obs_properties_destroy(props);
		ui_autotest_trace("build_form: done");
		/* 3.1.37：**这里什么都不做**。3.1.36 在末尾调 apply_adaptive_size(false)，
		 * 于是每拖一次滑块（触发重建）窗口都被打回固定尺寸 —— 用户永远
		 * 拖不动窗口。重建表单只是换控件，与窗口几何无关。 */
	}
};

/* ==================================================================
 * 顶层菜单
 * ================================================================== */
static QTimer *g_timer = nullptr;
static bool g_menu_added = false;
static int g_menu_attempts = 0;   /* ensure_menu() 轮询次数，用于兜底（见下） */
#ifdef BEAUTY_UI_AUTOTEST_BUILD
static bool g_autotest_started = false;
#endif
static class BeautyPanel *g_panel = nullptr;   /* 只允许存在一个面板 */

static void open_panel(QMainWindow *mw)
{
	if (g_panel) {          /* 已经开着一个：不再重复新建，直接提到前台 */
		g_panel->raise();
		g_panel->activateWindow();
		return;
	}
	auto *dlg = new BeautyPanel(mw);
	g_panel = dlg;
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	/* 关闭后指针要失效，否则下次点了会误判成"已经开着"，再也打不开 */
	QObject::connect(dlg, &QObject::destroyed, []() { g_panel = nullptr; });
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

#ifdef BEAUTY_UI_AUTOTEST_BUILD
static bool beauty_ui_autotest_enabled()
{
	char *value = nullptr;
	size_t len = 0;
	if (_dupenv_s(&value, &len, "BEAUTY_UI_AUTOTEST") != 0 || !value)
		return false;
	const bool enabled = strcmp(value, "1") == 0;
	free(value);
	return enabled;
}
#endif

static void ensure_menu(void)
{
	if (g_menu_added)
		return;
	/* 主窗口正常情况下 1~2 秒内就绪。若一直拿不到（例如不是桌面版 OBS、
	 * 或前端 API 行为变化），就让轮询停下来并留一行日志，而不是永远
	 * 每 250ms 唤醒一次。60s 的上限足够宽松，不会误伤慢启动。 */
	if (++g_menu_attempts > 240) {
		if (g_timer)
			g_timer->stop();
		blog(LOG_WARNING,
		     "[obs-beauty-ui] 主窗口 60 秒内未就绪，停止轮询（顶层菜单未创建）");
		return;
	}
	auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mw)
		return;
	QMenuBar *mb = mw->menuBar();
	if (!mb)
		return;

	QMenu *menu = mb->addMenu(QString::fromUtf8(BEAUTY_MENU_TITLE));
	ui_autotest_trace("ensure_menu: top-level menu added");

	QAction *panel = menu->addAction(QString::fromUtf8("参数面板..."));
	QObject::connect(panel, &QAction::triggered, [mw]() { open_panel(mw); });

	menu->addSeparator();
	QAction *about = menu->addAction(QString::fromUtf8("关于"));
	QObject::connect(about, &QAction::triggered, [mw]() {
		QMessageBox::information(
			mw, QString::fromUtf8("关于"),
			QString::fromUtf8("OBS美颜 参数面板 v" BEAUTY_UI_VERSION "\n\n"
#if BEAUTY_UI_CPU_EDITION
					  "CPU 版：纯 CPU 实现，不依赖 GPU / D3D。\n"
					  "版本轴 " BEAUTY_UI_VERSION "（四段式）与 GPU 版 3.0.1 独立，不可互比。\n\n"
#endif
					  "把「OBS美颜」滤镜的参数面板挂到 OBS 顶层菜单。\n"
					  "面板内容由滤镜自身的属性定义自动生成，\n"
					  "与滤镜属性页共用同一份设置，改哪边都即时生效。\n\n"
					  "3.0.20 起面板不再显示「恢复默认 / 保存参数到 preset.json /\n"
					  "从 preset.json 读取参数」三个按钮，需要时请在 OBS 的\n"
					  "「滤镜属性」窗口里使用。"));
	});

	g_menu_added = true;
	if (g_timer)
		g_timer->stop();
	blog(LOG_INFO, "[obs-beauty-ui] 顶层菜单已添加");

#ifdef BEAUTY_UI_AUTOTEST_BUILD
	/* Disposable test-build branch; never included in the production DLL. */
	if (!g_autotest_started) {
		const bool test_enabled = beauty_ui_autotest_enabled();
		blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: env enabled=%d", test_enabled);
		if (test_enabled) {
			g_autotest_started = true;
			QTimer::singleShot(5000, [mw]() {
				blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: opening panel");
				open_panel(mw);
				blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: panel constructed");
				QTimer::singleShot(1200, []() {
					if (g_panel) {
						blog(LOG_INFO, "[obs-beauty-ui] ui-autotest: running panel test");
						g_panel->run_stability_test();
					} else {
						blog(LOG_ERROR, "[obs-beauty-ui] ui-autotest: panel missing");
					}
				});
			});
		}
	}
#endif
}

bool obs_module_load(void)
{
	/* 模块加载时主窗口可能还没建好，用定时器轮询到菜单栏可用为止 */
	g_timer = new QTimer();
	g_timer->setInterval(250);
	QObject::connect(g_timer, &QTimer::timeout, []() { ensure_menu(); });
	g_timer->start();
#if BEAUTY_UI_CPU_EDITION
	blog(LOG_INFO, "[obs-beauty-ui][CPU] 模块已加载 v%s，等待主窗口就绪", BEAUTY_UI_VERSION);
#else
	blog(LOG_INFO, "[obs-beauty-ui] 模块已加载 v%s，等待主窗口就绪", BEAUTY_UI_VERSION);
#endif
	return true;
}

void obs_module_unload(void)
{
	if (g_timer) {
		g_timer->stop();
		delete g_timer;
		g_timer = nullptr;
	}
}
