/**
 * 多线程工作服务 v4 —— 真·多线程高性能版（基于 libcservice SDK）
 *
 * 与 v3.1 的本质区别：
 *  - v3.1 虽起多个 worker 线程，但连接实际都在主线程 loop 上处理（假多线程）；
 *  - 本版本完全使用 SDK 的 cservice_* API，服务器内部为每个 worker 线程创建
 *    独立的 uv_loop + 独立 SO_REUSEPORT 监听 socket，内核把连接分发到不同
 *    loop，实现真正的多核并行（handler 在对应 loop 线程内执行）。
 *
 * 因为多个 loop 线程会并发执行 handler，所有跨请求共享状态（任务表）都用
 * 互斥锁保护，保证线程安全。
 *
 * 编译：make -C service_worker worker_service
 * 运行：./worker_service                      （默认读取同目录 worker_config.xml）
 *   或：./worker_service /path/to/config.xml  （指定 XML 配置文件）
 *
 * 所有运行参数（服务名/监听地址/线程数/网关地址/路径前缀/健康检查路径/
 * 任务表上限）均从 XML 配置文件读取，解析由 SDK 的 q_xml_cfg 模块完成。
 */
#include <cservice.h>
#include <q_xml_cfg.h>   /* SDK: XML 配置读取（config_init / config_get_*） */
#include <cjson/cJSON.h>
#include <pthread.h>
#include <unistd.h>   /* sysconf */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============================ 共享状态（线程安全） ============================ */

typedef struct {
    int   id;
    char  name[64];
    int   priority;
    char  status[16];
} job_t;

/* 任务表上限由配置 service.max_jobs 决定，运行时动态分配 */
static job_t *g_jobs    = NULL;
static int    g_job_cap = 4096;   /* 有效上限（来自配置，兜底 4096） */
static int    g_job_count = 0;   /* 已占用槽位数 */
static int    g_job_seq   = 0;   /* 自增任务 ID */
static pthread_mutex_t g_job_lock  = PTHREAD_MUTEX_INITIALIZER;

/* 静态只读雇员表（不可变，读取无需加锁） */
typedef struct { int id; const char *name; const char *dept; int salary; } emp_t;
static const emp_t g_employees[] = {
    {1001, "张伟",   "研发部", 18000},
    {1002, "李娜",   "市场部", 15000},
    {1003, "王强",   "财务部", 16000},
    {1004, "刘洋",   "研发部", 20000},
    {1005, "陈静",   "人事部", 14000},
};
static const int g_emp_count = (int)(sizeof(g_employees) / sizeof(g_employees[0]));

static int g_threads = 1;   /* 实际 worker loop 数，main 中设置，供 /health 展示 */

/* 在响应里带上处理本请求的 loop 序号，便于观测“真·多线程”分发 */
static void tag_worker_loop(cservice_res_t *res, const cservice_req_t *req)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", cservice_req_loop_id(req));
    cservice_res_header(res, "X-Worker-Loop", buf);
}

/* ============================ Handlers ============================ */

/* GET /health —— 覆盖 SDK 默认健康检查，附线程分发信息 */
static void h_health(cservice_req_t *req, cservice_res_t *res)
{
    tag_worker_loop(res, req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "healthy");
    cJSON_AddStringToObject(root, "service", "worker-service");
    cJSON_AddNumberToObject(root, "worker_loops", g_threads);
    cJSON_AddNumberToObject(root, "served_by_loop", cservice_req_loop_id(req));
    cJSON_AddNumberToObject(root, "jobs_total", g_job_count);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cservice_res_json(res, s);
    free(s);
}

/* GET /api/worker/jobs —— 列出全部任务 */
static void h_jobs_list(cservice_req_t *req, cservice_res_t *res)
{
    tag_worker_loop(res, req);
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_job_lock);
    for (int i = 0; i < g_job_count; i++)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", g_jobs[i].id);
        cJSON_AddStringToObject(o, "name", g_jobs[i].name);
        cJSON_AddNumberToObject(o, "priority", g_jobs[i].priority);
        cJSON_AddStringToObject(o, "status", g_jobs[i].status);
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&g_job_lock);

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    cservice_res_json(res, s);
    free(s);
}

