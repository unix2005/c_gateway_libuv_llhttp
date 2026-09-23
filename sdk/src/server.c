/**
 * @file server.c
 * @brief libcservice 内嵌 HTTP 服务器（libuv + llhttp，多 loop + 长连接）
 */
#include "sdk_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* 连接写上下文：负责在写完成后释放资源 */
typedef struct {
    uv_write_t  req;
    char       *header;
    char       *body;     /* 即 conn->res.body，回调中释放 */
    sdk_conn_t *conn;
} sdk_write_ctx_t;

/* ---------- 连接重置（用于长连接复用） ---------- */
void sdk_conn_reset(sdk_conn_t *c)
{
    free(c->body);            c->body = NULL;     c->body_len = 0;
    free(c->req.body);        c->req.body = NULL; c->req.body_len = 0;
    c->req.header_count = 0;
    c->req.path[0] = '\0';
    c->cur_field[0] = '\0';
    c->cur_value[0] = '\0';
    c->hdr_active = 0;

    free(c->res.body);        c->res.body = NULL; c->res.body_len = 0;
    c->res.header_count = 0;  c->res.status = 0;

    llhttp_init(&c->parser, HTTP_REQUEST, &c->settings);
    c->parser.data = c;
    c->url[0] = '\0';
}

/* ---------- llhttp 回调 ---------- */
static int on_url(llhttp_t *parser, const char *at, size_t len)
{
    sdk_conn_t *c = (sdk_conn_t *)parser->data;
    if (strlen(c->url) + len < sizeof(c->url))
        strncat(c->url, at, len);
    return 0;
}

static void sdk_store_pending_header(sdk_conn_t *c)
{
    if (!c->hdr_active || c->cur_field[0] == '\0') return;
    if (c->req.header_count >= SDK_MAX_HEADERS) return;
    sdk_kv_t *h = &c->req.headers[c->req.header_count++];
    snprintf(h->name, sizeof(h->name), "%s", c->cur_field);
    snprintf(h->value, sizeof(h->value), "%s", c->cur_value);
    c->hdr_active = 0;
    c->cur_field[0] = '\0';
    c->cur_value[0] = '\0';
}

static int on_header_field(llhttp_t *parser, const char *at, size_t len)
{
    sdk_conn_t *c = (sdk_conn_t *)parser->data;
    /* 新字段开始：先把上一条落库，再重置为当前字段名 */
    sdk_store_pending_header(c);
    if (strlen(c->cur_field) + len < sizeof(c->cur_field))
        strncat(c->cur_field, at, len);
    c->hdr_active = 1;
    return 0;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t len)
{
    sdk_conn_t *c = (sdk_conn_t *)parser->data;
    if (strlen(c->cur_value) + len < sizeof(c->cur_value))
        strncat(c->cur_value, at, len);
    return 0;
}

static int on_body(llhttp_t *parser, const char *at, size_t len)
{
    sdk_conn_t *c = (sdk_conn_t *)parser->data;
    c->body = realloc(c->body, c->body_len + len + 1);
    if (c->body)
    {
        memcpy(c->body + c->body_len, at, len);
        c->body_len += len;
        c->body[c->body_len] = '\0';
    }
    return 0;
}

/* 内置健康检查 */
static void sdk_default_health(cservice_req_t *req, cservice_res_t *res)
{
    (void)req;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "healthy");
    cJSON_AddStringToObject(root, "service", ((sdk_conn_t *)req->conn)->svc->name);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cservice_res_json(res, s);
    free(s);
}

static int on_message_complete(llhttp_t *parser)
{
    sdk_conn_t *c = (sdk_conn_t *)parser->data;
    cservice_t *svc = c->svc;

    /* 落库最后一条未保存的 header */
    sdk_store_pending_header(c);

    c->keep_alive = llhttp_should_keep_alive(&c->parser);

    /* 路径（去掉 query） */
    char *q = strchr(c->url, '?');
    if (q) { *q = '\0'; }
    snprintf(c->req.path, sizeof(c->req.path), "%s", c->url);
    if (q) { *q = '?'; }   /* 还原以便 query 解析 */

    c->req.method = sdk_method_from_llhttp(c->parser.method);
    snprintf(c->req.method_str, sizeof(c->req.method_str), "%s",
             llhttp_method_name(c->parser.method));

    c->req.body = c->body; c->req.body_len = c->body_len; /* 借用，发送后由 reset 释放 */

    c->res.status = 200;

    /* 路由匹配 */
    sdk_route_t *route = sdk_route_match(svc, c->req.method, c->req.path);
    if (route)
    {
        route->handler(&c->req, &c->res);
    }
    else if ((c->req.method & CSERVICE_GET) &&
             strcmp(c->req.path, svc->health_path) == 0)
    {
        sdk_default_health(&c->req, &c->res);
    }
    else
    {
        cservice_res_send(&c->res, 404, "application/json",
                          "{\"error\":\"Not Found\"}", 21);
    }

    /* 发送响应 */
    sdk_send_response(c);

    /* body 已被发送逻辑接管/释放，避免 reset 重复释放 */
    c->body = NULL; c->body_len = 0;
    c->req.body = NULL; c->req.body_len = 0;
    return 0;
}

/* ---------- 网络回调 ---------- */
static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
    (void)h;
    buf->base = malloc(suggested);
    buf->len = suggested;
}

static void on_conn_close(uv_handle_t *h)
{
    sdk_conn_t *c = (sdk_conn_t *)h->data;
    free(c->body);
    free(c->req.body);
    free(c);
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    sdk_conn_t *c = (sdk_conn_t *)stream->data;
    if (nread > 0)
    {
        enum llhttp_errno err = llhttp_execute(&c->parser, (const char *)buf->base, nread);
        if (err != HPE_OK)
            sdk_log("WARN", "HTTP 解析错误：%s", llhttp_errno_name(err));
    }
    else if (nread < 0)
    {
        if (nread != UV_EOF)
            sdk_log("WARN", "read error: %s", uv_err_name(nread));
        uv_close((uv_handle_t *)stream, on_conn_close);
    }
    if (buf->base) free(buf->base);
}

