/**
 * @file gateway.h
 * @brief 微服务网关核心头文件
 * 
 * 定义了网关的所有核心数据结构、类型和函数声明。
 * 包括网络层、路由层、代理层、健康检查、可观测性等模块。
 * 
 * @author 乔水
 * @date 2026-03-25
 * @version 1.0.0
 */

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
/**
 * @brief 日志级别枚举
 * 
 * 定义系统支持的日志级别，用于控制日志输出的详细程度。
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,    ///< 调试级别：最详细的输出
    LOG_LEVEL_INFO,         ///< 信息级别：正常运行日志
    LOG_LEVEL_WARN,         ///< 警告级别：潜在问题告警
    LOG_LEVEL_ERROR         ///< 错误级别：严重错误记录
} log_level_t;

/**
 * @brief 可观测性配置结构
 * 
 * 包含日志、指标、追踪三大可观测性支柱的完整配置。
 * 支持动态启用/禁用各个功能模块。
 */
typedef struct {
    // 日志配置
    int enable_logging;           ///< 日志总开关 (0:禁用，1:启用)
    log_level_t log_level;        ///< 日志级别过滤
    int enable_json_log;          ///< JSON 格式日志开关
    
    // 指标配置 (Prometheus)
    int enable_metrics;           ///< 指标收集开关
    int metrics_port;             ///< Prometheus 抓取端口
    char metrics_path[256];       ///< 指标暴露路径
    
    // 追踪配置 (OpenTelemetry)
    int enable_tracing;           ///< 分布式追踪开关
    char tracing_exporter[256];   ///< 导出器类型：console/jaeger/zipkin/otlp
    char tracing_endpoint[512];   ///< 导出端点 URL
    double tracing_sample_rate;   ///< 采样率 (0.0-1.0)
} observability_config_t;

/**
 * @brief 协议类型枚举
 * 
 * 定义后端服务支持的协议类型。
 */
typedef enum {
    PROTOCOL_HTTP,     ///< HTTP 协议
    PROTOCOL_HTTPS     ///< HTTPS 协议（加密传输）
} protocol_t;

/**
 * @brief 服务实例健康状态
 * 
 * 定义后端服务实例的健康检查状态。
 */
typedef enum {
    SERVICE_HEALTHY,      ///< 健康：服务正常响应
    SERVICE_UNHEALTHY,    ///< 不健康：服务异常或无响应
    SERVICE_UNKNOWN       ///< 未知：尚未进行健康检查
} service_health_t;

/**
 * @brief IP 地址结构
 * 
 * 存储 IP 地址信息，支持 IPv4/IPv6/域名解析。
 */
typedef struct {
    char address[256];      ///< IP 地址字符串或域名
    int is_ipv6;            ///< IPv6 标志 (1:IPv6, 0:IPv4)
    int is_domain;          ///< 域名标志 (1:域名，0:IP 地址)
} ip_address_t;

/**
 * @brief 服务实例信息
 * 
 * 描述单个后端服务实例的完整信息，包括地址、协议、健康状态等。
 */
typedef struct {
    char host[SERVICE_HOST_LEN];     ///< 主机地址（IP 或域名）
    int port;                        ///< 服务端口
    protocol_t protocol;             ///< 协议类型 (HTTP/HTTPS)
    ip_address_t ip_addr;            ///< IP 地址信息
    service_health_t health;         ///< 健康状态
    int request_count;               ///< 请求计数（负载均衡用）
    long last_check_time;            ///< 最后检查时间戳
    char endpoint[256];              ///< 健康检查端点路径
    
    // SSL/TLS 配置
    int verify_ssl;                  ///< 是否验证 SSL 证书
    char ca_cert_path[512];          ///< CA 证书路径
    
    // 健康检查相关
    int failure_count;               ///< 连续失败次数
} service_instance_t;

/**
 * @brief 服务定义结构
 * 
 * 描述一个微服务的完整信息，包括多个实例和路由配置。
 */
