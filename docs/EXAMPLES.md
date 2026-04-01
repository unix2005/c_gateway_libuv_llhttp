# 微服务网关使用示例

## 📋 场景一：基本 HTTP 服务注册

### 1. 配置文件方式

`services.json`:
```json
{
  "services": [
    {
      "name": "user-service",
      "path_prefix": "/api/users",
      "host": "localhost",
      "port": 8081,
      "protocol": "http"
    }
  ]
}
```

运行网关：
```bash
./bin/c_gateway
```

访问：
```bash
curl http://localhost:8080/api/users/123
# 自动转发到 localhost:8081/api/users/123
```

### 2. 动态注册方式

启动一个用户服务（示例）：
```bash
python3 -m http.server 8081 --bind 127.0.0.1
```

动态注册：
```bash
./bin/register_service localhost 8080 user-service /api/users localhost 8081
```

## 🔐 场景二：HTTPS 安全服务

### 配置 HTTPS 后端服务

`services.json`:
```json
{
  "services": [
    {
      "name": "payment-service",
      "path_prefix": "/api/payments",
      "host": "secure.payment.com",
      "port": 443,
      "protocol": "https",
      "verify_ssl": true,
      "health_endpoint": "/api/health"
    }
  ]
}
```

开发环境（自签名证书）：
```json
{
  "services": [
    {
      "name": "test-https-service",
      "path_prefix": "/api/test",
      "host": "localhost",
      "port": 8443,
      "protocol": "https",
      "verify_ssl": false
    }
  ]
}
```

测试 HTTPS 服务：
```bash
# 创建测试证书
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes

# 启动 Python HTTPS 服务器
python3 -c "
import http.server
import ssl
import socketserver

handler = http.server.SimpleHTTPRequestHandler
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain('cert.pem', 'key.pem')

with socketserver.TCPServer(('localhost', 8443), handler) as httpd:
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    print('HTTPS 服务器运行在 https://localhost:8443')
    httpd.serve_forever()
"

# 注册到网关
./bin/register_service localhost 8080 test-https /api/test localhost 8443 https /health false false

# 通过网关访问
curl http://localhost:8080/api/test/file.html
```

## 🌐 场景三：IPv6 服务支持

### IPv6 单栈服务

`services.json`:
```json
{
  "services": [
    {
      "name": "ipv6-service",
      "path_prefix": "/api/ipv6",
      "host": "::1",
      "port": 8083,
      "protocol": "http",
      "ipv6": true
    }
  ]
}
```

测试 IPv6 服务：
```bash
# 启动 IPv6 HTTP 服务器
python3 -m http.server 8083 --bind ::1

# 注册到网关
./bin/register_service localhost 8080 ipv6-service /api/ipv6 ::1 8083 http /health false true

# 通过网关访问（注意使用 -g 参数禁用 curl 的 IPv6 特殊字符处理）
curl -g http://[::1]:8080/api/ipv6/test
```

### IPv4/IPv6 双栈服务

```json
{
  "services": [
    {
      "name": "dual-stack-service",
      "path_prefix": "/api/dual",
      "host": "service.example.com",
      "port": 8080,
      "protocol": "http",
      "ipv6": false
    }
  ]
}
```

DNS 会自动解析为 IPv4 或 IPv6 地址。

## ⚖️ 场景四：负载均衡

### 多实例配置

`services.json`:
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

启动多个实例：
```bash
# 实例 1
python3 -m http.server 8082 --bind 127.0.0.1 &

# 实例 2
python3 -m http.server 8084 --bind 127.0.0.1 &

# 实例 3
python3 -m http.server 8085 --bind 127.0.0.1 &
```

动态注册多个实例：
```bash
./bin/register_service localhost 8080 order-service /api/orders localhost 8082
./bin/register_service localhost 8080 order-service /api/orders localhost 8084
./bin/register_service localhost 8080 order-service /api/orders localhost 8085
```

查看负载分布：
```bash
curl http://localhost:8080/api/services
```

连续发送请求，观察请求计数分布：
```bash
for i in {1..10}; do
  curl http://localhost:8080/api/orders/test
  echo ""
done
```

## 🏥 场景五：健康检查与故障隔离

### 配置健康检查端点

`services.json`:
```json
{
  "services": [
    {
      "name": "critical-service",
      "path_prefix": "/api/critical",
      "host": "localhost",
      "port": 9000,
      "protocol": "http",
      "health_endpoint": "/status"
    }
  ]
}
```

模拟服务宕机：
```bash
# 启动服务
python3 -m http.server 9000 --bind 127.0.0.1 &
SERVICE_PID=$!

# 注册服务
./bin/register_service localhost 8080 critical-service /api/critical localhost 9000

# 发送请求 - 应该成功
curl http://localhost:8080/api/critical/test

# 停止服务（模拟宕机）
kill $SERVICE_PID

# 等待健康检查（约 5 秒）
sleep 6

# 再次发送请求 - 返回 503 Service Unavailable
curl -v http://localhost:8080/api/critical/test
```

