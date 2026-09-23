/**
 * @file logger.c
 * @brief SDK 日志封装：转发到 qboot 的 q_log（无锁异步日志）。
 *
 * 设计：sdk_log() 保持原先 "级别字符串 + printf 格式" 的签名（调用点零改动），
 * 内部把消息预格式化成缓冲区，再以 "%s" 交给 q_log_write。
 * q_log 在未初始化或单条超过槽位时自动降级为同步写，因此即使 q_log_init
 * 尚未调用也不致崩溃。
 */
#include "sdk_internal.h"
#include <q/log.h>
#include <string.h>
#include <stdarg.h>

/* 与 q_log 的 Q_LOG_LINE_MAX 保持一致 */
#define SDK_LOG_BUF  2048

static q_log_level_t map_level(const char *level)
{
    if (!level)        return Q_LOG_INFO;
    if (!strcmp(level, "TRACE")) return Q_LOG_TRACE;
    if (!strcmp(level, "DEBUG")) return Q_LOG_DEBUG;
    if (!strcmp(level, "INFO"))  return Q_LOG_INFO;
    if (!strcmp(level, "WARN"))  return Q_LOG_WARN;
    if (!strcmp(level, "ERROR")) return Q_LOG_ERROR;
    if (!strcmp(level, "FATAL")) return Q_LOG_FATAL;
    return Q_LOG_INFO;
}

void sdk_log(const char *level, const char *fmt, ...)
{
    char buf[SDK_LOG_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    q_log_write(map_level(level), "app", "%s", buf);
}
