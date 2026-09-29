/**
 * @file vr_question_service.c
 * @brief VR 献血题库服务：读取 MySQL `vr_question` 表，生成 JSON 返回给前台
 *
 * 依赖 SDK 模块：
 *   - cservice：HTTP 服务框架（路由 + 延迟响应）
 *   - db：MySQL 连接池（q_dbp_new / q_db_register_mysql）
 *   - mapper：XML SQL Mapper，q_ctx_query 直接返回 jansson JSON 数组（列->值）
 *   - async：把阻塞式 DB 查询 offload 到 worker 线程，不卡事件循环
 *
 * 关键约定：
 *   1. handler 内只取请求参数，调用 cservice_res_defer() 后 async_exec() 丢线程池。
 *   2. 阻塞的 q_ctx_query 在 worker 线程安全执行，done 回调（loop 线程）里回写 JSON。
 *   3. answer_text 为空时回退 standard_answer（业务规则，见建表注释）。
 *
 * 配置文件（XML，基于 SDK 的 q_xml_cfg 模块，点分键读取）：
 *   默认 ./vr_question_service.xml，可用 VR_CONFIG 环境变量或命令行参数覆盖。
 *   包含：服务名/监听地址/端口、MySQL（host/port/user/password/database/...）、
 *        将要加载的 mapper 文件（或目录）、网关注册信息。详见同名 xml。
 *
 * 编译：make -C services
 * 运行：./services/vr_question_service [config.xml]
 *
 * 接口：
 *   GET /api/vr-questions          通用查询（role_code/category/status/is_current/city_name 可选）
 *   GET /api/vr-questions/current 设备/数字人拉取当前题目（必填 role_code，可选 city_name）
 *   GET /api/vr-questions/code     按 question_code 取单条（必填 question_code）
 */
#include <cservice.h>
#include <mapper.h>   /* 内含 db.h + jansson.h：q_ctx_* / q_dbp_* / q_db_register_mysql */
#include <async.h>
#include <q_xml_cfg.h>/* SDK 配置模块：config_init / config_get_* */
#include <sm4.h>      /* SDK SM4-CBC 加解密：q_sm4_init / q_sm4_decrypt_str */

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 全局：连接池 + mapper（main 初始化，worker 只读共享） ---------------- */
static q_dbp_t    *g_pool   = NULL;
static q_mapper_t *g_mapper = NULL;

/* 请求上下文：在 work / done 之间传递 */
typedef struct 
{
    cservice_res_t *res;
    const char     *mapper_id;  /* 如 "VrQuestion.selectList" */
    json_t         *params;     /* work 消费，done 释放 */
    char           *result;     /* work 产出 JSON 文本，done 消费后 free */
} biz_job_t;

/* ---- 阻塞式业务逻辑（worker 线程，可安全调用同步 DB API） ---- */
static void biz_work(void *data)
{
    biz_job_t *j = data;
    char       err[256];

    q_ctx_t *ctx = q_ctx_new(g_mapper, g_pool);
    if (ctx == NULL) 
    {
        j->result = strdup("{\"code\":500,\"message\":\"out of memory\"}");
        return;
    }

    json_t *rows = NULL;
    int rc = q_ctx_query(ctx, j->mapper_id, j->params, &rows, err, sizeof(err));
    q_ctx_free(ctx);

    if (rc != Q_OK) 
    {
        size_t need = (size_t)snprintf(NULL, 0, "{\"code\":500,\"message\":\"db error: %s\"}", err) + 1;
        char  *s = malloc(need);
        if (s != NULL)
            snprintf(s, need, "{\"code\":500,\"message\":\"db error: %s\"}", err);
        j->result = s;
        return;
    }

    /* 业务兜底：answer_text 为空时回退 standard_answer（下发给数字人/VR 设备用） */
    if (json_is_array(rows)) 
    {
        size_t  i;
        json_t *row;
        json_array_foreach(rows, i, row) 
        {
            json_t *ans = json_object_get(row, "answer_text");
            if (ans == NULL || json_is_null(ans) ||
                (json_is_string(ans) && json_string_value(ans)[0] == '\0')) 
            {
                json_t *std = json_object_get(row, "standard_answer");
                if (std != NULL && !json_is_null(std))
                    json_object_set(row, "answer_text", std);  /* 拷贝引用 */
            }
        }
    }

    /* 包装成前台友好的信封 {code, message, count, data} */
    size_t n = rows ? json_array_size(rows) : 0;
    json_t *env = json_object();
    int rt_code = (n > 0 ) ? 200:0;
    json_object_set_new(env, "code",    json_integer(rt_code));
    json_object_set_new(env, "message", json_string("success"));
    json_object_set_new(env, "count",   json_integer((int)n));
    json_object_set_new(env, "data",    rows);   /* 转移所有权，json_decref(env) 会一起释放 */

    char *out = json_dumps(env, JSON_COMPACT);
    json_decref(env);
    j->result = (out != NULL) ? out : strdup("{\"code\":500,\"message\":\"serialize failed\"}");
}