typedef struct {
    char name[SERVICE_NAME_LEN];         ///< 服务名称
    char description[256];               ///< 服务描述
    char path_prefix[64];                ///< 路由路径前缀
    service_instance_t instances[MAX_SERVICE_INSTANCES];  ///< 服务实例数组
    int instance_count;                  ///< 实例数量
    int current_instance;                ///< 当前轮询索引
    pthread_mutex_t lock;                ///< 线程锁
    char health_endpoint[128];           ///< 健康检查端点
} service_t;

/**
 * @brief 服务注册表
 * 
 * 全局服务注册表，管理所有已注册的微服务。
 * 线程安全，支持并发读写。
 */
typedef struct {
    service_t services[MAX_SERVICES];   ///< 服务数组
    int service_count;                   ///< 服务总数
    pthread_mutex_t lock;                ///< 全局线程锁
} service_registry_t;

/**
 * @brief 内存池结构
 * 
 * 预分配的内存池，用于减少 malloc/free 调用，提升性能。
 * 每个连接独占一个内存池，请求间可复用。
 */
typedef struct 
{
    char data[POOL_SIZE];   ///< 内存池数据区 (8KB)
    size_t used;            ///< 已使用字节数
} mem_pool_t;

/**
 * @brief 客户端上下文结构
 * 
 * 维护每个客户端连接的完整状态信息，包括连接句柄、
 * HTTP 解析器、内存池、可观测性字段等。
 */
typedef struct 
{
    uv_tcp_t handle;                    ///< libuv TCP 连接句柄
    llhttp_t parser;                    ///< HTTP 解析器
    llhttp_settings_t settings;         ///< HTTP 解析回调配置

    mem_pool_t pool;                    ///< 内存池 (8KB)

    char* body_buffer;                  ///< 请求体缓冲区
    size_t body_len;                    ///< 请求体长度

    char url[512];                      ///< 请求 URL
    
    // === 可观测性字段 ===
    uint64_t request_start_time;        ///< 请求开始时间戳（纳秒）
    char request_id[64];                ///< 唯一请求 ID
    char trace_id[64];                  ///< 分布式追踪 ID
    char span_id[64];                   ///< 当前跨度 ID
    int is_sampled;                     ///< 是否被采样 (0/1)
    
    // 转发相关
    service_t* target_service;          ///< 目标服务
    service_instance_t* target_instance;///< 目标实例
    char forward_url[512];              ///< 转发 URL
} client_ctx_t;

/**
 * @brief 写入上下文结构
 * 
 * 封装异步写入操作的上下文，支持 scatter-gather IO。
 */
typedef struct {
    uv_write_t req;       ///< libuv 写入请求
    uv_buf_t bufs[2];     ///< 缓冲数组 (header + body)
    char* header_ptr;     ///< HTTP 头部指针
    char* body_ptr;       ///< HTTP 主体指针
} write_ctx_t;

/**
 * @brief 代理请求上下文
 * 
 * 维护向后端服务发起代理请求的完整状态。
 */
typedef struct {
    CURL *curl;                    ///< CURL 句柄
    struct curl_slist *headers;    ///< HTTP 头部链表
    client_ctx_t* client;          ///< 关联的客户端上下文
    char* response_data;           ///< 响应数据缓冲区
    size_t response_size;          ///< 响应数据大小
    int status_code;               ///< 响应状态码
} proxy_request_t;

/**
 * @brief 模拟员工数据（示例 API 用）
 * 
 * 用于演示的简单数据库结构。
 */
typedef struct {
    int id;             ///< 员工 ID
    const char* name;   ///< 姓名
    const char* dept;   ///< 部门
    double salary;      ///< 薪资
} employee_t;

/**
 * @brief 网关配置结构
 * 
 * 全局网关配置，包含网络、线程、可观测性等所有配置项。
 */
typedef struct {
    int worker_threads;           ///< 工作线程数
    int service_port;             ///< 服务监听端口
    int enable_ipv6;              ///< IPv6 开关
    int enable_https;             ///< HTTPS 开关
    char log_path[512];           ///< 日志文件路径
    int health_check_interval;    ///< 健康检查间隔 (毫秒)
    char ssl_cert_path[512];      ///< SSL 证书路径
    char ssl_key_path[512];       ///< SSL 私钥路径
    
    // === 可观测性配置 ===
    observability_config_t observability;  ///< 可观测性配置
} gateway_config_t;

