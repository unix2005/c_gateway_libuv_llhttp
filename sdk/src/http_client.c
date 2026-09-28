/**
 * @file http_client.c
 * @brief 异步出站 HTTP 客户端：libcurl multi + libuv（uv_poll + uv_timer 驱动）
 *
 * 这是 libcurl 官方 “multi + libuv” 集成范式的最小可靠实现：
 *  - CURLMOPT_SOCKETFUNCTION 把每个 socket 映射到一个 uv_poll_t；
 *  - CURLMOPT_TIMERFUNCTION 用 uv_timer_t 处理超时；
 *  - 二者都绑定到“发起请求的那个 loop”，保证回调在同一 loop 线程。
 */
#include "http_client.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 内部数据结构 ---------------- */

typedef struct sock_ctx {
    uv_poll_t       poll;
    curl_socket_t   fd;
    struct loop_ctx *lc;
} sock_ctx_t;

typedef struct req_ctx {
    http_cb_t       cb;
    void           *ud;
    char           *body;
    size_t          body_len;
    size_t          cap;
    size_t          read_off;   /* PUT 上传进度 */
    struct loop_ctx *lc;
} req_ctx_t;

typedef struct loop_ctx {
    uv_loop_t      *loop;
    CURLM          *multi;
    uv_timer_t      timer;
    int             timer_active;
    /* fd -> sock_ctx* 简易映射（动态数组，规模通常很小） */
    sock_ctx_t    **socks;
    int             nsocks;
    int             capsocks;
    struct loop_ctx *next;
} loop_ctx_t;

static loop_ctx_t *g_ctxs = NULL;
static int         g_curl_inited = 0;

/* ---------------- fd -> sock_ctx 映射 ---------------- */

static void sock_put(loop_ctx_t *lc, sock_ctx_t *sc)
{
    if (lc->nsocks == lc->capsocks) {
        lc->capsocks = lc->capsocks ? lc->capsocks * 2 : 8;
        lc->socks = realloc(lc->socks, (size_t)lc->capsocks * sizeof(sock_ctx_t *));
    }
    lc->socks[lc->nsocks++] = sc;
}

static void sock_free_cb(uv_handle_t *h) { free(h); }

static void sock_del(loop_ctx_t *lc, curl_socket_t fd)
{
    for (int i = 0; i < lc->nsocks; i++) {
        if (lc->socks[i] && lc->socks[i]->fd == fd) {
            sock_ctx_t *sc = lc->socks[i];
            lc->socks[i] = lc->socks[--lc->nsocks];
            if (sc->poll.type != 0) {          /* 已 init 过 */
                uv_poll_stop(&sc->poll);
                uv_close((uv_handle_t *)&sc->poll, sock_free_cb);
            } else {
                free(sc);
            }
            return;
        }
    }
}

/* ---------------- loop 上下文 ---------------- */

/* 前向声明（ensure_ctx 会引用这些回调） */
static int  sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);
static int  timer_cb(CURLM *multi, long timeout_ms, void *cbp);
static void poll_cb(uv_poll_t *p, int status, int events);

static loop_ctx_t *find_ctx(uv_loop_t *loop)
{
    for (loop_ctx_t *l = g_ctxs; l; l = l->next)
        if (l->loop == loop) return l;
    return NULL;
}

static loop_ctx_t *ensure_ctx(uv_loop_t *loop)
{
    loop_ctx_t *l = find_ctx(loop);
    if (l) return l;
    if (!g_curl_inited) { curl_global_init(CURL_GLOBAL_ALL); g_curl_inited = 1; }

    l = calloc(1, sizeof(*l));
    l->loop  = loop;
    l->multi = curl_multi_init();
    if (!l->multi) { free(l); return NULL; }

    curl_multi_setopt(l->multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(l->multi, CURLMOPT_SOCKETDATA, l);
    curl_multi_setopt(l->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
    curl_multi_setopt(l->multi, CURLMOPT_TIMERDATA, l);

    uv_timer_init(loop, &l->timer);
    l->timer.data = l;

    l->next = g_ctxs;
    g_ctxs  = l;
    return l;
}

/* ---------------- curl -> libuv 粘合 ---------------- */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    req_ctx_t *rc = ud;
    size_t realsize = size * nmemb;
    if (realsize == 0) return 0;
    if (rc->body_len + realsize + 1 > rc->cap) {
        size_t ncap = (rc->body_len + realsize + 1) * 2;
        char *nb = realloc(rc->body, ncap);
        if (!nb) return 0;            /* 内存不足，中止本次传输 */
        rc->body = nb; rc->cap = ncap;
    }
    memcpy(rc->body + rc->body_len, ptr, realsize);
    rc->body_len += realsize;
    rc->body[rc->body_len] = '\0';
    return realsize;
}

static size_t read_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    req_ctx_t *rc = ud;
    size_t avail = size * nmemb;
    if (rc->read_off >= rc->body_len) return 0;
    size_t tocopy = rc->body_len - rc->read_off;
    if (tocopy > avail) tocopy = avail;
    memcpy(ptr, rc->body + rc->read_off, tocopy);
    rc->read_off += tocopy;
    return tocopy;
}

