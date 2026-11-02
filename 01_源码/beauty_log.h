#pragma once
// beauty_log.h —— 引擎层 → 宿主日志的唯一出口。
//
// 引擎层（legacy_engine.cpp / legacy_region.cpp）刻意不 include 任何宿主头文件，
// 这样它既能在 OBS 插件里用，也能被自测程序直接 LoadLibrary 驱动。代价是它
// 自己不能写宿主的日志系统，所以这里挂一个函数指针：
//
//   OBS 插件在 obs_module_load 里调 beauty_set_log_fn() 指到 blog()；
//   自测程序不设，消息落到 stderr（与原 OFX 版行为一致）。
//
// 日志级别沿用 OBS 的约定（0..4 见 obs-logging.h），这样插件层可以直接透传。

enum {
    BEAUTY_LOG_ERROR = 1,
    BEAUTY_LOG_WARNING = 2,
    BEAUTY_LOG_INFO = 3,
    BEAUTY_LOG_DEBUG = 4,
};

typedef void (*beauty_log_fn)(int level, const char* msg);

// 传入 nullptr 恢复默认（stderr）。线程安全：只在模块加载时设置一次。
void beauty_set_log_fn(beauty_log_fn fn);

// 内部用：格式化后送日志钩子（无钩子时写 stderr）。
void beauty_logv(int level, const char* fmt, ...);

#define NALOG(lvl, ...) beauty_logv(lvl, __VA_ARGS__)
