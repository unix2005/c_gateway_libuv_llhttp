/**
 * @file async_http.c
 * @brief 异步上游 HTTP 转发实现（libcurl multi + libuv uv_poll）
 *
 * 详见 async_http.h 的设计说明。
 */

#include "async_http.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------- 内部数据结构 ---------------- */

/* 每个 socket 对应的 uv_poll 包装 */
typedef struct {
    uv_poll_t      poll;     /* 必须是结构体首个成员，便于互转 */
    curl_socket_t  sockfd;
} async_sock_t;

/* 一次在途请求 */
typedef struct async_http_req {
    struct async_http_req *next; /* 同一 loop 内的在途链表 */
    CURL *easy;
    struct curl_slist *headers;
    char  *body;                 /* 请求体（上传用），拷贝自调用方 */
    size_t body_len;
    size_t body_cap;
    char  *resp;                 /* 响应体累积缓冲（与请求体分离！） */
    size_t resp_len;
    size_t resp_cap;
    async_http_done_cb cb;
    void   *user_data;
} async_http_req_t;

/* 每个 uv_loop 一个 */
typedef struct {
    CURLM     *multi;
    uv_timer_t timer;
    int        still_running;
    async_http_req_t *reqs; /* 在途请求链表头 */
} async_http_loop_t;

/* ---------------- 前向声明 ---------------- */

static async_http_loop_t *loop_ctx_get(uv_loop_t *loop);
static void poll_event_cb(uv_poll_t *handle, int status, int events);
static void check_completions(async_http_loop_t *ctx);
static void start_timeout(async_http_loop_t *ctx, long ms);

/* ---------------- libcurl 回调 ---------------- */

/* 写回数据累积（写入独立的 resp 缓冲，绝不复用请求体缓冲） */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    async_http_req_t *r = (async_http_req_t *)userp;

    if (r->resp_len + realsize + 1 > r->resp_cap) {
        size_t ncap = (r->resp_cap ? r->resp_cap * 2 : 4096);
        while (ncap < r->resp_len + realsize + 1) ncap *= 2;
        r->resp = realloc(r->resp, ncap);
        r->resp_cap = ncap;
    }
    memcpy(r->resp + r->resp_len, ptr, realsize);
    r->resp_len += realsize;
    r->resp[r->resp_len] = '\0';
    return realsize;
}

/* libcurl 通知我们某个 socket 关心的事件变化 */
static int sock_cb(CURL *e, curl_socket_t s, int what, void *userp, void *socketp)
{
    (void)e;
    async_http_loop_t *ctx = (async_http_loop_t *)userp;
    async_sock_t *sock = (async_sock_t *)socketp;

    if (what == CURL_POLL_REMOVE) {
        if (sock) {
            uv_poll_stop(&sock->poll);
            uv_close((uv_handle_t *)&sock->poll, (uv_close_cb)free);
        }
        return 0;
    }

    if (!sock) {
        sock = (async_sock_t *)malloc(sizeof(*sock));
        sock->sockfd = s;
        uv_poll_init(ctx->timer.loop, &sock->poll, (int)s);
        curl_multi_assign(ctx->multi, s, sock);
    }

    int events = 0;
    if (what & CURL_POLL_IN)  events |= UV_READABLE;
    if (what & CURL_POLL_OUT) events |= UV_WRITABLE;
    uv_poll_start(&sock->poll, events, poll_event_cb);
    return 0;
}

/* libcurl 让我们在指定毫秒后"唤醒"一次（驱动超时） */
static int timer_cb(CURLM *multi, long timeout_ms, void *userp)
{
    (void)multi;
    async_http_loop_t *ctx = (async_http_loop_t *)userp;
    start_timeout(ctx, timeout_ms);
    return 0;
}

/* ---------------- libuv 事件回调 ---------------- */

static void poll_event_cb(uv_poll_t *handle, int status, int events)
{
    async_sock_t *sock = (async_sock_t *)handle;
    async_http_loop_t *ctx =
        (async_http_loop_t *)((async_sock_t *)handle)->poll.loop->data;

    int action = 0;
    if (events & UV_READABLE) action |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE) action |= CURL_CSELECT_OUT;

    curl_multi_socket_action(ctx->multi, sock->sockfd, action, &ctx->still_running);
    check_completions(ctx);
}

static void timer_event_cb(uv_timer_t *t)
{
    async_http_loop_t *ctx = (async_http_loop_t *)t->data;
    curl_multi_socket_action(ctx->multi, CURL_SOCKET_TIMEOUT, 0, &ctx->still_running);
    check_completions(ctx);
}

/* ---------------- 内部工具 ---------------- */

static void start_timeout(async_http_loop_t *ctx, long ms)
{
    uv_timer_stop(&ctx->timer);
    if (ms < 0) return;            /* libcurl 表示无需定时器 */
    if (ms == 0) ms = 1;           /* 立即驱动 */
    uv_timer_start(&ctx->timer, timer_event_cb, ms, 0);
}

