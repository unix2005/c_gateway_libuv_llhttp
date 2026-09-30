/**
 * @file async.c
 * @brief 通用异步执行原语（基于 libuv 线程池 uv_queue_work）
 */
#include "q_async.h"
#include <stdlib.h>

typedef struct
{
    async_work_t work;
    async_done_t done;
    void *data;
} async_job_t;

static void work_cb(uv_work_t *w)
{
    async_job_t *j = w->data;
    if (j->work)
        j->work(j->data);
}

static void done_cb(uv_work_t *w, int status)
{
    (void)status;
    async_job_t *j = w->data;
    if (j->done)
        j->done(j->data);
    free(j);
    free(w);
}

int q_async_exec(uv_loop_t *loop, async_work_t work, async_done_t done, void *data)
{
    if (!loop)
        return -1;
    uv_work_t *w = malloc(sizeof(*w));
    if (!w)
        return -1;
    async_job_t *j = malloc(sizeof(*j));
    if (!j)
    {
        free(w);
        return -1;
    }
    j->work = work;
    j->done = done;
    j->data = data;
    w->data = j;
    return uv_queue_work(loop, w, work_cb, done_cb);
}
