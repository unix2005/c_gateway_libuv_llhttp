/**
 * Prometheus 指标收集模块
 * 高性能、线程安全的指标收集
 */

#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <uv.h>
#include <inttypes.h>
#include <stddef.h>

// ===== 指标定义 =====

// 指标类型
typedef enum {
    METRIC_COUNTER,
    METRIC_GAUGE,
    METRIC_HISTOGRAM
} metric_type_t;

// 计数器指标
typedef struct {
    const char* name;
    const char* help;
    int64_t value;
    pthread_mutex_t lock;
} metric_counter_t;

// 直方图桶
typedef struct {
    double le;        // less than or equal
    int64_t count;
} histogram_bucket_t;

// 直方图指标
typedef struct {
    const char* name;
    const char* help;
    histogram_bucket_t* buckets;
    int bucket_count;
    int64_t total_count;
    double sum;
    pthread_mutex_t lock;
} metric_histogram_t;

// ===== 全局指标实例 =====

// 请求相关指标
static metric_counter_t http_requests_total = {
    .name = "gateway_http_requests_total",
    .help = "Total number of HTTP requests"
};

static metric_counter_t http_request_errors_total = {
    .name = "gateway_http_request_errors_total",
    .help = "Total number of failed HTTP requests"
};

// 延迟指标（直方图）
static histogram_bucket_t request_duration_buckets[] = {
    {0.001, 0},   // 1ms
    {0.005, 0},   // 5ms
    {0.010, 0},   // 10ms
    {0.025, 0},   // 25ms
    {0.050, 0},   // 50ms
    {0.100, 0},   // 100ms
    {0.250, 0},   // 250ms
    {0.500, 0},   // 500ms
    {1.000, 0},   // 1s
    {2.500, 0},   // 2.5s
    {5.000, 0},   // 5s
    {10.000, 0},  // 10s
    {-1, 0}       // +Inf
};

static metric_histogram_t http_request_duration_seconds = {
    .name = "gateway_http_request_duration_seconds",
    .help = "HTTP request latency in seconds",
    .buckets = request_duration_buckets,
    .bucket_count = sizeof(request_duration_buckets) / sizeof(request_duration_buckets[0])
};

// 上游服务延迟指标
static metric_histogram_t http_upstream_duration_seconds = {
    .name = "gateway_http_upstream_duration_seconds",
    .help = "Upstream service latency in seconds",
    .buckets = request_duration_buckets,
    .bucket_count = sizeof(request_duration_buckets) / sizeof(request_duration_buckets[0])
};

// 并发请求数
static metric_counter_t http_requests_in_flight = {
    .name = "gateway_http_requests_in_flight",
    .help = "Current number of requests being processed"
};

// 初始化锁
static void init_metrics(void)
{
    pthread_mutex_init(&http_requests_total.lock, NULL);
    pthread_mutex_init(&http_request_errors_total.lock, NULL);
    pthread_mutex_init(&http_request_duration_seconds.lock, NULL);
    pthread_mutex_init(&http_upstream_duration_seconds.lock, NULL);
    pthread_mutex_init(&http_requests_in_flight.lock, NULL);
}

// ===== 指标操作函数 =====

// 增加计数器
static void counter_inc(metric_counter_t* counter)
{
    pthread_mutex_lock(&counter->lock);
    counter->value++;
    pthread_mutex_unlock(&counter->lock);
}

// 减少计数器
static void counter_dec(metric_counter_t* counter)
{
    pthread_mutex_lock(&counter->lock);
    counter->value--;
    pthread_mutex_unlock(&counter->lock);
}

// 记录直方图
static void histogram_observe(metric_histogram_t* hist, double value)
{
    pthread_mutex_lock(&hist->lock);
    
    // 累加总和和总数
    hist->sum += value;
    hist->total_count++;
    
    // 更新对应的桶
    for (int i = 0; i < hist->bucket_count; i++) {
        if (hist->buckets[i].le < 0 || value <= hist->buckets[i].le) {
            hist->buckets[i].count++;
        }
    }
    
    pthread_mutex_unlock(&hist->lock);
}

// ===== 公共 API =====

// 初始化指标系统
void metrics_init(void)
{
    if (!g_gateway_config.observability.enable_metrics) {
        return;
    }
    init_metrics();
    log_info(NULL, "metrics_initialized", "Prometheus metrics initialized");
}

// 记录请求开始
void metrics_request_start(client_ctx_t* ctx)
{
    if (!g_gateway_config.observability.enable_metrics) {
        return;
    }
    
    counter_inc(&http_requests_in_flight);
    counter_inc(&http_requests_total);
}

