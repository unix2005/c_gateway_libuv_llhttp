#include "gateway.h"

service_registry_t g_registry;

void service_registry_init() 
{
    memset(&g_registry, 0, sizeof(g_registry));
    pthread_mutex_init(&g_registry.lock, NULL);
    
    printf("[Gateway] 服务注册表初始化完成\n");
}

// IP 地址解析（支持 IPv4、IPv6 和域名）
int parse_ip_address(const char* host, ip_address_t* addr) 
{
    // 初始化
    memset(addr, 0, sizeof(ip_address_t));
    
    if (!host || strlen(host) == 0) {
        return -1;
    }
    
    // 保存原始 host（可能是域名）
    strncpy(addr->address, host, sizeof(addr->address) - 1);
    
    // 检测是否为域名（包含字母）
    int is_domain = 0;
    for (size_t i = 0; i < strlen(host); i++) {
        if ((host[i] >= 'a' && host[i] <= 'z') || 
            (host[i] >= 'A' && host[i] <= 'Z')) {
            is_domain = 1;
            break;
        }
    }
    
    addr->is_domain = is_domain;
    
    // 如果是域名，尝试 DNS 解析获取 IP
    if (is_domain) {
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;  // 允许 IPv4 或 IPv6
        hints.ai_socktype = SOCK_STREAM;
        
        int error = getaddrinfo(host, NULL, &hints, &res);
        if (error != 0) {
            fprintf(stderr, "[DNS] 无法解析主机：%s\n", host);
            // 即使解析失败，也保留域名，后续由 libcurl 处理
            addr->is_ipv6 = 0;
            return 0;  // 不返回错误，允许使用域名
        }
        
        // 检查是否为 IPv6
        if (res->ai_family == AF_INET6) {
            addr->is_ipv6 = 1;
            struct sockaddr_in6* ipv6_addr = (struct sockaddr_in6*)res->ai_addr;
            inet_ntop(AF_INET6, &ipv6_addr->sin6_addr, addr->address, sizeof(addr->address));
            printf("[DNS] 解析 %s -> IPv6: %s\n", host, addr->address);
        } else {
            addr->is_ipv6 = 0;
            struct sockaddr_in* ipv4_addr = (struct sockaddr_in*)res->ai_addr;
            inet_ntop(AF_INET, &ipv4_addr->sin_addr, addr->address, sizeof(addr->address));
            printf("[DNS] 解析 %s -> IPv4: %s\n", host, addr->address);
        }
        
        freeaddrinfo(res);
    } else {
        // 直接 IP 地址，检测 IPv4/IPv6
        addr->is_ipv6 = (strchr(host, ':') != NULL) ? 1 : 0;
        printf("[IP] 直接使用%s地址：%s\n", 
               addr->is_ipv6 ? "IPv6" : "IPv4", host);
    }
    
    return 0;
}

int service_register(const char* name, const char* path_prefix, 
                     const char* host, int port, protocol_t protocol,
                     const char* health_endpoint, int verify_ssl)
{
    return service_register_with_ipv6(name, NULL, path_prefix, host, port, protocol,
                                      health_endpoint, verify_ssl, 0);
}

