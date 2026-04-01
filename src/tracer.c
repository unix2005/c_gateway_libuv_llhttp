/**
 * 分布式追踪模块
 * 遵循 W3C Trace Context 标准，兼容 OpenTelemetry
 */

#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

// ===== 工具函数 =====

// 生成随机 Hex 字符串
static void generate_random_hex(char *buffer, size_t length)
{
    static const char hex_chars[] = "0123456789abcdef";

    for (size_t i = 0; i < length - 1; i++)
    {
        buffer[i] = hex_chars[rand() % 16];
    }
    buffer[length - 1] = '\0';
}

// 生成 Trace ID（32 个字符的 hex 字符串）
void generate_trace_id(char *trace_id, size_t size)
{
    if (size < 33)
        return;
    generate_random_hex(trace_id, 33);
}

// 生成 Span ID（16 个字符的 hex 字符串）
void generate_span_id(char *span_id, size_t size)
{
    if (size < 17)
        return;
    generate_random_hex(span_id, 17);
}

// 解析 W3C Trace Context 头
// 格式：traceparent: 00-<trace-id>-<span-id>-<flags>
int parse_traceparent(const char *header, char *trace_id, char *parent_span_id, int *sampled)
{
    if (!header || strlen(header) != 55)
    {
        return -1;
    }

    // 验证版本
    if (strncmp(header, "00-", 3) != 0)
    {
        return -1;
    }

    // 提取 trace-id (位置 3-34)
    strncpy(trace_id, header + 3, 32);
    trace_id[32] = '\0';

    // 提取 parent-span-id (位置 35-50)
    strncpy(parent_span_id, header + 35, 16);
    parent_span_id[16] = '\0';

    // 提取 flags (位置 52-53)
    int flags = (int)strtol(header + 52, NULL, 16);
    *sampled = (flags & 0x01);

    return 0;
}

// 构建 W3C Trace Context 头
void build_traceparent_header(char *buffer, size_t size,
                              const char *trace_id, const char *span_id, int sampled)
{
    snprintf(buffer, size, "00-%s-%s-0%c", trace_id, span_id, sampled ? '1' : '0');
}

// ===== 采样逻辑 =====

// 判断是否应该采样
static int should_sample(void)
{
    double sample_rate = g_gateway_config.observability.tracing_sample_rate;

    // 采样率 1.0 = 100% 采样
    if (sample_rate >= 1.0)
        return 1;
    if (sample_rate <= 0.0)
        return 0;

    return ((double)rand() / RAND_MAX) < sample_rate;
}

// ===== 追踪上下文初始化 =====

// 在连接建立时初始化追踪上下文
void tracing_init_context(client_ctx_t *ctx, const char *incoming_traceparent)
{
    if (!g_gateway_config.observability.enable_tracing)
    {
        return;
    }

    // 生成 Request ID（用于日志关联）
    generate_random_hex(ctx->request_id, sizeof(ctx->request_id));

    if (incoming_traceparent && parse_traceparent(incoming_traceparent,
                                                  ctx->trace_id,
                                                  ctx->span_id,
                                                  &ctx->is_sampled) == 0)
    {
        // 从上游继承追踪上下文
        log_debug(ctx, "trace_context_inherited", "Inherited trace context from upstream");
    }
    else
    {
        // 创建新的追踪
        generate_trace_id(ctx->trace_id, sizeof(ctx->trace_id));
        generate_span_id(ctx->span_id, sizeof(ctx->span_id));
        ctx->is_sampled = should_sample();

        if (ctx->is_sampled)
        {
            log_debug(ctx, "trace_context_created", "Created new trace context");
        }
    }
}

// ===== 导出追踪数据 =====

// 导出到控制台（调试用）
static void export_to_console(const char *trace_id, const char *span_id,
                              const char *operation, double duration_ms)
{
    printf("[Tracing] trace=%s span=%s op=%s duration=%.3fms\n",
           trace_id, span_id, operation, duration_ms);
}

// 导出到 Jaeger/Zipkin（简化版 Thrift/JSON over HTTP）
static void export_to_http_collector(const char *trace_id, const char *span_id,
                                     const char *operation, double duration_ms)
{
    // 这里可以实现发送到 Jaeger/Zipkin 的 HTTP API
    // 由于篇幅限制，这里只打印日志
    // 实际生产环境可以使用 libcurl 发送 JSON 到收集器

    char json_payload[2048];
    snprintf(json_payload, sizeof(json_payload),
             "{"
             "\"traceId\":\"%s\","
             "\"id\":\"%s\","
             "\"name\":\"%s\","
             "\"duration\":%.0f,"
             "\"timestamp\":%ld"
             "}",
             trace_id, span_id, operation, duration_ms * 1000, time(NULL) * 1000000);

    // TODO: 使用 curl 发送到 g_gateway_config.observability.tracing_endpoint
    log_debug(NULL, "trace_exported", "Export trace to %s: %s",
              g_gateway_config.observability.tracing_endpoint, json_payload);
}

// 导出 Span
void tracing_export_span(client_ctx_t *ctx, const char *operation, double duration_ms)
{
    if (!g_gateway_config.observability.enable_tracing || !ctx->is_sampled)
    {
        return;
    }

    const char *exporter = g_gateway_config.observability.tracing_exporter;

    if (strcmp(exporter, "console") == 0)
    {
        export_to_console(ctx->trace_id, ctx->span_id, operation, duration_ms);
    }
    else if (strcmp(exporter, "jaeger") == 0 ||
             strcmp(exporter, "zipkin") == 0 ||
             strcmp(exporter, "otlp") == 0)
    {
        export_to_http_collector(ctx->trace_id, ctx->span_id, operation, duration_ms);
    }
}

// ===== 辅助函数 =====

// 获取当前 Span 的追踪头（用于向下游传播）
void tracing_get_outgoing_traceparent(client_ctx_t *ctx, char *buffer, size_t size)
{
    if (!g_gateway_config.observability.enable_tracing)
    {
        buffer[0] = '\0';
        return;
    }

    // 为转发到下游服务创建新的 Span ID
    char child_span_id[32];
    generate_span_id(child_span_id, sizeof(child_span_id));

    build_traceparent_header(buffer, size, ctx->trace_id, child_span_id, ctx->is_sampled);
}

// 记录 Span 事件（可选）
void tracing_add_event(client_ctx_t *ctx, const char *event_name, const char *attributes)
{
    if (!g_gateway_config.observability.enable_tracing || !ctx->is_sampled)
    {
        return;
    }

    // 可以在这里添加更详细的事件信息
    log_debug(ctx, "trace_event", "event=%s attrs=%s", event_name, attributes);
}
