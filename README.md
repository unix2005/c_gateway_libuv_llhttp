# 微服务网关 - 高性能 C 语言实现

[![Performance](https://img.shields.io/badge/performance-1915%20RPS-brightgreen.svg)]()
[![Latency](https://img.shields.io/badge/latency-5.2ms-brightgreen.svg)]()
[![License](https://img.shields.io/badge/license-MIT-blue.svg)]()

## 项目简介

这是一个基于 libuv 事件驱动模型的高性能微服务网关，采用纯 C 语言实现。通过多线程 + 事件循环的混合架构，实现了**1,915 RPS**的吞吐量和**5.2ms**的平均响应延迟，达到生产级别标准。

### 核心特性

✅ **高性能** - 1,915 RPS 吞吐量，5.2ms 平均延迟  
✅ **多线程** - 支持多 worker 线程，充分利用多核 CPU  
✅ **内存池** - 预分配策略减少 80% malloc 调用  
✅ **健康检查** - 自动探测后端服务可用性  
✅ **负载均衡** - Round-Robin 轮询调度  
✅ **可观测性** - 集成日志、Prometheus 指标、分布式追踪  
✅ **优雅退出** - 完整的资源清理机制，无段错误

---

## 快速开始

### 依赖要求

- libuv (>= 1.0)
- llhttp (>= 2.0)
- libcurl (>= 7.0)
- cJSON (>= 1.7)
- GCC (>= 4.8) 或 Clang

### 编译（Linux）

```bash
# 克隆项目
git clone <repository-url>
cd c-restful-api

# 编译
make clean
make

# 运行
./bin/c_gateway gateway_config.json
```

### 编译（Windows）

```cmd
REM 使用提供的批处理脚本
build.bat
```

### 配置文件

编辑 `gateway_config.json`：

```json
{
  "worker_threads": 4,
  "service_port": 8080,
  "enable_ipv6": false,
  "enable_https": false,
  "health_check_interval": 5000,
  "observability": {
    "enable_logging": true,
    "log_level": "info",
    "enable_metrics": true,
    "metrics_port": 9090,
    "metrics_path": "/metrics",
    "enable_tracing": false,
    "tracing_sample_rate": 0.1
  }
}
```

---

## 性能测试

### 基准测试

使用 ApacheBench 进行压力测试：

```bash
# 基本性能测试（1000 请求，10 并发）
ab -n 1000 -c 10 http://localhost:8080/api/employees

# 高并发测试（10000 请求，100 并发）
ab -n 10000 -c 100 http://localhost:8080/api/employees
```

### 测试结果

#### 核心指标（1000 请求，10 并发）

| 指标 | 数值 |
|------|------|
| **每秒请求数** | **1,915.07 req/s** |
| 平均响应时间 | 5.22 ms |
| 中位数 | 5 ms |
| 99 百分位 | 7 ms |
| 成功率 | **100%** |

#### 与其他技术栈对比

| 技术栈 | RPS | 平均延迟 |
|--------|-----|---------|
| **C + libuv (本网关)** | **~1,915** | **~5ms** |
| Go (net/http) | ~2,000-3,000 | ~3-8ms |
| Node.js | ~1,500-2,500 | ~5-10ms |
| Python (Flask) | ~500-1,000 | ~20-50ms |

详细性能分析报告：[docs/PERFORMANCE_TEST_REPORT.md](docs/PERFORMANCE_TEST_REPORT.md)

---

## 架构设计

### 系统架构

```
┌─────────────────────────────────────┐
│        业务逻辑层                    │
│  - 代理转发 (libcurl)               │
│  - 健康检查                         │
│  - 指标收集                         │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│        路由层                        │
│  - 路径匹配                         │
│  - 服务发现                         │
│  - 负载均衡                         │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│      协议解析层 (llhttp)             │
│  - HTTP 词法/语法分析                │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│       网络层 (libuv)                 │
│  - TCP 服务器                       │
│  - 异步 IO                          │
└─────────────────────────────────────┘
```

### 多线程模型

采用"主从 Reactor"模式：

```
主线程 (健康检查 + 指标服务器)
    │
    ├──────┬──────┬──────┐
    ↓      ↓      ↓      ↓
 Worker1 Worker2 Worker3 ...
```

每个 worker 线程拥有独立的 `uv_loop_t`，监听同一端口（通过 SO_REUSEPORT/SO_REUSEADDR）。

### 关键技术

#### 1. 内存池优化

```c
#define POOL_SIZE 8192  // 8KB

typedef struct {
    char data[POOL_SIZE];
    size_t used;
} mem_pool_t;

// 请求间复用，减少 80% malloc 调用
ctx->pool.used = 0;
```

#### 2. 异步 HTTP 代理

通过 `uv_async` 实现 libuv 和 libcurl 的跨线程通信：

```c
// CURL 完成后通过 uv_async 通知网关线程
uv_async_send(async);
```

#### 3. libuv 优雅退出

正确的资源清理顺序：

```c
// 1. 关闭 handle
uv_close((uv_handle_t*)server, NULL);

// 2. 等待所有 handle 关闭完成
while (uv_loop_alive(loop)) {
    uv_run(loop, UV_RUN_ONCE);
}

// 3. 删除 loop
uv_loop_delete(loop);
```

详细技术文档：[docs/SEGFAULT_UV_LOOP_FIX.md](docs/SEGFAULT_UV_LOOP_FIX.md)

---

## API 端点

### 内置端点

| 端点 | 方法 | 描述 |
|------|------|------|
| `/api/employees` | GET | 获取员工列表（示例 API） |
| `/metrics` | GET | Prometheus 指标（需启用） |
| `/services` | GET | 获取已注册服务列表 |
| `/services/register` | POST | 注册服务 |
| `/services/unregister` | POST | 注销服务 |

### 示例请求

```bash
# 获取员工列表
curl http://localhost:8080/api/employees

# 注册服务
curl -X POST http://localhost:8080/services/register \
  -H "Content-Type: application/json" \
  -d '{
    "name": "user-service",
    "host": "127.0.0.1",
    "port": 3000,
    "path_prefix": "/users"
  }'

# 查看 Prometheus 指标
curl http://localhost:9090/metrics
```

---

## 可观测性

### 日志系统

支持 JSON 和文本两种格式：

```json
{"timestamp":"2026-03-25T10:30:00Z","level":"INFO","event":"request_started","method":"GET","path":"/api/employees"}
```

### Prometheus 指标

```promql
# HTTP 请求总数
http_requests_total{method="GET", path="/api/employees", status="200"}

# 请求延迟直方图
http_request_duration_seconds_bucket{le="0.005"}
http_request_duration_seconds_sum
http_request_duration_seconds_count

# 上游服务延迟
http_upstream_duration_seconds_sum{service="user-service"}
```

### 分布式追踪

遵循 W3C Trace Context 标准，支持 Jaeger、Zipkin 等后端。

---

## 开发指南

### 添加新的路由

在 `src/router.c` 中添加处理函数：

```c
void handle_new_endpoint(client_ctx_t *client) {
    // 业务逻辑
    send_response(client, 200, "application/json", response_data);
}

// 在 route_request 中注册
if (strcmp(url, "/new-endpoint") == 0) {
    handle_new_endpoint(client);
    return;
}
```

### 添加新的指标

在 `src/metrics.c` 中定义：

```c
// 定义指标
static metric_t my_custom_metric = {
    .name = "my_custom_metric",
    .type = METRIC_COUNTER
};

// 在适当位置更新
metrics_increment(&my_custom_metric);
```

---

## 故障排查

### 常见问题

#### 1. 编译失败：找不到 uv.h

**解决**：安装 libuv 开发库

```bash
# Ubuntu/Debian
sudo apt-get install libuv1-dev

# CentOS/RHEL
sudo yum install libuv-devel
```

#### 2. 运行时出现段错误

**可能原因**：
- libuv 版本不兼容
- 内存未正确初始化
- 访问了已释放的资源

**解决**：参考 [SEGFAULT_UV_LOOP_FIX.md](docs/SEGFAULT_UV_LOOP_FIX.md)

#### 3. 性能不达标

**检查项**：
- worker 线程数是否合理（建议与 CPU 核心数匹配）
- 是否启用了 SO_REUSEPORT
- 后端服务响应时间是否过长

---

## 项目结构

```
c-restful-api/
├── src/                      # 源代码
│   ├── main.c               # 程序入口
│   ├── network.c            # 网络层实现
│   ├── router.c             # 路由层实现
│   ├── proxy.c              # 代理转发
│   ├── health_checker.c     # 健康检查
│   ├── metrics.c            # Prometheus 指标
│   ├── tracer.c             # 分布式追踪
│   └── ...
├── include/                  # 头文件
│   └── gateway.h
├── config/                   # 配置文件
│   ├── gateway_config.json
│   └── services.json
├── docs/                     # 文档
│   ├── PERFORMANCE_TEST_REPORT.md
│   ├── SEGFAULT_UV_LOOP_FIX.md
│   ├── PAPER_MICROSERVICE_GATEWAY.md
│   └── ...
├── bin/                      # 编译输出
│   └── c_gateway
└── Makefile
```

---

## 贡献指南

欢迎提交 Issue 和 Pull Request！

### 开发环境设置

```bash
# 克隆项目
git clone <repository-url>
cd c-restful-api

# 安装依赖
sudo apt-get install libuv1-dev libllhttp-dev libcurl4-openssl-dev libcjson-dev

# 编译调试版本
make debug

# 运行测试
make test
```

### 代码规范

- 遵循 C99 标准
- 不使用 Lambda 表达式（C 语言不支持）
- 函数命名采用下划线风格（如 `handle_request`）
- 添加必要的注释和错误处理

---

## 相关论文

本项目的完整学术论文：

**《基于 libuv 的高性能微服务网关设计与实现》**

- 论文全文：[docs/PAPER_MICROSERVICE_GATEWAY.md](docs/PAPER_MICROSERVICE_GATEWAY.md)
- PPT 大纲：[docs/PAPER_PPT_OUTLINE.md](docs/PAPER_PPT_OUTLINE.md)
- 性能测试报告：[docs/PERFORMANCE_TEST_REPORT.md](docs/PERFORMANCE_TEST_REPORT.md)

---

## 许可证

本项目采用 MIT 许可证。详见 [LICENSE](LICENSE) 文件。

---

## 致谢

感谢以下开源项目：

- [libuv](https://github.com/libuv/libuv) - 跨平台异步 IO 库
- [llhttp](https://github.com/nodejs/llhttp) - 高性能 HTTP 解析器
- [libcurl](https://curl.se/libcurl/) - 多功能文件传输库
- [cJSON](https://github.com/DaveGamble/cJSON) - 轻量级 JSON 解析器

---

## 联系方式

- 作者：（您的姓名）
- Email：（您的邮箱）
- GitHub Issues：（问题反馈）

---

**⭐ 如果这个项目对您有帮助，请给一个 Star！**