static void check_done(loop_ctx_t *lc)
{
    CURLMsg *m;
    int queued = 0;
    while ((m = curl_multi_info_read(lc->multi, &queued))) {
        if (m->msg != CURLMSG_DONE) continue;

        CURL *e = m->easy_handle;
        req_ctx_t *rc = NULL;
        curl_easy_getinfo(e, CURLINFO_PRIVATE, &rc);

        http_resp_t resp;
        memset(&resp, 0, sizeof resp);
        if (m->data.result == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
            resp.status = (int)code;
            resp.code   = 0;
            char *ct = NULL;
            curl_easy_getinfo(e, CURLINFO_CONTENT_TYPE, &ct);
            if (ct) snprintf(resp.content_type, sizeof resp.content_type, "%s", ct);
        } else {
            resp.code = -1;
        }
        resp.body     = rc ? rc->body : NULL;
        resp.body_len = rc ? rc->body_len : 0;

        http_cb_t cb = rc ? rc->cb : NULL;
        void *ud = rc ? rc->ud : NULL;

        curl_multi_remove_handle(lc->multi, e);
        curl_easy_cleanup(e);

        if (cb) cb(&resp, ud);

        if (rc) {
            free(rc->body);
            free(rc);
        }
    }
}

static void timer_timeout_cb(uv_timer_t *t)
{
    loop_ctx_t *lc = t->data;
    int running = 0;
    curl_multi_socket_action(lc->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    check_done(lc);
}

static int timer_cb(CURLM *multi, long timeout_ms, void *cbp)
{
    (void)multi;
    loop_ctx_t *lc = cbp;
    if (timeout_ms < 0) {
        uv_timer_stop(&lc->timer);
        lc->timer_active = 0;
        return 0;
    }
    if (timeout_ms == 0) {
        int running = 0;
        curl_multi_socket_action(lc->multi, CURL_SOCKET_TIMEOUT, 0, &running);
        check_done(lc);
    } else {
        uv_timer_start(&lc->timer, timer_timeout_cb, (uint64_t)timeout_ms, 0);
        lc->timer_active = 1;
    }
    return 0;
}

static void poll_cb(uv_poll_t *p, int status, int events)
{
    (void)status;
    sock_ctx_t *sc = p->data;
    loop_ctx_t *lc = sc->lc;
    int action = 0;
    if (events & UV_READABLE) action |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE) action |= CURL_CSELECT_OUT;
    int running = 0;
    curl_multi_socket_action(lc->multi, sc->fd, action, &running);
    check_done(lc);
}

static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp)
{
    (void)e;
    loop_ctx_t *lc = cbp;
    sock_ctx_t *sc = sockp;

    if (what == CURL_POLL_REMOVE) {
        if (sc) sock_del(lc, s);
        return 0;
    }
    if (what == CURL_POLL_NONE) return 0;

    if (!sc) {
        sc = calloc(1, sizeof(*sc));
        sc->fd = s;
        sc->lc = lc;
        uv_poll_init(lc->loop, &sc->poll, (int)s);
        sc->poll.data = sc;
        sock_put(lc, sc);
        curl_multi_assign(lc->multi, s, sc);
    }

    int events = 0;
    if (what & CURL_POLL_IN)  events |= UV_READABLE;
    if (what & CURL_POLL_OUT) events |= UV_WRITABLE;
    uv_poll_start(&sc->poll, events, poll_cb);
    return 0;
}

/* ---------------- 对外 API ---------------- */

int http_client_init(uv_loop_t *loop)
{
    return ensure_ctx(loop) ? 0 : -1;
}

int http_client_do(uv_loop_t *loop, const http_req_t *req,
                   http_cb_t cb, void *ud)
{
    loop_ctx_t *lc = ensure_ctx(loop);
    if (!lc || !req || !req->url) return -1;

    CURL *e = curl_easy_init();
    if (!e) return -1;

    req_ctx_t *rc = calloc(1, sizeof(*rc));
    rc->cb = cb;
    rc->ud = ud;
    rc->lc = lc;

    curl_easy_setopt(e, CURLOPT_URL, req->url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, rc);
    curl_easy_setopt(e, CURLOPT_PRIVATE, rc);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);   /* 多线程/事件循环下必须 */

    const char *m = req->method;
    if (m && strcasecmp(m, "POST") == 0) {
        curl_easy_setopt(e, CURLOPT_POST, 1L);
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, req->body ? req->body : "");
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    } else if (m && strcasecmp(m, "PUT") == 0) {
        curl_easy_setopt(e, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(e, CURLOPT_INFILESIZE_LARGE, (curl_off_t)req->body_len);
        curl_easy_setopt(e, CURLOPT_READFUNCTION, read_cb);
        curl_easy_setopt(e, CURLOPT_READDATA, rc);
    } else if (m && strcasecmp(m, "HEAD") == 0) {
        curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
    } else if (m && strcasecmp(m, "DELETE") == 0) {
        curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    if (req->headers) curl_easy_setopt(e, CURLOPT_HTTPHEADER, req->headers);

    curl_multi_add_handle(lc->multi, e);
    int running = 0;
    curl_multi_socket_action(lc->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    return 0;
}

void http_client_cleanup(uv_loop_t *loop)
{
    loop_ctx_t *prev = NULL, *l = g_ctxs;
    while (l) {
        if (l->loop == loop) {
            for (int i = 0; i < l->nsocks; i++) free(l->socks[i]);
            free(l->socks);
            if (l->timer_active) uv_timer_stop(&l->timer);
            curl_multi_cleanup(l->multi);
            loop_ctx_t *nx = l->next;
            if (prev) prev->next = nx; else g_ctxs = nx;
            free(l);
            l = nx;
            continue;
        }
        prev = l; l = l->next;
    }
}
