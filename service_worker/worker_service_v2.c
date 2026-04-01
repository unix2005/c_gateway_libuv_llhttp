/**
 * 多线程工作服务 - 增强版
 * 
 * 功能特性:
 * 1. 支持 IPv4/IPv6 双栈
 * 2. 支持 HTTP/HTTPS (SSL/TLS)
 * 3. 从 JSON 配置文件读取所有配置
 * 4. 启动时自动在网关注册
 * 5. 退出时自动在网关注销
 * 6. 处理业务请求（模拟具体工作）
 * 7. 支持优雅关闭（捕获 SIGINT/SIGTERM）
 * 8. 支持日志文件输出
 * 9. 可配置线程数
 * 
 * 编译:
 * gcc -o worker_service_v2 worker_service_v2.c -luv -lcurl -lcjson -lpthread -lssl -lcrypto
 * 
 * 运行:
 * ./worker_service_v2 config.json
 */

#include "worker_service_v2.h"

/* 禁用未使用参数警告的宏 */
#define UNUSED(x) (void)(x)

/* ============================================================================
   函数前向声明
   ============================================================================ */
static void handle_health(uv_buf_t* response);
static void handle_get_jobs(uv_buf_t* response);
static void handle_create_job(const char* body, uv_buf_t* response);

/* 线程函数包装器 - 用于 pthread_create */
typedef struct {
    int client_fd;
} thread_arg_t;

static void* handle_client_wrapper(void* arg) {
    thread_arg_t* targ = (thread_arg_t*)arg;
    if (targ) {
        handle_client(targ->client_fd);
        free(targ);
    }
    return NULL;
}

#define MAX_PATH_LEN 512
#define MAX_HOST_LEN 256
#define MAX_LOG_LINE 1024

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
// 全局变量
// ============================================================================

static volatile int running = 1;
static CURL* g_curl = NULL;
static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* log_fp = NULL;
static service_config_t config;

// SSL 上下文（如果使用 HTTPS）
#ifdef HAVE_OPENSSL
    #include <openssl/ssl.h>
    static SSL_CTX* ssl_ctx = NULL;
#endif

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
    if ((int)level < (int)config.log_level) {  /* 类型转换避免符号比较警告 */
        return;
    }
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    va_list args;
    va_start(args, format);
    
    pthread_mutex_lock(&log_lock);
    
    // 输出到控制台
    if (config.log_to_console) {
        printf("[%s] [%s] ", time_buf, log_level_str[level]);
        vprintf(format, args);
        printf("\n");
        fflush(stdout);
    }
    
    // 输出到文件
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
    else strcpy(config.name, "worker-service");
    
    item = cJSON_GetObjectItem(root, "description");
    if (item) strncpy(config.description, item->valuestring, sizeof(config.description) - 1);
    else strcpy(config.description, "多线程工作服务");
    
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
        strcpy(config.log_file, "worker.log");
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
// SSL/TLS 初始化
// ============================================================================

int init_ssl() {
    if (!config.is_https) {
        return 0;
    }
    
#ifdef HAVE_OPENSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    
    ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx) {
        LOG_ERROR("创建 SSL 上下文失败");
        return -1;
    }
    
    // 加载证书
    if (strlen(config.ssl_cert_file) > 0 && strlen(config.ssl_key_file) > 0) {
        if (SSL_CTX_use_certificate_file(ssl_ctx, config.ssl_cert_file, SSL_FILETYPE_PEM) <= 0) {
            LOG_ERROR("加载证书失败：%s", config.ssl_cert_file);
            return -1;
        }
        
        if (SSL_CTX_use_PrivateKey_file(ssl_ctx, config.ssl_key_file, SSL_FILETYPE_PEM) <= 0) {
            LOG_ERROR("加载私钥失败：%s", config.ssl_key_file);
            return -1;
        }
        
        if (!SSL_CTX_check_private_key(ssl_ctx)) {
            LOG_ERROR("私钥与证书不匹配");
            return -1;
        }
        
        LOG_INFO("SSL/TLS 初始化成功");
    } else {
        LOG_WARN("未配置 SSL 证书，将使用普通 HTTP");
        config.is_https = 0;
    }
    
    return 0;
#else
    LOG_WARN("未编译 OpenSSL 支持，将使用普通 HTTP");
    config.is_https = 0;
    return 0;
#endif
}

// ============================================================================
// 信号处理
// ============================================================================

void signal_handler(int sig) {
    LOG_INFO("收到退出信号：%d，正在优雅关闭...", sig);
    running = 0;
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGHUP, signal_handler);
#endif
}

