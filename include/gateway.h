#ifndef GATEWAY_H
#define GATEWAY_H

#include <uv.h>
#include <llhttp.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <cJSON.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <sys/syscall.h>
#endif

// 获取线程 ID 的工具函数
#ifdef _WIN32
    static inline unsigned long gettid(void) {
        return (unsigned long)pthread_self();
    }
#else
    #define gettid() syscall(__NR_gettid)
#endif

#define POOL_SIZE 8192
#define MAX_SERVICES 64
#define MAX_SERVICE_INSTANCES 16
#define SERVICE_NAME_LEN 64
#define SERVICE_HOST_LEN 256
#define DEFAULT_HEALTH_CHECK_INTERVAL 5000  // 5 秒
// 日志级别
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

// 可观测性配置
typedef struct {
    // 日志配置
    int enable_logging;           // 总开关
    log_level_t log_level;        // 日志级别
    int enable_json_log;          // JSON 格式日志开关
    
    // 指标配置 (Prometheus)
    int enable_metrics;           // 指标收集开关
    int metrics_port;             // Prometheus 抓取端口
    char metrics_path[256];       // 指标暴露路径
    
    // 追踪配置 (OpenTelemetry)
    int enable_tracing;           // 分布式追踪开关
    char tracing_exporter[256];   // 导出器类型：console/jaeger/zipkin/otlp
    char tracing_endpoint[512];   // 导出端点 URL
    double tracing_sample_rate;   // 采样率 (0.0-1.0)
} observability_config_t;

// 协议类型
typedef enum {
    PROTOCOL_HTTP,
    PROTOCOL_HTTPS
} protocol_t;

// 服务实例状态
typedef enum {
    SERVICE_HEALTHY,
    SERVICE_UNHEALTHY,
    SERVICE_UNKNOWN
} service_health_t;

// IP 地址类型
typedef struct {
    char address[256];      // IP 地址字符串
    int is_ipv6;            // 1 for IPv6, 0 for IPv4
    int is_domain;          // ✓ 1 if host is a domain name, 0 if direct IP
} ip_address_t;

// 服务实例信息
typedef struct {
    char host[SERVICE_HOST_LEN];
    int port;
    protocol_t protocol;      // HTTP 或 HTTPS
    ip_address_t ip_addr;     // IP 地址（支持 v4/v6）
    service_health_t health;
    int request_count;        // 请求计数
    long last_check_time;     // 最后检查时间
    char endpoint[256];       // 健康检查端点
    
    // SSL/TLS 配置
    int verify_ssl;           // 是否验证 SSL 证书
    char ca_cert_path[512];   // CA 证书路径
    
    // 健康检查相关
    int failure_count;        // ✓ 连续失败次数
} service_instance_t;

// 服务定义
typedef struct {
    char name[SERVICE_NAME_LEN];
    char description[256];      // ✓ 服务描述
    char path_prefix[64];       // 路由路径前缀
    service_instance_t instances[MAX_SERVICE_INSTANCES];
    int instance_count;
    int current_instance;       // 轮询索引
    pthread_mutex_t lock;
    char health_endpoint[128];
} service_t;

// 服务注册表
typedef struct {
    service_t services[MAX_SERVICES];
    int service_count;
    pthread_mutex_t lock;
} service_registry_t;

// 内存池结构
typedef struct 
{
    char data[POOL_SIZE];
    size_t used;
} mem_pool_t;

// 客户端上下文
typedef struct 
{
    uv_tcp_t handle;
    llhttp_t parser;
    llhttp_settings_t settings;

    mem_pool_t pool;

    char* body_buffer;
    size_t body_len;

    char url[512];
    
    // === 可观测性字段 ===
    uint64_t request_start_time;      // 请求开始时间戳（纳秒）
    char request_id[64];              // 唯一请求 ID
    char trace_id[64];                // 分布式追踪 ID
    char span_id[64];                 // 当前跨度 ID
    int is_sampled;                   // 是否被采样 (0/1)
    
    // 转发相关
    service_t* target_service;
    service_instance_t* target_instance;
    char forward_url[512];
} client_ctx_t;

