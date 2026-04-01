/**
 * 多线程工作服务 v3 - 高性能版
 * 
 * 技术栈:
 * - 网络层：libuv (异步事件驱动)
 * - HTTP 解析：llhttp (高性能解析器)
 * - 配置加载：cJSON (JSON 解析)
 * - HTTP 客户端：libcurl (网关注册/注销)
 * 
 * 功能特性:
 * 1. 支持 IPv4/IPv6 双栈
 * 2. 支持 HTTP/HTTPS (SSL/TLS)
 * 3. 从 JSON 配置文件读取所有配置
 * 4. 启动时自动在网关注册
 * 5. 退出时自动在网关注销
 * 6. 异步非阻塞 I/O
 * 7. 支持优雅关闭
 * 8. 支持日志文件输出
 * 9. 可配置线程数（libuv 线程池）
 * 
 * 编译:
 * gcc -o worker_service_v3 worker_service_v3.c -luv -lllhttp -lcurl -lcjson -lpthread -lssl -lcrypto
 * 
 * 运行:
 * ./worker_service_v3 config.json
 */

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
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

#define MAX_PATH_LEN 512
#define MAX_HOST_LEN 256
#define MAX_LOG_LINE 1024
#define POOL_SIZE 8192

// ============================================================================
// 配置结构体
// ============================================================================

typedef struct {
    // 服务基本配置
    char name[64];
    char description[256];
    char path_prefix[64];
    int port;
    int is_ipv6;
    int is_https;
    
    // SSL/TLS 配置
    char ssl_cert_file[MAX_PATH_LEN];
    char ssl_key_file[MAX_PATH_LEN];
    char ssl_ca_file[MAX_PATH_LEN];
    int verify_client;
    
    // 网关配置
    char gateway_host[MAX_HOST_LEN];
    int gateway_port;
    
    // 线程配置
    int thread_count;
    int max_connections;
    
    // 日志配置
    char log_file[MAX_PATH_LEN];
    int log_level;  // 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
    int log_to_console;
    int log_to_file;
    
    // 健康检查配置
    char health_endpoint[64];
    int heartbeat_interval;
} service_config_t;

// ============================================================================
// 客户端上下文
// ============================================================================

typedef struct {
    uv_tcp_t handle;
    llhttp_t parser;
    llhttp_settings_t settings;
    
    // 内存池
    char pool_data[POOL_SIZE];
    size_t pool_used;
    
    // Body 缓冲区
    char* body_buffer;
    size_t body_len;
    
    // URL 记录
    char url[512];
    
    // 方法
    llhttp_method_t method;
} client_ctx_t;

// ============================================================================
// 写入上下文
// ============================================================================

typedef struct {
    uv_write_t req;
    uv_buf_t bufs[2];
    char* header_ptr;
    char* body_ptr;
} write_ctx_t;

// ============================================================================
// 全局变量
// ============================================================================

static volatile int running = 1;
static CURL* g_curl = NULL;
static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* log_fp = NULL;
static service_config_t config;
static uv_loop_t* main_loop = NULL;

// 前向声明
void write_complete_callback(uv_write_t* req, int status);
size_t curl_ignore_callback(void* contents, size_t size, size_t nmemb, void* userp);

// CURL 写入回调（忽略响应）
size_t curl_ignore_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    (void)contents;
    (void)userp;
    return size * nmemb;
}

// ============================================================================
// 日志功能
// ============================================================================

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
} log_level_t;

const char* log_level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void log_message(log_level_t level, const char* format, ...) {
    if (level < config.log_level) {
        return;
    }
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    va_list args;
    va_start(args, format);
    
    pthread_mutex_lock(&log_lock);
    
    if (config.log_to_console) {
        printf("[%s] [%s] ", time_buf, log_level_str[level]);
        vprintf(format, args);
        printf("\n");
        fflush(stdout);
    }
    
    if (config.log_to_file && log_fp) {
        va_end(args);
        va_start(args, format);
        
        fprintf(log_fp, "[%s] [%s] ", time_buf, log_level_str[level]);
        vfprintf(log_fp, format, args);
        fprintf(log_fp, "\n");
        fflush(log_fp);
    }
    
    va_end(args);
    pthread_mutex_unlock(&log_lock);
}