/* 处理已完成的请求并触发业务回调 */
static void check_completions(async_http_loop_t *ctx)
{
    CURLMsg *msg;
    int msgs_left;

    while ((msg = curl_multi_info_read(ctx->multi, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL *easy = msg->easy_handle;
        async_http_req_t *r = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &r);

        long code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);

        /* 从在途链表移除 */
        async_http_req_t **pp = &ctx->reqs;
        while (*pp) {
            if (*pp == r) { *pp = r->next; break; }
            pp = &(*pp)->next;
        }

        if (r && r->cb)
            r->cb(r, (int)code, r->resp ? r->resp : "", r->resp_len, r->user_data);

        curl_multi_remove_handle(ctx->multi, easy);
        curl_easy_cleanup(easy);
        curl_slist_free_all(r->headers);
        free(r->body);   /* 请求体 */
        free(r->resp);   /* 响应体 */
        free(r);
    }
}

/* 懒初始化：每个 loop 一个 CURLM + timer，并挂到 loop->data */
static async_http_loop_t *loop_ctx_get(uv_loop_t *loop)
{
    if (loop->data) return (async_http_loop_t *)loop->data;

    async_http_loop_t *ctx = (async_http_loop_t *)calloc(1, sizeof(*ctx));
    ctx->multi = curl_multi_init();
    curl_multi_setopt(ctx->multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(ctx->multi, CURLMOPT_SOCKETDATA, ctx);
    curl_multi_setopt(ctx->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
    curl_multi_setopt(ctx->multi, CURLMOPT_TIMERDATA, ctx);
    curl_multi_setopt(ctx->multi, CURLMOPT_MAXCONNECTS, 16L);

    uv_timer_init(loop, &ctx->timer);
    ctx->timer.data = ctx;
    ctx->reqs = NULL;

    loop->data = ctx;
    return ctx;
}

/* ---------------- 对外 API ---------------- */

void async_http_global_init(void)
{
    curl_global_init(CURL_GLOBAL_ALL);
}

void async_http_global_cleanup(void)
{
    curl_global_cleanup();
}

int async_http_submit(uv_loop_t *loop,
                      const char *method,
                      const char *url,
                      const char *headers[],
                      const char *body,
                      size_t body_len,
                      long timeout_ms,
                      async_http_done_cb cb,
                      void *user_data)
{
    async_http_loop_t *ctx = loop_ctx_get(loop);
    if (!ctx || !ctx->multi) return -1;

    async_http_req_t *r = (async_http_req_t *)calloc(1, sizeof(*r));
    r->cb = cb;
    r->user_data = user_data;

    r->easy = curl_easy_init();
    if (!r->easy) { free(r); return -1; }

    curl_easy_setopt(r->easy, CURLOPT_URL, url);
    curl_easy_setopt(r->easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(r->easy, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(r->easy, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(r->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(r->easy, CURLOPT_PRIVATE, r);
    curl_easy_setopt(r->easy, CURLOPT_FOLLOWLOCATION, 1L);

    /* 拷贝请求体（因转发是异步的，调用方缓冲可能已被释放/复用） */
    if (body && body_len > 0) {
        r->body = malloc(body_len);
        memcpy(r->body, body, body_len);
        r->body_len = body_len;
        r->body_cap = body_len;
    }

    /* 方法 */
    if (method) {
        if (strcmp(method, "POST") == 0) {
            curl_easy_setopt(r->easy, CURLOPT_POST, 1L);
            if (r->body) {
                curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, r->body);
                curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
            }
        } else if (strcmp(method, "PUT") == 0) {
            curl_easy_setopt(r->easy, CURLOPT_CUSTOMREQUEST, "PUT");
            if (r->body) {
                curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, r->body);
                curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
            }
        } else if (strcmp(method, "DELETE") == 0) {
            curl_easy_setopt(r->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
        } else if (strcmp(method, "HEAD") == 0) {
            curl_easy_setopt(r->easy, CURLOPT_CUSTOMREQUEST, "HEAD");
        } else if (strcmp(method, "GET") != 0) {
            curl_easy_setopt(r->easy, CURLOPT_CUSTOMREQUEST, method);
        }
    }

    /* 请求头 */
    if (headers) {
        for (int i = 0; headers[i]; i++)
            r->headers = curl_slist_append(r->headers, headers[i]);
        if (r->headers)
            curl_easy_setopt(r->easy, CURLOPT_HTTPHEADER, r->headers);
    }

    /* 入队 */
    CURLMcode m = curl_multi_add_handle(ctx->multi, r->easy);
    if (m) {
        curl_easy_cleanup(r->easy);
        curl_slist_free_all(r->headers);
        free(r->body);
        free(r->resp);
        free(r);
        return -1;
    }

    /* 挂入在途链表 */
    r->next = ctx->reqs;
    ctx->reqs = r;

    /* 立即驱动一次，处理可能已就绪的连接（如 DNS 缓存命中） */
    curl_multi_socket_action(ctx->multi, CURL_SOCKET_TIMEOUT, 0, &ctx->still_running);
    check_completions(ctx);
    return 0;
}

void async_http_cancel_by_userdata(uv_loop_t *loop, void *user_data)
{
    async_http_loop_t *ctx = (async_http_loop_t *)loop->data;
    if (!ctx) return;

    async_http_req_t **pp = &ctx->reqs;
    while (*pp) {
        async_http_req_t *r = *pp;
        if (r->user_data == user_data) {
            *pp = r->next;
            curl_multi_remove_handle(ctx->multi, r->easy);
            curl_easy_cleanup(r->easy);
            curl_slist_free_all(r->headers);
            free(r->body);
            free(r->resp);
            free(r);
            /* 不 break：同一 user_data 可能对应多个请求 */
        } else {
            pp = &(*pp)->next;
        }
    }
}
