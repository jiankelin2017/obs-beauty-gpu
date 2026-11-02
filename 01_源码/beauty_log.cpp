// beauty_log.cpp
#include "beauty_log.h"

#include <stdio.h>
#include <stdarg.h>

static beauty_log_fn g_log = nullptr;

void beauty_set_log_fn(beauty_log_fn fn)
{
    g_log = fn;
}

void beauty_logv(int level, const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;

    if (g_log) {
        g_log(level, buf);
        return;
    }
    // 无钩子（自测 / 独立进程）：退回 stderr，与 OFX 版行为一致。
    fprintf(stderr, "[beauty] %s\n", buf);
    fflush(stderr);
}
