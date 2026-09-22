/**
 * @file logger.c
 * @brief 简单日志（输出到 stderr，带级别与时间）
 */
#include "sdk_internal.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

void sdk_log(const char *level, const char *fmt, ...)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    fprintf(stderr, "[%s][%s] ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
