/**
 * 多线程工作服务示例
 * 
 * 功能：
 * 1. 启动时自动在网关注册
 * 2. 退出时自动在网关注销
 * 3. 处理业务请求（模拟具体工作）
 * 4. 支持优雅关闭（捕获 SIGINT/SIGTERM）
 * 
 * 编译：
 * gcc -o worker_service.exe worker_service.c -lcurl -lcjson -lpthread
 * 
 * 运行：
 * ./worker_service.exe [gateway_host] [gateway_port] [service_port]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <curl/curl.h>
#include <cJSON.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define sleep(seconds) Sleep((seconds) * 1000)
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

// 全局变量
static volatile int running = 1;
static CURL* g_curl = NULL;
static char gateway_host[256] = "localhost";
static int gateway_port = 8080;
static int service_port = 8081;
static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;

// 服务配置
typedef struct {
    char name[64];
    char path_prefix[64];
    int port;
    int is_https;
} service_config_t;

static service_config_t service_config = {
    .name = "worker-service",
    .path_prefix = "/api/worker",
    .port = 8081,
    .is_https = 0
};

// 信号处理函数
void signal_handler(int sig) {
    printf("\n[Worker] 收到退出信号：%d，正在优雅关闭...\n", sig);
    running = 0;
}

// 设置信号处理器
void setup_signal_handlers() {
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill 命令
#ifndef _WIN32
    signal(SIGHUP, signal_handler);   // 终端断开
#endif
}

/**
 * HTTP 请求回调函数（用于接收响应）
 */
size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    (void)userp;
    // 这里可以处理响应内容，我们暂时忽略
    return realsize;
}

/**
 * 在网关注册服务
 */
int register_to_gateway() {
    pthread_mutex_lock(&curl_lock);
    
    if (!g_curl) {
        pthread_mutex_unlock(&curl_lock);
        return -1;
    }
    
    // 构建注册 JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", service_config.name);
    cJSON_AddStringToObject(root, "path_prefix", service_config.path_prefix);
    cJSON_AddStringToObject(root, "host", "localhost");
    cJSON_AddNumberToObject(root, "port", service_config.port);
    cJSON_AddStringToObject(root, "protocol", service_config.is_https ? "https" : "http");
    cJSON_AddStringToObject(root, "health_endpoint", "/health");
    cJSON_AddBoolToObject(root, "verify_ssl", 0);
    cJSON_AddBoolToObject(root, "ipv6", 0);
    
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    // 构建 URL
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/register", 
             gateway_host, gateway_port);
    
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
    
    printf("[Worker] 正在注册到网关：%s\n", url);
    
    // 执行请求
    CURLcode res = curl_easy_perform(g_curl);
    
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    free(json_str);
    
    pthread_mutex_unlock(&curl_lock);
    
    if (res == CURLE_OK && (http_code == 200 || http_code == 201)) {
        printf("[Worker] ✓ 服务注册成功\n");
        return 0;
    } else {
        fprintf(stderr, "[Worker] ✗ 服务注册失败 (HTTP %ld): %s\n", 
                http_code, curl_easy_strerror(res));
        return -1;
    }
}

/**
 * 从网关注销服务
 */
int unregister_from_gateway() {
    pthread_mutex_lock(&curl_lock);
    
    if (!g_curl) {
        pthread_mutex_unlock(&curl_lock);
        return -1;
    }
    
    // 构建注销 JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", service_config.name);
    cJSON_AddStringToObject(root, "host", "localhost");
    cJSON_AddNumberToObject(root, "port", service_config.port);
    
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    // 构建 URL
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/unregister", 
             gateway_host, gateway_port);
    
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
    
    printf("[Worker] 正在从网关注销：%s\n", url);
    
    // 执行请求
    CURLcode res = curl_easy_perform(g_curl);
    
    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    free(json_str);
    
    pthread_mutex_unlock(&curl_lock);
    
    if (res == CURLE_OK && http_code == 200) {
        printf("[Worker] ✓ 服务注销成功\n");
        return 0;
    } else {
        fprintf(stderr, "[Worker] ✗ 服务注销失败 (HTTP %ld): %s\n", 
                http_code, curl_easy_strerror(res));
        return -1;
    }
}

/**
 * 业务处理：健康检查端点
 */
void handle_health(uv_buf_t* response) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "healthy");
    cJSON_AddStringToObject(root, "service", service_config.name);
    
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
}

/**
 * 业务处理：获取工作列表
 */
void handle_get_jobs(uv_buf_t* response) {
    cJSON* root = cJSON_CreateArray();
    
    // 模拟一些工作数据
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
}

/**
 * 业务处理：创建新工作（POST）
 */