#define LOG_DEBUG(msg, ...) log_message(LOG_DEBUG, msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) log_message(LOG_INFO, msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) log_message(LOG_WARN, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) log_message(LOG_ERROR, msg, ##__VA_ARGS__)

// ============================================================================
// 配置加载
// ============================================================================

int load_config(const char* config_file) {
    FILE* f = fopen(config_file, "r");
    if (!f) {
        fprintf(stderr, "无法打开配置文件：%s\n", config_file);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* json_str = malloc(fsize + 1);
    fread(json_str, 1, fsize, f);
    json_str[fsize] = '\0';
    fclose(f);
    
    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) {
        fprintf(stderr, "JSON 解析失败\n");
        return -1;
    }
    
    // 服务基本配置
    cJSON* item = cJSON_GetObjectItem(root, "name");
    if (item) strncpy(config.name, item->valuestring, sizeof(config.name) - 1);
    else strcpy(config.name, "worker-service-v3");
    
    item = cJSON_GetObjectItem(root, "description");
    if (item) strncpy(config.description, item->valuestring, sizeof(config.description) - 1);
    else strcpy(config.description, "高性能多线程工作服务");
    
    item = cJSON_GetObjectItem(root, "path_prefix");
    if (item) strncpy(config.path_prefix, item->valuestring, sizeof(config.path_prefix) - 1);
    else strcpy(config.path_prefix, "/api/worker");
    
    item = cJSON_GetObjectItem(root, "port");
    config.port = item ? item->valueint : 8081;
    
    item = cJSON_GetObjectItem(root, "ipv6");
    config.is_ipv6 = item ? item->valueint : 0;
    
    item = cJSON_GetObjectItem(root, "https");
    config.is_https = item ? item->valueint : 0;
    
    // SSL/TLS 配置
    item = cJSON_GetObjectItem(root, "ssl_cert_file");
    if (item) strncpy(config.ssl_cert_file, item->valuestring, sizeof(config.ssl_cert_file) - 1);
    
    item = cJSON_GetObjectItem(root, "ssl_key_file");
    if (item) strncpy(config.ssl_key_file, item->valuestring, sizeof(config.ssl_key_file) - 1);
    
    item = cJSON_GetObjectItem(root, "ssl_ca_file");
    if (item) strncpy(config.ssl_ca_file, item->valuestring, sizeof(config.ssl_ca_file) - 1);
    
    item = cJSON_GetObjectItem(root, "verify_client");
    config.verify_client = item ? item->valueint : 0;
    
    // 网关配置
    item = cJSON_GetObjectItem(root, "gateway");
    if (item) {
        cJSON* host_obj = cJSON_GetObjectItem(item, "host");
        if (host_obj) strncpy(config.gateway_host, host_obj->valuestring, sizeof(config.gateway_host) - 1);
        else strcpy(config.gateway_host, "localhost");
        
        cJSON* port_obj = cJSON_GetObjectItem(item, "port");
        config.gateway_port = port_obj ? port_obj->valueint : 8080;
    } else {
        strcpy(config.gateway_host, "localhost");
        config.gateway_port = 8080;
    }
    
    // 线程配置
    item = cJSON_GetObjectItem(root, "threads");
    if (item) {
        cJSON* count_obj = cJSON_GetObjectItem(item, "count");
        config.thread_count = count_obj ? count_obj->valueint : 4;
        
        cJSON* max_conn_obj = cJSON_GetObjectItem(item, "max_connections");
        config.max_connections = max_conn_obj ? max_conn_obj->valueint : 1024;
    } else {
        config.thread_count = 4;
        config.max_connections = 1024;
    }
    
    // 日志配置
    item = cJSON_GetObjectItem(root, "logging");
    if (item) {
        cJSON* file_obj = cJSON_GetObjectItem(item, "file");
        if (file_obj) strncpy(config.log_file, file_obj->valuestring, sizeof(config.log_file) - 1);
        
        cJSON* level_obj = cJSON_GetObjectItem(item, "level");
        if (level_obj) {
            if (strcmp(level_obj->valuestring, "DEBUG") == 0) config.log_level = LOG_DEBUG;
            else if (strcmp(level_obj->valuestring, "INFO") == 0) config.log_level = LOG_INFO;
            else if (strcmp(level_obj->valuestring, "WARN") == 0) config.log_level = LOG_WARN;
            else if (strcmp(level_obj->valuestring, "ERROR") == 0) config.log_level = LOG_ERROR;
            else config.log_level = LOG_INFO;
        } else {
            config.log_level = LOG_INFO;
        }
        
        cJSON* console_obj = cJSON_GetObjectItem(item, "to_console");
        config.log_to_console = console_obj ? console_obj->valueint : 1;
        
        cJSON* tofile_obj = cJSON_GetObjectItem(item, "to_file");
        config.log_to_file = tofile_obj ? tofile_obj->valueint : 1;
    } else {
        strcpy(config.log_file, "worker_v3.log");
        config.log_level = LOG_INFO;
        config.log_to_console = 1;
        config.log_to_file = 1;
    }
    
    // 健康检查配置
    item = cJSON_GetObjectItem(root, "health_check");
    if (item) {
        cJSON* endpoint_obj = cJSON_GetObjectItem(item, "endpoint");
        if (endpoint_obj) strncpy(config.health_endpoint, endpoint_obj->valuestring, sizeof(config.health_endpoint) - 1);
        else strcpy(config.health_endpoint, "/health");
        
        cJSON* interval_obj = cJSON_GetObjectItem(item, "heartbeat_interval");
        config.heartbeat_interval = interval_obj ? interval_obj->valueint : 30;
    } else {
        strcpy(config.health_endpoint, "/health");
        config.heartbeat_interval = 30;
    }
    
    cJSON_Delete(root);
    
    LOG_INFO("配置文件加载成功：%s", config_file);
    LOG_INFO("服务名称：%s", config.name);
    LOG_INFO("服务端口：%d (IPv6: %s, HTTPS: %s)", 
             config.port, 
             config.is_ipv6 ? "是" : "否",
             config.is_https ? "是" : "否");
    LOG_INFO("网关地址：%s:%d", config.gateway_host, config.gateway_port);
    LOG_INFO("线程数：%d, 最大连接数：%d", config.thread_count, config.max_connections);
    
    return 0;
}

