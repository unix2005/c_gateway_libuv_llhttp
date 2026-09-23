# HTTPS 快速入门指南

## 🚀 5 分钟快速测试

### 步骤 1: 生成证书（30 秒）

```bash
openssl req -x509 -newkey rsa:2048 \
  -keyout server.key -out server.crt \
  -days 365 -nodes \
  -subj "/C=CN/ST=Test/L=Test/O=Test/CN=localhost"
```

### 步骤 2: 创建配置文件（1 分钟）

创建 `gateway_config_https.json`:

```json
{
  "gateway": {
    "worker_threads": 2,
    "service_port": 8443,
    "enable_https": 1,
    "ssl_cert_path": "./server.crt",
    "ssl_key_path": "./server.key"
  }
}
```

### 步骤 3: 编译运行（2 分钟）

```bash
make clean && make
./bin/c_gateway gateway_config_https.json
```

看到以下输出表示成功：
```
[SSL] ✓ SSL/TLS 和 BIO 初始化完成
[SSL] 初始化 OpenSSL 库...
[SSL] ✓ SSL/TLS 初始化成功完成
```

### 步骤 4: 测试连接（30 秒）

打开另一个终端：

```bash
curl -k https://localhost:8443/api/employees
```

返回 JSON 数据表示成功！

---

## 📋 完整示例

### 配置文件（生产环境）

```json
{
  "gateway": {
    "worker_threads": 4,
    "service_port": 443,
    "enable_ipv6": 0,
    "enable_https": 1,
    "ssl_cert_path": "/etc/letsencrypt/live/your-domain.com/fullchain.pem",
    "ssl_key_path": "/etc/letsencrypt/live/your-domain.com/privkey.pem",
    "log_path": "logs/gateway.log",
    "health_check_interval": 5000,
    "observability": {
      "enable_logging": 1,
      "enable_metrics": 1,
      "enable_tracing": 0,
      "log_format": "json",
      "log_level": "info",
      "metrics_port": 9090
    }
  }
}
```

### 使用 Let's Encrypt 证书

```bash
# 安装 certbot
sudo apt-get install certbot

# 获取证书
sudo certbot certonly --standalone -d your-domain.com

# 证书位置
# /etc/letsencrypt/live/your-domain.com/fullchain.pem
# /etc/letsencrypt/live/your-domain.com/privkey.pem
```

---

## 🔍 调试技巧

### 查看详细日志

```bash
# 启动时增加日志级别
./bin/c_gateway gateway_config_https.json 2>&1 | grep -i ssl
```

### 使用 openssl 客户端测试

```bash
# 查看完整的 TLS 握手过程
echo | openssl s_client -connect localhost:8443 -debug 2>&1 | less

# 查看证书信息
echo | openssl s_client -connect localhost:8443 2>/dev/null | openssl x509 -text
```

### 性能测试

```bash
# 使用 ab 测试
ab -n 10000 -c 100 -k https://localhost:8443/api/employees

# 使用 wrk 测试
wrk -t4 -c100 -d30s https://localhost:8443/api/employees
```

---

## ❓ 常见问题

### Q1: 为什么需要 BIO？

**A**: BIO 是 OpenSSL 的抽象层，让我们可以自定义数据的读写方式。在这个实现中，我们用 BIO 连接 libuv 的 TCP 流和 OpenSSL 的加密逻辑。

### Q2: 自签名证书能用于生产吗？

**A**: 不能！生产环境必须使用受信任的 CA 签发的证书（如 Let's Encrypt）。

### Q3: 如何启用 TLS 1.3？

**A**: 在 `init_ssl_context()` 中添加：
```c
SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_3_VERSION);
```

### Q4: 支持 SNI 吗？

**A**: 当前版本不支持。如需支持多域名，需要实现 SNI 回调函数。

---

## 🎯 下一步

- [ ] 阅读 [`HTTPS_IMPLEMENTATION.md`](file://e:\win_e\c-restful-api\HTTPS_IMPLEMENTATION.md) 了解详细架构
- [ ] 阅读 [`SSL_IMPLEMENTATION_SUMMARY.md`](file://e:\win_e\c-restful-api\SSL_IMPLEMENTATION_SUMMARY.md) 查看完整功能
- [ ] 运行 [`test_https.sh`](file://e:\win_e\c-restful-api\test_https.sh) 自动化测试
- [ ] 配置生产环境证书
- [ ] 性能基准测试

---

## 📞 获取帮助

如果遇到问题：

1. 检查 [`SSL_IMPLEMENTATION_SUMMARY.md`](file://e:\win_e\c-restful-api\SSL_IMPLEMENTATION_SUMMARY.md) 的故障排查章节
2. 查看详细日志：`grep -i ssl logs/gateway.log`
3. 使用 openssl 调试：`openssl s_client -connect host:port -debug`

祝你好运！🎉
