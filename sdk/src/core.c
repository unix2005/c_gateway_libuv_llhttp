/**
 * @file core.c
 * @brief libcservice 生命周期与路由管理
 */
#include "sdk_internal.h"
#include <q/log.h>
#include <q/core/types.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

cservice_t *cservice_init(const char *name, const char *host, int port)
{
    cservice_t *svc = calloc(1, sizeof(*svc));
    if (!svc) return NULL;

    snprintf(svc->name, sizeof(svc->name), "%s", name ? name : "unnamed-service");
    snprintf(svc->host, sizeof(svc->host), "%s", host ? host : "0.0.0.0");
    svc->port = port > 0 ? port : 8080;
    svc->thread_count = 1;
    snprintf(svc->health_path, sizeof(svc->health_path), "/health");
    svc->register_enabled = 1;
    snprintf(svc->gw_host, sizeof(svc->gw_host), "127.0.0.1");
    svc->gw_port = 8080;
    svc->path_prefix[0] = '\0';
    snprintf(svc->log_dir, sizeof(svc->log_dir), "logs");
    svc->log_level = Q_LOG_INFO;

    svc->route_cap = 16;
    svc->routes = calloc(svc->route_cap, sizeof(sdk_route_t));
    return svc;
}

void cservice_set_log_dir(cservice_t *svc, const char *dir)
{
    if (svc && dir) snprintf(svc->log_dir, sizeof(svc->log_dir), "%s", dir);
}

void cservice_set_log_level(cservice_t *svc, int level)
{
    if (svc) svc->log_level = level;
}

void cservice_set_threads(cservice_t *svc, int n)
{
    if (svc && n > 0) svc->thread_count = n;
}

void cservice_set_health_path(cservice_t *svc, const char *path)
{
    if (svc && path) snprintf(svc->health_path, sizeof(svc->health_path), "%s", path);
}

void cservice_set_gateway(cservice_t *svc, const char *host, int port)
{
    if (!svc) return;
    if (host) snprintf(svc->gw_host, sizeof(svc->gw_host), "%s", host);
    if (port > 0) svc->gw_port = port;
}

void cservice_set_path_prefix(cservice_t *svc, const char *prefix)
{
    if (svc && prefix) snprintf(svc->path_prefix, sizeof(svc->path_prefix), "%s", prefix);
}

void cservice_enable_gateway_register(cservice_t *svc, int enable)
{
    if (svc) svc->register_enabled = enable;
}

void cservice_set_config_file(cservice_t *svc, const char *path)
{
    if (svc && path) snprintf(svc->config_file, sizeof(svc->config_file), "%s", path);
}

int cservice_route(cservice_t *svc,
                   cservice_method_t m,
                   const char *path,
                   cservice_handler_t h)
{
    if (!svc || !path || !h) return -1;

    if (svc->route_count >= svc->route_cap)
    {
        int nc = svc->route_cap * 2;
        sdk_route_t *nr = realloc(svc->routes, nc * sizeof(sdk_route_t));
        if (!nr) return -1;
        svc->routes = nr;
        svc->route_cap = nc;
    }

    sdk_route_t *r = &svc->routes[svc->route_count++];
    r->method = m;
    r->path = strdup(path);
    r->is_prefix = (strlen(path) > 0 && path[strlen(path) - 1] == '/');
    r->handler = h;
    return 0;
}

/* 信号处理器需要访问当前运行的 svc */
static cservice_t *g_running_svc = NULL;

static void svc_signal_handler(int signum)
{
    (void)signum;
    if (g_running_svc) cservice_stop(g_running_svc);
}

int cservice_run(cservice_t *svc)
{
    if (!svc) return -1;

    if (svc->config_file[0])
        sdk_config_load(svc, svc->config_file);

    /* 初始化异步日志（必须在创建任何线程之前调用） */
    if (q_log_init(svc->log_dir, svc->name, (q_log_level_t)svc->log_level) != Q_OK)
        sdk_log("WARN", "q_log 初始化失败或已初始化，日志可能降级为同步输出");

    /* 向网关注册 + 启动心跳 */
    if (svc->register_enabled)
    {
        snprintf(svc->reg_url, sizeof(svc->reg_url),
                 "http://%s:%d/api/services/register",
                 svc->gw_host, svc->gw_port);
        snprintf(svc->unreg_url, sizeof(svc->unreg_url),
                 "http://%s:%d/api/services/unregister",
                 svc->gw_host, svc->gw_port);

        if (sdk_registry_register(svc) == 0)
            sdk_log("INFO", "已注册到网关 %s:%d", svc->gw_host, svc->gw_port);
        else
            sdk_log("WARN", "注册网关失败（网关可能未启动），继续独立运行");

        if (svc->register_enabled)
        {
            svc->hb_running = 1;
            pthread_create(&svc->hb_thread, NULL, sdk_registry_heartbeat, svc);
        }
    }

    /* 启动服务器（多 loop） */
    if (sdk_server_start_loops(svc) != 0)
    {
        sdk_log("ERROR", "服务器启动失败");
        return -1;
    }

    /* 注册信号，优雅退出 */
    g_running_svc = svc;
    signal(SIGINT, svc_signal_handler);
    signal(SIGTERM, svc_signal_handler);

    sdk_log("INFO", "服务 %s 已启动，监听 %s:%d", svc->name, svc->host, svc->port);

    /* 阻塞：join 所有工作线程 */
    for (int i = 0; i < svc->nloops; i++)
        pthread_join(svc->threads[i], NULL);

    /* 停止心跳线程 */
    svc->hb_running = 0;
    pthread_join(svc->hb_thread, NULL);

    /* 从网关注销 */
    if (svc->register_enabled)
        sdk_registry_unregister(svc);

    sdk_server_stop(svc);
    return 0;
}

void cservice_stop(cservice_t *svc)
{
    if (!svc) return;
    svc->stop = 1;
    for (int i = 0; i < svc->nloops; i++)
        if (svc->loops[i])
            uv_stop(svc->loops[i]);
}

void cservice_destroy(cservice_t *svc)
{
    if (!svc) return;

    /* 关闭并冲刷异步日志（在其余资源释放前，保证日志不丢） */
    q_log_close();

    for (int i = 0; i < svc->route_count; i++)
        free(svc->routes[i].path);
    free(svc->routes);
    free(svc->loops);
    free(svc->servers);
    free(svc->threads);
    free(svc);
}
