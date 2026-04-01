#include <stdio.h>
#include <curl/curl.h>
#include <cJSON.h>
#include <string.h>

/**
 * 动态注册服务到网关
 * 
 * 用法：
 * register_service localhost 8080 my-service /api/myservice localhost 8081 http false false
 */
int register_service(const char* gateway_host, int gateway_port,
                     const char* service_name, const char* path_prefix,
                     const char* service_host, int service_port,
                     const char* protocol, const char* health_endpoint,
                     int verify_ssl, int is_ipv6)
{
    CURL *curl = curl_easy_init();
    if(!curl) {
        fprintf(stderr, "CURL 初始化失败\n");
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", service_name);
    cJSON_AddStringToObject(root, "path_prefix", path_prefix);
    cJSON_AddStringToObject(root, "host", service_host);
    cJSON_AddNumberToObject(root, "port", service_port);
    
    if (protocol) {
        cJSON_AddStringToObject(root, "protocol", protocol);
    }
    if (health_endpoint) {
        cJSON_AddStringToObject(root, "health_endpoint", health_endpoint);
    }
    cJSON_AddBoolToObject(root, "verify_ssl", verify_ssl ? 1 : 0);
    cJSON_AddBoolToObject(root, "ipv6", is_ipv6 ? 1 : 0);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/services/register", 
             gateway_host, gateway_port);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    printf("注册服务到网关:\n");
    printf("  网关：%s:%d\n", gateway_host, gateway_port);
    printf("  服务名：%s\n", service_name);
    printf("  路径前缀：%s\n", path_prefix);
    printf("  地址：%s:%d\n", service_host, service_port);
    printf("  协议：%s\n", protocol ? protocol : "http");
    printf("  IPv6: %s\n", is_ipv6 ? "是" : "否");
    printf("\n");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(json_str);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        printf("✓ 服务注册成功\n");
        return 0;
    } else {
        fprintf(stderr, "✗ 服务注册失败 (HTTP %ld): %s\n", http_code, curl_easy_strerror(res));
        return -1;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 7) {
        fprintf(stderr, "用法：%s <gateway_host> <gateway_port> <service_name> "
                       "<path_prefix> <service_host> <service_port> "
                       "[protocol] [health_endpoint] [verify_ssl] [ipv6]\n\n", 
                argv[0]);
        fprintf(stderr, "参数说明:\n");
        fprintf(stderr, "  gateway_host     - 网关主机地址\n");
        fprintf(stderr, "  gateway_port     - 网关端口\n");
        fprintf(stderr, "  service_name     - 服务名称\n");
        fprintf(stderr, "  path_prefix      - 路由路径前缀 (如：/api/users)\n");
        fprintf(stderr, "  service_host     - 服务主机地址\n");
        fprintf(stderr, "  service_port     - 服务端口\n");
        fprintf(stderr, "  protocol         - 协议 (http/https, 默认:http)\n");
        fprintf(stderr, "  health_endpoint  - 健康检查端点 (默认:/health)\n");
        fprintf(stderr, "  verify_ssl       - 是否验证 SSL 证书 (true/false, 默认:false)\n");
        fprintf(stderr, "  ipv6             - 是否 IPv6 (true/false, 默认:false)\n");
        fprintf(stderr, "\n示例:\n");
        fprintf(stderr, "  %s localhost 8080 user-service /api/users localhost 8081\n", argv[0]);
        fprintf(stderr, "  %s localhost 8080 payment-service /api/payments 192.168.1.100 8443 https /api/health false false\n", argv[0]);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    const char* protocol = (argc > 7) ? argv[7] : "http";
    const char* health_ep = (argc > 8) ? argv[8] : "/health";
    int verify_ssl = (argc > 9) ? (strcmp(argv[9], "true") == 0 ? 1 : 0) : 0;
    int is_ipv6 = (argc > 10) ? (strcmp(argv[10], "true") == 0 ? 1 : 0) : 0;

    int result = register_service(argv[1], atoi(argv[2]),
                                  argv[3], argv[4],
                                  argv[5], atoi(argv[6]),
                                  protocol, health_ep, verify_ssl, is_ipv6);

    curl_global_cleanup();
    return result ? 1 : 0;
}