// 记录请求完成
void metrics_request_end(client_ctx_t* ctx, int status_code, double duration_sec)
{
    if (!g_gateway_config.observability.enable_metrics) {
        return;
    }
    
    counter_dec(&http_requests_in_flight);
    
    // 记录延迟
    histogram_observe(&http_request_duration_seconds, duration_sec);
    
    // 错误计数
    if (status_code >= 500) {
        counter_inc(&http_request_errors_total);
    }
}

// 记录上游延迟
void metrics_upstream_duration(double duration_sec)
{
    if (!g_gateway_config.observability.enable_metrics) {
        return;
    }
    
    histogram_observe(&http_upstream_duration_seconds, duration_sec);
}

// 生成 Prometheus 格式的指标文本
void metrics_generate_output(char* buffer, size_t buffer_size)
{
    size_t offset = 0;
    
#define APPEND(fmt, ...) do { \
        int n = snprintf(buffer + offset, buffer_size - offset, fmt, ##__VA_ARGS__); \
        if (n > 0 && (size_t)n < buffer_size - offset) { \
            offset += n; \
        } \
    } while(0)
    
    // HELP 和 TYPE 声明
    APPEND("# HELP %s %s\n# TYPE %s counter\n", 
           http_requests_total.name, http_requests_total.help, http_requests_total.name);
    APPEND("# HELP %s %s\n# TYPE %s counter\n", 
           http_request_errors_total.name, http_request_errors_total.help, http_request_errors_total.name);
    APPEND("# HELP %s %s\n# TYPE %s gauge\n", 
           http_requests_in_flight.name, http_requests_in_flight.help, http_requests_in_flight.name);
    APPEND("# HELP %s %s\n# TYPE %s histogram\n", 
           http_request_duration_seconds.name, http_request_duration_seconds.help, http_request_duration_seconds.name);
    APPEND("# HELP %s %s\n# TYPE %s histogram\n", 
           http_upstream_duration_seconds.name, http_upstream_duration_seconds.help, http_upstream_duration_seconds.name);
    
    // 计数器值
    pthread_mutex_lock(&http_requests_total.lock);
    APPEND("%s %lld\n", http_requests_total.name, (long long)http_requests_total.value);
    pthread_mutex_unlock(&http_requests_total.lock);
    
    pthread_mutex_lock(&http_request_errors_total.lock);
    APPEND("%s %lld\n", http_request_errors_total.name, (long long)http_request_errors_total.value);
    pthread_mutex_unlock(&http_request_errors_total.lock);
    
    pthread_mutex_lock(&http_requests_in_flight.lock);
    APPEND("%s %lld\n", http_requests_in_flight.name, (long long)http_requests_in_flight.value);
    pthread_mutex_unlock(&http_requests_in_flight.lock);
    
    // 直方图值
    pthread_mutex_lock(&http_request_duration_seconds.lock);
    for (int i = 0; i < http_request_duration_seconds.bucket_count; i++) {
        if (http_request_duration_seconds.buckets[i].le < 0) {
            APPEND("%s_bucket{le=\"+Inf\"} %lld\n",
                   http_request_duration_seconds.name,
                   (long long)http_request_duration_seconds.total_count);
        } else {
            APPEND("%s_bucket{le=\"%.3f\"} %lld\n",
                   http_request_duration_seconds.name,
                   http_request_duration_seconds.buckets[i].le,
                   (long long)http_request_duration_seconds.buckets[i].count);
        }
    }
    APPEND("%s_sum %.6f\n", http_request_duration_seconds.name, http_request_duration_seconds.sum);
    APPEND("%s_count %lld\n", http_request_duration_seconds.name, (long long)http_request_duration_seconds.total_count);
    pthread_mutex_unlock(&http_request_duration_seconds.lock);
    
    // 上游服务直方图
    pthread_mutex_lock(&http_upstream_duration_seconds.lock);
    for (int i = 0; i < http_upstream_duration_seconds.bucket_count; i++) {
        if (http_upstream_duration_seconds.buckets[i].le < 0) {
            APPEND("%s_bucket{le=\"+Inf\"} %lld\n",
                   http_upstream_duration_seconds.name,
                   (long long)http_upstream_duration_seconds.total_count);
        } else {
            APPEND("%s_bucket{le=\"%.3f\"} %lld\n",
                   http_upstream_duration_seconds.name,
                   http_upstream_duration_seconds.buckets[i].le,
                   (long long)http_upstream_duration_seconds.buckets[i].count);
        }
    }
    APPEND("%s_sum %.6f\n", http_upstream_duration_seconds.name, http_upstream_duration_seconds.sum);
    APPEND("%s_count %lld\n", http_upstream_duration_seconds.name, (long long)http_upstream_duration_seconds.total_count);
    pthread_mutex_unlock(&http_upstream_duration_seconds.lock);
    
#undef APPEND
}

