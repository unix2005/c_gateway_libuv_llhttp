# 网关配置文件使用指南

## 📖 概述

网关服务现在支持通过 JSON 配置文件动态加载所有运行时参数，无需重新编译即可调整服务行为。

## 🚀 使用方法

### 启动命令

```bash
# 使用默认配置文件
./bin/gateway

# 指定配置文件
./bin/gateway gateway_config.json
```

## 📋 配置结构

### 完整示例

```json
{
  "gateway": {
    "worker_threads": 4,
    "service_port": 8080,
    "enable_ipv6": 0,
    "enable_https": 0,
    "log_path": "logs/gateway.log",
    "health_check_interval": 5000,
    "ssl_cert_path": "certs/server.crt",
    "ssl_key_path": "certs/server.key"
  },
  "services": [
    {
      "name": "user-service",
      "path_prefix": "/api/users",
      "host": "localhost",
      "port": 8081,
      "protocol": "http",
      "health_endpoint": "/health",
      "verify_ssl": false,
      "ipv6": 0
    }
  ]
}
```

---

## 🔧 网关配置 (gateway)

### worker_threads
- **类型**: Integer
- **默认值**: 4
- **说明**: 工作线程数量，负责处理客户端连接
- **建议**: 
  - CPU 密集型：设置为 CPU 核心数
  - I/O 密集型：设置为 CPU 核心数的 2-4 倍

### service_port
- **类型**: Integer
- **默认值**: 8080
- **范围**: 1-65535
- **说明**: 网关监听的服务端口
- **注意**: 
  - Linux 下 1024 以下端口需要 root 权限
  - 推荐使用 8080、8443 等高位端口

### enable_ipv6
- **类型**: Boolean (0/1)
- **默认值**: 0 (禁用)
- **说明**: 是否启用 IPv6 双栈支持
- **效果**:
  - `0`: 仅 IPv4
  - `1`: IPv6 双栈（同时支持 IPv4 和 IPv6）

### enable_https
- **类型**: Boolean (0/1)
- **默认值**: 0 (禁用)
- **说明**: 是否启用 HTTPS/TLS 加密传输
- **注意**: 启用时需要提供 SSL 证书和私钥路径

### log_path
- **类型**: String
- **默认值**: "gateway.log"
- **说明**: 日志文件路径
- **建议**: 
  - 开发环境：`gateway.log`
  - 生产环境：`/var/log/gateway/gateway.log`

### health_check_interval
- **类型**: Integer (毫秒)
- **默认值**: 5000 (5 秒)
- **范围**: 1000-60000
- **说明**: 健康检查的时间间隔
- **建议**:
  - 开发环境：5000-10000ms
  - 生产环境：3000-5000ms

### ssl_cert_path
- **类型**: String
- **默认值**: ""
- **说明**: SSL/TLS 证书文件路径（PEM 格式）
- **启用条件**: `enable_https: 1`
- **示例**: 
  - Windows: `C:\certs\server.crt`
  - Linux: `/etc/ssl/certs/server.crt`

### ssl_key_path
- **类型**: String
- **默认值**: ""
- **说明**: SSL/TLS 私钥文件路径（PEM 格式）
- **启用条件**: `enable_https: 1`
- **安全提示**: 确保私钥文件权限为 600

---

## 🎯 后端服务配置 (services)

### name
- **类型**: String
- **必填**: 是
- **说明**: 服务的唯一标识名称

### path_prefix
- **类型**: String
- **必填**: 是
- **说明**: URL 路径前缀，用于路由匹配
- **示例**: `/api/users`, `/api/orders`

### host
- **类型**: String
- **必填**: 是
- **说明**: 后端服务的主机地址
- **支持**: IPv4、IPv6、域名

### port
- **类型**: Integer
- **必填**: 是
- **说明**: 后端服务的端口号

### protocol
- **类型**: String
- **可选值**: "http", "https"
- **默认值**: "http"
- **说明**: 与后端服务的通信协议

