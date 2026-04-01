# 网关服务配置化升级说明

## 📋 升级概述

已将网关服务的硬编码配置改为外部 JSON 配置文件，实现灵活的运行时配置管理。

---

## ✨ 新增功能

### 1. 配置外置化
所有网关运行参数现在都可以通过 JSON 配置文件进行管理：

- ✅ **工作线程数** - 动态调整并发能力
- ✅ **服务端口** - 灵活切换监听端口
- ✅ **IPv6 开关** - 一键启用 IPv6 双栈
- ✅ **HTTPS 开关** - 快速启用加密传输
- ✅ **日志路径** - 自定义日志输出位置
- ✅ **健康检查间隔** - 调整故障检测频率
- ✅ **SSL 证书路径** - 指定 HTTPS 证书位置

### 2. 配置文件结构

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

## 🔧 代码变更

### 1. 头文件 (include/gateway.h)

**新增**:
```c
// 网关配置结构体
typedef struct {
    int worker_threads;           // 工作线程数
    int service_port;             // 服务端口
    int enable_ipv6;              // IPv6 开关
    int enable_https;             // HTTPS 开关
    char log_path[512];           // 日志文件路径
    int health_check_interval;    // 健康检查间隔 (毫秒)
    char ssl_cert_path[512];      // SSL 证书路径
    char ssl_key_path[512];       // SSL 私钥路径
} gateway_config_t;

// 全局配置变量
extern gateway_config_t g_gateway_config;
```

**修改**:
```c
// 原常量定义已删除
// #define WORKER_THREADS 4
// #define ENABLE_IPV6 0
// #define HEALTH_CHECK_INTERVAL 5000
```

### 2. 配置文件 (src/config.c)

**新增函数**:
```c
int load_gateway_config(const char* config_file);
```

功能：
- 读取并解析 JSON 配置文件
- 填充全局配置结构体 `g_gateway_config`
- 提供默认值回退机制
- 打印加载的配置信息

### 3. 主程序 (src/main.c)

**修改前**:
```c
#define WORKER_THREADS 4
#define ENABLE_IPV6 0

int main() 
{
    pthread_t threads[WORKER_THREADS];
    // ...
}
```

**修改后**:
```c
int main(int argc, char* argv[]) 
{
    // 加载配置文件
    const char* config_file = "gateway_config.json";
    if (argc > 1) {
        config_file = argv[1];
    }
    
    if (load_gateway_config(config_file) < 0) {
        fprintf(stderr, "警告：未能加载网关配置文件\n");
    }
    
    // 使用动态配置
    pthread_t threads[g_gateway_config.worker_threads];
    // ...
}
```

---

## 📁 新增文件

### 配置文件
- `gateway_config.json` - 基础 HTTP 配置示例
- `gateway_config_https.json` - HTTPS + IPv6 配置示例

### 文档文件
- `GATEWAY_CONFIG_GUIDE.md` - 完整配置指南
- `CONFIG_UPGRADE_NOTES.md` - 本升级说明文档

---

## 🚀 使用方法

### 编译

```bash
# Linux/macOS
make clean
make

# Windows (MSYS2/MinGW)
gcc -O3 -Wall -I./include -o bin/c_gateway.exe \
    src/main.c src/network.c src/service_registry.c \
    src/health_checker.c src/proxy.c src/config.c \
    src/router.c src/utils.c \
    -luv -lllhttp -lcurl -lpthread -lcjson
```

### 运行

```bash
# 使用默认配置文件
./bin/c_gateway

# 使用指定配置文件
./bin/c_gateway gateway_config.json

# 使用 HTTPS 配置
./bin/c_gateway gateway_config_https.json
```

### 启动输出示例

```
=== 微服务网关启动 (HTTPS + IPv6 支持) ===
[Config] 网关配置已加载
  - 工作线程：4
  - 服务端口：8080
  - IPv6: disabled
  - HTTPS: disabled
  - 日志路径：logs/gateway.log
  - 健康检查间隔：5000ms
[Config] 加载了 3 个服务配置
[Thread 0x7f8b4c000700] 网关正在监听 8080 端口... (IPv6: disabled, HTTPS: disabled)
```

---

## 🎯 配置场景示例

### 场景 1: 开发环境

```json
{
  "gateway": {
    "worker_threads": 2,
    "service_port": 8080,
    "enable_ipv6": 0,
    "enable_https": 0,
    "log_path": "gateway.log",
    "health_check_interval": 5000
  }
}
```

**特点**:
- 少量线程，节省资源
- 纯 HTTP，便于调试
- 宽松的健康检查

