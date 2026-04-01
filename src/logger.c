/**
 * 结构化日志模块
 * 支持 JSON 格式输出，高性能，线程安全
 */

#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

// 获取当前时间戳（ISO 8601 格式）
static void get_timestamp_iso(char *buffer, size_t size)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);

  time_t now = tv.tv_sec;
  struct tm *tm_info = localtime(&now);

  char time_buffer[32];
  strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%dT%H:%M:%S", tm_info);

  // 修复：使用 int 类型，微秒范围是 0-999999，除以 1000 后是 0-999
  int milliseconds = (int)(tv.tv_usec / 1000);
  if (milliseconds < 0)
    milliseconds += 1000;
  snprintf(buffer, size, "%s.%03dZ", time_buffer, milliseconds);
}

// 获取纳秒级时间戳（用于计算延迟）
uint64_t get_time_nanoseconds(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// 日志级别字符串
static const char *log_level_str(log_level_t level)
{
  switch (level)
  {
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_WARN:
    return "WARN";
  case LOG_LEVEL_ERROR:
    return "ERROR";
  default:
    return "UNKNOWN";
  }
}

// 转义 JSON 字符串中的特殊字符
static void escape_json_string(const char *input, char *output, size_t output_size)
{
  size_t j = 0;
  for (size_t i = 0; input[i] && j < output_size - 2; i++)
  {
    char c = input[i];
    if (c == '"' || c == '\\')
    {
      if (j < output_size - 3)
      {
        output[j++] = '\\';
        output[j++] = c;
      }
    }
    else if (c == '\n')
    {
      if (j < output_size - 3)
      {
        output[j++] = '\\';
        output[j++] = 'n';
      }
    }
    else if (c == '\r')
    {
      if (j < output_size - 3)
      {
        output[j++] = '\\';
        output[j++] = 'r';
      }
    }
    else if (c == '\t')
    {
      if (j < output_size - 3)
      {
        output[j++] = '\\';
        output[j++] = 't';
      }
    }
    else
    {
      output[j++] = c;
    }
  }
  output[j] = '\0';
}

// 核心日志函数 - 结构化 JSON 格式
void log_request(log_level_t level, client_ctx_t *ctx,
                 const char *event, const char *format, ...)
{
  // 检查日志开关和级别
  if (!g_gateway_config.observability.enable_logging ||
      level < g_gateway_config.observability.log_level)
  {
    return;
  }

  // 获取时间戳
  char timestamp[64];
  get_timestamp_iso(timestamp, sizeof(timestamp));

  // 构建日志消息
  char message[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  // 转义消息
  char escaped_message[sizeof(message) * 2];
  escape_json_string(message, escaped_message, sizeof(escaped_message));

  FILE *log_file = stdout;
  if (strlen(g_gateway_config.log_path) > 0)
  {
    log_file = fopen(g_gateway_config.log_path, "a");
    if (!log_file)
    {
      log_file = stdout;
    }
  }

  if (g_gateway_config.observability.enable_json_log)
  {
    // JSON 格式输出
    fprintf(log_file,
            "{"
            "\"timestamp\":\"%s\","
            "\"level\":\"%s\","
            "\"service\":\"gateway\","
            "\"event\":\"%s\"",
            timestamp,
            log_level_str(level),
            event);

    // 添加请求上下文（如果有）
    if (ctx)
    {
      char escaped_req_id[128];
      char escaped_trace_id[128];
      escape_json_string(ctx->request_id, escaped_req_id, sizeof(escaped_req_id));
      escape_json_string(ctx->trace_id, escaped_trace_id, sizeof(escaped_trace_id));

      fprintf(log_file,
              ",\"request_id\":\"%s\","
              "\"trace_id\":\"%s\","
              "\"span_id\":\"%s\"",
              escaped_req_id,
              escaped_trace_id,
              ctx->span_id);
    }

    // 添加消息
    fprintf(log_file, ",\"message\":\"%s\"}\n", escaped_message);
  }
  else
  {
    // 传统文本格式（兼容旧日志）
    if (ctx)
    {
      fprintf(log_file, "[%s] [%s] [req=%s] [trace=%s] %s: %s\n",
              timestamp,
              log_level_str(level),
              ctx->request_id,
              ctx->trace_id,
              event,
              message);
    }
    else
    {
      fprintf(log_file, "[%s] [%s] %s: %s\n",
              timestamp,
              log_level_str(level),
              event,
              message);
    }
  }

  if (log_file != stdout && log_file != stderr)
  {
    fclose(log_file);
  }
}

// 便捷日志宏
void log_debug(client_ctx_t *ctx, const char *event, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  char msg[1024];
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  log_request(LOG_LEVEL_DEBUG, ctx, event, "%s", msg);
}

void log_info(client_ctx_t *ctx, const char *event, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  char msg[1024];
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  log_request(LOG_LEVEL_INFO, ctx, event, "%s", msg);
}

void log_warn(client_ctx_t *ctx, const char *event, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  char msg[1024];
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  log_request(LOG_LEVEL_WARN, ctx, event, "%s", msg);
}

void log_error(client_ctx_t *ctx, const char *event, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  char msg[1024];
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  log_request(LOG_LEVEL_ERROR, ctx, event, "%s", msg);
}