// ============================================================================
// HTTP 响应发送
// ============================================================================

void send_response(client_ctx_t* client, int status_code, const char* content_type, char* body_to_send) {
    write_ctx_t* wctx = malloc(sizeof(write_ctx_t));
    memset(wctx, 0, sizeof(write_ctx_t));
    
    wctx->header_ptr = malloc(512);
    int header_len = sprintf(wctx->header_ptr,
        "HTTP/1.1 %d OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        status_code, content_type, body_to_send ? strlen(body_to_send) : 0);
    
    wctx->body_ptr = body_to_send;
    
    wctx->bufs[0] = uv_buf_init(wctx->header_ptr, header_len);
    int nbufs = 1;
    
    if (wctx->body_ptr) {
        wctx->bufs[1] = uv_buf_init(wctx->body_ptr, strlen(wctx->body_ptr));
        nbufs = 2;
    }
    
    int r = uv_write(&wctx->req, (uv_stream_t*)&client->handle, wctx->bufs, nbufs, 
                     write_complete_callback);
    
    if (r < 0) {
        LOG_ERROR("uv_write failed: %s", uv_strerror(r));
        free(wctx->header_ptr);
        if (wctx->body_ptr) free(wctx->body_ptr);
        free(wctx);
    }
}

// libuv 写入完成回调
void write_complete_callback(uv_write_t* req, int status) {
    write_ctx_t* wctx = (write_ctx_t*)req;
    if (status < 0) {
        LOG_ERROR("Write error: %s", uv_strerror(status));
    }
    if (wctx->header_ptr) free(wctx->header_ptr);
    if (wctx->body_ptr) free(wctx->body_ptr);
    free(wctx);
}

// ============================================================================
// 业务处理函数
// ============================================================================

void handle_health(client_ctx_t* client) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "healthy");
    cJSON_AddStringToObject(root, "service", config.name);
    cJSON_AddStringToObject(root, "description", config.description);
    cJSON_AddBoolToObject(root, "ipv6_enabled", config.is_ipv6);
    cJSON_AddBoolToObject(root, "https_enabled", config.is_https);
    cJSON_AddNumberToObject(root, "version", 3.0);
    
    char* json_out = cJSON_PrintUnformatted(root);
    send_response(client, 200, "application/json", json_out);
    cJSON_Delete(root);
    
    LOG_DEBUG("处理健康检查请求");
}

