/**
 * @file async.h
 * @brief 通用异步执行原语：把“阻塞任务”offload 到线程池
 *
 * 业务服务（基于本 SDK 的 libuv 单 loop HTTP server）若直接在 handler 里调用
 * 阻塞式 API（如同步的 db/mapper 查询），会卡住整个事件循环。本模块提供
 * async_exec()：把阻塞工作放到 worker 线程执行，完成后在“发起请求的 loop 线程”
 * 回调，从而既能写阻塞式业务逻辑，又不阻塞事件循环。
 *
 * 底层复用 libuv 内置线程池（uv_queue_work）。若 DB 密集，可在启动前设置
 * 环境变量 UV_THREADPOOL_SIZE 调大池大小；后续也可替换为独立专用线程池。
 */
#ifndef ASYNC_H
#define ASYNC_H

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*async_work_t)(void *data);   /* 在 worker 线程执行（可阻塞） */
typedef void (*async_done_t)(void *data);   /* 在 loop 线程执行（回写响应） */

/**
 * 异步执行：work 在 worker 线程运行，done 在 loop 线程运行。
 * @param loop 必须是发起本请求的事件循环（cservice_req_loop(req) 获取）
 * @return 0 成功，<0 失败
 */
int async_exec(uv_loop_t *loop, async_work_t work, async_done_t done, void *data);

#ifdef __cplusplus
}
#endif

#endif /* ASYNC_H */