### 场景 2: 生产环境（高性能）

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
  }
}
```

**特点**:
- 多线程，高并发
- HTTPS 加密传输
- IPv6 双栈支持
- 严格的健康检查

### 场景 3: 容器化部署

```json
{
  "gateway": {
    "worker_threads": 4,
    "service_port": 8080,
    "enable_ipv6": 0,
    "enable_https": 0,
    "log_path": "/dev/stdout",
    "health_check_interval": 5000
  }
}
```

**特点**:
- 标准输出日志
- 适配容器环境
- 简化配置

---

## ⚙️ 配置项详解

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| worker_threads | int | 4 | 工作线程数 |
| service_port | int | 8080 | 监听端口 |
| enable_ipv6 | int | 0 | IPv6 开关 (0/1) |
| enable_https | int | 0 | HTTPS 开关 (0/1) |
| log_path | string | gateway.log | 日志路径 |
| health_check_interval | int | 5000 | 健康检查间隔 (ms) |
| ssl_cert_path | string | - | SSL 证书路径 |
| ssl_key_path | string | - | SSL 私钥路径 |

---

## 🔍 验证配置

### 1. JSON 语法检查

```bash
python -m json.tool gateway_config.json > /dev/null && echo "✓ JSON 格式正确"
```

### 2. 配置加载测试

```bash
./bin/c_gateway gateway_config.json 2>&1 | grep "网关配置已加载"
```

### 3. 运行时验证

```bash
# 查看启动日志中的配置信息
tail -f gateway.log | grep "网关配置"
```

---

## 🛠️ 故障排查

### 问题：配置文件未找到

**症状**:
```
[Config] 无法打开配置文件：gateway_config.json
```

**解决**:
```bash
# 检查文件是否存在
ls -l gateway_config.json

# 检查当前目录
pwd
```

### 问题：JSON 解析失败

**症状**:
```
[Config] JSON 解析失败
```

**解决**:
```bash
# 验证 JSON 格式
python -m json.tool gateway_config.json

# 常见错误：
# - 缺少逗号
# - 多余的逗号
# - 引号不匹配
# - 使用了注释（JSON 不支持注释）
```

### 问题：配置不生效

**症状**: 修改配置后重启服务，但配置未更新

**解决**:
- 确认修改的是正确的配置文件
- 检查配置文件路径是否正确
- 查看启动日志确认配置加载信息
- 确保服务完全重启（不是热重载）

---

## 📊 性能建议

### 工作线程数配置

| CPU 核心数 | 推荐线程数 | 场景 |
|-----------|-----------|------|
| 2 核 | 2-4 | 开发/测试 |
| 4 核 | 4-8 | 中小型生产 |
| 8 核 | 8-16 | 大型生产 |
| 16+ 核 | 16-32 | 高并发场景 |

### 健康检查间隔

| 环境 | 推荐间隔 | 说明 |
|------|---------|------|
| 开发 | 10000ms | 减少资源消耗 |
| 测试 | 5000ms | 平衡检测与负载 |
| 生产 | 3000ms | 快速故障检测 |
| 关键系统 | 1000-2000ms | 极高可用性要求 |

---

## 🔒 安全建议

### SSL/TLS 配置

1. **证书管理**
   ```bash
   # 生成自签名证书（仅用于测试）
   openssl req -x509 -newkey rsa:4096 \
     -keyout server.key -out server.crt \
     -days 365 -nodes
   ```

2. **文件权限**
   ```bash
   # 设置私钥权限（仅所有者可读写）
   chmod 600 /etc/ssl/private/server.key
   
   # 设置证书权限（可读）
   chmod 644 /etc/ssl/certs/server.crt
   ```

3. **配置文件权限**
   ```bash
   # 限制配置文件访问
   chmod 644 gateway_config.json
   ```

---

## 📈 监控与日志

### 日志级别

当前版本日志包含：
- ✅ 配置加载信息
- ✅ 服务启动状态
- ✅ 网络连接事件
- ✅ 健康检查结果
- ✅ 错误和警告

### 日志轮转（Linux）

```bash
# /etc/logrotate.d/gateway
/var/log/gateway/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    create 644 root root
}
```

---

## 🎓 最佳实践

### 1. 版本控制

```bash
# 将配置文件纳入版本控制
git add gateway_config.json
git commit -m "添加网关配置"
```

### 2. 环境分离

```
config/
├── development.json    # 开发环境
├── staging.json        # 预发布环境
└── production.json     # 生产环境
```

### 3. 配置审核

- 定期审查配置参数
- 更新 SSL 证书
- 优化线程数和超时设置
- 记录配置变更历史

---

## 📚 相关文档

- [GATEWAY_CONFIG_GUIDE.md](GATEWAY_CONFIG_GUIDE.md) - 完整配置指南
- [README.md](README.md) - 项目说明
- [TESTING.md](TESTING.md) - 测试指南

---

## 🎉 总结

**升级收益**:
- ✅ 无需重新编译即可调整配置
- ✅ 支持多环境灵活部署
- ✅ 提高运维效率
- ✅ 降低配置错误风险

**向后兼容**:
- ✅ 保留原有 services.json 格式
- ✅ 默认值回退机制
- ✅ 渐进式迁移支持

---

**升级日期**: 2026-03-23  
**版本**: v1.0  
**状态**: ✅ 完成
