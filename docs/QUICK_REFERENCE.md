# 微服务网关 - 快速参考卡

## 🚀 一分钟快速开始

### 1. 编译
```bash
# Linux/macOS
make

# Windows
build.bat
```

### 2. 配置服务 (services.json)
```json
{
  "services": [{
    "name": "test-service",
    "path_prefix": "/api/test",
    "host": "localhost",
    "port": 8081,
    "protocol": "http"
  }]
}
```

### 3. 运行
```bash
./bin/c_gateway
```

### 4. 测试
```bash
curl http://localhost:8080/api/test
```

---

## 📋 常用命令速查

### 注册服务
```bash
# 基本格式
./bin/register_service <网关 host> <网关 port> <服务名> <路径前缀> <服务 host> <服务端口> [协议] [健康端点] [SSL 验证] [IPv6]

# HTTP 服务
./bin/register_service localhost 8080 user-svc /api/users localhost 8081

# HTTPS 服务（不验证证书）
./bin/register_service localhost 8080 pay-svc /api/payments localhost 8443 https /health false

# IPv6 服务
./bin/register_service localhost 8080 ipv6-svc /api/v6 ::1 8083 http /health false true
```

### 查看状态
```bash
# 查看已注册服务
curl http://localhost:8080/api/services

# 查看网关健康状态
curl http://localhost:8080/health
```

### 测试转发
```bash
# 转发到后端服务
curl http://localhost:8080/api/users/123
curl -X POST http://localhost:8080/api/payments -d '{"amount":100}'
```

---

## 🔧 配置文件模板

### HTTP 服务
```json
{
  "name": "my-service",
  "path_prefix": "/api/myservice",
  "host": "localhost",
  "port": 8081,
  "protocol": "http",
  "health_endpoint": "/health",
  "verify_ssl": false,
  "ipv6": false
}
```

### HTTPS 服务（生产环境）
```json
{
  "name": "secure-service",
  "path_prefix": "/api/secure",
  "host": "api.example.com",
  "port": 443,
  "protocol": "https",
  "health_endpoint": "/api/health",
  "verify_ssl": true,
  "ipv6": false
}
```

### HTTPS 服务（开发环境）
```json
{
  "name": "dev-https-service",
  "path_prefix": "/api/dev",
  "host": "localhost",
  "port": 8443,
  "protocol": "https",
  "health_endpoint": "/health",
  "verify_ssl": false,
  "ipv6": false
}
```

### IPv6 服务
```json
{
  "name": "ipv6-service",
  "path_prefix": "/api/ipv6",
  "host": "::1",
  "port": 8083,
  "protocol": "http",
  "health_endpoint": "/health",
  "verify_ssl": false,
  "ipv6": true
}
```

### 多实例负载均衡
```json
{
  "services": [
    {
      "name": "order-service",
      "path_prefix": "/api/orders",
      "host": "localhost",
      "port": 8082,
      "protocol": "http"
    }
  ]
}
```
然后动态注册多个实例：
```bash
./bin/register_service ... localhost 8082
./bin/register_service ... localhost 8083
./bin/register_service ... localhost 8084
```

---

## 🎯 常见使用场景

### 场景 1: 本地开发测试
```json
{
  "services": [{
    "name": "local-api",
    "path_prefix": "/api",
    "host": "localhost",
    "port": 3000,
    "protocol": "http"
  }]
}
```

### 场景 2: 微服务架构
```json
{
  "services": [
    {"name": "user-svc", "path_prefix": "/api/users", "host": "localhost", "port": 8001},
    {"name": "product-svc", "path_prefix": "/api/products", "host": "localhost", "port": 8002},
    {"name": "order-svc", "path_prefix": "/api/orders", "host": "localhost", "port": 8003},
    {"name": "payment-svc", "path_prefix": "/api/payments", "host": "localhost", "port": 8443, "protocol": "https"}
  ]
}
```