// 全局网关配置
gateway_config_t g_gateway_config;

// 全局服务注册表
extern service_registry_t g_registry;
extern gateway_config_t g_gateway_config;

// === 网络层函数 ===

/**
 * @brief 新连接回调函数
 * 
 * 当服务器接受新客户端连接时调用此函数。
 * 初始化客户端上下文、HTTP 解析器，并开始读取数据。
 * 
 * @param server 服务器 TCP 句柄
 * @param status 状态码 (0 表示成功)
 */
void on_new_connection(uv_stream_t *server, int status);

/**
 * @brief 写入完成回调函数
 * 
 * 当响应数据写入完成后调用此函数。
 * 负责清理写入上下文，并根据 Keep-Alive 设置决定是否关闭连接。
 * 
 * @param req 写入请求句柄
 * @param status 状态码
 */
void on_write_completed(uv_write_t *req, int status);

// === 路由层函数 ===

/**
 * @brief 路由请求处理函数
 * 
 * 根据请求 URL 和方法进行路由匹配，分发到对应的处理函数。
 * 支持内置端点和后端服务代理。
 * 
 * @param client 客户端上下文
 */
void route_request(client_ctx_t* client);

/**
 * @brief 发送 HTTP 响应
 * 
 * 构造并发送完整的 HTTP 响应报文。
 * 使用异步写入，在写入完成回调中清理资源。
 * 
 * @param client 客户端上下文
 * @param status_code HTTP 状态码
 * @param content_type Content-Type 头部值
 * @param body_to_send 响应体（堆内存，由回调释放）
 */
void send_response(client_ctx_t* client, int status_code, const char* content_type, char* body_to_send);

/**
 * @brief 获取员工列表处理器
 * 
 * 处理 GET /api/employees 请求，返回模拟的员工数据。
 * 
 * @param client 客户端上下文
 */
void handle_get_employees(client_ctx_t* client);

/**
 * @brief 获取服务列表处理器
 * 
 * 处理 GET /services 请求，返回已注册的服务列表。
 * 
 * @param client 客户端上下文
 */
void handle_get_services(client_ctx_t* client);

/**
 * @brief 服务注册处理器
 * 
 * 处理 POST /services/register 请求，注册新的服务实例。
 * 
 * @param client 客户端上下文
 */
void handle_service_register(client_ctx_t* client);

/**
 * @brief 服务注销处理器
 * 
 * 处理 POST /services/unregister 请求，注销服务实例。
 * 
 * @param client 客户端上下文
 */
void handle_service_unregister(client_ctx_t* client);

// === 工具函数 ===

/**
 * @brief 获取查询参数
 * 
 * 从 URL 查询字符串中提取指定参数的值。
 * 
 * @param ctx 客户端上下文
 * @param key 参数名
 * @return char* 参数值（栈内存，生命周期同请求）
 */
char* get_query_param(client_ctx_t* ctx, const char* key);

/**
 * @brief 从内存池分配内存
 * 
 * 从连接的内存池中分配指定大小的内存。
 * 如果池空间不足，回退到 malloc。
 * 
 * @param ctx 客户端上下文
 * @param size 需要的字节数
 * @return void* 分配的内存指针
 */
void* pool_alloc(client_ctx_t* ctx, size_t size);

/**
 * @brief 解析 IP 地址
 * 
 * 将主机名或 IP 地址字符串解析为 ip_address_t 结构。
 * 支持 IPv4、IPv6 和域名。
 * 
 * @param host 主机名字符串
 * @param addr 输出参数，解析结果
 * @return int 0 表示成功，-1 表示失败
 */
int parse_ip_address(const char* host, ip_address_t* addr);

// === 可观测性模块函数声明 ===

// --- 日志模块 ---