// 启动 Prometheus 指标服务器
static uv_tcp_t metrics_server;
static uv_loop_t* metrics_loop = NULL;

typedef struct {
    uv_tcp_t client;
    uv_write_t write_req;
    char* response_data;  // 存储响应数据，在 close 时释放
} metrics_client_t;

static void metrics_on_close(uv_handle_t* handle)
{
    // 获取客户端结构
    metrics_client_t* mc = (metrics_client_t*)handle->data;
    
    // 释放响应数据
    if (mc->response_data) {
        free(mc->response_data);
    }
    
    // 释放客户端结构
    free(mc);
}

static void metrics_on_write_complete(uv_write_t* req, int status)
{
    // 从 write_req 的 data 字段获取客户端结构
    metrics_client_t* mc = (metrics_client_t*)req->data;
    
    // 释放响应数据（如果有）
    if (mc && req->data) {
        // req->data 已经被设置为 full_response，在 metrics_read_cb 中设置
        // 这里不需要再次释放，因为已经在 metrics_read_cb 中通过 mc->write_req.data 设置了
    }
    
    // 安全关闭连接
    if (mc) {
        uv_close((uv_handle_t*)&mc->client, metrics_on_close);
    }
}

static void metrics_alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    static char slab[8192];
    buf->base = slab;
    buf->len = sizeof(slab);
}

static void metrics_read_cb(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf)
{
    if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "[Metrics] Read error: %s\n", uv_err_name(nread));
        }
        // 修复：正确获取 client 指针
        metrics_client_t* mc = (metrics_client_t*)client->data;
        uv_close((uv_handle_t*)&mc->client, metrics_on_close);
        return;
    }
    
    // 收到请求，生成指标响应
    metrics_client_t* mc = (metrics_client_t*)client->data;
    
    char response[65536];
    metrics_generate_output(response, sizeof(response));
    
    char http_header[] = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    char* full_response = malloc(strlen(http_header) + strlen(response) + 1);
    strcpy(full_response, http_header);
    strcat(full_response, response);
    
    // 保存响应数据指针，以便在 close 时释放
    mc->response_data = full_response;
    
    // 设置 write_req 的 data 指向 metrics_client_t，以便在回调中获取
    mc->write_req.data = mc;
    uv_buf_t write_buf = uv_buf_init(full_response, strlen(full_response));
    
    uv_write(&mc->write_req, (uv_stream_t*)&mc->client, &write_buf, 1, metrics_on_write_complete);
}

static void metrics_new_connection(uv_stream_t* server, int status)
{
    if (status < 0) {
        fprintf(stderr, "[Metrics] New connection error: %s\n", uv_strerror(status));
        return;
    }
    
    metrics_client_t* mc = calloc(1, sizeof(metrics_client_t));
    uv_tcp_init(metrics_loop, &mc->client);
    mc->client.data = mc;
    
    uv_accept(server, (uv_stream_t*)&mc->client);
    uv_read_start((uv_stream_t*)&mc->client, metrics_alloc_buffer, metrics_read_cb);
}

static void* metrics_server_thread(void* arg)
{
    (void)arg;
    
    metrics_loop = uv_loop_new();
    uv_tcp_init(metrics_loop, &metrics_server);
    
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", g_gateway_config.observability.metrics_port, &addr);
    uv_tcp_bind(&metrics_server, (const struct sockaddr*)&addr, 0);
    
    uv_listen((uv_stream_t*)&metrics_server, 128, metrics_new_connection);
    
    printf("[Metrics] Prometheus 指标服务器已启动：http://0.0.0.0:%d%s\n",
           g_gateway_config.observability.metrics_port,
           g_gateway_config.observability.metrics_path);
    
    uv_run(metrics_loop, UV_RUN_DEFAULT);
    
    uv_loop_delete(metrics_loop);
    return NULL;
}

// 启动指标服务器
void metrics_server_start(void)
{
    if (!g_gateway_config.observability.enable_metrics) {
        return;
    }
    
    pthread_t thread;
    pthread_create(&thread, NULL, metrics_server_thread, NULL);
    pthread_detach(thread);
}