### 场景 3: 混合部署（HTTP + HTTPS + IPv6）
```json
{
  "services": [
    {"name": "public-api", "path_prefix": "/api/public", "host": "localhost", "port": 8080},
    {"name": "secure-api", "path_prefix": "/api/secure", "host": "localhost", "port": 8443, "protocol": "https"},
    {"name": "internal-api", "path_prefix": "/api/internal", "host": "::1", "port": 9000, "ipv6": true}
  ]
}
```

---

## 🐛 故障排查

### 问题 1: 服务注册失败
```bash
# 检查服务是否可达
curl http://localhost:8081/health

# 查看网关日志
tail -f gateway.log
```

### 问题 2: HTTPS 证书错误
```json
// 开发环境临时方案
{
  "verify_ssl": false
}

// 生产环境正确做法
{
  "verify_ssl": true,
  // 确保系统有正确的 CA 证书
}
```

### 问题 3: IPv6 不可用
```bash
# 检查 IPv6 支持
ping6 ::1

# Windows 启用 IPv6
netsh interface ipv6 install
```

### 问题 4: 编译失败
```bash
# Linux 安装依赖
sudo apt-get install libuv1-dev libcurl4-openssl-dev libcjson-dev

# macOS
brew install libuv curl cjson

# Windows (MSYS2)
pacman -S mingw-w64-x86_64-libuv mingw-w64-x86_64-curl mingw-w64-x86_64-cjson
```

---

## 📊 性能调优

### 增加工作线程数
```c
// src/main.c
#define WORKER_THREADS 8  // 默认 4
```

### 调整内存池大小
```c
// include/gateway.h
#define POOL_SIZE 16384  // 默认 8192 (8KB)
```

### 修改健康检查间隔
```c
// include/gateway.h
#define HEALTH_CHECK_INTERVAL 3000  // 默认 5000 (5 秒)
```

### 启用 IPv6 优先
```c
// src/main.c
#define ENABLE_IPV6 1  // 默认 0
```

---

## 🔍 调试技巧

### 查看详细日志
```bash
# 重定向日志到文件
./bin/c_gateway > gateway.log 2>&1

# 实时查看日志
tail -f gateway.log
```

### 测试 SSL 证书
```bash
# 检查证书信息
openssl s_client -connect localhost:8443

# 生成测试证书
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes
```

### 网络抓包
```bash
# HTTP
tcpdump -i lo0 -s 0 -w http.pcap port 8080

# HTTPS
tcpdump -i lo0 -s 0 -w https.pcap port 8443
```

---

## 📈 监控指标

### 通过 API 获取
```bash
# 服务列表和状态
curl http://localhost:8080/api/services | jq '.[].instances[].health'

# 请求统计
curl http://localhost:8080/api/services | jq '.[].instances[].requests'
```

### 日志分析
```bash
# 统计健康检查成功/失败
grep "\[Health\]" gateway.log | grep "✓" | wc -l
grep "\[Health\]" gateway.log | grep "✗" | wc -l

# 统计转发请求数
grep "\[Proxy\]" gateway.log | grep "转发请求" | wc -l
```

---

## 🆘 帮助信息

### register_service 工具
```bash
./bin/register_service
# 显示用法和参数说明
```

### 查看示例文档
```bash
cat EXAMPLES.md
```

### 查看完整文档
```bash
cat README.md
```

---

## 💡 最佳实践 Tips

1. ✅ **生产环境**必须启用 SSL 验证 (`verify_ssl: true`)
2. ✅ 每个服务至少**2 个实例**实现高可用
3. ✅ 使用**自定义健康检查端点**而非根路径
4. ✅ 定期**轮换日志文件**避免磁盘占满
5. ✅ 配置**防火墙规则**限制访问
6. ✅ 使用**域名**而非 IP 地址便于迁移
7. ✅ 开发环境可禁用 SSL 验证方便调试
8. ✅ IPv6 环境优先使用 IPv6 地址

---

**快速参考卡版本**: v1.0  
**最后更新**: 2026-03-20