/* POST /api/worker/jobs —— 创建任务（线程安全写入共享表） */
static void h_jobs_create(cservice_req_t *req, cservice_res_t *res)
{
    tag_worker_loop(res, req);
    size_t len = 0;
    const char *body = cservice_req_body(req, &len);

    int   new_id = -1;
    char  name[64] = "unnamed";
    int   priority = 5;
    int   created = 0;

    cJSON *j = body ? cJSON_Parse(body) : NULL;
    if (j)
    {
        cJSON *f = cJSON_GetObjectItem(j, "name");
        if (f && cJSON_IsString(f)) snprintf(name, sizeof(name), "%s", f->valuestring);
        f = cJSON_GetObjectItem(j, "priority");
        if (f && cJSON_IsNumber(f)) priority = f->valueint;
        cJSON_Delete(j);

        pthread_mutex_lock(&g_job_lock);
        if (g_job_count < g_job_cap)
        {
            new_id = ++g_job_seq;
            job_t *jt = &g_jobs[g_job_count++];
            jt->id = new_id;
            snprintf(jt->name, sizeof(jt->name), "%s", name);
            jt->priority = priority;
            snprintf(jt->status, sizeof(jt->status), "queued");
            created = 1;
        }
        pthread_mutex_unlock(&g_job_lock);
    }

    if (!created)
    {
        cservice_res_send(res, 503, "application/json",
                          "{\"error\":\"job queue full or bad request\"}", 39);
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", new_id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "priority", priority);
    cJSON_AddStringToObject(o, "status", "queued");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    cservice_res_json(res, s);
    free(s);
}

/* GET /api/employees?id=1001 或 GET /api/employees —— 雇员查询 */
static void h_employees(cservice_req_t *req, cservice_res_t *res)
{
    tag_worker_loop(res, req);
    const char *idq = cservice_req_query(req, "id");

    cJSON *root = (idq != NULL) ? cJSON_CreateObject() : cJSON_CreateArray();

    if (idq != NULL)
    {
        int target = atoi(idq);
        const emp_t *found = NULL;
        for (int i = 0; i < g_emp_count; i++)
            if (g_employees[i].id == target) { found = &g_employees[i]; break; }

        if (found)
        {
            cJSON_AddNumberToObject(root, "id", found->id);
            cJSON_AddStringToObject(root, "name", found->name);
            cJSON_AddStringToObject(root, "dept", found->dept);
            cJSON_AddNumberToObject(root, "salary", found->salary);
        }
        else
        {
            cservice_res_send(res, 404, "application/json",
                              "{\"error\":\"Employee Not Found\"}", 28);
            cJSON_Delete(root);
            return;
        }
    }
    else
    {
        for (int i = 0; i < g_emp_count; i++)
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", g_employees[i].id);
            cJSON_AddStringToObject(o, "name", g_employees[i].name);
            cJSON_AddStringToObject(o, "dept", g_employees[i].dept);
            cJSON_AddNumberToObject(o, "salary", g_employees[i].salary);
            cJSON_AddItemToArray(root, o);
        }
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cservice_res_json(res, s);
    free(s);
}

/* ============================ main ============================ */

int main(int argc, char *argv[])
{
    /* 启动横幅按行刷新，确保被信号终止时也能看到配置加载结果 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;

    /* ---------- 1. 读取 XML 配置（点分键 -> XPath） ---------- */
    const char *cfg_file = (argc > 1) ? argv[1] : "worker_config.xml";
    config_ctx_t *cfg = config_init(cfg_file);
    if (!cfg)
    {
        fprintf(stderr, "[worker] 无法加载 XML 配置文件：%s\n", cfg_file);
        return 1;
    }

    /* 服务参数 */
    char *svc_name = config_get_string(cfg, "service.name", "worker-service");
    char *svc_host = config_get_string(cfg, "service.host", "0.0.0.0");
    int   svc_port = config_get_int(cfg, "service.port", 8082);
    int   cfg_threads = config_get_int(cfg, "service.threads", 0); /* 0 = 自动按 CPU 核数 */
    char *svc_prefix = config_get_string(cfg, "service.path_prefix", "/api/worker");
    char *svc_health = config_get_string(cfg, "service.health_path", "/api/worker/health");

    /* 任务表上限（来自 service.max_jobs，越界则兜底 4096） */
    g_job_cap = config_get_int(cfg, "service.max_jobs", 4096);
    if (g_job_cap < 1 || g_job_cap > 1048576) g_job_cap = 4096;

    /* 网关参数（位于 service.gateway 下） */
    char *gw_host = config_get_string(cfg, "service.gateway.host", "127.0.0.1");
    int   gw_port = config_get_int(cfg, "service.gateway.port", 8080);

    /* 路由路径（来自 service.routes.*；处理函数指针固定在代码派发表，不可配置）。
       health 路由默认与 health_path 一致，确保网关探针（方案 B）路径对齐。 */
    char *rt_health = config_get_string(cfg, "service.routes.health", svc_health);
    char *rt_jobs   = config_get_string(cfg, "service.routes.jobs", "/api/worker/jobs");
    char *rt_emp    = config_get_string(cfg, "service.routes.employees", "/api/worker/employees");

    /* 实际 worker loop 数：配置显式 >0 用配置，否则按 CPU 核数 */
    int threads = (cfg_threads > 0) ? cfg_threads : cores;
    g_threads = threads;

    /* 动态分配任务表 */
    g_jobs = calloc((size_t)g_job_cap, sizeof(job_t));
    if (!g_jobs)
    {
        fprintf(stderr, "[worker] 任务表分配失败 (max_jobs=%d)\n", g_job_cap);
        config_destroy(cfg);
        return 1;
    }

    /* ---------- 2. 初始化 SDK 服务（参数全部来自配置） ---------- */
    cservice_t *svc = cservice_init(svc_name, svc_host, svc_port);

    /* 按配置/CPU 核数开启 worker loop（SDK 内部 SO_REUSEPORT 多 loop = 真·多线程） */
    cservice_set_threads(svc, threads);

    /* 自动注册到网关，仅承接配置前缀的流量 */
    cservice_set_gateway(svc, gw_host, gw_port);
    cservice_set_path_prefix(svc, svc_prefix);
    /* 方案 B：健康检查路径也统一到前缀下，网关探针由上面注册的 h_health 应答。
       （SDK 默认健康检查处理器只在“无路由匹配且 path==health_path”时触发，
         此处路由已覆盖，不会重复处理。） */
    cservice_set_health_path(svc, svc_health);

    /* 路由注册：路径来自配置，method 与 handler 固定（API 契约不可配置）。
       SDK 内部会对 path 做 strdup，注册后即可释放本地副本。 */
    CSERVICE_ROUTE(svc, CSERVICE_GET,  rt_health, h_health);
    CSERVICE_ROUTE(svc, CSERVICE_GET,  rt_jobs,   h_jobs_list);
    CSERVICE_ROUTE(svc, CSERVICE_POST, rt_jobs,   h_jobs_create);
    CSERVICE_ROUTE(svc, CSERVICE_GET,  rt_emp,    h_employees);

    printf("=== 多线程工作服务 v4 (libcservice SDK, 真·多线程 SO_REUSEPORT) ===\n");
    printf("配置文件: %s\n", cfg_file);
    printf("服务名: %s  监听: %s:%d\n", svc_name, svc_host, svc_port);
    printf("worker loops = %d (%s)\n",
           threads, (cfg_threads > 0) ? "配置文件指定" : "自动按 CPU 核数");
    printf("任务表上限 = %d\n", g_job_cap);
    printf("注册网关: %s:%d  路径前缀: %s  健康检查: %s\n",
           gw_host, gw_port, svc_prefix, svc_health);
    printf("路由: GET %s/health  GET %s/jobs  POST %s/jobs  GET %s/employees\n",
           svc_prefix, svc_prefix, svc_prefix, svc_prefix);

    /* 释放配置读取产生的字符串副本 */
    free(svc_name); free(svc_host); free(svc_prefix);
    free(svc_health); free(gw_host);
    free(rt_health); free(rt_jobs); free(rt_emp);

    /* ---------- 3. 启动（阻塞直到 SIGINT/SIGTERM，SDK 已修复唤醒，可优雅退出） ---------- */
    cservice_run(svc);
    cservice_destroy(svc);
    config_destroy(cfg);
    free(g_jobs);
    printf("工作服务已退出\n");
    return 0;
}