// ============================================================================
// HTTP 回调函数
// ============================================================================

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    UNUSED(contents);  /* 有意未使用 */
    UNUSED(userp);
    return size * nmemb;
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
    
    // 构建注册 JSON
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
    
    // 构建 URL
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/register", 
             config.gateway_host, config.gateway_port);
    
    // 设置 HTTP 头
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    // 配置 curl
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 5L);
    
    LOG_INFO("正在注册到网关：%s", url);
    
    // 执行请求
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
    
    // 构建注销 JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", config.name);
    cJSON_AddStringToObject(root, "host", config.is_ipv6 ? "::1" : "localhost");
    cJSON_AddNumberToObject(root, "port", config.port);
    
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    // 构建 URL
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/unregister", 
             config.gateway_host, config.gateway_port);
    
    // 设置 HTTP 头
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    // 配置 curl
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 5L);
    
    LOG_INFO("正在从网关注销：%s", url);
    
    // 执行请求
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
// 业务处理函数
// ============================================================================

void handle_health(uv_buf_t* response) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "healthy");
    cJSON_AddStringToObject(root, "service", config.name);
    cJSON_AddStringToObject(root, "description", config.description);
    cJSON_AddBoolToObject(root, "ipv6_enabled", config.is_ipv6);
    cJSON_AddBoolToObject(root, "https_enabled", config.is_https);
    
    char* json_str = cJSON_PrintUnformatted(root);
    
    const char* http_response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s";
    
    int len = snprintf(NULL, 0, http_response, json_str);
    response->base = malloc(len + 1);
    sprintf(response->base, http_response, json_str);
    response->len = len;
    
    free(json_str);
    cJSON_Delete(root);
    
    LOG_DEBUG("处理健康检查请求");
}

void handle_get_jobs(uv_buf_t* response) {
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
    
    char* json_str = cJSON_PrintUnformatted(root);
    
    const char* http_response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s";
    
    int len = snprintf(NULL, 0, http_response, json_str);
    response->base = malloc(len + 1);
    sprintf(response->base, http_response, json_str);
    response->len = len;
    
    free(json_str);
    cJSON_Delete(root);
    
    LOG_DEBUG("处理获取任务列表请求");
}

void handle_create_job(const char* body, uv_buf_t* response) {
    LOG_INFO("收到创建任务请求：%s", body ? body : "(空)");
    
    cJSON* req = NULL;
    if (body) {
        req = cJSON_Parse(body);
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
    
    char* json_str = cJSON_PrintUnformatted(root);
    
    const char* http_response = 
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: application/json\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s";
    
    int len = snprintf(NULL, 0, http_response, json_str);
    response->base = malloc(len + 1);
    sprintf(response->base, http_response, json_str);
    response->len = len;
    
    free(json_str);
    cJSON_Delete(root);
}

// ============================================================================
// HTTP 请求解析
// ============================================================================

int parse_request(const char* buffer, size_t len, char** method, char** path, char** body) {
    UNUSED(len);  /* 当前实现未使用长度参数 */
    
    char method_str[16], path_str[256];
    int content_length = 0;
    
    if (sscanf(buffer, "%15s %255s", method_str, path_str) != 2) {
        return -1;
    }
    
    *method = strdup(method_str);
    *path = strdup(path_str);
    
    const char* cl = strstr(buffer, "Content-Length:");
    if (cl) {
        content_length = atoi(cl + 15);
        
        const char* body_start = strstr(buffer, "\r\n\r\n");
        if (body_start && content_length > 0) {
            body_start += 4;
            *body = malloc(content_length + 1);
            strncpy(*body, body_start, content_length);
            (*body)[content_length] = '\0';
        }
    }
    
    return 0;
}

// ============================================================================
// 客户端连接处理
// ============================================================================

void handle_client(int client_fd) {
    char buffer[4096];
    char* method = NULL;
    char* path = NULL;
    char* body = NULL;
    uv_buf_t response;
    
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        closesocket(client_fd);
        return;
    }
    buffer[bytes_read] = '\0';
    
    if (parse_request(buffer, bytes_read, &method, &path, &body) != 0) {
        closesocket(client_fd);
        return;
    }
    
    LOG_DEBUG("收到请求：%s %s", method, path);
    
    if (strcmp(path, config.health_endpoint) == 0) {
        handle_health(&response);
    } else if (strcmp(path, "/api/worker/jobs") == 0 && strcmp(method, "GET") == 0) {
        handle_get_jobs(&response);
    } else if (strcmp(path, "/api/worker/jobs") == 0 && strcmp(method, "POST") == 0) {
        handle_create_job(body, &response);
    } else {
        const char* not_found = 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "Not Found";
        response.base = strdup(not_found);
        response.len = strlen(not_found);
    }
    
    send(client_fd, response.base, response.len, 0);
    
    if (method) free(method);
    if (path) free(path);
    if (body) free(body);
    if (response.base) free(response.base);
    
    closesocket(client_fd);
}

