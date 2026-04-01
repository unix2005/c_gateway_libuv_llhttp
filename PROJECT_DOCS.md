# 项目文档合集

本文档包含 C 语言微服务网关项目的所有相关文档索引和核心内容。

---

## 📁 文档目录

### 根目录文档
- [GATEWAY_CONFIG_GUIDE.md](#网关配置指南) - 网关配置文件使用指南
- [QUICK_CONFIG_REFERENCE.md](#快速配置参考) - 快速配置参考
- [QUICK_REFERENCE.md](#快速参考) - 快速参考手册
- [EXAMPLES.md](#示例) - 使用示例
- [TESTING.md](#测试) - 测试指南
- [PROJECT_DELIVERABLES.md](#项目交付物) - 项目交付物清单
- [UPGRADE_SUMMARY.md](#升级总结) - 升级总结

### Bug 修复文档
- [BUGFIX_CURL_SLIST.md](#curl-slist-修复) - libcurl slist 释放错误修复
- [BUGFIX_C_LAMBDA.md](#c-lambda 修复) - C 语言 lambda 表达式修复
- [BUGFIX_C_LAMBDA_ROUND2.md](#c-lambda 第二轮修复) - C lambda 第二轮修复
- [BUGFIX_FORWARD_DECLARATION.md](#前向声明修复) - 函数前向声明修复
- [FIX_CONFIRMATION.md](#确认修复) - 修复确认文档

### 配置文件相关
- [CONFIG_ARCHITECTURE.md](#配置架构) - 配置架构设计
- [CONFIG_IMPLEMENTATION_SUMMARY.md](#配置实现总结) - 配置实现总结
- [CONFIG_UPGRADE_NOTES.md](#配置升级说明) - 配置升级说明

### include 目录
- [HEADER_MERGE_NOTES.md](#头文件合并说明) - 头文件合并说明
- [FIX_GETTID_WARNING.md](#gettid 警告修复) - gettid 编译警告修复

### src 目录（新增功能）
- [UNIFIED_REGISTER_JSON.md](#统一注册-json 格式) - 统一服务注册 JSON 格式
- [DOMAIN_NAME_SUPPORT.md](#域名支持) - 域名解析支持
- [HEALTH_CHECK_AUTO_REMOVE.md](#健康检查自动剔除) - 健康检查自动剔除机制
- [HEALTH_CHECK_DELETE_SERVICE.md](#空服务删除) - 空服务自动删除
- [FIX_INVALID_POINTER_FREE.md](#无效指针修复) - free() invalid pointer 修复
- [FIX_NOT_FOUND_STRING.md](#not-found 字符串修复) - "Not Found"字符串字面量修复

### service_worker 目录
- [README.md](#worker-service-readme) - Worker Service 说明
- [BUILD.md](#构建指南) - 构建指南
- [CONFIG_GUIDE.md](#worker-配置指南) - 配置指南
- [WORKERUnifiedRegisterJSON.md](#worker 统一注册格式) - Worker 统一注册格式
- [HOST_CONFIG_FEATURE.md](#host 配置功能) - Host 地址配置功能
- [WORKER_CONFIG_EXAMPLES.md](#worker 配置示例) - 配置示例

---

## 核心文档内容

### 网关配置指南

> **来源**: GATEWAY_CONFIG_GUIDE.md

#### 概述

网关服务现在支持通过 JSON 配置文件动态加载所有运行时参数，无需重新编译即可调整服务行为。

#### 启动命令

```bash
# 使用默认配置文件
./bin/gateway

# 指定配置文件
./bin/gateway gateway_config.json
```

#### 配置结构

##### 网关配置 (gateway)

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `worker_threads` | Integer | 4 | 工作线程数量 |
| `service_port` | Integer | 8080 | 监听端口 |
| `enable_ipv6` | Boolean | 0 | IPv6 双栈支持 |
| `enable_https` | Boolean | 0 | HTTPS 支持 |
| `log_path` | String | "gateway.log" | 日志路径 |
| `health_check_interval` | Integer | 5000ms | 健康检查间隔 |
| `ssl_cert_path` | String | "" | SSL 证书路径 |
| `ssl_key_path` | String | "" | SSL 私钥路径 |

##### 后端服务配置 (services)

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | String | ✓ | 服务名称 |
| `path_prefix` | String | ✓ | 路由前缀 |
| `host` | String | ✓ | 主机地址（支持域名） |
| `port` | Integer | ✓ | 端口号 |
| `protocol` | String | ✗ | "http"/"https" |
| `health_endpoint` | String | ✗ | 健康检查端点 |
| `verify_ssl` | Boolean | ✗ | SSL 验证 |
| `ipv6` | Boolean | ✗ | IPv6 支持 |

#### 配置示例

**基础 HTTP 配置（开发环境）**:
```json
{
  "gateway": {
    "worker_threads": 2,
    "service_port": 8080,
    "enable_ipv6": 0,
    "enable_https": 0,
    "log_path": "gateway.log",
    "health_check_interval": 5000
  },
  "services": [
    {
      "name": "user-service",
      "path_prefix": "/api/users",
      "host": "localhost",
      "port": 8081
    }
  ]
}
```

**HTTPS + IPv6 配置（生产环境）**:
```json
{
  "gateway": {
    "worker_threads": 8,
    "service_port": 443,
    "enable_ipv6": 1,
    "enable_https": 1,
    "log_path": "/var/log/gateway/gateway.log",
    "health_check_interval": 3000,
    "ssl_cert_path": "/etc/ssl/certs/server.crt",
    "ssl_key_path": "/etc/ssl/private/server.key"
  },
  "services": [
    {
      "name": "user-service",
      "path_prefix": "/api/users",
      "host": "localhost",
      "port": 8081,
      "protocol": "https",
      "verify_ssl": true
    }
  ]
}
```

---

### 统一注册 JSON 格式

> **来源**: src/UNIFIED_REGISTER_JSON.md

#### 服务注册请求

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

#### 服务列表响应

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

---

### 域名支持

> **来源**: src/DOMAIN_NAME_SUPPORT.md

#### 支持的 Host 格式

1. **IPv4 地址**: `192.168.1.100`
2. **IPv6 地址**: `::1`
3. **域名**: `api.example.com`
4. **localhost**: `localhost`
5. **容器名**: `user-service.default.svc.cluster.local`

#### 实现原理

```c
typedef struct {
    char address[256];      // IP 地址字符串
    int is_ipv6;            // 1 for IPv6, 0 for IPv4
    int is_domain;          // 1 if host is a domain name
} ip_address_t;
```

**解析逻辑**:
- 检测是否为域名（包含字母）
- DNS 解析获取 IP（失败时保留域名）
- libcurl 运行时处理

---

### 健康检查自动剔除

> **来源**: src/HEALTH_CHECK_AUTO_REMOVE.md

#### 机制说明

当健康检查连续失败达到阈值（MAX_FAILURE_COUNT=3）时：
1. 将该实例从服务注册表移除
2. 如果服务的所有实例都被移除，删除整个服务定义

#### 代码逻辑

```c
if (svc->instances[j].failure_count >= MAX_FAILURE_COUNT) {
    remove_unhealthy_service(svc, j);
    
    if (svc->instance_count == 0) {
        remove_service_from_registry(&g_registry, i);
    }
}
```

---

### Worker Service 统一注册格式

> **来源**: service_worker/WORKERUnifiedRegisterJSON.md

#### 配置文件示例

```json
{
  "name": "vms_get_employees",
  "description": "支持 IPv6 和 HTTPS 的多线程工作服务",
  "host": "192.168.19.10",
  "port": 8081,
  "ipv6": false,
  "https": false,
  "path_prefix": "/api/employees",
  "health_endpoint": "/health",
  "verify_ssl": false,
  "gateway": {
    "host": "192.168.19.10",
    "port": 8080
  }
}
```

---

## 文档索引

完整文档请参考各个 MD 文件。本文档仅提供核心内容索引。

---

**更新日期**: 2026-03-23  
**版本**: v2.0  
**状态**: ✅ 已合并