### health_endpoint
- **类型**: String
- **可选**: 是
- **说明**: 健康检查的端点路径
- **示例**: `/health`, `/api/health`

### verify_ssl
- **类型**: Boolean
- **默认值**: false
- **说明**: 是否验证后端服务的 SSL 证书
- **建议**:
  - 开发环境：false
  - 生产环境：true

### ipv6
- **类型**: Boolean (0/1)
- **默认值**: 0
- **说明**: 后端服务是否使用 IPv6

---

## 📝 配置文件示例

### 1. 基础 HTTP 配置（开发环境）

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

### 2. HTTPS + IPv6 配置（生产环境）

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

### 3. 多服务混合配置

```json
{
  "gateway": {
    "worker_threads": 4,
    "service_port": 8080,
    "enable_ipv6": 1,
    "enable_https": 0,
    "log_path": "gateway.log",
    "health_check_interval": 5000
  },
  "services": [
    {
      "name": "user-service",
      "path_prefix": "/api/users",
      "host": "192.168.1.100",
      "port": 8081
    },
    {
      "name": "order-service",
      "path_prefix": "/api/orders",
      "host": "192.168.1.101",
      "port": 8082,
      "protocol": "https",
      "verify_ssl": false
    },
    {
      "name": "product-service",
      "path_prefix": "/api/products",
      "host": "::1",
      "port": 8083,
      "ipv6": 1
    }
  ]
}
```

---

## 🔍 配置验证

### 检查配置文件语法

```bash
# 使用 Python 验证 JSON 格式
python -m json.tool gateway_config.json > /dev/null && echo "JSON 格式正确" || echo "JSON 格式错误"
```

### 测试配置加载

```bash
# 启动服务并查看配置输出
./bin/gateway gateway_config.json
```

预期输出：
```
=== 微服务网关启动 (HTTPS + IPv6 支持) ===
[Config] 网关配置已加载
  - 工作线程：4
  - 服务端口：8080
  - IPv6: disabled
  - HTTPS: disabled
  - 日志路径：gateway.log
  - 健康检查间隔：5000ms
[Config] 加载了 3 个服务配置
```

---

## ⚠️ 注意事项

1. **文件权限**（Linux/Unix）
   ```bash
   # 设置配置文件权限
   chmod 644 gateway_config.json
   
   # 设置 SSL 私钥权限
   chmod 600 /etc/ssl/private/server.key
   ```

2. **路径分隔符**
   - Windows: 使用 `\` 或 `\\`
   - Linux/macOS: 使用 `/`

3. **SSL 证书要求**
   - 必须是 PEM 格式
   - 证书和私钥必须匹配
   - 建议使用受信任 CA 签发的证书

4. **性能调优**
   - 增加 `worker_threads` 可提高并发能力
   - 减小 `health_check_interval` 可加快故障检测
   - 但会增加系统负载

---

## 🛠️ 故障排查

### 问题 1: 配置文件未找到

**错误信息**: `[Config] 无法打开配置文件：xxx`

**解决方案**:
- 检查文件路径是否正确
- 检查文件是否存在：`ls -l gateway_config.json`
- 检查文件权限

### 问题 2: JSON 解析失败

**错误信息**: `[Config] JSON 解析失败`

**解决方案**:
- 使用 JSON 验证工具检查语法
- 确保没有注释（标准 JSON 不支持注释）
- 检查引号、逗号、括号是否匹配

### 问题 3: SSL 证书加载失败

**错误信息**: 相关 SSL 错误

**解决方案**:
- 检查证书文件路径是否正确
- 检查证书格式是否为 PEM
- 验证证书和私钥是否匹配

---

## 📚 相关文件

- `gateway_config.json` - 基础 HTTP 配置示例
- `gateway_config_https.json` - HTTPS + IPv6 配置示例
- `services.json` - 后端服务注册配置

---

**更新日期**: 2026-03-23  
**版本**: v1.0