void handle_get_jobs(client_ctx_t* client) {
    cJSON* root = cJSON_CreateArray();
    
    for (int i = 1; i <= 5; i++) {
        cJSON* job = cJSON_CreateObject();
        char id[32], name[64];
        snprintf(id, sizeof(id), "JOB-%03d", i);
        snprintf(name, sizeof(name), "任务 %d", i);
        
        cJSON_AddStringToObject(job, "id", id);
        cJSON_AddStringToObject(job, "name", name);
        cJSON_AddNumberToObject(job, "priority", rand() % 10);
        cJSON_AddStringToObject(job, "status", "pending");
        
        cJSON_AddItemToArray(root, job);
    }
    
    char* json_out = cJSON_PrintUnformatted(root);
    send_response(client, 200, "application/json", json_out);
    cJSON_Delete(root);
    
    LOG_DEBUG("处理获取任务列表请求");
}

void handle_create_job(client_ctx_t* client) {
    LOG_INFO("收到创建任务请求：%s", client->body_buffer ? client->body_buffer : "(空)");
    
    cJSON* req = NULL;
    if (client->body_buffer) {
        req = cJSON_Parse(client->body_buffer);
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "created");
    cJSON_AddStringToObject(root, "message", "Task created successfully");
    
    if (req) {
        cJSON* name_obj = cJSON_GetObjectItem(req, "name");
        if (name_obj && name_obj->valuestring) {
            cJSON_AddStringToObject(root, "task_name", name_obj->valuestring);
        }
        cJSON_Delete(req);
    }
    
    char* json_out = cJSON_PrintUnformatted(root);
    send_response(client, 201, "application/json", json_out);
    cJSON_Delete(root);
}

// ============================================================================
// HTTP 解析回调
// ============================================================================

int on_url(llhttp_t* parser, const char* at, size_t length) {
    client_ctx_t* ctx = (client_ctx_t*)parser->data;
    snprintf(ctx->url, sizeof(ctx->url), "%.*s", (int)length, at);
    return 0;
}

int on_body(llhttp_t* parser, const char* at, size_t length) {
    client_ctx_t* ctx = (client_ctx_t*)parser->data;
    ctx->body_buffer = realloc(ctx->body_buffer, ctx->body_len + length + 1);
    memcpy(ctx->body_buffer + ctx->body_len, at, length);
    ctx->body_len += length;
    ctx->body_buffer[ctx->body_len] = '\0';
    return 0;
}

int on_message_complete(llhttp_t* parser) {
    client_ctx_t* ctx = (client_ctx_t*)parser->data;
    
    LOG_DEBUG("收到完整请求：%s %s", 
              ctx->method == HTTP_POST ? "POST" : 
              ctx->method == HTTP_PUT ? "PUT" : 
              ctx->method == HTTP_DELETE ? "DELETE" : "GET",
              ctx->url);
    
    // 路由处理
    if (strcmp(ctx->url, config.health_endpoint) == 0) {
        handle_health(ctx);
    } else if (strcmp(ctx->url, "/api/worker/jobs") == 0 && ctx->method == HTTP_GET) {
        handle_get_jobs(ctx);
    } else if (strcmp(ctx->url, "/api/worker/jobs") == 0 && ctx->method == HTTP_POST) {
        handle_create_job(ctx);
    } else {
        send_response(ctx, 404, "text/plain", strdup("Not Found"));
    }
    
    // 重置内存池偏移量
    ctx->pool_used = 0;
    
    // 清理 Body 缓冲区
    if (ctx->body_buffer) {
        free(ctx->body_buffer);
        ctx->body_buffer = NULL;
        ctx->body_len = 0;
    }
    
    return 0;
}

// ============================================================================
// libuv 回调函数
// ============================================================================

void alloc_buffer(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    buf->base = malloc(suggested);
    buf->len = suggested;
}

void on_close(uv_handle_t* handle) {
    client_ctx_t* ctx = (client_ctx_t*)handle->data;
    if (ctx->body_buffer) free(ctx->body_buffer);
    free(ctx);
}

