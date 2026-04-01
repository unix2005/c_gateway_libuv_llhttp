#include "gateway.h"

size_t proxy_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    proxy_request_t *ctx = (proxy_request_t *)userp;

    ctx->response_data = realloc(ctx->response_data,
                                 ctx->response_size + realsize + 1);

    memcpy(ctx->response_data + ctx->response_size, contents, realsize);
    ctx->response_size += realsize;
    ctx->response_data[ctx->response_size] = '\0';

    return realsize;
}

void proxy_complete_callback(uv_async_t *async)
{
    proxy_request_t *ctx = (proxy_request_t *)async->data;

    // ✅ 关键修复：检查 client 是否还有效
    // 如果 worker 线程正在退出，client 可能已经无效
    if (ctx->response_data && ctx->client)
    {
        printf("[Proxy] 转发响应：%d, 大小：%zu bytes\n",
               ctx->status_code, ctx->response_size);

        send_response(ctx->client, ctx->status_code,
                      "application/json", strdup(ctx->response_data));
    }
    else
    {
        // client 已失效，只清理资源，不发送响应
        if (ctx->response_data) {
            printf("[Proxy] 警告：client 已失效，丢弃响应数据\n");
            free(ctx->response_data);
        }
    }

    // 清理 CURL 资源
    if (ctx->curl)
        curl_easy_cleanup(ctx->curl);
    if (ctx->headers)
        curl_slist_free_all(ctx->headers);
    
    // 释放上下文
    free(ctx);
    
    // ✅ 关键：正确关闭 async handle
    // 先关闭 handle，让 libuv 在适当的时候释放内存
    uv_close((uv_handle_t*)async, NULL);
}

void forward_to_service(client_ctx_t *client, service_instance_t *instance)
{
    if (!instance || instance->health == SERVICE_UNHEALTHY)
    {
        send_response(client, 503, "text/plain", strdup("Service Unavailable"));
        return;
    }

    // 创建代理请求上下文
    proxy_request_t *ctx = calloc(1, sizeof(proxy_request_t));
    ctx->client = client;
    ctx->response_data = malloc(1);
    ctx->response_data[0] = '\0';
    ctx->status_code = 0;

    char url[1024];
    const char *proto = (instance->protocol == PROTOCOL_HTTPS) ? "https" : "http";

    // 支持 IPv6 地址格式
    if (instance->ip_addr.is_ipv6)
    {
        snprintf(url, sizeof(url), "%s://[%s]:%d%s",
                 proto, instance->host, instance->port, client->url);
    }
    else
    {
        snprintf(url, sizeof(url), "%s://%s:%d%s",
                 proto, instance->host, instance->port, client->url);
    }

    ctx->curl = curl_easy_init();
    if (!ctx->curl)
    {
        free(ctx->response_data);
        free(ctx);
        send_response(client, 500, "text/plain", strdup("Internal Error"));
        return;
    }

    ctx->headers = curl_slist_append(NULL, "Content-Type: application/json");
    ctx->headers = curl_slist_append(ctx->headers, "Expect:");
    
    // === 添加分布式追踪头 ===
    if (g_gateway_config.observability.enable_tracing && client->is_sampled) {
        char traceparent[256];
        tracing_get_outgoing_traceparent(client, traceparent, sizeof(traceparent));
        ctx->headers = curl_slist_append(ctx->headers, "traceparent: ");
        ctx->headers = curl_slist_append(ctx->headers, traceparent);
        
        log_debug(client, "trace_forwarded", "Forwarding trace context to upstream");
    }

    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, proxy_write_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 15L);

    // HTTPS SSL 配置
    if (instance->protocol == PROTOCOL_HTTPS)
    {
        if (!instance->verify_ssl)
        {
            curl_easy_setopt(ctx->curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(ctx->curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        else
        {
            curl_easy_setopt(ctx->curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(ctx->curl, CURLOPT_SSL_VERIFYHOST, 2L);
            if (strlen(instance->ca_cert_path) > 0)
            {
                curl_easy_setopt(ctx->curl, CURLOPT_CAINFO, instance->ca_cert_path);
            }
        }
    }

    // 设置 HTTP 方法
    if (client->parser.method == HTTP_POST && client->body_buffer)
    {
        curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, client->body_buffer);
        curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE, client->body_len);
    }
    else if (client->parser.method == HTTP_PUT)
    {
        curl_easy_setopt(ctx->curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (client->body_buffer)
        {
            curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, client->body_buffer);
            curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE, client->body_len);
        }
    }
    else if (client->parser.method == HTTP_DELETE)
    {
        curl_easy_setopt(ctx->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    const char *method_str = "GET";
    if (client->parser.method == HTTP_POST)
        method_str = "POST";
    else if (client->parser.method == HTTP_PUT)
        method_str = "PUT";
    else if (client->parser.method == HTTP_DELETE)
        method_str = "DELETE";

    // === 记录转发请求日志 ===
    if (g_gateway_config.observability.enable_logging) {
        log_info(client, "request_forwarded", "method=%s path=%s -> [%s] %s:%d",
                 method_str, client->url,
                 (instance->ip_addr.is_ipv6 ? "IPv6" : "IPv4"),
                 instance->host, instance->port);
    }
    
    printf("[Proxy] 转发请求：%s %s -> [%s] %s:%d\n",
           method_str, client->url,
           (instance->ip_addr.is_ipv6 ? "IPv6" : "IPv4"),
           instance->host, instance->port);

    // === 记录上游延迟开始 ===
    uint64_t upstream_start = get_time_nanoseconds();
    
    // 执行 HTTP 请求（同步方式，但在独立线程中运行）
    CURLcode res = curl_easy_perform(ctx->curl);
    
    // === 记录上游延迟结束 ===
    uint64_t upstream_end = get_time_nanoseconds();
    double upstream_duration = (double)(upstream_end - upstream_start) / 1000000000.0;
    metrics_upstream_duration(upstream_duration);

    if (res == CURLE_OK)
    {
        curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &ctx->status_code);
        printf("[Proxy] 收到后端响应：HTTP %d\n", ctx->status_code);
        
        if (g_gateway_config.observability.enable_logging) {
            log_info(client, "upstream_response", "status=%d duration=%.3fms",
                     ctx->status_code, upstream_duration * 1000.0);
        }
    }
    else
    {
        ctx->status_code = 503;
        fprintf(stderr, "[Proxy] 请求失败：%s\n", curl_easy_strerror(res));
        
        if (g_gateway_config.observability.enable_logging) {
            log_error(client, "upstream_error", "error=%s", curl_easy_strerror(res));
        }
    }

    // 使用 uv_async 通知主线程处理结果
    uv_async_t *async = malloc(sizeof(uv_async_t));
    async->data = ctx;

    uv_async_init(client->handle.loop, async, proxy_complete_callback);
    uv_async_send(async);
}