观察日志输出：
```
[Health] ✓ localhost:9000 (HTTP) 健康 [HTTP 200]
[Health] ✗ localhost:9000 (HTTP) 不健康 [HTTP 0, error: Connection refused]
[Gateway] 选择服务实例：critical-service -> 无可用实例
```

## 🔄 场景六：混合协议环境

### 同时管理 HTTP 和 HTTPS 服务

`services.json`:
```json
{
  "services": [
    {
      "name": "public-api",
      "path_prefix": "/api/public",
      "host": "localhost",
      "port": 8081,
      "protocol": "http"
    },
    {
      "name": "secure-api",
      "path_prefix": "/api/secure",
      "host": "localhost",
      "port": 8443,
      "protocol": "https",
      "verify_ssl": false
    },
    {
      "name": "internal-api",
      "path_prefix": "/api/internal",
      "host": "::1",
      "port": 9090,
      "protocol": "http",
      "ipv6": true
    }
  ]
}
```

测试不同服务：
```bash
# HTTP 服务
curl http://localhost:8080/api/public/data

# HTTPS 服务
curl http://localhost:8080/api/secure/data

# IPv6 服务
curl -g http://[::1]:8080/api/internal/data
```

## 🎯 场景七：微服务架构示例

### 完整的电商系统

假设有以下微服务：
- 用户服务 (8081)
- 商品服务 (8082)
- 订单服务 (8083)
- 支付服务 (8443 - HTTPS)

`services.json`:
```json
{
  "services": [
    {
      "name": "user-service",
      "path_prefix": "/api/users",
      "host": "localhost",
      "port": 8081,
      "protocol": "http"
    },
    {
      "name": "product-service",
      "path_prefix": "/api/products",
      "host": "localhost",
      "port": 8082,
      "protocol": "http"
    },
    {
      "name": "order-service",
      "path_prefix": "/api/orders",
      "host": "localhost",
      "port": 8083,
      "protocol": "http"
    },
    {
      "name": "payment-service",
      "path_prefix": "/api/payments",
      "host": "localhost",
      "port": 8443,
      "protocol": "https",
      "verify_ssl": false
    }
  ]
}
```

启动所有服务（示例用 Python HTTP 服务器模拟）：
```bash
python3 -m http.server 8081 --bind 127.0.0.1 &
python3 -m http.server 8082 --bind 127.0.0.1 &
python3 -m http.server 8083 --bind 127.0.0.1 &

# HTTPS 服务需要证书
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"
python3 << 'EOF'
import http.server, ssl, socketserver
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('cert.pem', 'key.pem')
with socketserver.TCPServer(('localhost', 8443), http.server.SimpleHTTPRequestHandler) as s:
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
    s.serve_forever()
EOF &
```

启动网关并测试：
```bash
./bin/c_gateway

# 测试各个服务
curl http://localhost:8080/api/users/profile
curl http://localhost:8080/api/products/list
curl http://localhost:8080/api/orders/create
curl -X POST http://localhost:8080/api/payments/process -d '{"amount":100}'
```

## 🛠️ 场景八：服务注销

### 从网关移除服务

```bash
# 编写注销脚本（需要实现 API 调用）
curl -X DELETE http://localhost:8080/api/services/unregister \
  -H "Content-Type: application/json" \
  -d '{"name":"old-service","host":"localhost","port":8081}'
```

或者重启网关并修改 `services.json`。

## 📊 监控与调试

### 查看服务状态
```bash
curl http://localhost:8080/api/services | python3 -m json.tool
```

输出示例：
```json
[
  {
    "name": "user-service",
    "path_prefix": "/api/users",
    "instances": [
      {
        "host": "localhost",
        "port": 8081,
        "health": "healthy",
        "requests": 156
      }
    ]
  }
]
```

### 压力测试
```bash
# 使用 ab (Apache Bench)
ab -n 1000 -c 10 http://localhost:8080/api/users/test

# 使用 wrk
wrk -t4 -c100 -d30s http://localhost:8080/api/users/test
```

### 查看网关日志
网关会实时输出：
```
[Gateway] 注册新服务：user-service (路径前缀：/api/users, 协议：HTTP)
[Health] 启动健康检查器，间隔 5000ms
[Health] ✓ localhost:8081 (HTTP) 健康 [HTTP 200]
[Proxy] 转发请求：GET /api/users/123 -> IPv4 localhost:8081
[Proxy] 转发响应：200, 大小：256 bytes
```

## 💡 最佳实践

1. **生产环境**：启用 SSL 证书验证
   ```json
   {
     "protocol": "https",
     "verify_ssl": true
   }
   ```

2. **高可用部署**：每个服务至少 2 个实例
   ```bash
   ./bin/register_service ... service1:8081
   ./bin/register_service ... service1:8082
   ```

3. **健康检查优化**：自定义健康端点
   ```json
   {
     "health_endpoint": "/api/health?deep=true"
   }
   ```

4. **IPv6 优先**：在双栈环境中
   ```c
   #define ENABLE_IPV6 1  // src/main.c
   ```

5. **日志记录**：重定向日志到文件
   ```bash
   ./bin/c_gateway > gateway.log 2>&1
   ```
