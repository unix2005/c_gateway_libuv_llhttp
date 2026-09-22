/**
 * @file async_http.h
 * @brief 异步上游 HTTP 转发模块（基于 libcurl multi + libuv uv_poll）
 *
 * 设计要点：
 *  - 完全事件驱动，绝不阻塞 libuv 事件循环（修复原 proxy.c 中
 *    curl_easy_perform 同步阻塞事件循环的问题）。
 *  - 每个 uv_loop 绑定一个独立的 CURLM*，所有操作都在该 loop 所属线程内完成，
 *    无跨线程共享状态，无需加锁。
 *  - 通过 uv_poll 把 libcurl 的 socket 事件接入 libuv，通过 uv_timer 接入超时。
 *  - 请求完成时通过回调（与 loop 同线程）把响应交还业务层。
 *  - 业务层在客户端断开时必须调用 async_http_cancel_by_userdata 取消在途请求，
 *    避免向已释放的 client_ctx 写响应（典型 use-after-free）。
 *
 * 本模块对外隐藏所有 libcurl / libuv 细节，符合模块化边界。
 *
 * @author 乔水
 * @date 2026-09-22
 */

#ifndef ASYNC_HTTP_H
#define ASYNC_HTTP_H

/* 启用 GNU 扩展，保证在 -std=c99 下 pthread_rwlock_t 等类型可见 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <uv.h>

/* 上游响应完成回调。
 * @param req       本次请求句柄（由模块内部拥有，回调内不可释放）
 * @param status_code  HTTP 状态码（如 200 / 503），连接失败时为 0
 * @param body       响应体指针（模块内部分配，回调内可读取，返回后即被模块释放）
 * @param body_len   响应体长度
 * @param user_data  提交时传入的业务上下文（通常为 client_ctx_t*）
 */
typedef void (*async_http_done_cb)(void *req,
                                    int status_code,
                                    const char *body,
                                    size_t body_len,
                                    void *user_data);

/* 提交一次上游转发请求。
 * @param loop        发起请求所在的事件循环（必须是调用线程拥有的 loop）
 * @param method      "GET"/"POST"/"PUT"/"DELETE"/"HEAD" 等
 * @param url         完整上游 URL
 * @param headers     NULL 结尾的 "Name: Value" 字符串数组，可为 NULL
 * @param body        请求体（函数内会拷贝，调用方可立即释放/复用），可为 NULL
 * @param body_len    请求体长度
 * @param timeout_ms  整体超时（毫秒）
 * @param cb          完成回调
 * @param user_data   透传业务上下文
 * @return 0 成功，<0 失败（失败时不会触发回调）
 */
int async_http_submit(uv_loop_t *loop,
                      const char *method,
                      const char *url,
                      const char *headers[],
                      const char *body,
                      size_t body_len,
                      long timeout_ms,
                      async_http_done_cb cb,
                      void *user_data);

/* 取消某个 user_data 关联的所有在途请求（客户端断开时调用）。
 * 取消后不会触发回调，关联资源立即释放。幂等、可重复调用。
 */
void async_http_cancel_by_userdata(uv_loop_t *loop, void *user_data);

/* 进程级 libcurl 初始化/清理（只需调用一次）。
 * 注意：调用方需保证在创建任何 loop / 提交请求之前完成 init。
 */
void async_http_global_init(void);
void async_http_global_cleanup(void);

#endif /* ASYNC_HTTP_H */