/* ---- 完成回调（loop 线程，可安全操作响应） ---- */
static void biz_done(void *data)
{
    biz_job_t *j = data;
    if (j->result)
        cservice_res_json(j->res, j->result);
    else
        cservice_res_send(j->res, 500, "application/json",
                          "{\"code\":500,\"message\":\"internal\"}", 33);
    cservice_res_finish(j->res);   /* 真正发送 */

    if (j->result) free(j->result);
    if (j->params) json_decref(j->params);
    free(j);
}

/* 统一派发：延迟响应 + 线程池执行 */
static void dispatch(cservice_req_t *req, cservice_res_t *res,
                     const char *mapper_id, json_t *params)
{
    cservice_res_defer(res);
    uv_loop_t *loop = cservice_req_loop(req);
    biz_job_t *j = calloc(1, sizeof(*j));
    j->res       = res;
    j->mapper_id = mapper_id;
    j->params    = params;
    async_exec(loop, biz_work, biz_done, j);
}

/* 取可选查询参数字符串（空串当不存在） */
static const char *qstr(cservice_req_t *req, const char *name)
{
    const char *v = cservice_req_query(req, name);
    return (v != NULL && *v != '\0') ? v : NULL;
}

/* GET /api/vr-questions —— 通用条件查询 */
static void questions_list(cservice_req_t *req, cservice_res_t *res)
{
    json_t *params = json_object();

    const char *role = qstr(req, "role_code");
    if (role) json_object_set_new(params, "role_code", json_string(role));

    const char *cat = qstr(req, "category");
    if (cat) json_object_set_new(params, "category", json_string(cat));

    /* status 默认启用(1)，前端可覆盖(含 0=禁用) */
    const char *st = qstr(req, "status");
    json_object_set_new(params, "status", json_integer(st ? atoi(st) : 1));

    /* is_current 不默认，按需过滤 */
    const char *cur = qstr(req, "is_current");
    if (cur) json_object_set_new(params, "is_current", json_integer(atoi(cur)));

    const char *city = qstr(req, "city_name");
    if (city) json_object_set_new(params, "city_name", json_string(city));

    dispatch(req, res, "VrQuestion.selectList", params);
}

/* GET /api/vr-questions/current —— 设备/数字人拉取当前题目 */
static void questions_current(cservice_req_t *req, cservice_res_t *res)
{
    const char *role = qstr(req, "role_code");
    if (role == NULL) 
    {
        cservice_res_send(res, 400, "application/json",
                          "{\"code\":400,\"message\":\"role_code required\"}", 39);
        return;
    }
    json_t *params = json_object();
    json_object_set_new(params, "role_code", json_string(role));
    const char *city = qstr(req, "city_name");
    if (city) json_object_set_new(params, "city_name", json_string(city));

    dispatch(req, res, "VrQuestion.selectCurrentByRole", params);
}

/* GET /api/vr-questions/code —— 按 question_code 取单条 */
static void questions_by_code(cservice_req_t *req, cservice_res_t *res)
{
    const char *code = qstr(req, "question_code");
    if (code == NULL) 
    {
        cservice_res_send(res, 400, "application/json",
                          "{\"code\":400,\"message\":\"question_code required\"}", 42);
        return;
    }
    json_t *params = json_object();
    json_object_set_new(params, "question_code", json_string(code));

    dispatch(req, res, "VrQuestion.selectByCode", params);
}

