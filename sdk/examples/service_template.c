/**
 * @file service_template.c
 * @brief libcservice 业务服务模板（新程序员套用此模板写业务逻辑）
 *
 * 关键点（异步不阻塞事件循环）：
 *   1. handler 里调用 cservice_res_defer() 标记“延迟响应”。
 *   2. 用 async_exec() 把【阻塞式业务逻辑】（如 db/mapper 的 q_ctx_query）
 *      丢到 worker 线程执行，handler 立刻返回，loop 继续接客。
 *   3. worker 线程完成（done 回调，运行在 loop 线程）后，回写响应并
 *      cservice_res_finish()，真正发送给前端。
 *
 * 真实 DB 用法（替换 db_work 中的占位实现）：
 *   q_dbp_t *pool = q_dbp_new("mock://...", 4, 60, 0);  // 或 "mysql://..."
 *   q_ctx_t *ctx  = ...;                                 // 由 mapper 解析 SQL
 *   q_result_t *r = NULL;
 *   q_ctx_query(conn, ctx, NULL, &r);                   // 阻塞，在 worker 线程安全
 *   // 把 r 序列化成 JSON 存入 job->result，done 里 cservice_res_json 发出
 *
 * 编译：make -C sdk examples
 * 运行：./sdk/examples/service_template
 */
#include <cservice.h>
#include <async.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

/* 业务请求上下文：在 work/done 之间传递 */
typedef struct {
    cservice_res_t *res;
    char           *result;   /* work 产出，done 消费后释放 */
} biz_job_t;

/* ---- 阻塞式业务逻辑（在 worker 线程执行，可安全调用同步 DB API） ---- */
static void biz_work(void *data)
{
    biz_job_t *j = data;

    /* 占位：模拟一次 200ms 的 DB 查询。真实场景下替换为 q_ctx_query(...) 等。 */
    usleep(200000);
    char *s = malloc(64);
    snprintf(s, 64, "{\"status\":\"ok\",\"from\":\"db\",\"ts\":%ld}", (long)time(NULL));
    j->result = s;
}

/* ---- 完成回调（在事件循环线程执行，可安全操作响应） ---- */
static void biz_done(void *data)
{
    biz_job_t *j = data;
    if (j->result)
        cservice_res_json(j->res, j->result);
    else
        cservice_res_send(j->res, 500, "application/json",
                          "{\"error\":\"internal\"}", 18);
    cservice_res_finish(j->res);   /* 真正发送 */
    free(j->result);
    free(j);
}

static void biz_handler(cservice_req_t *req, cservice_res_t *res)
{
    (void)req;
    cservice_res_defer(res);                       /* 1) 延迟响应 */

    uv_loop_t *loop = cservice_req_loop(req);
    biz_job_t *j = calloc(1, sizeof(*j));
    j->res = res;

    async_exec(loop, biz_work, biz_done, j);       /* 2) 阻塞逻辑 offload 到线程池 */
}

int main(void)
{
    cservice_t *svc = cservice_init("biz-service", "0.0.0.0", 8097);
    cservice_enable_gateway_register(svc, 0);      /* 演示环境无需网关注册 */

    CSERVICE_ROUTE(svc, CSERVICE_GET, "/biz", biz_handler);

    cservice_run(svc);
    cservice_destroy(svc);
    return 0;
}