// 写入上下文
typedef struct {
    uv_write_t req;
    uv_buf_t bufs[2];
    char* header_ptr;
    char* body_ptr;
} write_ctx_t;

// 异步 HTTP 请求上下文
typedef struct {
    CURL *curl;
    struct curl_slist *headers;
    client_ctx_t* client;
    char* response_data;
    size_t response_size;
    int status_code;
} proxy_request_t;

// 模拟数据库数据（保留原有功能）
typedef struct {
    int id;
    const char* name;
    const char* dept;
    double salary;
} employee_t;

// 网关配置结构
typedef struct {
    int worker_threads;           // 工作线程数
    int service_port;             // 服务端口
    int enable_ipv6;              // IPv6 开关
    int enable_https;             // HTTPS 开关
    char log_path[512];           // 日志文件路径
    int health_check_interval;    // 健康检查间隔 (毫秒)
    char ssl_cert_path[512];      // SSL 证书路径
    char ssl_key_path[512];       // SSL 私钥路径
    
    // === 可观测性配置 ===
    observability_config_t observability;  // 可观测性配置
} gateway_config_t;

// 全局网关配置
gateway_config_t g_gateway_config;

// 全局服务注册表
extern service_registry_t g_registry;
extern gateway_config_t g_gateway_config;

// 函数声明 - 网络层
void on_new_connection(uv_stream_t *server, int status);
void on_write_completed(uv_write_t *req, int status);

// 函数声明 - 路由层
void route_request(client_ctx_t* client);
void send_response(client_ctx_t* client, int status_code, const char* content_type, char* body_to_send);
void handle_get_employees(client_ctx_t* client);
void handle_get_services(client_ctx_t* client);
void handle_service_register(client_ctx_t* client);
void handle_service_unregister(client_ctx_t* client);

// 函数声明 - 工具函数
char* get_query_param(client_ctx_t* ctx, const char* key);
void* pool_alloc(client_ctx_t* ctx, size_t size);
int parse_ip_address(const char* host, ip_address_t* addr);

// === 可观测性模块函数声明 ===
// 日志
void log_request(log_level_t level, client_ctx_t* ctx, const char* event, const char* format, ...);
void log_debug(client_ctx_t* ctx, const char* event, const char* format, ...);
void log_info(client_ctx_t* ctx, const char* event, const char* format, ...);
void log_warn(client_ctx_t* ctx, const char* event, const char* format, ...);
void log_error(client_ctx_t* ctx, const char* event, const char* format, ...);
uint64_t get_time_nanoseconds(void);

// 指标
void metrics_init(void);
void metrics_server_start(void);
void metrics_request_start(client_ctx_t* ctx);
void metrics_request_end(client_ctx_t* ctx, int status_code, double duration_sec);
void metrics_upstream_duration(double duration_sec);
void metrics_generate_output(char* buffer, size_t buffer_size);

// 追踪
void tracing_init_context(client_ctx_t* ctx, const char* incoming_traceparent);
void tracing_export_span(client_ctx_t* ctx, const char* operation, double duration_ms);
void tracing_get_outgoing_traceparent(client_ctx_t* ctx, char* buffer, size_t size);
void tracing_add_event(client_ctx_t* ctx, const char* event_name, const char* attributes);

// 函数声明 - 服务注册与发现
void service_registry_init();
int service_register(const char* name, const char* path_prefix,
                     const char* host, int port, protocol_t protocol,
                     const char* health_endpoint, int verify_ssl);
int service_register_with_ipv6(const char* name, const char* description,
                                const char* path_prefix, const char* host, int port, 
                                protocol_t protocol, const char* health_endpoint, 
                                int verify_ssl, int is_ipv6);
int service_deregister(const char* name, const char* host, int port);
service_t* service_find_by_path(const char* path);
service_instance_t* service_select_instance(service_t* service);
void start_health_checker();
void forward_to_service(client_ctx_t* client, service_instance_t* instance);

// 配置加载
int load_service_config(const char* config_file);

// TCP 服务器初始化（支持 IPv6）
int init_tcp_server_ipv6(uv_loop_t* loop, uv_tcp_t* server, const char* addr, int port);

int load_gateway_config(const char* config_file);

#endif
