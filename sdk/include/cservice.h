/**
 * @file cservice.h
 * @brief libcservice —— C 语言微服务开发 SDK（对标 Spring Boot 内嵌服务）
 *
 * 设计目标：
 *  - 业务开发者只需“声明路由 + 写 handler”，无需接触 libuv / llhttp / socket。
 *  - 内嵌高性能 HTTP 服务器（libuv 事件驱动 + llhttp 解析 + Keep-Alive）。
 *  - 内置服务自注册/心跳到本仓库的网关（c_gateway），形成微服务闭环。
 *  - 可编译为独立静态库 libcservice.a 与动态库 libcservice.so。
 *
 * 典型用法：
 * @code
 *   cservice_t *svc = cservice_init("order-service", "0.0.0.0", 8081);
 *   cservice_route(svc, CSERVICE_GET,  "/api/orders", orders_list);
 *   cservice_route(svc, CSERVICE_POST, "/api/orders", orders_create);
 *   cservice_set_gateway(svc, "127.0.0.1", 8080);
 *   cservice_run(svc);                 // 阻塞运行
 *   cservice_destroy(svc);
 * @endcode
 *
 * @author 乔水
 * @date 2026-09-22
 */

#ifndef CSERVICE_H
#define CSERVICE_H

#include <stddef.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 类型 ============================ */

typedef enum {
    CSERVICE_GET    = 1 << 0,
    CSERVICE_POST   = 1 << 1,
    CSERVICE_PUT    = 1 << 2,
    CSERVICE_DELETE = 1 << 3,
    CSERVICE_HEAD   = 1 << 4,
    CSERVICE_PATCH  = 1 << 5
} cservice_method_t;

/* 不透明句柄 */
typedef struct cservice      cservice_t;
typedef struct cservice_req  cservice_req_t;
typedef struct cservice_res  cservice_res_t;

/* 请求处理器：在服务器事件循环线程内调用。
 *  - 禁止在 handler 内做阻塞操作（如同步 DB 查询、sleep）。
 *  - 若需异步（转发后端、查 DB 等），调用 cservice_res_defer() 标记延迟响应，
 *    在异步回调（必须运行于事件循环线程）中回写响应并调用 cservice_res_finish()。 */
typedef void (*cservice_handler_t)(cservice_req_t *req, cservice_res_t *res);

/* ============================ 生命周期 ============================ */

/**
 * 创建微服务实例。
 * @param name 服务名（用于注册到网关）
 * @param host 监听地址，如 "0.0.0.0"
 * @param port 监听端口
 * @return 实例指针，失败返回 NULL
 */
cservice_t *cservice_init(const char *name, const char *host, int port);

/** 设置工作线程数（多 loop + SO_REUSEPORT，建议 <= CPU 核数） */
void cservice_set_threads(cservice_t *svc, int n);

/** 自定义健康检查路径（默认 "/health"） */
void cservice_set_health_path(cservice_t *svc, const char *path);

/** 设置网关地址，用于自动注册 */
void cservice_set_gateway(cservice_t *svc, const char *host, int port);

/** 设置注册到网关的路径前缀（如 "/api/orders"），默认 ""（兜底匹配） */
void cservice_set_path_prefix(cservice_t *svc, const char *prefix);

/** 是否启用向网关注册（默认开启；调用 set_gateway 后才生效） */
void cservice_enable_gateway_register(cservice_t *svc, int enable);

/** 设置配置文件路径（可选，覆盖 name/host/port 等） */
void cservice_set_config_file(cservice_t *svc, const char *path);

/** 设置日志输出目录（默认 "logs"），需在 cservice_run 前调用 */
void cservice_set_log_dir(cservice_t *svc, const char *dir);

/**
 * 设置日志级别（0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=FATAL），默认 INFO。
 * 需在 cservice_run 前调用。
 */
void cservice_set_log_level(cservice_t *svc, int level);

/**
 * 运行服务（阻塞，直到收到 SIGINT/SIGTERM 或调用 cservice_stop）。
 * 内部完成：加载配置、启动服务器、向网关注册、启动心跳。
 * @return 0 正常退出，<0 启动失败
 */
int cservice_run(cservice_t *svc);

/** 优雅停止（可在信号处理器/其他线程调用） */
void cservice_stop(cservice_t *svc);

/** 释放资源（run 返回后调用） */
void cservice_destroy(cservice_t *svc);

/* ============================ 路由 ============================ */

/**
 * 注册路由。
 * @param m     允许的 HTTP 方法（可或运算，如 CSERVICE_GET|CSERVICE_POST）
 * @param path  路径；以 '/' 结尾表示“前缀匹配”，否则精确匹配
 * @param h     处理器
 * @return 0 成功，-1 失败（如重复）
 */
int cservice_route(cservice_t *svc,
                   cservice_method_t m,
                   const char *path,
                   cservice_handler_t h);

/* 便捷宏：需传入 svc 实例 */
#define CSERVICE_ROUTE(svc, M, p, h)  cservice_route((svc), (M), (p), (h))

/* ============================ 请求 API ============================ */

/** 请求路径（不含 query） */
const char *cservice_req_path(cservice_req_t *req);

/** 请求方法字符串，如 "GET" */
const char *cservice_req_method(cservice_req_t *req);

/** 取请求头（大小写不敏感），不存在返回 NULL */
const char *cservice_req_header(cservice_req_t *req, const char *name);

/** 取查询参数（?key=val），不存在返回 NULL */
const char *cservice_req_query(cservice_req_t *req, const char *name);

/** 请求体指针；len 可为 NULL。返回 NULL 表示无 body */
const char *cservice_req_body(cservice_req_t *req, size_t *len);

/** 返回处理本请求的 loop/线程序号（0..thread_count-1）。
 *  用于在 handler 中观测“真·多线程”连接分发（每个 worker loop 独立接连接）。 */
int cservice_req_loop_id(const cservice_req_t *req);

/** 返回处理本请求的事件循环（uv_loop_t*）。
 *  异步操作（异步出站客户端、uv_queue_work 等）必须绑定到这个 loop。 */
uv_loop_t *cservice_req_loop(cservice_req_t *req);

/* ============================ 响应 API ============================ */

/** 设置 HTTP 状态码（默认 200） */
void cservice_res_status(cservice_res_t *res, int code);

/** 设置/追加响应头 */
void cservice_res_header(cservice_res_t *res, const char *name, const char *value);

/** 以 JSON 文本响应（内部拷贝，调用后可释放/复用传入指针） */
void cservice_res_json(cservice_res_t *res, const char *json);

/** 以任意文本内容响应 */
void cservice_res_text(cservice_res_t *res, const char *text, const char *content_type);

/** 直接发送（body 内部拷贝） */
void cservice_res_send(cservice_res_t *res, int code,
                       const char *content_type, const char *body, size_t len);

/** printf 风格构造文本响应 */
void cservice_res_printf(cservice_res_t *res, int code,
                         const char *content_type, const char *fmt, ...);

/* ============================ 异步 / 延迟响应 ============================ */

/** 标记本响应为“延迟响应”：handler 返回后框架不会自动发送，也不会回收连接，
 *  需由后续异步回调（必须运行于事件循环线程）调用 cservice_res_finish() 真正发送。 */
void cservice_res_defer(cservice_res_t *res);

/** 完成延迟响应（须在事件循环线程调用）：发送此前已设置好的响应。
 *  若客户端在响应在途期间已断开，则直接安全回收连接，不会崩溃。 */
void cservice_res_finish(cservice_res_t *res);

#ifdef __cplusplus
}
#endif

#endif /* CSERVICE_H */
