#ifndef Q_LOG_H
#define Q_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Q_LOG_PATH_LEN  512
#define Q_LOG_LINE_MAX  2048      /* 单条日志上限，超过截断 */
#define Q_LOG_CAT_MAX   8         /* 分类数上限 */

typedef enum {
    Q_LOG_TRACE = 0,
    Q_LOG_DEBUG,
    Q_LOG_INFO,
    Q_LOG_WARN,
    Q_LOG_ERROR,
    Q_LOG_FATAL
} q_log_level_t;

int64_t q_time_now_ms(void);
int64_t q_time_now_us(void);
void    q_time_sleep_ms(int ms);

/* 格式化为 "2026-09-21 22:35:10.238"，返回写入长度（不含 '\0'） */
size_t  q_time_format_ms(int64_t ms, char *buf, size_t cap);

/* 按 strftime 格式串输出本地时间（不含毫秒），返回写入长度 */
size_t  q_time_format(int64_t ms, const char *fmt, char *buf, size_t cap);

typedef enum {
    Q_OK           =   0,
    Q_ERR          =  -1,   /* 通用失败 */
    Q_ERR_NOMEM    =  -2,
    Q_ERR_INVAL    =  -3,
    Q_ERR_NOTFOUND =  -4,
    Q_ERR_BUSY     =  -5,   /* 队列满 / 资源暂时不可用 */
    Q_ERR_TIMEOUT  =  -6,
    Q_ERR_IO       =  -7,
    Q_ERR_EXIST    =  -8
} q_err_t;

#define Q_UNUSED(x)       ((void)(x))
#define Q_LIKELY(x)       __builtin_expect(!!(x), 1)
#define Q_UNLIKELY(x)     __builtin_expect(!!(x), 0)
#define Q_ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define Q_ALIGN_UP(n, a)  (((n) + ((a) - 1)) & ~((a) - 1))
#define Q_MIN(a, b)       ((a) < (b) ? (a) : (b))
#define Q_MAX(a, b)       ((a) > (b) ? (a) : (b))

/* 容器成员指针 -> 容器指针，链表实现里省掉额外的 next 字段语义 */
#define Q_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

const char *q_err_str(int rc);

/*
 * 初始化：dir 日志目录（不存在则创建），app 文件名前缀，level 全局级别阈值。
 * 必须在创建线程之前调用。
 */
int  q_log_init(const char *dir, const char *app, q_log_level_t level);

/*
 * 注册分类。每个分类一个独立文件。
 * 未注册的分类自动落到默认分类（init 时创建的 "app"）。
 * max_bytes 超过则轮转（0 表示不轮转），keep_days 为保留天数（0 表示永久）。
 */
int  q_log_add_cat(const char *name, size_t max_bytes, int keep_days);

/* 线程上下文，设置后该线程日志自动带上这两个字段 */
void q_log_set_ctx(const char *trace_id, const char *req_id);
void q_log_clear_ctx(void);

/* 核心写入。cat 为 NULL 时用默认分类 */
void q_log_write(q_log_level_t lvl, const char *cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void q_log_flush(void);        /* 通知后台线程尽快落盘（异步） */
void q_log_sync_flush(void);   /* 同步落盘，只调 write(2)，可在信号处理器里用 */
void q_log_close(void);

/* 运行期改级别 */
void          q_log_set_level(q_log_level_t level);
q_log_level_t q_log_get_level(void);

const char   *q_log_level_str(q_log_level_t lvl);
int           q_log_level_from_str(const char *s);

/* 计数器，暴露给 /metrics 用 */
typedef struct {
    unsigned long long written;       /* 已落盘条数 */
    unsigned long long dropped;       /* 因队列满被丢弃（DEBUG 级以下） */
    unsigned long long sync_writes;   /* 降级为同步写的条数 */
    unsigned long long rotates;       /* 轮转次数 */
    unsigned long long open_failed;   /* 打开文件失败次数 */
} q_log_stat_t;

void q_log_stat(q_log_stat_t *out);

/* ---- 便捷宏 ---- */

#define q_trace(fmt, ...) q_log_write(Q_LOG_TRACE, "app", fmt, ##__VA_ARGS__)
#define q_debug(fmt, ...) q_log_write(Q_LOG_DEBUG, "app", fmt, ##__VA_ARGS__)
#define q_info( fmt, ...) q_log_write(Q_LOG_INFO,  "app", fmt, ##__VA_ARGS__)
#define q_warn( fmt, ...) q_log_write(Q_LOG_WARN,  "app", fmt, ##__VA_ARGS__)
#define q_error(fmt, ...) q_log_write(Q_LOG_ERROR, "app", fmt, ##__VA_ARGS__)
#define q_fatal(fmt, ...) q_log_write(Q_LOG_FATAL, "app", fmt, ##__VA_ARGS__)

#define q_info_cat(cat, fmt, ...)  q_log_write(Q_LOG_INFO,  (cat), fmt, ##__VA_ARGS__)
#define q_warn_cat(cat, fmt, ...)  q_log_write(Q_LOG_WARN,  (cat), fmt, ##__VA_ARGS__)
#define q_error_cat(cat, fmt, ...) q_log_write(Q_LOG_ERROR, (cat), fmt, ##__VA_ARGS__)
#define q_debug_cat(cat, fmt, ...) q_log_write(Q_LOG_DEBUG, (cat), fmt, ##__VA_ARGS__)

/* 兼容老项目：debugLog / errLog 签名不变，直接转发 */
#define debugLog(fmt, ...) q_log_write(Q_LOG_DEBUG, "app", fmt, ##__VA_ARGS__)
#define errLog(  fmt, ...) q_log_write(Q_LOG_ERROR, "app", fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* Q_LOG_H */
