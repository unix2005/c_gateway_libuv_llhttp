/**
 * @file http_client.h
 * @brief 异步出站 HTTP 客户端（基于 libcurl multi + libuv 事件循环）
 *
 * 设计要点：
 *  - 每个 uv_loop_t 维护独立的 CURLM* 与 uv_timer_t，出站请求绑定到“发起它的那个
 *    loop 线程”（与 cservice 多 loop 模型一致，避免跨线程操作 handle）。
 *  - 所有 I/O 由 libuv 的事件循环驱动（uv_poll + uv_timer），因此调用方所在的
 *    loop 线程在等待后端响应期间不会被阻塞，可继续处理其它连接 —— 这正是网关
 *    “全程异步”的关键。
 *  - 完成回调在 loop 线程内触发，可直接调用 cservice_res_finish() 回写前端响应。
 *
 * 典型用法（在 handler 内）：
 *   cservice_res_defer(res);
 *   uv_loop_t *loop = cservice_req_loop(req);
 *   http_client_init(loop);                       // 幂等：按 loop 建立 multi
 *   http_req_t hr = { cservice_req_method(req), backend_url, body, len, NULL };
 *   http_client_do(loop, &hr, on_backend, ctx);   // 立即返回，不阻塞
 *   // on_backend 在 loop 线程被调用，回写并 cservice_res_finish(res)
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 出站请求描述 */
typedef struct {
    const char            *method;   /* "GET"/"POST"/"PUT"/"DELETE"/"HEAD"，默认 GET */
    const char            *url;
    const char            *body;     /* 可为 NULL */
    size_t                 body_len;
    struct curl_slist     *headers;  /* 可选，外部构造并释放；为 NULL 表示无自定义头 */
} http_req_t;

/* 后端响应（body 由客户端分配，回调返回后即释放，需自行拷贝保存） */
typedef struct {
    int         code;       /* 0=成功(含 HTTP 错误状态码)，<0=传输层错误(超时/断连) */
    int         status;     /* HTTP 状态码（成功时） */
    char       *body;
    size_t      body_len;
    char        content_type[128];
} http_resp_t;

typedef void (*http_cb_t)(http_resp_t *resp, void *ud);

/**
 * 为指定 loop 建立（若尚未建立）异步客户端上下文。幂等，可重复调用。
 * 必须在 loop 所属线程调用（handler 内天然满足）。
 */
int http_client_init(uv_loop_t *loop);

/**
 * 发起一次异步 HTTP 请求。
 * @param loop  发起请求的事件循环（与 http_client_init 同一 loop）
 * @param req   请求描述
 * @param cb    完成回调（在 loop 线程触发）
 * @param ud    回调用户数据
 * @return 0 成功，<0 失败（如 loop 未初始化）
 */
int http_client_do(uv_loop_t *loop, const http_req_t *req,
                   http_cb_t cb, void *ud);

/** 释放某 loop 的客户端上下文（进程退出前可选调用） */
void http_client_cleanup(uv_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
