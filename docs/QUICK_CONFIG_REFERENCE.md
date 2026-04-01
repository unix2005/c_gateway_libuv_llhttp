# 网关配置快速参考

## 📖 启动命令

```bash
# 默认配置
./bin/c_gateway

# 指定配置
./bin/c_gateway <config.json>
```

---

## ⚙️ 网关配置 (gateway)

| 字段 | 类型 | 默认值 | 说明 | 示例 |
|------|------|--------|------|------|
| `worker_threads` | int | 4 | 工作线程数 | `4` |
| `service_port` | int | 8080 | 监听端口 | `8080` |
| `enable_ipv6` | int | 0 | IPv6 开关 | `0` 或 `1` |
| `enable_https` | int | 0 | HTTPS 开关 | `0` 或 `1` |
| `log_path` | string | gateway.log | 日志路径 | `logs/gateway.log` |
| `health_check_interval` | int | 5000 | 健康检查间隔 (ms) | `3000` |
| `ssl_cert_path` | string | - | SSL 证书路径 | `certs/server.crt` |
| `ssl_key_path` | string | - | SSL 私钥路径 | `certs/server.key` |

---

## 🎯 后端服务配置 (services)

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `name` | string | ✅ | - | 服务名称 |
| `path_prefix` | string | ✅ | - | 路由前缀 |
| `host` | string | ✅ | - | 后端主机 |
| `port` | int | ✅ | - | 后端端口 |
| `protocol` | string | ❌ | http | `http` 或 `https` |
| `health_endpoint` | string | ❌ | - | 健康检查端点 |
| `verify_ssl` | bool | ❌ | false | 验证 SSL 证书 |
| `ipv6` | bool | ❌ | false | IPv6 支持 |

---

## 🚀 快速示例

### 基础 HTTP 配置

```json
{
  "gateway": {
    "worker_threads": 4,
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

### HTTPS + IPv6 配置

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

## 💡 常用场景

### 开发环境

```json
{
  "gateway": {
    "worker_threads": 2,
    "service_port": 8080,
    "enable_ipv6": 0,
    "enable_https": 0,
    "health_check_interval": 10000
  }
}
```

### 生产环境

```json
{
  "gateway": {
    "worker_threads": 8,
    "service_port": 443,
    "enable_ipv6": 1,
    "enable_https": 1,
    "health_check_interval": 3000
  }
}
```

### 容器部署

```json
{
  "gateway": {
    "worker_threads": 4,
    "service_port": 8080,
    "log_path": "/dev/stdout",
    "health_check_interval": 5000
  }
}
```

---

## 🔧 配置建议

### 工作线程数

| CPU 核心 | 推荐线程 |
|---------|---------|
| 2 | 2-4 |
| 4 | 4-8 |
| 8 | 8-16 |
| 16+ | 16-32 |

### 健康检查间隔

| 环境 | 间隔 (ms) |
|------|----------|
| 开发 | 10000 |
| 测试 | 5000 |
| 生产 | 3000 |

---

## ⚠️ 注意事项

1. **端口权限**: Linux 下 1024 以下端口需要 root
2. **SSL 证书**: HTTPS 启用时必须提供证书和私钥
3. **JSON 格式**: 不支持注释，注意逗号和引号
4. **文件权限**: 私钥文件权限应为 600

---

## 🛠️ 故障排查

```bash
# 检查 JSON 格式
python -m json.tool config.json > /dev/null

# 验证配置加载
./bin/c_gateway config.json 2>&1 | grep "配置已加载"

# 查看日志
tail -f gateway.log
```

---

## 📚 完整文档

- [GATEWAY_CONFIG_GUIDE.md](GATEWAY_CONFIG_GUIDE.md) - 详细指南
- [CONFIG_UPGRADE_NOTES.md](CONFIG_UPGRADE_NOTES.md) - 升级说明

---

**更新日期**: 2026-03-23  
**版本**: v1.0
