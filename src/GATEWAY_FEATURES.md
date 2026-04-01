# 网关核心功能文档

本文档包含网关核心功能的实现说明和使用指南。

---

## 📁 文档目录

- [UNIFIED_REGISTER_JSON.md](#统一注册-json 格式) - 统一服务注册 JSON 格式
- [DOMAIN_NAME_SUPPORT.md](#域名支持) - 域名解析支持
- [HEALTH_CHECK_AUTO_REMOVE.md](#健康检查自动剔除) - 健康检查自动剔除机制
- [HEALTH_CHECK_DELETE_SERVICE.md](#空服务删除) - 空服务自动删除
- [FIX_INVALID_POINTER_FREE.md](#无效指针修复) - free() invalid pointer 修复
- [FIX_NOT_FOUND_STRING.md](#not-found 字符串修复) - "Not Found"字符串字面量修复

---

## 统一注册 JSON 格式

> **来源**: UNIFIED_REGISTER_JSON.md

### 概述

统一所有服务注册和查询接口的 JSON 格式，确保包含完整的字段信息。

### 服务注册请求

```json
{
  "name": "worker-service",
  "description": "多线程工作服务",
  "path_prefix": "/api/worker",
  "host": "192.168.1.100",
  "port": 8081,
  "protocol": "http",
  "ipv6": false,
  "health_endpoint": "/health",
  "verify_ssl": false
}
```

**字段说明**:

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `name` | string | ✓ | - | 服务名称（唯一标识） |
| `description` | string | ✗ | `""` | 服务描述 |
| `path_prefix` | string | ✓ | - | 路由路径前缀 |
| `host` | string | ✓ | - | 服务主机地址（支持域名） |
| `port` | number | ✓ | - | 服务端口 |
| `protocol` | string | ✗ | `"http"` | `"http"` 或 `"https"` |
| `ipv6` | boolean | ✗ | `false` | 是否 IPv6 |
| `health_endpoint` | string | ✗ | `"/health"` | 健康检查端点 |
| `verify_ssl` | boolean | ✗ | `false` | SSL 验证开关 |

### 服务列表响应

```json
[
  {
    "name": "worker-service",
    "description": "多线程工作服务",
    "path_prefix": "/api/worker",
    "health_endpoint": "/health",
    "instances": [
      {
        "host": "192.168.1.100",
        "port": 8081,
        "protocol": "http",
        "ipv6": false,
        "health": "healthy",
        "requests": 42,
        "verify_ssl": false
      }
    ]
  }
]
```

### 代码变更

#### 扩展 service_t 结构体

```c
typedef struct {
    char name[SERVICE_NAME_LEN];
    char description[256];      // ✓ 新增：服务描述
    char path_prefix[64];       // 路由路径前缀
    service_instance_t instances[MAX_SERVICE_INSTANCES];
    int instance_count;
    int current_instance;       // 轮询索引
    pthread_mutex_t lock;
    char health_endpoint[128];
} service_t;
```

#### 更新注册函数签名

```c
int service_register_with_ipv6(const char* name, const char* description,
                                const char* path_prefix, const char* host, int port, 
                                protocol_t protocol, const char* health_endpoint, 
                                int verify_ssl, int is_ipv6);
```

---

## 域名支持

> **来源**: DOMAIN_NAME_SUPPORT.md

### 支持的 Host 格式

1. **IPv4 地址**: `192.168.1.100`
2. **IPv6 地址**: `::1`
3. **域名**: `api.example.com`
4. **localhost**: `localhost`
5. **容器名**: `user-service.default.svc.cluster.local`

### 技术实现

#### 扩展 ip_address_t 结构体

```c
typedef struct {
    char address[256];      // IP 地址字符串
    int is_ipv6;            // 1 for IPv6, 0 for IPv4
    int is_domain;          // ✓ 新增：1 if host is a domain name
} ip_address_t;
```

#### IP 地址解析函数

```c
int parse_ip_address(const char* host, ip_address_t* addr) 
{
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
    
    // 如果是域名，尝试 DNS 解析
    if (is_domain) {
        getaddrinfo(host, NULL, &hints, &res);
        // DNS 解析逻辑...
    } else {
        // 直接 IP 地址
        addr->is_ipv6 = (strchr(host, ':') != NULL) ? 1 : 0;
    }
    
    return 0;
}
```

### 工作流程

```
注册请求：{"host": "api.example.com", "port": 443}
    ↓
parse_ip_address("api.example.com")
    ↓
检测字母 → 是域名
    ↓
DNS 解析：api.example.com → 10.0.0.15
    ↓
保存：
  - host: "api.example.com"
  - ip_addr.address: "10.0.0.15"
  - ip_addr.is_domain: 1
  - ip_addr.is_ipv6: 0
    ↓
libcurl 构建 URL: http://api.example.com:443
    ↓
实际连接（可能使用缓存的 IP）
```

---

## 健康检查自动剔除

> **来源**: HEALTH_CHECK_AUTO_REMOVE.md

### 机制说明

当健康检查连续失败达到阈值（MAX_FAILURE_COUNT=3）时：
1. 将该实例从服务注册表移除
2. 如果服务的所有实例都被移除，删除整个服务定义

### 代码实现

```c
#define MAX_FAILURE_COUNT 3

void health_check_callback(uv_timer_t *handle)
{
    pthread_mutex_lock(&g_registry.lock);
    
    for (int i = 0; i < g_registry.service_count; i++)
    {
        service_t *svc = &g_registry.services[i];
        
        pthread_mutex_lock(&svc->lock);
        for (int j = 0; j < svc->instance_count; j++)
        {
            check_service_health(&svc->instances[j]);
            
            // 检查是否达到移除条件
            if (svc->instances[j].failure_count >= MAX_FAILURE_COUNT)
            {
                remove_unhealthy_service(svc, j);
                j--;  // 回退索引
                
                // 检查是否所有实例都被移除
                if (svc->instance_count == 0)
                {
                    pthread_mutex_unlock(&svc->lock);
                    remove_service_from_registry(&g_registry, i);
                    i--;  // 回退索引
                    break;
                }
            }
        }
        pthread_mutex_unlock(&svc->lock);
    }
    
    pthread_mutex_unlock(&g_registry.lock);
}
```

### 移除函数

```c
static void remove_unhealthy_service(service_t *svc, int instance_index)
{
    printf("[Health] ✗ 服务 %s:%d 连续 %d 次检查失败，从注册表移除\n",
           svc->instances[instance_index].host,
           svc->instances[instance_index].port,
           MAX_FAILURE_COUNT);
    
    // 将后续实例前移
    for (int i = instance_index; i < svc->instance_count - 1; i++) {
        svc->instances[i] = svc->instances[i + 1];
    }
    
    svc->instance_count--;
    
    if (svc->current_instance >= svc->instance_count) {
        svc->current_instance = 0;
    }
}

static void remove_service_from_registry(service_registry_t *registry, int service_index)
{
    printf("[Health] ✗ 服务 %s 已无健康实例，从注册表删除整个服务\n",
           registry->services[service_index].name);
    
    // 将后续服务前移
    for (int i = service_index; i < registry->service_count - 1; i++) {
        registry->services[i] = registry->services[i + 1];
    }
    
    registry->service_count--;
}
```

---

## 空服务删除

> **来源**: HEALTH_CHECK_DELETE_SERVICE.md

### 功能说明

当服务的所有实例都被健康检查剔除后，自动删除整个服务定义。

### 触发条件

1. 服务的最后一个实例被 `remove_unhealthy_service()` 移除
2. `svc->instance_count == 0`
3. 调用 `remove_service_from_registry()`

### 日志输出

```
[Health] ✗ 服务 192.168.1.100:8081 连续 3 次检查失败，从注册表移除
[Health] 当前服务 user-service 剩余实例数：0
[Health] ✗ 服务 user-service 已无健康实例，从注册表删除整个服务
[Health] 注册表剩余服务数：2
```

---

## 无效指针修复

> **来源**: FIX_INVALID_POINTER_FREE.md

### 问题描述

运行时崩溃：
```
*** Error in `./c_gateway': free(): invalid pointer: 0x0000000000405996 ***
```

### 根本原因

传递字符串字面量给 `send_response()`，`on_write_completed()` 尝试释放静态数据区地址。

**错误代码**:
```c
send_response(client, 200, "application/json", 
              "{\"status\":\"UP\",\"type\":\"gateway\"}");
// ↑ 字符串字面量在静态数据区 (0x405996)，不能 free
```

### 修复方案

```c
send_response(client, 200, "application/json", 
              strdup("{\"status\":\"UP\",\"type\":\"gateway\"}"));
// ↑ strdup 分配在堆上，可以被安全释放
```

### 内存管理原则

**send_response 的契约**:
- ✓ body 参数必须是 malloc/calloc/strdup 分配的堆内存
- ✗ 不能是字符串字面量（静态数据区）
- ✗ 不能是栈上数组
- ✗ 不能是全局/static 字符串

### 检查清单

所有 send_response 调用必须满足:
- [x] 参数是 `malloc()` 分配的
- [x] 参数是 `strdup()` 复制的
- [x] 参数是 `cJSON_Print()` 返回的
- [ ] **不是**字符串字面量
- [ ] **不是**栈上数组
- [ ] **不是**全局/static 字符串

---

## Not Found 字符串修复

> **来源**: FIX_NOT_FOUND_STRING.md

### 问题重现

GDB 调试信息:
```
Program terminated with signal 11, Segmentation fault.
#1  0x000000000040263a in on_write_completed at src/network.c:93
93        if (wctx->body_ptr) free(wctx->body_ptr);
(gdb) p wctx->body_ptr
$1 = 0x40598b "Not Found"
```

### 修复内容

**文件**: router.c

**修复前**:
```c
else
{
  send_response(client, 404, "text/plain", "Not Found");
  //                                         ^^^^^^^^^^^^
  //                                         ❌ 字面量
}
```

**修复后**:
```c
else
{
  send_response(client, 404, "text/plain", strdup("Not Found"));
  //                                         ^^^^^^^^^^^^^^^^^
  //                                         ✓ 堆内存
}
```

---

## 最佳实践

### 1. 服务注册

**推荐做法**:
```bash
curl -X POST http://localhost:8080/api/services/register \
  -H "Content-Type: application/json" \
  -d '{
    "name": "user-service",
    "description": "用户管理服务",
    "path_prefix": "/api/users",
    "host": "user-service.internal",
    "port": 8080,
    "protocol": "http",
    "health_endpoint": "/health",
    "verify_ssl": false
  }'
```

### 2. 域名使用

**开发环境**:
```json
{
  "host": "localhost",
  "port": 8081
}
```

**生产环境**:
```json
{
  "host": "api.example.com",
  "port": 443,
  "protocol": "https",
  "verify_ssl": true
}
```

**容器环境**:
```json
{
  "host": "user-service.default.svc.cluster.local",
  "port": 8080
}
```

### 3. 健康检查配置

```json
{
  "health_endpoint": "/health",
  "health_check_interval": 5000
}
```

**建议**:
- 健康检查间隔：3000-10000ms
- 失败阈值：3 次
- 超时时间：3 秒

---

## 相关文档

- [GATEWAY_CONFIG_GUIDE.md](../GATEWAY_CONFIG_GUIDE.md) - 网关配置指南
- [WORKER_DOCS.md](../service_worker/WORKER_DOCS.md) - Worker Service 文档

---

**更新日期**: 2026-03-23  
**版本**: v2.0  
**状态**: ✅ 已合并
