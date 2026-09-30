/**
 * @file proxy_service.c
 * @brief libcservice 示例：异步反向代理网关
 *
 * 演示“网关全程异步”：
 *  前端请求 → 记录（此处仅关联 res） → 异步转发后端（curl multi + libuv）
 *  → handler 立即返回，loop 继续收新请求 → 后端回包（loop 线程回调）
 *  → 按关联找到前端连接 → cservice_res_finish 写回前端。
 *
 * 后端地址通过环境变量 BACKEND_BASE 指定，默认 http://127.0.0.1:9000
 *
 * 编译：make -C sdk examples
 * 运行：BACKEND_BASE=http://127.0.0.1:9000 ./sdk/examples/proxy_service
 */
#include <cservice.h>
#include <http_client.h>
#include <stdlib.h>
#include <string.h>

#ifndef BACKEND_BASE
#define BACKEND_BASE "http://127.0.0.1:9000"
#endif

typedef struct
{
  cservice_res_t *res; /* 前端响应：用于延迟回写 */
} proxy_ctx_t;

/* 后端回包回调：运行在事件循环线程，可直接回写前端 */
static void on_backend(http_resp_t *r, void *ud)
{
  proxy_ctx_t *p = ud;
  if (r->code == 0 && r->status > 0)
    cservice_res_send(p->res, r->status, r->content_type, r->body, r->body_len);
  else
    cservice_res_send(p->res, 502, "application/json", "{\"error\":\"bad gateway\"}", 21);
  cservice_res_finish(p->res); /* 真正发送，loop 线程 */
  free(p);
}

static void proxy_handler(cservice_req_t *req, cservice_res_t *res)
{
  /* 1) 标记延迟响应：handler 返回后框架不会自动发送，也不回收连接 */
  cservice_res_defer(res);

  /* 2) 取本请求所属 loop，发起异步出站请求（立即返回，不阻塞 loop） */
  uv_loop_t *loop = cservice_req_loop(req);
  http_client_init(loop);

  /* 3) 拼后端 URL（保留原始 path/query） */
  const char *path = cservice_req_path(req);
  char url[2048];
  snprintf(url, sizeof(url), "%s%s", BACKEND_BASE, path);

  size_t blen = 0;
  const char *body = cservice_req_body(req, &blen);

  http_req_t hr;
  memset(&hr, 0, sizeof hr);
  hr.method = cservice_req_method(req);
  hr.url = url;
  hr.body = body;
  hr.body_len = blen;

  proxy_ctx_t *p = malloc(sizeof *p);
  p->res = res;

  /* 4) 异步转发；后端回包时 on_backend 在 loop 线程被调用 */
  http_client_do(loop, &hr, on_backend, p);
}

int main(void)
{
  cservice_t *svc = cservice_init("proxy-gw", "0.0.0.0", 8098);
  cservice_enable_gateway_register(svc, 0); /* 演示环境无网关注册，关闭 */

  /* 前缀路由 "/"：承接所有路径 */
  CSERVICE_ROUTE(svc, CSERVICE_GET | CSERVICE_POST | CSERVICE_PUT | CSERVICE_DELETE | CSERVICE_HEAD, "/",
                 proxy_handler);

  cservice_run(svc); /* 阻塞运行 */
  cservice_destroy(svc);
  return 0;
}
