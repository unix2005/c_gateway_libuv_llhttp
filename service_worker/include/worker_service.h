#ifndef __WORKER_SERVICE_H__
#define __WORKER_SERVICE_H__

// 启用 POSIX 标准以支持 strdup 等函数
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <uv.h>
#include <llhttp.h>
#include <curl/curl.h>
#include <cJSON.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

#define MAX_WORKERS 64
#define POOL_SIZE 8192

uv_signal_t sigint_handle;

// 配置结构体（简化）
typedef struct 
{
    char name[64];
    char description[256];     // 服务描述
    char host[256];            // ✓ 服务监听地址（支持 IPv4/IPv6）
    int port;
    int is_ipv6;
    int https;                 // 是否启用 HTTPS
    char path_prefix[64];      // 路由路径前缀
    char health_endpoint[128]; // ✓ 健康检查端点
    int verify_ssl;            // ✓ SSL 验证开关
    char gateway_host[256];
    int gateway_port;
    int thread_count;
    int max_connections;
    char log_file[512];
    int log_level;
} service_config_t;

// 工作线程上下文
typedef struct i
{
    int id;
    uv_loop_t* loop;
    volatile int running;
    pthread_t thread;
} worker_ctx_t;

// 客户端上下文
typedef struct 
{
    uv_tcp_t handle;
    llhttp_t parser;
    llhttp_settings_t settings;
    char pool_data[POOL_SIZE];
    size_t pool_used;
    char* body_buffer;
    size_t body_len;
    char url[512];
    llhttp_method_t method;
} client_ctx_t;

// 写入上下文
typedef struct 
{
    uv_write_t req;
    uv_buf_t bufs[2];
    char* header_ptr;
    char* body_ptr;
} write_ctx_t;

// 模拟数据库数据（保留原有功能）
typedef struct {
    int id;
    const char* name;
    const char* dept;
    double salary;
} employee_t;


// 模拟数据库数据（保留原有功能）
static employee_t employee_db[] = {
  {1001, "张三", "技术部", 15000.5},
  {1002, "李四", "市场部", 12000.0},
  {1003, "王五", "研发部", 18000.7}
};
static int db_count = 3;

// 全局变量
static service_config_t config;
static CURL* g_curl = NULL;
// static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER; // 未使用，注释掉
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* log_fp = NULL;
static worker_ctx_t workers[MAX_WORKERS];
static int next_worker = 0;
static uv_mutex_t worker_mutex;

// 前向声明
void write_complete_callback(uv_write_t* req, int status);
size_t curl_ignore_callback(void* contents, size_t size, size_t nmemb, void* userp);

void log_info(const char* format, ...);


/* ============================================================================
   函数前向声明
   ============================================================================ */
static void* pool_alloc(client_ctx_t* ctx, size_t size);
static char* get_query_param(client_ctx_t* ctx, const char* key);
static void handle_get_employees(client_ctx_t* client);

#endif