int main(int argc, char **argv)
{
    /* 日志目录 / 级别改由 XML 配置（service.log.dir / service.log.level）驱动，
     * 待配置文件加载后（见下方 config_init 之后）再初始化，以读取 XML 中的值 */

    /* 1) 定位配置文件：命令行 > 环境变量 VR_CONFIG > 默认 ./vr_question_service.xml */
    const char *cfg_path = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
                           ? argv[1]
                           : (getenv("VR_CONFIG") ? getenv("VR_CONFIG") : "./vr_question_service.xml");

    config_ctx_t *cfg = config_init(cfg_path);
    if (cfg == NULL) 
    {
        fprintf(stderr, "[vr-question] 读取配置文件失败: %s\n", cfg_path);
        return 1;
    }

    /* 日志目录 / 级别从 XML 读取（兼容旧配置：缺省 logs / INFO） */
    char *log_dir = config_get_string(cfg, "service.log.dir", "logs");
    char *log_level_str = config_get_string(cfg, "service.log.level", "INFO");
    q_log_init(log_dir, "vr-question-service", (q_log_level_t)q_log_level_from_str(log_level_str));
    free(log_dir);
    free(log_level_str);

    /* 初始化 SM4（密钥/IV 取自环境变量 SM4_KEY / SM4_IV）；未设置则解密不可用 */
    q_sm4_init();

    /* 2) 服务基本信息 */
    char *svc_name = config_get_string(cfg, "service.name", "vr-question-service");
    char *svc_host = config_get_string(cfg, "service.host", "0.0.0.0");
    int   svc_port = config_get_int(cfg, "service.port", 8090);
    int   svc_threads = config_get_int(cfg, "service.threads", 1);   /* worker 线程数（多 event-loop 模型）*/

    /* 3) MySQL 连接信息（用户名 / 密码 / ip / port / 库名，分别配置） */
    char *db_host = config_get_string(cfg, "service.mysql.host", "127.0.0.1");
    int   db_port = config_get_int(cfg, "service.mysql.port", 3306);
    char *db_user = config_get_string(cfg, "service.mysql.user", "root");
    char *db_pass = config_get_string(cfg, "service.mysql.password", "");
    char *db_name = config_get_string(cfg, "service.mysql.database", "plasma");
    char *db_charset = config_get_string(cfg, "service.mysql.charset", "utf8mb4");
    int   db_max_open = config_get_int(cfg, "service.mysql.max_open", 8);
    int   db_idle     = config_get_int(cfg, "service.mysql.idle_secs", 60);
    int   db_nshards  = config_get_int(cfg, "service.mysql.nshards", 4);

    /* 若设置了 SM4 密钥/IV，则把配置里的（十六进制）密文密码解密为明文再连库 */
    char db_pass_plain[256];
    const char *real_pass = db_pass;
    if (q_sm4_ready() && db_pass != NULL && *db_pass != '\0') 
    {
        char serr[256];
        if (q_sm4_decrypt_str(db_pass, db_pass_plain, sizeof(db_pass_plain), serr, sizeof(serr)) == Q_OK)
            real_pass = db_pass_plain;
        else
            q_warn("sm4 解密数据库密码失败(%s)，将按明文使用", serr);
    }
    

    /* 拼成连接池 URL：mysql://user:pass@host:port/db */
    char dsn[512];
    snprintf(dsn, sizeof(dsn), "mysql://%s:%s@%s:%d/%s",
             db_user, real_pass, db_host, db_port, db_name);
    printf("dsn: %s\n", dsn);
    /* 4) 将要读取的 mapper 文件：优先多个 <file>，否则退回单个 <path>（文件或目录） */
    char **mfiles = NULL;
    int   nmfiles = config_get_string_list(cfg, "service.mapper.file", &mfiles);
    if (nmfiles > 0 && mfiles != NULL) 
    {
        g_mapper = q_mapper_load_files((const char * const *)mfiles, nmfiles);
        for (int i = 0; i < nmfiles; i++) free(mfiles[i]);
        free(mfiles);
        if (g_mapper != NULL)
            q_info("vr-question: mapper 已加载（多文件）%d 条语句", q_mapper_size(g_mapper));
    } 
    else 
    {
        char *mapper_path = config_get_string(cfg, "service.mapper.path", "./mapper");
        g_mapper = q_mapper_load(mapper_path);
        q_info("vr-question: mapper 已加载 %d 条语句，来自 %s",
               g_mapper ? q_mapper_size(g_mapper) : 0, mapper_path);
        free(mapper_path);
    }

    /* 5) 注册 MySQL 驱动（driver 在 libcservice 构建期编入） */
    if (q_db_register_mysql() != Q_OK) 
    {
        fprintf(stderr, "[vr-question] 注册 mysql 驱动失败\n");
        config_destroy(cfg);
        return 1;
    }

    g_pool = q_dbp_new(dsn, db_max_open, db_idle, db_nshards);
    if (g_pool == NULL) 
    {
        fprintf(stderr, "[vr-question] 创建连接池失败: %s\n", dsn);
        config_destroy(cfg);
        return 1;
    }

    if (g_mapper == NULL || q_mapper_size(g_mapper) == 0) 
    {
        fprintf(stderr, "[vr-question] 加载 mapper 失败（无可用语句）\n");
        config_destroy(cfg);
        return 1;
    }

    /* 6) 启动 HTTP 服务 */
    cservice_t *svc = cservice_init(svc_name, svc_host, svc_port);
    cservice_set_threads(svc, svc_threads);   /* 必须在 cservice_run 之前设置 worker 线程数 */

    if (config_get_bool(cfg, "service.gateway.enable", true)) 
    {
        char *gw_host = config_get_string(cfg, "service.gateway.host", "127.0.0.1");
        int   gw_port = config_get_int(cfg, "service.gateway.port", 8080);
        cservice_set_gateway(svc, gw_host, gw_port);          /* 自动注册到网关 */
        char *prefix = config_get_string(cfg, "service.gateway.path_prefix", "/api/vr-questions");
        cservice_set_path_prefix(svc, prefix);
        free(gw_host);
        free(prefix);
    }

    CSERVICE_ROUTE(svc, CSERVICE_GET, "/api/vr-questions",           questions_list);
    CSERVICE_ROUTE(svc, CSERVICE_GET, "/api/vr-questions/current",   questions_current);
    CSERVICE_ROUTE(svc, CSERVICE_GET, "/api/vr-questions/code",      questions_by_code);

    cservice_run(svc);
    cservice_destroy(svc);

    q_mapper_free(g_mapper);
    q_dbp_free(g_pool);

    /* 释放配置与字符串 */
    free(svc_name); free(svc_host);
    free(db_host); free(db_user); free(db_pass); free(db_name); free(db_charset);
    config_destroy(cfg);
    return 0;
}