/**
 * @brief 记录请求日志（通用接口）
 * 
 * 根据配置的日志级别和格式，记录请求相关的日志。
 * 支持 JSON 和文本两种格式。
 * 
 * @param level 日志级别
 * @param ctx 客户端上下文
 * @param event 事件名称
 * @param format 格式化字符串
 */
void log_request(log_level_t level, client_ctx_t* ctx, const char* event, const char* format, ...);

/**
 * @brief 记录调试日志
 * 
 * @param ctx 客户端上下文
 * @param event 事件名称
 * @param format 格式化字符串
 */
void log_debug(client_ctx_t* ctx, const char* event, const char* format, ...);

/**
 * @brief 记录信息日志
 * 
 * @param ctx 客户端上下文
 * @param event 事件名称
 * @param format 格式化字符串
 */
void log_info(client_ctx_t* ctx, const char* event, const char* format, ...);

/**
 * @brief 记录警告日志
 * 
 * @param ctx 客户端上下文
 * @param event 事件名称
 * @param format 格式化字符串
 */
void log_warn(client_ctx_t* ctx, const char* event, const char* format, ...);

/**
 * @brief 记录错误日志
 * 
 * @param ctx 客户端上下文
 * @param event 事件名称
 * @param format 格式化字符串
 */
void log_error(client_ctx_t* ctx, const char* event, const char* format, ...);

/**
 * @brief 获取纳秒级时间戳
 * 
 * @return uint64_t 纳秒级时间戳（相对于系统启动时间）
 */
uint64_t get_time_nanoseconds(void);

// --- 指标模块 ---

/**
 * @brief 初始化指标系统
 * 
 * 初始化所有 Prometheus 指标的初始值。
 */
void metrics_init(void);

/**
 * @brief 启动指标服务器
 * 
 * 在独立线程中启动 Prometheus 指标抓取服务器。
 */
void metrics_server_start(void);

/**
 * @brief 记录请求开始
 * 
 * 在请求开始时调用，记录开始时间和初始状态。
 * 
 * @param ctx 客户端上下文
 */
void metrics_request_start(client_ctx_t* ctx);

/**
 * @brief 记录请求完成
 * 
 * 在请求完成时调用，更新请求总数、延迟直方图等指标。
 * 
 * @param ctx 客户端上下文
 * @param status_code HTTP 状态码
 * @param duration_sec 请求持续时间（秒）
 */
void metrics_request_end(client_ctx_t* ctx, int status_code, double duration_sec);

/**
 * @brief 记录上游服务延迟
 * 
 * 记录向后端服务发起请求的延迟。
 * 
 * @param duration_sec 上游服务响应时间（秒）
 */
void metrics_upstream_duration(double duration_sec);

/**
 * @brief 生成 Prometheus 格式输出
 * 
 * 将所有指标格式化为 Prometheus 文本格式。
 * 
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 */
void metrics_generate_output(char* buffer, size_t buffer_size);

// --- 追踪模块 ---

/**
 * @brief 初始化追踪上下文
 * 
 * 从请求头解析 traceparent，或生成新的追踪 ID。
 * 
 * @param ctx 客户端上下文
 * @param incoming_traceparent 传入的 traceparent 字符串
 */
void tracing_init_context(client_ctx_t* ctx, const char* incoming_traceparent);

/**
 * @brief 导出追踪跨度
 * 
 * 在请求完成时导出追踪数据到 Jaeger/Zipkin 等后端。
 * 
 * @param ctx 客户端上下文
 * @param operation 操作名称
 * @param duration_ms 操作持续时间（毫秒）
 */
void tracing_export_span(client_ctx_t* ctx, const char* operation, double duration_ms);

/**
 * @brief 获取传出追踪上下文
 * 
 * 生成用于向后端服务发起请求的 traceparent。
 * 
 * @param ctx 客户端上下文
 * @param buffer 输出缓冲区
 * @param size 缓冲区大小
 */
void tracing_get_outgoing_traceparent(client_ctx_t* ctx, char* buffer, size_t size);

/**
 * @brief 添加追踪事件
 * 
 * 在当前跨度中添加事件标记。
 * 
 * @param ctx 客户端上下文
 * @param event_name 事件名称
 * @param attributes 事件属性（JSON 格式）
 */