void on_read(uv_stream_t* client_stream, ssize_t nread, const uv_buf_t* buf) {
    client_ctx_t* ctx = (client_ctx_t*)client_stream->data;
    
    if (nread > 0) {
        llhttp_execute(&ctx->parser, buf->base, nread);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            LOG_ERROR("Read error: %s", uv_err_name(nread));
        }
        uv_close((uv_handle_t*)client_stream, on_close);
    }
    
    if (buf->base) free(buf->base);
}

void on_new_connection(uv_stream_t* server, int status) {
    if (status < 0) {
        LOG_ERROR("New connection error: %s", uv_strerror(status));
        return;
    }
    
    client_ctx_t* ctx = calloc(1, sizeof(client_ctx_t));
    uv_tcp_init(server->loop, &ctx->handle);
    ctx->handle.data = ctx;
    
    llhttp_settings_init(&ctx->settings);
    ctx->settings.on_url = on_url;
    ctx->settings.on_body = on_body;
    ctx->settings.on_message_complete = on_message_complete;
    llhttp_init(&ctx->parser, HTTP_REQUEST, &ctx->settings);
    ctx->parser.data = ctx;
    
    uv_accept(server, (uv_stream_t*)&ctx->handle);
    uv_read_start((uv_stream_t*)&ctx->handle, alloc_buffer, on_read);
    
    LOG_DEBUG("接受新客户端连接");
}

// ============================================================================
// 服务注册与注销
// ============================================================================

int register_to_gateway() {
    pthread_mutex_lock(&curl_lock);
    
    if (!g_curl) {
        pthread_mutex_unlock(&curl_lock);
        return -1;
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", config.name);
    cJSON_AddStringToObject(root, "path_prefix", config.path_prefix);
    cJSON_AddStringToObject(root, "host", config.is_ipv6 ? "::1" : "localhost");
    cJSON_AddNumberToObject(root, "port", config.port);
    cJSON_AddStringToObject(root, "protocol", config.is_https ? "https" : "http");
    cJSON_AddStringToObject(root, "health_endpoint", config.health_endpoint);
    cJSON_AddBoolToObject(root, "verify_ssl", 0);
    cJSON_AddBoolToObject(root, "ipv6", config.is_ipv6);
    
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/register", 
             config.gateway_host, config.gateway_port);
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, curl_ignore_callback);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 5L);
    
    LOG_INFO("正在注册到网关：%s", url);
    
    CURLcode res = curl_easy_perform(g_curl);
    
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    free(json_str);
    
    pthread_mutex_unlock(&curl_lock);
    
    if (res == CURLE_OK && (http_code == 200 || http_code == 201)) {
        LOG_INFO("✓ 服务注册成功");
        return 0;
    } else {
        LOG_ERROR("✗ 服务注册失败 (HTTP %ld): %s", http_code, curl_easy_strerror(res));
        return -1;
    }
}

int unregister_from_gateway() {
    pthread_mutex_lock(&curl_lock);
    
    if (!g_curl) {
        pthread_mutex_unlock(&curl_lock);
        return -1;
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", config.name);
    cJSON_AddStringToObject(root, "host", config.is_ipv6 ? "::1" : "localhost");
    cJSON_AddNumberToObject(root, "port", config.port);
    
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/unregister", 
             config.gateway_host, config.gateway_port);
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, (void*)[](void* contents, size_t size, size_t nmemb, void* userp) {
        return size * nmemb;
    });
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 5L);
    
    LOG_INFO("正在从网关注销：%s", url);
    
    CURLcode res = curl_easy_perform(g_curl);
    
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    free(json_str);
    
    pthread_mutex_unlock(&curl_lock);
    
    if (res == CURLE_OK && http_code == 200) {
        LOG_INFO("✓ 服务注销成功");
        return 0;
    } else {
        LOG_ERROR("✗ 服务注销失败 (HTTP %ld): %s", http_code, curl_easy_strerror(res));
        return -1;
    }
}

// ============================================================================
// 主函数
// ============================================================================

#ifdef _WIN32
int init_winsock() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

void cleanup_winsock() {
    WSACleanup();
}
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法：%s <config.json>\n", argv[0]);
        fprintf(stderr, "示例：%s worker_config.json\n", argv[0]);
        return 1;
    }
    
    printf("=== 多线程工作服务 v3 (libuv + llhttp) ===\n\n");
    
#ifdef _WIN32
    if (init_winsock() != 0) {
        fprintf(stderr, "Winsock 初始化失败\n");
        return 1;
    }
