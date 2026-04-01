# Worker Service 文档合集

本文档包含 Worker Service（工作服务）的所有相关文档。

---

## 📁 文档目录

### 基础文档
- [README.md](#readme) - Worker Service 概述
- [BUILD.md](#构建指南) - 构建指南
- [TESTING.md](#测试指南) - 测试指南

### 配置相关
- [CONFIG_GUIDE.md](#配置指南) - 配置指南
- [HOST_CONFIG_FEATURE.md](#host 配置功能) - Host 地址配置
- [WORKER_CONFIG_EXAMPLES.md](#配置示例) - 配置示例
- [WORKERUnifiedRegisterJSON.md](#统一注册格式) - 统一注册 JSON 格式
- [CONFIG_REGISTER_UNREGISTER.md](#注册注销配置) - 注册注销配置

### 功能特性
- [MULTI_THREAD_ANALYSIS.md](#多线程分析) - 多线程分析
- [POOL_ALLOC_IMPLEMENTATION.md](#内存池实现) - 内存池实现
- [POOL_ALLOC_INIT_EXPLANATION.md](#内存池初始化) - 内存池初始化说明

### Bug 修复
- [FIX_GRACEFUL_SHUTDOWN.md](#优雅退出) - 优雅退出修复
- [FIX_USLEEP_WARNING.md](#usleep 警告) - usleep 编译警告修复
- [FIX_UV_RUN_EXIT.md](#uv_run 退出) - uv_run 退出修复
- [FIX_WORKER_V2_COMPILE.md](#v2 编译修复) - worker_service_v2 编译修复

### 版本说明
- [VERSION_NOTES.md](#版本说明) - 版本说明
- [UPGRADE_v2.md](#v2 升级) - v2 升级指南
- [COMPILE_SUCCESS.md](#编译成功) - 编译成功确认

---

## 核心内容

### Worker Service 概述

Worker Service 是一个支持 IPv6 和 HTTPS 的多线程工作服务，可以独立运行并向网关注册。

**主要特性**:
- ✓ 多线程处理客户端请求
- ✓ 支持 IPv4/IPv6 双栈
- ✓ 支持 HTTP/HTTPS
- ✓ 自动向网关注册和注销
- ✓ 健康检查端点
- ✓ 配置驱动（JSON 配置文件）

---

### 构建指南

> **来源**: BUILD.md

#### 编译命令

```bash
# Linux
make clean; make worker_service_v3_1

# Windows (MinGW)
mingw32-make -f Makefile.win clean
mingw32-make -f Makefile.win worker_service_v3_1
```

#### 依赖库

- libuv
- OpenSSL
- libcurl
- cJSON
- pthread (Linux)

---

### 配置指南

> **来源**: CONFIG_GUIDE.md

#### 配置文件结构

```json
{
  "name": "worker-service",
  "description": "工作服务描述",
  "host": "0.0.0.0",
  "port": 8081,
  "ipv6": false,
  "https": false,
  "path_prefix": "/api/worker",
  "health_endpoint": "/health",
  "verify_ssl": false,
  "gateway": {
    "host": "localhost",
    "port": 8080
  },
  "threads": {
    "count": 4,
    "max_connections": 1024
  },
  "logging": {
    "file": "logs/worker.log",
    "level": "INFO"
  }
}
```

#### 字段说明

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | String | - | 服务名称（唯一标识） |
| `description` | String | "" | 服务描述 |
| `host` | String | "0.0.0.0" | 监听地址（支持域名） |
| `port` | Integer | 8081 | 监听端口 |
| `ipv6` | Boolean | false | IPv6 支持 |
| `https` | Boolean | false | HTTPS 支持 |
| `path_prefix` | String | "/api/worker" | 路由前缀 |
| `health_endpoint` | String | "/health" | 健康检查端点 |
| `verify_ssl` | Boolean | false | SSL 验证 |
| `gateway.host` | String | "localhost" | 网关地址 |
| `gateway.port` | Integer | 8080 | 网关端口 |
| `threads.count` | Integer | 4 | 工作线程数 |
| `threads.max_connections` | Integer | 1024 | 最大连接数 |

---

### Host 配置功能

> **来源**: HOST_CONFIG_FEATURE.md

#### 功能说明

Worker Service 现在支持通过配置文件设置 host 地址，在注册和注销时发送给网关。

**修改前**:
```c
// 硬编码
cJSON_AddStringToObject(root, "host", config.is_ipv6 ? "::1" : "localhost");
```

**修改后**:
```c
// 从配置读取
cJSON_AddStringToObject(root, "host", config.host);
```

#### 配置示例

```json
{
  "host": "192.168.19.10"  // ✓ 使用配置的 host
}
```

---

### 统一注册 JSON 格式

> **来源**: WORKERUnifiedRegisterJSON.md

#### 注册请求格式

```json
{
  "name": "vms_get_employees",
  "description": "支持 IPv6 和 HTTPS 的多线程工作服务",
  "path_prefix": "/api/employees",
  "host": "192.168.19.10",
  "port": 8081,
  "protocol": "http",
  "ipv6": false,
  "health_endpoint": "/health",
  "verify_ssl": false
}
```

**必需字段**:
- ✓ `name` - 服务名称
- ✓ `description` - 服务描述
- ✓ `path_prefix` - 路由前缀
- ✓ `host` - 主机地址（支持域名）
- ✓ `port` - 端口号
- ✓ `protocol` - 协议类型
- ✓ `ipv6` - IPv6 标志
- ✓ `health_endpoint` - 健康检查端点
- ✓ `verify_ssl` - SSL 验证

---

### 配置示例

> **来源**: WORKER_CONFIG_EXAMPLES.md

#### 示例 1: 基础 HTTP 服务

```json
{
  "name": "employee-service",
  "description": "雇员信息服务",
  "host": "0.0.0.0",
  "port": 8081,
  "path_prefix": "/api/employees",
  "gateway": {
    "host": "localhost",
    "port": 8080
  }
}
```

#### 示例 2: HTTPS 服务

```json
{
  "name": "secure-worker",
  "description": "安全的工作服务",
  "host": "192.168.1.100",
  "port": 8443,
  "ipv6": false,
  "https": true,
  "path_prefix": "/api/secure",
  "health_endpoint": "/health",
  "verify_ssl": true,
  "ssl_cert_file": "certs/server.crt",
  "ssl_key_file": "certs/server.key"
}
```

#### 示例 3: Docker 环境

```json
{
  "name": "docker-worker",
  "host": "0.0.0.0",
  "port": 8081,
  "path_prefix": "/api/worker",
  "gateway": {
    "host": "host.docker.internal",
    "port": 8080
  }
}
```

---

### 注册注销配置

> **来源**: CONFIG_REGISTER_UNREGISTER.md

#### 注册流程

1. 启动 Worker Service
2. 加载配置文件
3. 调用 `register_to_gateway()`
4. 发送 POST 请求到网关 `/api/services/register`
5. 等待响应

#### 注销流程

1. 用户按 Ctrl+C
2. 信号处理函数捕获 SIGINT
3. 调用 `unregister_from_gateway()`
4. 发送 DELETE 请求到网关 `/api/services/unregister`
5. 清理资源并退出

#### 日志输出

**注册成功**:
```
[Gateway] 服务实例注册：worker-service -> [IPv4] 192.168.1.10:8081 (HTTP)
```

**注销成功**:
```
[Gateway] 服务实例注销：worker-service (192.168.1.10:8081)
```

---

### 多线程分析

> **来源**: MULTI_THREAD_ANALYSIS.md

#### 架构设计

```
主线程（事件循环）
    ├── Worker Thread 1
    ├── Worker Thread 2
    ├── Worker Thread 3
    └── Worker Thread 4
```

#### 线程模型

- **主线程**: libuv 事件循环，处理网络 I/O
- **工作线程**: 处理具体业务逻辑（每个线程独立 server socket）

#### 优势

- ✓ 并发处理能力更强
- ✓ 避免线程间竞争
- ✓ 更好的资源利用

---

### 内存池实现

> **来源**: POOL_ALLOC_IMPLEMENTATION.md

#### 内存池结构

```c
#define POOL_SIZE 8192

typedef struct {
    char pool_data[POOL_SIZE];
    size_t pool_used;
} client_ctx_t;
```

#### 分配函数

```c
static void* pool_alloc(client_ctx_t* ctx, size_t size)
{
    // 8 字节对齐
    size = (size + 7) & ~7;
    
    if (ctx->pool_used + size > POOL_SIZE) {
        return NULL;  // 溢出
    }
    
    void* ptr = ctx->pool_data + ctx->pool_used;
    ctx->pool_used += size;
    memset(ptr, 0, size);
    return ptr;
}
```

#### 特点

- ✓ 8 字节对齐提高性能
- ✓ 溢出保护
- ✓ 自动清零
- ✓ 请求结束自动释放

---

### 内存池初始化

> **来源**: POOL_ALLOC_INIT_EXPLANATION.md

#### 初始化方式

```c
client_ctx_t *client = calloc(1, sizeof(client_ctx_t));
```

**calloc 的优势**:
- ✓ 自动清零内存
- ✓ 无需手动初始化 pool_data
- ✓ 防止脏数据

#### 生命周期

1. 客户端连接 → `calloc` 分配
2. 请求处理 → `pool_alloc` 分配临时数据
3. 客户端断开 → `free` 释放整个 client_ctx

---

### 优雅退出

> **来源**: FIX_GRACEFUL_SHUTDOWN.md

#### 问题

程序收到 Ctrl+C 时没有正确清理资源就退出了。

#### 解决方案

```c
void signal_handler(int signum) {
    uv_stop(&main_loop);  // 停止事件循环
    should_exit = 1;      // 设置退出标志
}

// 主循环后清理
uv_run(&main_loop, UV_RUN_DEFAULT);
cleanup_resources();  // ✓ 清理资源
unregister_from_gateway();  // ✓ 网关注销
```

#### 退出流程

```
Ctrl+C → SIGINT → signal_handler → uv_stop → 
继续执行清理代码 → 关闭工作线程 → 
发送注销请求 → 清理资源 → 进程退出
```

---

### usleep 警告修复

> **来源**: FIX_USLEEP_WARNING.md

#### 错误信息

```
warning: implicit declaration of function 'usleep'
```

#### 修复方案

```c
/* 跨平台头文件 */
#ifdef _WIN32
    #include <process.h>
#else
    #include <unistd.h>      /* 提供 usleep() */
#endif
```

---

### 编译修复

> **来源**: FIX_WORKER_V2_COMPILE.md

#### 常见问题

1. **未知类型 `uv_buf_t`**
   - 解决：包含 `<uv.h>`

2. **strdup 未声明**
   - 解决：定义 `_POSIX_C_SOURCE 200809L`

3. **符号比较警告**
   - 解决：强制类型转换 `(int)strlen(...)`

---

## 快速参考

### 启动 Worker Service

```bash
./worker_service_v3_1 worker_config.json
```

### 查看服务状态

```bash
curl http://localhost:8080/api/services
```

### 测试健康检查

```bash
curl http://localhost:8081/health
```

---

## 相关文档

- [GATEWAY_CONFIG_GUIDE.md](../GATEWAY_CONFIG_GUIDE.md) - 网关配置指南
- [UNIFIED_REGISTER_JSON.md](../src/UNIFIED_REGISTER_JSON.md) - 统一注册格式
- [DOMAIN_NAME_SUPPORT.md](../src/DOMAIN_NAME_SUPPORT.md) - 域名支持

---

**更新日期**: 2026-03-23  
**版本**: v3.1  
**状态**: ✅ 已合并