static void on_write_completed(uv_write_t *req, int status)
{
    sdk_write_ctx_t *w = (sdk_write_ctx_t *)req;
    sdk_conn_t *c = w->conn;
    (void)status;

    free(w->header);
    free(w->body);
    c->res.body = NULL;   /* 已被释放，避免 reset 重复 */

    if (c->keep_alive)
    {
        sdk_conn_reset(c);
        uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
    }
    else
    {
        uv_close((uv_handle_t *)&c->handle, on_conn_close);
    }
    free(w);
}

void sdk_send_response(sdk_conn_t *c)
{
    int code = c->res.status > 0 ? c->res.status : 200;
    const char *ct = c->res.content_type[0] ? c->res.content_type : "application/octet-stream";
    size_t blen = c->res.body ? c->res.body_len : 0;

    /* 构造响应头 */
    sdk_write_ctx_t *w = malloc(sizeof(*w));
    w->conn = c;
    w->body = c->res.body;   /* 接管所有权 */
    size_t hl = 256 + strlen(ct) + 16;
    w->header = malloc(hl);
    int n = snprintf(w->header, hl,
                     "HTTP/1.1 %d OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     code, ct, blen,
                     c->keep_alive ? "keep-alive" : "close");

    uv_buf_t bufs[2];
    bufs[0] = uv_buf_init(w->header, (size_t)n);
    int nbufs = 1;
    if (blen > 0)
    {
        bufs[1] = uv_buf_init(w->body, blen);
        nbufs = 2;
    }
    uv_write(&w->req, (uv_stream_t *)&c->handle, bufs, nbufs, on_write_completed);
}

static void on_new_connection(uv_stream_t *server, int status)
{
    if (status < 0) return;
    cservice_t *svc = (cservice_t *)server->data;
    sdk_conn_t *c = calloc(1, sizeof(*c));
    c->svc = svc;
    uv_tcp_init(server->loop, &c->handle);
    c->handle.data = c;

    llhttp_settings_init(&c->settings);
    c->settings.on_url = on_url;
    c->settings.on_header_field = on_header_field;
    c->settings.on_header_value = on_header_value;
    c->settings.on_body = on_body;
    c->settings.on_message_complete = on_message_complete;
    llhttp_init(&c->parser, HTTP_REQUEST, &c->settings);
    c->parser.data = c;
    c->req.conn = c;

    if (uv_accept(server, (uv_stream_t *)&c->handle) == 0)
        uv_read_start((uv_stream_t *)&c->handle, on_alloc, on_read);
    else
        uv_close((uv_handle_t *)&c->handle, on_conn_close);
}

/* ---------- 多 loop 启动 ---------- */

/* uv_walk 回调：关闭 loop 内所有残留 handle（监听 + 长连接），避免 uv_loop_delete 断言 */
static void close_walk_cb(uv_handle_t *h, void *arg)
{
    (void)arg;
    if (h) uv_close(h, NULL);
}

static void *worker_loop(void *arg)
{
    uv_loop_t *loop = (uv_loop_t *)arg;
    uv_run(loop, UV_RUN_DEFAULT);
    /* 在本线程内完成优雅关闭：关闭所有残留 handle（含长连接）后再关 loop。
       必须在所属线程内做，避免跨线程操作 loop 导致关闭回调未完成的竞态。 */
    uv_walk(loop, close_walk_cb, NULL);
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
    return NULL;
}

int sdk_server_start_loops(cservice_t *svc)
{
    int n = svc->thread_count > 0 ? svc->thread_count : 1;
    svc->nloops = n;
    svc->loops = calloc(n, sizeof(uv_loop_t *));
    svc->servers = calloc(n, sizeof(uv_tcp_t *));
    svc->threads = calloc(n, sizeof(pthread_t));

    int port = svc->port;
    int af = AF_INET; /* SDK 默认 IPv4；可扩展 */

    for (int i = 0; i < n; i++)
    {
        uv_loop_t *loop = uv_loop_new();
        uv_tcp_t *server = malloc(sizeof(uv_tcp_t));
        uv_tcp_init(loop, server);
        server->data = svc;

        int fd = socket(af, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); goto fail; }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
        struct sockaddr_in a;
        uv_ip4_addr(svc->host, port, &a);
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        {
            sdk_log("ERROR", "绑定 %s:%d 失败：%s", svc->host, port, strerror(errno));
            close(fd); svc->nloops = i; goto fail;
        }
        if (uv_tcp_open(server, fd) != 0) { close(fd); svc->nloops = i; goto fail; }
        if (uv_listen((uv_stream_t *)server, 128, on_new_connection) != 0)
        {
            sdk_log("ERROR", "监听失败"); svc->nloops = i; goto fail;
        }

        svc->loops[i] = loop;
        svc->servers[i] = server;
        pthread_create(&svc->threads[i], NULL, worker_loop, loop);
    }
    return 0;

fail:
    sdk_server_stop(svc);
    return -1;
}

void sdk_server_stop(cservice_t *svc)
{
    if (!svc) return;
    for (int i = 0; i < svc->nloops; i++)
    {
        if (svc->loops[i])
        {
            uv_stop(svc->loops[i]);
            pthread_join(svc->threads[i], NULL);
            /* worker 线程内部已 uv_loop_close，这里仅释放结构体 */
            free(svc->loops[i]);
        }
        free(svc->servers[i]);
    }
    svc->nloops = 0;
}
