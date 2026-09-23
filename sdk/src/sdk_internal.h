/**
 * @file sdk_internal.h
 * @brief libcservice 内部数据结构（不对外暴露）
 */
#ifndef CSERVICE_INTERNAL_H
#define CSERVICE_INTERNAL_H

#include "cservice.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <uv.h>
#include <llhttp.h>
#include <cjson/cJSON.h>

#define SDK_MAX_HEADERS 32
#define SDK_URL_LEN     2048
#define SDK_METHOD_STR  16

/* ---------- 请求 / 响应（对外不透明，内部定义） ---------- */

typedef struct {
    char name[128];
    char value[512];
} sdk_kv_t;

struct cservice_req {
    char            method_str[SDK_METHOD_STR];
    cservice_method_t method;
    char            path[SDK_URL_LEN];      /* 含 query 的原始路径 */
    char           *body;
    size_t          body_len;
    sdk_kv_t        headers[SDK_MAX_HEADERS];
    int             header_count;
    struct sdk_conn *conn;                  /* 私有回指 */
};

struct cservice_res {
    int      status;
    char    *body;          /* 内部拥有，发送后释放 */
    size_t   body_len;
    char     content_type[64];
    sdk_kv_t headers[SDK_MAX_HEADERS];
    int      header_count;
};

/* ---------- 路由 ---------- */

typedef struct {
    cservice_method_t  method;   /* 方法掩码 */
    char              *path;     /* strdup */
    int                is_prefix;/* 1=前缀匹配（path 以 '/' 结尾），0=精确 */
    cservice_handler_t handler;
} sdk_route_t;

/* ---------- 连接 ---------- */

typedef struct sdk_conn {
    uv_tcp_t           handle;
    llhttp_t           parser;
    llhttp_settings_t  settings;
    char               url[SDK_URL_LEN];
    char              *body;
    size_t             body_len;
    int                keep_alive;
    cservice_t        *svc;

    cservice_req_t     req;
    cservice_res_t     res;

    /* 解析临时状态 */
    char               cur_field[128];
    char               cur_value[512];
    int                hdr_active;   /* 当前是否处于某条 header 解析中 */
} sdk_conn_t;

/* ---------- 服务实例 ---------- */

struct cservice {
    char  name[64];
    char  host[64];
    int   port;
    int   thread_count;
    char  health_path[64];
    int   register_enabled;
    char  gw_host[64];
    int   gw_port;
    char  path_prefix[128];   /* 注册到网关的路径前缀，默认 ""（兜底） */
    char  config_file[256];

    /* 日志配置（传给 q_log_init） */
    char  log_dir[256];
    int   log_level;           /* 取值见 q_log_level_t */

    sdk_route_t *routes;
    int          route_count;
    int          route_cap;

    volatile int stop;
    uv_loop_t  **loops;
    uv_tcp_t   **servers;
    int          nloops;
    pthread_t  *threads;
    pthread_t   hb_thread;
    int         hb_running;

    char  reg_url[256];
    char  unreg_url[256];
};

/* ---------- 内部函数声明 ---------- */

/* server.c */
int  sdk_server_start_loops(cservice_t *svc);
void sdk_server_stop(cservice_t *svc);
void sdk_conn_reset(sdk_conn_t *c);
void sdk_send_response(sdk_conn_t *c);

/* router.c */
sdk_route_t *sdk_route_match(cservice_t *svc,
                             cservice_method_t m, const char *path);
cservice_method_t sdk_method_from_str(const char *s);
cservice_method_t sdk_method_from_llhttp(int lm);

/* registry.c */
int  sdk_registry_register(cservice_t *svc);
int  sdk_registry_unregister(cservice_t *svc);
void *sdk_registry_heartbeat(void *arg);

/* config.c */
int  sdk_config_load(cservice_t *svc, const char *file);

/* logger.c */
void sdk_log(const char *level, const char *fmt, ...);

#endif /* CSERVICE_INTERNAL_H */