void tracing_add_event(client_ctx_t* ctx, const char* event_name, const char* attributes);

// === 服务注册与发现模块 ===

/**
 * @brief 初始化服务注册表
 * 
 * 初始化全局服务注册表，包括互斥锁等。
 */
void service_registry_init(void);

/**
 * @brief 注册服务（简化版）
 * 
 * 向注册表中注册一个服务实例。
 * 
 * @param name 服务名称
 * @param path_prefix 路由路径前缀
 * @param host 服务主机地址
 * @param port 服务端口
 * @param protocol 协议类型
 * @param health_endpoint 健康检查端点
 * @param verify_ssl 是否验证 SSL
 * @return int 0 表示成功，-1 表示失败
 */
int service_register(const char* name, const char* path_prefix,
                     const char* host, int port, protocol_t protocol,
                     const char* health_endpoint, int verify_ssl);

/**
 * @brief 注册服务（完整版，支持 IPv6）
 * 
 * 向注册表中注册一个服务实例，支持 IPv6 地址。
 * 
 * @param name 服务名称
 * @param description 服务描述
 * @param path_prefix 路由路径前缀
 * @param host 服务主机地址
 * @param port 服务端口
 * @param protocol 协议类型
 * @param health_endpoint 健康检查端点
 * @param verify_ssl 是否验证 SSL
 * @param is_ipv6 是否 IPv6 地址
 * @return int 0 表示成功，-1 表示失败
 */
int service_register_with_ipv6(const char* name, const char* description,
                                const char* path_prefix, const char* host, int port, 
                                protocol_t protocol, const char* health_endpoint, 
                                int verify_ssl, int is_ipv6);

/**
 * @brief 注销服务
 * 
 * 从注册表中移除指定的服务实例。
 * 
 * @param name 服务名称
 * @param host 服务主机地址
 * @param port 服务端口
 * @return int 0 表示成功，-1 表示未找到
 */
int service_deregister(const char* name, const char* host, int port);

/**
 * @brief 根据路径查找服务
 * 
 * 根据请求路径匹配对应的后端服务。
 * 
 * @param path 请求路径
 * @return service_t* 匹配的服务指针，未找到返回 NULL
 */
service_t* service_find_by_path(const char* path);

/**
 * @brief 选择服务实例（负载均衡）
 * 
 * 使用 Round-Robin 算法从服务实例中选择一个。
 * 
 * @param service 服务指针
 * @return service_instance_t* 选中的实例指针
 */
service_instance_t* service_select_instance(service_t* service);

/**
 * @brief 启动健康检查器
 * 
 * 在独立线程中启动定时健康检查任务。
 */
void start_health_checker(void);

/**
 * @brief 转发请求到后端服务
 * 
 * 将客户端请求异步转发到选中的后端服务实例。
 * 
 * @param client 客户端上下文
 * @param instance 目标服务实例
 */
void forward_to_service(client_ctx_t* client, service_instance_t* instance);

// === 配置加载函数 ===

/**
 * @brief 加载服务配置文件
 * 
 * 从 JSON 配置文件加载服务注册信息。
 * 
 * @param config_file 配置文件路径
 * @return int 0 表示成功，-1 表示失败
 */
int load_service_config(const char* config_file);

/**
 * @brief TCP 服务器初始化（支持 IPv6）
 * 
 * 初始化 TCP 服务器，优先使用 IPv6，失败则回退到 IPv4。
 * 
 * @param loop libuv 事件循环
 * @param server TCP 服务器句柄
 * @param addr 监听地址
 * @param port 监听端口
 * @return int 0 表示成功，-1 表示失败
 */
int init_tcp_server_ipv6(uv_loop_t* loop, uv_tcp_t* server, const char* addr, int port);

/**
 * @brief 加载网关配置文件
 * 
 * 从 JSON 配置文件加载网关配置。
 * 
 * @param config_file 配置文件路径
 * @return int 0 表示成功，-1 表示失败
 */
int load_gateway_config(const char* config_file);

#endif