void handle_create_job(const char* body, uv_buf_t* response) {
    printf("[Worker] 收到创建任务请求：%s\n", body ? body : "(空)");
    
    // 解析请求体
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

/**
 * 解析 HTTP 请求
 */
int parse_request(const char* buffer, size_t len, char** method, char** path, char** body) {
    // 简单解析 HTTP 请求行和头
    char method_str[16], path_str[256];
    int content_length = 0;
    
    // 解析请求行
    if (sscanf(buffer, "%15s %255s", method_str, path_str) != 2) {
        return -1;
    }
    
    *method = strdup(method_str);
    *path = strdup(path_str);
    
    // 查找 Content-Length
    const char* cl = strstr(buffer, "Content-Length:");
    if (cl) {
        content_length = atoi(cl + 15);
        
        // 查找空行（头和体的分隔）
        const char* body_start = strstr(buffer, "\r\n\r\n");
        if (body_start && content_length > 0) {
            body_start += 4;  // 跳过 \r\n\r\n
            *body = malloc(content_length + 1);
            strncpy(*body, body_start, content_length);
            (*body)[content_length] = '\0';
        }
    }
    
    return 0;
}

/**
 * 处理客户端连接
 */
void handle_client(int client_fd) {
    char buffer[4096];
    char* method = NULL;
    char* path = NULL;
    char* body = NULL;
    uv_buf_t response;
    
    // 读取请求
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        closesocket(client_fd);
        return;
    }
    buffer[bytes_read] = '\0';
    
    // 解析请求
    if (parse_request(buffer, bytes_read, &method, &path, &body) != 0) {
        closesocket(client_fd);
        return;
    }
    
    printf("[Worker] 收到请求：%s %s\n", method, path);
    
    // 路由处理
    if (strcmp(path, "/health") == 0) {
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
    
    // 发送响应
    send(client_fd, response.base, response.len, 0);
    
    // 清理
    if (method) free(method);
    if (path) free(path);
    if (body) free(body);
    if (response.base) free(response.base);
    
    closesocket(client_fd);
}

/**
 * TCP 服务器线程
 */
void* server_thread(void* arg) {
    (void)arg;
    
    int server_fd;
    struct sockaddr_in addr;
    int opt = 1;
    
    // 创建 socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return NULL;
    }
    
    // 设置 SO_REUSEADDR
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    // 绑定地址
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(service_config.port);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        closesocket(server_fd);
        return NULL;
    }
    
    // 开始监听
    if (listen(server_fd, 128) < 0) {
        perror("listen failed");
        closesocket(server_fd);
        return NULL;
    }
    
    printf("[Worker] ✓ 服务器正在监听端口 %d\n", service_config.port);
    
    // 接受连接
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // 设置超时
#ifdef _WIN32
        setsockopt(server_fd, SOL_SOCKET, SO_RCVVTIMEO, (const char*)&running, sizeof(running));
#else
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0 && running) {
            // 创建线程处理客户端
            pthread_t tid;
            pthread_create(&tid, NULL, (void* (*)(void*))handle_client, (void*)(intptr_t)client_fd);
            pthread_detach(tid);
        }
    }
    
    closesocket(server_fd);
    return NULL;
}

/**
 * 初始化 Winsock（仅 Windows）
 */
#ifdef _WIN32
int init_winsock() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

void cleanup_winsock() {
    WSACleanup();
}
#endif

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    // 解析命令行参数
    if (argc > 1) {
        strncpy(gateway_host, argv[1], sizeof(gateway_host) - 1);
    }
    if (argc > 2) {
        gateway_port = atoi(argv[2]);
    }
    if (argc > 3) {
        service_config.port = service_port = atoi(argv[3]);
    }
    
    printf("=== 多线程工作服务 ===\n");
    printf("网关地址：%s:%d\n", gateway_host, gateway_port);
    printf("服务端口：%d\n", service_config.port);
    printf("服务名称：%s\n", service_config.name);
    printf("路径前缀：%s\n", service_config.path_prefix);
    printf("\n");
    
#ifdef _WIN32
    // 初始化 Winsock
    if (init_winsock() != 0) {
        fprintf(stderr, "Winsock 初始化失败\n");
        return 1;
    }
#endif
    
    // 设置信号处理器
    setup_signal_handlers();
    
    // 初始化 CURL
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        fprintf(stderr, "CURL 初始化失败\n");
        return 1;
    }
    
    g_curl = curl_easy_init();
    if (!g_curl) {
        fprintf(stderr, "CURL 句柄创建失败\n");
        curl_global_cleanup();
        return 1;
    }
    
    // 注册到网关
    if (register_to_gateway() != 0) {
        fprintf(stderr, "警告：服务注册失败，但将继续运行\n");
    }
    
    // 启动服务器线程
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread, NULL);
    
    printf("[Worker] ✓ 服务启动完成，按 Ctrl+C 退出\n\n");
    
    // 主循环（等待退出信号）
    while (running) {
        sleep(1);
    }
    
    // 等待服务器线程结束
    pthread_join(server_tid, NULL);
    
    // 从网关注销
    printf("\n[Worker] 正在清理资源...\n");
    unregister_from_gateway();
    
    // 清理 CURL
    curl_easy_cleanup(g_curl);
    curl_global_cleanup();
    
#ifdef _WIN32
    cleanup_winsock();
#endif
    
    printf("[Worker] ✓ 服务已优雅关闭\n");
    
    return 0;
}