int service_register_with_ipv6(const char* name, const char* description,
                                const char* path_prefix, const char* host, int port, 
                                protocol_t protocol, const char* health_endpoint, 
                                int verify_ssl, int is_ipv6) 
{
    pthread_mutex_lock(&g_registry.lock);
    
    // 查找是否已存在该服务
    service_t* existing = NULL;
    for (int i = 0; i < g_registry.service_count; i++) {
        if (strcmp(g_registry.services[i].name, name) == 0) {
            existing = &g_registry.services[i];
            break;
        }
    }
    
    // 如果是新服务，添加到注册表
    if (!existing) {
        if (g_registry.service_count >= MAX_SERVICES) {
            pthread_mutex_unlock(&g_registry.lock);
            fprintf(stderr, "[Gateway] 服务数量已达上限\n");
            return -1;
        }
        
        existing = &g_registry.services[g_registry.service_count++];
        memset(existing, 0, sizeof(service_t));  // ✓ 清零整个结构
        strncpy(existing->name, name, SERVICE_NAME_LEN - 1);
        if (description) {
            strncpy(existing->description, description, sizeof(existing->description) - 1);
        }
        strncpy(existing->path_prefix, path_prefix, sizeof(existing->path_prefix) - 1);
        strncpy(existing->health_endpoint, health_endpoint ? health_endpoint : "/health", 
                sizeof(existing->health_endpoint) - 1);
        pthread_mutex_init(&existing->lock, NULL);
        
        const char* proto_str = (protocol == PROTOCOL_HTTPS) ? "HTTPS" : "HTTP";
        printf("[Gateway] 注册新服务：%s (路径前缀：%s, 协议：%s)\n", name, path_prefix, proto_str);
    }
    
    // 添加服务实例
    pthread_mutex_lock(&existing->lock);
    if (existing->instance_count >= MAX_SERVICE_INSTANCES) {
        pthread_mutex_unlock(&existing->lock);
        pthread_mutex_unlock(&g_registry.lock);
        fprintf(stderr, "[Gateway] 服务 %s 实例数已达上限\n", name);
        return -1;
    }
    
    service_instance_t* inst = &existing->instances[existing->instance_count++];
    memset(inst, 0, sizeof(service_instance_t));  // ✓ 清零实例
    
    strncpy(inst->host, host, SERVICE_HOST_LEN - 1);
    inst->port = port;
    inst->protocol = protocol;
    inst->verify_ssl = verify_ssl;
    inst->health = SERVICE_UNKNOWN;
    inst->request_count = 0;
    inst->last_check_time = 0;
    
    // 解析 IP 地址（支持域名）
    parse_ip_address(host, &inst->ip_addr);
    
    if (health_endpoint) {
        strncpy(inst->endpoint, health_endpoint, sizeof(inst->endpoint) - 1);
    } else {
        snprintf(inst->endpoint, sizeof(inst->endpoint), "/health");
    }
    
    // 设置 SSL 验证选项
    if (protocol == PROTOCOL_HTTPS && !verify_ssl) {
        strcpy(inst->ca_cert_path, "");  // 不验证证书
    } else if (protocol == PROTOCOL_HTTPS) {
        strcpy(inst->ca_cert_path, "/etc/ssl/certs/ca-certificates.crt");  // 默认 CA 路径
    }
    
    pthread_mutex_unlock(&existing->lock);
    pthread_mutex_unlock(&g_registry.lock);
    
    const char* proto_str = (protocol == PROTOCOL_HTTPS) ? "HTTPS" : "HTTP";
    const char* ip_ver = is_ipv6 ? "IPv6" : "IPv4";
    printf("[Gateway] 服务实例注册：%s -> [%s] %s:%d (%s)\n", 
           name, ip_ver, host, port, proto_str);
    return 0;
}

int service_deregister(const char* name, const char* host, int port) 
{
    pthread_mutex_lock(&g_registry.lock);
    
    for (int i = 0; i < g_registry.service_count; i++) {
        service_t* svc = &g_registry.services[i];
        
        if (strcmp(svc->name, name) == 0) {
            pthread_mutex_lock(&svc->lock);
            
            for (int j = 0; j < svc->instance_count; j++) {
                if (strcmp(svc->instances[j].host, host) == 0 && 
                    svc->instances[j].port == port) {
                    // 删除实例（移动数组元素）
                    for (int k = j; k < svc->instance_count - 1; k++) {
                        svc->instances[k] = svc->instances[k + 1];
                    }
                    svc->instance_count--;
                    
                    pthread_mutex_unlock(&svc->lock);
                    pthread_mutex_unlock(&g_registry.lock);
                    
                    printf("[Gateway] 服务实例注销：%s -> %s:%d\n", name, host, port);
                    return 0;
                }
            }
            
            pthread_mutex_unlock(&svc->lock);
        }
    }
    
    pthread_mutex_unlock(&g_registry.lock);
    return -1;
}

service_t* service_find_by_path(const char* path) 
{
    pthread_mutex_lock(&g_registry.lock);
    
    for (int i = 0; i < g_registry.service_count; i++) {
        service_t* svc = &g_registry.services[i];
        
        printf("[Gateway] [%s] 查找服务路径：%s\n", path,svc->path_prefix);
        // 检查路径是否匹配服务前缀
        if (strncmp(path, svc->path_prefix, strlen(svc->path_prefix)) == 0) {
            pthread_mutex_unlock(&g_registry.lock);
            printf("[Gateway] [%s] 找到服务路径：%s\n", path, svc->path_prefix);
            return svc;
        }
    }
    printf("[Gateway] [%s] 未找到服务路径\n", path);
    pthread_mutex_unlock(&g_registry.lock);
    return NULL;
}

service_instance_t* service_select_instance(service_t* service) 
{
    if (!service || service->instance_count == 0) {
        return NULL;
    }
    
    pthread_mutex_lock(&service->lock);
    
    // 轮询负载均衡
    int start = service->current_instance;
    service_instance_t* selected = NULL;
    
    do {
        service_instance_t* inst = &service->instances[service->current_instance];
        service->current_instance = (service->current_instance + 1) % service->instance_count;
        
        if (inst->health != SERVICE_UNHEALTHY) {
            selected = inst;
            inst->request_count++;
            break;
        }
    } while (service->current_instance != start);
    
    pthread_mutex_unlock(&service->lock);
    
    if (selected) {
        const char* proto_str = (selected->protocol == PROTOCOL_HTTPS) ? "HTTPS" : "HTTP";
        const char* ip_ver = selected->ip_addr.is_ipv6 ? "IPv6" : "IPv4";
        printf("[Gateway] 选择服务实例：%s -> [%s] %s:%d (%s) [请求数：%d]\n", 
               service->name, ip_ver, selected->host, selected->port, 
               proto_str, selected->request_count);
    }
    
    return selected;
}
