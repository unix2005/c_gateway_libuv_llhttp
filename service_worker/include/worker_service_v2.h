/**
 * worker_service_v2 专用头文件
 * 
 * 包含所有必需的系统头文件和依赖库头文件
 * 定义必要的宏以确保 POSIX 函数可用
 */

#ifndef WORKER_SERVICE_V2_H
#define WORKER_SERVICE_V2_H

/* 启用 POSIX 标准函数（如 strdup） */
#define _POSIX_C_SOURCE 200809L

/* Windows 特有配置 */
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #define sleep(seconds) Sleep((seconds) * 1000)
    #ifndef closesocket
        #define closesocket(s) closesocket(s)
    #endif
#else
    /* Unix/Linux 系统头文件 */
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#endif

/* 标准库头文件 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>

/* libuv 头文件 - 必须在 pthread 之后 */
#include <uv.h>

/* OpenSSL 头文件 */
#include <openssl/ssl.h>
#include <openssl/err.h>

/* libcurl 头文件 */
#include <curl/curl.h>
#include <curl/easy.h>

/* cJSON 头文件 */
#include <cJSON.h>

/* llhttp 头文件（如果有使用） */
#ifdef HAVE_LLHTTP
    #include <llhttp.h>
#endif

/* 日志级别枚举 */
/*
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
} log_level_t;
*/
/* 服务健康状态枚举 */
typedef enum {
    SERVICE_HEALTHY = 0,
    SERVICE_UNHEALTHY = 1,
    SERVICE_UNKNOWN = 2
} service_health_t;

/* 协议类型枚举 */
typedef enum {
    PROTOCOL_HTTP = 0,
    PROTOCOL_HTTPS = 1
} protocol_t;

/* IPv4/IPv6地址结构 */
typedef struct {
    char address[256];
    int is_ipv6;
} ip_address_t;

/* 服务实例信息 */
typedef struct {
    char host[MAX_HOST_LEN];
    int port;
    protocol_t protocol;
    ip_address_t ip_addr;
    service_health_t health;
    int request_count;
    long last_check_time;
    char endpoint[256];
    int verify_ssl;
    char ca_cert_path[512];
} service_instance_t;

/* 工具函数：获取线程 ID */
static inline unsigned long gettid(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)syscall(__NR_gettid);
#endif
}

#endif /* WORKER_SERVICE_V2_H */
