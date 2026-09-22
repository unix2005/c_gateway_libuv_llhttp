/**
 * @file order_service.c
 * @brief libcservice 示例：订单微服务
 *
 * 演示“类 Spring Boot”的开发体验：
 *  - 仅声明路由 + 写 handler，无需接触网络细节
 *  - 自动注册到网关，支持网关转发与负载均衡
 *
 * 编译：make -C sdk examples
 * 运行：./sdk/examples/order_service
 */
#include <cservice.h>
#include <string.h>

/* GET /api/orders?id=xxx */
static void orders_list(cservice_req_t *req, cservice_res_t *res)
{
    const char *id = cservice_req_query(req, "id");
    cservice_res_printf(res, 200, "application/json",
                        "{\"service\":\"order-service\",\"id\":\"%s\"}",
                        id ? id : "none");
}

/* POST /api/orders */
static void orders_create(cservice_req_t *req, cservice_res_t *res)
{
    size_t len = 0;
    const char *body = cservice_req_body(req, &len);
    int sample = (int)(len > 20 ? 20 : len);
    cservice_res_printf(res, 201, "application/json",
                        "{\"created\":true,\"received_bytes\":%zu,\"sample\":\"%.*s\"}",
                        len, sample, body ? body : "");
}

int main(void)
{
    cservice_t *svc = cservice_init("order-service", "0.0.0.0", 8081);

    /* 自动注册到网关（同机 127.0.0.1:8080），并仅承接 /api/orders 前缀 */
    cservice_set_gateway(svc, "127.0.0.1", 8080);
    cservice_set_path_prefix(svc, "/api/orders");

    CSERVICE_ROUTE(svc, CSERVICE_GET,  "/api/orders", orders_list);
    CSERVICE_ROUTE(svc, CSERVICE_POST, "/api/orders", orders_create);

    cservice_run(svc);     /* 阻塞运行，直到 SIGINT/SIGTERM */
    cservice_destroy(svc);
    return 0;
}