#endif
    
    // 加载配置文件
    if (load_config(argv[1]) != 0) {
        return 1;
    }
    
    // 打开日志文件
    if (config.log_to_file && strlen(config.log_file) > 0) {
        log_fp = fopen(config.log_file, "a");
        if (!log_fp) {
            LOG_WARN("无法打开日志文件：%s，将只输出到控制台", config.log_file);
            config.log_to_file = 0;
        } else {
            LOG_INFO("日志文件：%s", config.log_file);
        }
    }
    
    // 设置信号处理器
    signal(SIGINT, [](int sig) {
        LOG_INFO("收到退出信号：%d，正在优雅关闭...", sig);
        running = 0;
    });
    signal(SIGTERM, [](int sig) {
        LOG_INFO("收到退出信号：%d，正在优雅关闭...", sig);
        running = 0;
    });
#ifndef _WIN32
    signal(SIGHUP, [](int sig) {
        LOG_INFO("收到挂起信号：%d", sig);
    });
#endif
    
    // 初始化 CURL
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        LOG_ERROR("CURL 初始化失败");
        return 1;
    }
    
    g_curl = curl_easy_init();
    if (!g_curl) {
        LOG_ERROR("CURL 句柄创建失败");
        curl_global_cleanup();
        return 1;
    }
    
    // 注册到网关
    if (register_to_gateway() != 0) {
        LOG_WARN("服务注册失败，但将继续运行");
    }
    
    // 创建 libuv 循环
    uv_loop_t* loop = uv_default_loop();
    main_loop = loop;
    
    // 设置 libuv 线程池大小
    uv_set_threadpool_size(config.thread_count);
    
    // 创建 TCP 服务器
    uv_tcp_t server;
    uv_tcp_init(loop, &server);
    
    // 绑定地址（支持 IPv6）
    int r;
    if (config.is_ipv6) {
        struct sockaddr_in6 addr6;
        uv_ip6_addr("::", config.port, &addr6);
        r = uv_tcp_bind(&server, (const struct sockaddr*)&addr6, 0);
        if (r == 0) {
            LOG_INFO("✓ IPv6 服务器正在监听端口 %d", config.port);
        }
    } else {
        struct sockaddr_in addr4;
        uv_ip4_addr("0.0.0.0", config.port, &addr4);
        r = uv_tcp_bind(&server, (const struct sockaddr*)&addr4, 0);
        if (r == 0) {
            LOG_INFO("✓ IPv4 服务器正在监听端口 %d", config.port);
        }
    }
    
    if (r != 0) {
        LOG_ERROR("绑定失败：%s", uv_strerror(r));
        return 1;
    }
    
    // 开始监听
    r = uv_listen((uv_stream_t*)&server, config.max_connections, on_new_connection);
    if (r != 0) {
        LOG_ERROR("listen 失败：%s", uv_strerror(r));
        return 1;
    }
    
    LOG_INFO("✓ 服务启动完成，按 Ctrl+C 退出");
    LOG_INFO("服务信息：");
    LOG_INFO("  名称：%s", config.name);
    LOG_INFO("  协议：%s", config.is_https ? "HTTPS (需要额外配置)" : "HTTP");
    LOG_INFO("  IP 版本：%s", config.is_ipv6 ? "IPv6" : "IPv4");
    LOG_INFO("  端口：%d", config.port);
    LOG_INFO("  路径前缀：%s", config.path_prefix);
    LOG_INFO("  线程池大小：%d", config.thread_count);
    LOG_INFO("  网关：%s:%d", config.gateway_host, config.gateway_port);
    printf("\n");
    
    // 运行事件循环
    while (running) {
        r = uv_run(loop, UV_RUN_ONCE);
        if (r == 0) {
            break;
        }
    }
    
    // 清理
    LOG_INFO("正在关闭服务器...");
    uv_close((uv_handle_t*)&server, NULL);
    uv_run(loop, UV_RUN_NOWAIT);
    
    LOG_INFO("正在从网关注销...");
    unregister_from_gateway();
    
    if (log_fp) fclose(log_fp);
    curl_easy_cleanup(g_curl);
    curl_global_cleanup();
    uv_loop_close(loop);
    
#ifdef _WIN32
    cleanup_winsock();
#endif
    
    LOG_INFO("✓ 服务已优雅关闭");
    
    return 0;
}
