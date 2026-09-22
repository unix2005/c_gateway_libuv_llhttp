/**
 * @file proxy.c
 * @brief 上游转发（薄封装层）
 *
 * 本文件只负责"组装请求并交给异步转发模块"，不再自己做同步阻塞的
 * curl_easy_perform（那会卡死 libuv 事件循环）。真正的网络 IO 由
 * async_http.c（curl_multi + uv_poll）完成，并在同一 loop 线程内回调。
 */

#include "gateway.h"
#include "async_http.h"

/* 上游响应完成回调：把结果交还客户端 */
static void proxy_done(void *req,
                       int status_code,
                       const char *body,
                       size_t body_len,
                       void *user_data)
{
    (void)req;
    client_ctx_t *client = (client_ctx_t *)user_data;

    /* 客户端可能已断开并被取消，此时 user_data 不会被传回；若传回则必有效 */
    if (!client) return;

    char *copy = NULL;
    if (body && body_len > 0) {
        copy = malloc(body_len + 1);
        memcpy(copy, body, body_len);
        copy[body_len] = '\0';
    }

    send_response(client, status_code > 0 ? status_code : 502,
                  "application/json", copy);
}

void forward_to_service(client_ctx_t *client, service_instance_t *instance)
{
    if (!instance || instance->health == SERVICE_UNHEALTHY) {
        send_response(client, 503, "text/plain", strdup("Service Unavailable"));
        return;
    }

    const char *proto = (instance->protocol == PROTOCOL_HTTPS) ? "https" : "http";
    char url[1024];

    if (instance->ip_addr.is_ipv6) {
        snprintf(url, sizeof(url), "%s://[%s]:%d%s",
                 proto, instance->host, instance->port, client->url);
    } else {
        snprintf(url, sizeof(url), "%s://%s:%d%s",
                 proto, instance->host, instance->port, client->url);
    }

    /* 组装请求头（NULL 结尾） */
    const char *headers[8];
    int hi = 0;
    headers[hi++] = "Content-Type: application/json";
    headers[hi++] = "Expect:";
    if (g_gateway_config.observability.enable_tracing && client->is_sampled) {
        char traceparent[256];
        tracing_get_outgoing_traceparent(client, traceparent, sizeof(traceparent));
        /* 合并为一个完整头，避免被拆成两条无效记录 */
        char *tp = malloc(strlen(traceparent) + 16);
        sprintf(tp, "traceparent: %s", traceparent);
        headers[hi++] = tp; /* 注意：提交后由本函数统一释放 */
    }
    headers[hi] = NULL;

    const char *method_str = "GET";
    switch (client->parser.method) {
        case HTTP_POST:   method_str = "POST";   break;
        case HTTP_PUT:    method_str = "PUT";    break;
        case HTTP_DELETE: method_str = "DELETE"; break;
        case HTTP_HEAD:   method_str = "HEAD";   break;
        default:          method_str = "GET";    break;
    }

    if (g_gateway_config.observability.enable_logging) {
        log_info(client, "request_forwarded", "method=%s path=%s -> [%s] %s:%d",
                 method_str, client->url,
                 (instance->ip_addr.is_ipv6 ? "IPv6" : "IPv4"),
                 instance->host, instance->port);
    }

    int rc = async_http_submit(client->handle.loop,
                               method_str,
                               url,
                               headers,
                               client->body_buffer,  /* 模块内部会拷贝 */
                               client->body_len,
                               15000L,
                               proxy_done,
                               client);

    /* 释放本地分配的 traceparent 头字符串 */
    for (int i = 0; headers[i]; i++) {
        /* 仅释放我们额外 malloc 的那个（Content-Type/Expect 是字面量） */
        if (strncmp(headers[i], "traceparent: ", 13) == 0)
            free((void *)headers[i]);
    }

    if (rc != 0) {
        send_response(client, 502, "text/plain", strdup("Bad Gateway"));
    }
}