// ============================================================================
// TCP 服务器线程
// ============================================================================

void* server_thread(void* arg) {
    (void)arg;
    
    int server_fd;
    struct sockaddr_in6 addr6;
    struct sockaddr_in addr4;
    int opt = 1;
    
    // 创建 socket (支持 IPv6)
    if (config.is_ipv6) {
        server_fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (server_fd < 0) {
            LOG_ERROR("创建 IPv6 socket 失败");
            return NULL;
        }
        
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_any;  // ::
        addr6.sin6_port = htons(config.port);
        
        if (bind(server_fd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
            LOG_ERROR("IPv6 bind 失败");
            closesocket(server_fd);
            return NULL;
        }
        
        LOG_INFO("✓ IPv6 服务器正在监听端口 %d", config.port);
    } else {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            LOG_ERROR("创建 IPv4 socket 失败");
            return NULL;
        }
        
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = INADDR_ANY;
        addr4.sin_port = htons(config.port);
        
        if (bind(server_fd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
            LOG_ERROR("IPv4 bind 失败");
            closesocket(server_fd);
            return NULL;
        }
        
        LOG_INFO("✓ IPv4 服务器正在监听端口 %d", config.port);
    }
    
    if (listen(server_fd, config.max_connections) < 0) {
        LOG_ERROR("listen 失败");
        closesocket(server_fd);
        return NULL;
    }
    
    while (running) {
        struct sockaddr_in6 client_addr6;
        struct sockaddr_in client_addr4;
        socklen_t client_len;
        
#ifdef _WIN32
        setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&running, sizeof(running));
#else
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        
        if (config.is_ipv6) {
            client_len = sizeof(client_addr6);
        } else {
            client_len = sizeof(client_addr4);
        }
        
        int client_fd;
        if (config.is_ipv6) {
            client_fd = accept(server_fd, (struct sockaddr*)&client_addr6, &client_len);
        } else {
            client_fd = accept(server_fd, (struct sockaddr*)&client_addr4, &client_len);
        }
        
        if (client_fd >= 0 && running) {
            LOG_DEBUG("接受客户端连接");
            
            /* 分配线程参数 */
            thread_arg_t* targ = malloc(sizeof(thread_arg_t));
            if (targ) {
                targ->client_fd = client_fd;
                pthread_t tid;
                pthread_create(&tid, NULL, handle_client_wrapper, targ);
                pthread_detach(tid);
            } else {
                closesocket(client_fd);  /* 内存分配失败，关闭连接 */
            }
        }
    }
    
    closesocket(server_fd);
    return NULL;
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
    
    printf("=== 多线程工作服务 (增强版) ===\n\n");
    
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
    
    // 初始化 SSL/TLS
    if (config.is_https && init_ssl() != 0) {
        LOG_ERROR("SSL 初始化失败，退出");
        return 1;
    }
    
    // 设置信号处理器
    setup_signal_handlers();
    
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
    
    // 启动服务器线程
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread, NULL);
    
    LOG_INFO("✓ 服务启动完成，按 Ctrl+C 退出");
    LOG_INFO("服务信息：");
    LOG_INFO("  名称：%s", config.name);
    LOG_INFO("  协议：%s", config.is_https ? "HTTPS" : "HTTP");
    LOG_INFO("  IP 版本：%s", config.is_ipv6 ? "IPv6" : "IPv4");
    LOG_INFO("  端口：%d", config.port);
    LOG_INFO("  路径前缀：%s", config.path_prefix);
    LOG_INFO("  线程数：%d", config.thread_count);
    LOG_INFO("  网关：%s:%d", config.gateway_host, config.gateway_port);
    printf("\n");
    
    // 主循环
    while (running) {
        sleep(1);
    }
    
    // 清理
    LOG_INFO("正在关闭服务器...");
    pthread_join(server_tid, NULL);
    
    LOG_INFO("正在从网关注销...");
    unregister_from_gateway();
    
    if (log_fp) fclose(log_fp);
    curl_easy_cleanup(g_curl);
    curl_global_cleanup();
    
#ifdef HAVE_OPENSSL
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
#endif
    
#ifdef _WIN32
    cleanup_winsock();
#endif
    
    LOG_INFO("✓ 服务已优雅关闭");
    
    return 0;
}
