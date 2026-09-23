# 网关 HTTPS 完整实现文档

## 概述

本文档详细说明主网关的完整 HTTPS/TLS 功能实现，包括握手、加密读取和加密写入。

## 核心架构

### SSL BIO 机制

使用 OpenSSL 的自定义 BIO 机制连接 libuv 和 OpenSSL：

```
客户端 → TCP (libuv) → BIO → SSL (OpenSSL) → llhttp → 业务逻辑
响应 ← BIO ← SSL ← llhttp ← 业务逻辑 ← TCP
```

### 关键组件

1. **自定义 BIO 方法** (`bio_read`, `bio_write`)
2. **SSL 握手处理** (`do_ssl_handshake`)
3. **加密读取** (`ssl_read_and_process`)
4. **加密写入** (`ssl_write_encrypted_response`)

## 已实现的功能

### ✅ 1. SSL 上下文初始化

**文件**: `src/network.c`

```c
int init_ssl_context() {
    // ✓ 初始化 OpenSSL 库
    SSL_library_init();
    SSL_load_error_strings();
    
    // ✓ 创建 SSL 上下文
    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    
    // ✓ 禁用不安全的协议版本
    SSL_CTX_set_options(g_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    
    // ✓ 加载证书和私钥
    SSL_CTX_use_certificate_file(...);
    SSL_CTX_use_PrivateKey_file(...);
    
    // ✓ 验证密钥匹配
    SSL_CTX_check_private_key(...);
}
```

### ✅ 2. 自定义 BIO 方法

**文件**: `src/network.c`

```c
// BIO 读取（从 SSL 读缓冲区获取数据）
static int bio_read(BIO *b, char *out, int outl) {
    client_ctx_t *ctx = (client_ctx_t*)BIO_get_data(b);
    
    if (!ctx || ctx->ssl_read_len == 0) {
        BIO_set_retry_read(b);
        return -1;
    }
    
    // 从缓冲区复制数据
    int to_read = min(outl, ctx->ssl_read_len);
    memcpy(out, ctx->ssl_read_buffer, to_read);
    
    // 移动剩余数据
    memmove(ctx->ssl_read_buffer, ctx->ssl_read_buffer + to_read, 
            ctx->ssl_read_len - to_read);
    ctx->ssl_read_len -= to_read;
    
    return to_read;
}

// BIO 写入（将加密数据存入发送缓冲区）
static int bio_write(BIO *b, const char *in, int inl) {
    client_ctx_t *ctx = (client_ctx_t*)BIO_get_data(b);
    
    // 扩容或直接复制到 ssl_write_buffer
    // 稍后由 uv_write 发送到网络
}
```

### ✅ 3. SSL 握手流程

**文件**: `src/network.c`

```c
void on_new_connection(uv_stream_t *server, int status) {
    // ... 初始化 client_ctx_t ...
    
#ifdef HAVE_OPENSSL
    if (g_gateway_config.enable_https && g_ssl_ctx) {
        // ✓ 创建 SSL 对象
        ctx->ssl = SSL_new(g_ssl_ctx);
        
        // ✓ 创建并设置 BIO
        ctx->ssl_bio = BIO_new(g_bio_method);
        BIO_set_data(ctx->ssl_bio, ctx);
        SSL_set_bio(ctx->ssl, ctx->ssl_bio, ctx->ssl_bio);
        
        // ✓ 设置为服务器模式
        SSL_set_accept_state(ctx->ssl);
        ctx->ssl_handshake_state = 1;  // 握手中
        
        printf("[SSL] 新的 TLS 连接建立，开始握手...\n");
    }
#endif
    
    // ✓ 开始读取（会触发 SSL 握手）
    uv_read_start((uv_stream_t *)&ctx->handle, alloc_buffer, on_read);
}

// SSL 握手执行函数
int do_ssl_handshake(client_ctx_t *ctx) {
    int ret = SSL_do_handshake(ctx->ssl);
    
    if (ret == 1) {
        // ✓ 握手成功
        ctx->ssl_handshake_state = 2;
        printf("[SSL] ✓ TLS 握手成功：%s\n", SSL_get_cipher(ctx->ssl));
        return 0;
    }
    
    int err = SSL_get_error(ctx->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // ✓ 需要更多数据，继续读取
        return 1;
    }
    
    // ✗ 握手失败
    return -1;
}
```

### ✅ 4. 加密数据读取

**文件**: `src/network.c`

```c
void on_read(uv_stream_t *client_stream, ssize_t nread, const uv_buf_t *buf) {
    client_ctx_t *ctx = (client_ctx_t *)client_stream->data;
    
    if (nread > 0) {
#ifdef HAVE_OPENSSL
        if (ctx->ssl && ctx->ssl_handshake_state >= 1) {
            // ✓ HTTPS: 先处理 SSL 握手或解密
            ssl_read_and_process(ctx, buf->base, nread);
        } else {
            // HTTP: 直接处理明文
            llhttp_execute(&ctx->parser, buf->base, nread);
        }
#else
        llhttp_execute(&ctx->parser, buf->base, nread);
#endif
    }
}

// SSL 解密处理
int ssl_read_and_process(client_ctx_t *ctx, const char *data, size_t len) {
    if (!ctx->ssl || ctx->ssl_handshake_state != 2) {
        // 未启用 HTTPS 或握手未完成
        llhttp_execute(&ctx->parser, data, len);
        return 0;
    }
    
    // ✓ 将加密数据写入 BIO
    BIO_write(ctx->ssl_bio, data, len);
    
    // ✓ 如果正在握手，先完成握手
    if (ctx->ssl_handshake_state == 1) {
        do_ssl_handshake(ctx);
        if (ctx->ssl_handshake_state != 2) {
            return 0;  // 握手未完成
        }
    }
    
    // ✓ 从 SSL 读取解密后的数据
    char decrypted[8192];
    int decrypted_len;
    
    while ((decrypted_len = SSL_read(ctx->ssl, decrypted, sizeof(decrypted))) > 0) {
        // ✓ 将解密后的数据传递给 llhttp
        llhttp_execute(&ctx->parser, decrypted, decrypted_len);
    }
    
    return 0;
}
```

### ✅ 5. 加密数据写入

**文件**: `src/router.c`, `src/network.c`

```c
void send_response(client_ctx_t *client, ...) {
#ifdef HAVE_OPENSSL
    // ✓ 如果启用了 HTTPS 且握手完成
    if (client->ssl && client->ssl_handshake_state == 2) {
        // 构造完整的 HTTP 响应
        char header[512];
        sprintf(header, "HTTP/1.1 %d OK\r\n...", status_code, ...);
        
        // 合并 header 和 body
        char *response_data = malloc(total_len);
        memcpy(response_data, header, header_len);
        memcpy(response_data + header_len, body, body_len);
        
        // ✓ 使用 SSL 加密并发送
        ssl_write_encrypted_response(client, response_data, total_len);
        
        free(response_data);
        if (body_to_send) free(body_to_send);
        return;
    }
#endif
    
    // 普通 HTTP 明文发送（原有逻辑）
}

// SSL 加密写入
int ssl_write_encrypted_response(client_ctx_t *ctx, const char *data, size_t len) {
    // ✓ 使用 SSL_write 加密数据
    int encrypted_len = SSL_write(ctx->ssl, data, len);
    
    // ✓ 从 BIO 读取加密后的数据
    unsigned char bio_data[16384];
    int bio_len;
    
    while ((bio_len = BIO_read(ctx->ssl_bio, bio_data, sizeof(bio_data))) > 0) {
        // ✓ 通过 uv_write 发送到网络
        uv_buf_t buffer = uv_buf_init((char*)bio_data, bio_len);
        uv_write_t* req = malloc(sizeof(uv_write_t));
        uv_write(req, (uv_stream_t*)&ctx->handle, &buffer, 1, on_ssl_write_completed);
    }
    
    return 0;
}
```

### ✅ 6. 资源清理

**文件**: `src/network.c`

```c
void on_close(uv_handle_t *handle) {
    client_ctx_t *ctx = (client_ctx_t *)handle->data;
    
#ifdef HAVE_OPENSSL
    // ✓ SSL 关闭握手
    if (ctx->ssl) {
        SSL_shutdown(ctx->ssl);
        SSL_free(ctx->ssl);
    }
    
    // ✓ 清理 BIO
    if (ctx->ssl_bio) {
        BIO_free(ctx->ssl_bio);
    }
    
    // ✓ 清理缓冲区
    if (ctx->ssl_read_buffer) free(ctx->ssl_read_buffer);
    if (ctx->ssl_write_buffer) free(ctx->ssl_write_buffer);
#endif
    
    free(ctx->body_buffer);
    free(ctx);
}
```

## 数据结构扩展

### client_ctx_t 新增字段

```c
typedef struct {
    uv_tcp_t handle;
    llhttp_t parser;
    // ... 原有字段 ...
    
#ifdef HAVE_OPENSSL
    SSL *ssl;                    // SSL 连接句柄
    BIO *ssl_bio;                // SSL BIO
    char *ssl_read_buffer;       // SSL 读缓冲区
    size_t ssl_read_len;         // 读缓冲区已使用长度
    size_t ssl_read_capacity;    // 读缓冲区容量
    char *ssl_write_buffer;      // SSL 写缓冲区
    size_t ssl_write_len;        // 写缓冲区已使用长度
    int ssl_handshake_state;     // 0=未开始，1=进行中，2=完成
#endif
} client_ctx_t;
```

## 编译配置

### Makefile

```makefile
CC = gcc
CFLAGS = -O3 -Wall -I./include -g -DHAVE_OPENSSL
LIBS = -luv -lllhttp -lcurl -lpthread -lcjson -lssl -lcrypto
```

## 使用指南

### 1. 配置文件

创建 `gateway_config_https.json`:

```json
{
  "gateway": {
    "worker_threads": 4,
    "service_port": 443,
    "enable_ipv6": 0,
    "enable_https": 1,
    "ssl_cert_path": "/path/to/server.crt",
    "ssl_key_path": "/path/to/server.key"
  }
}
```

### 2. 生成测试证书

```bash
openssl req -x509 -newkey rsa:2048 \
  -keyout server.key -out server.crt \
  -days 365 -nodes
```

### 3. 编译和运行

```bash
make clean
make
./bin/c_gateway gateway_config_https.json
```

### 4. 测试 HTTPS 连接

```bash
# 忽略证书验证测试
curl -k https://localhost:443/api/employees

# 或使用 openssl 客户端
openssl s_client -connect localhost:443
```

## 工作流程详解

### 连接建立流程

```
1. 客户端发起 TCP 连接
   ↓
2. on_new_connection 回调
   ↓
3. 创建 SSL 对象和 BIO
   ↓
4. uv_read_start 开始读取
   ↓
5. 等待客户端发送 ClientHello
```

### SSL 握手流程

```
1. 客户端发送 ClientHello (加密数据包)
   ↓
2. on_read 接收到加密数据
   ↓
3. BIO_write 存入缓冲区
   ↓
4. SSL_do_handshake 执行握手
   ↓
5. 可能需要多次往返 (ServerHello, Certificate, ...)
   ↓
6. 握手成功，ssl_handshake_state = 2
```

### 请求处理流程

```
1. 客户端发送加密的 HTTP 请求
   ↓
2. on_read 接收加密数据
   ↓
3. BIO_write 存入缓冲区
   ↓
4. SSL_read 解密数据
   ↓
5. llhttp_execute 解析 HTTP
   ↓
6. route_request 业务处理
   ↓
7. send_response 构造响应
   ↓
8. SSL_write 加密响应
   ↓
9. BIO_read 读取加密数据
   ↓
10. uv_write 发送到网络
```

## 性能优化建议

### 1. SSL 会话缓存

```c
// 在 init_ssl_context 中添加
SSL_CTX_set_session_cache_mode(g_ssl_ctx, SSL_SESS_CACHE_SERVER);
SSL_CTX_sess_set_cache_size(g_ssl_ctx, 1024);
```

### 2. 密码套件优化

```c
// 使用强密码套件
SSL_CTX_set_cipher_list(g_ssl_ctx, 
    "ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES128-GCM-SHA256");
```

### 3. 零拷贝优化

当前实现中数据需要多次复制：
- TCP 缓冲区 → BIO → SSL → 解密缓冲区 → llhttp

未来可以优化为直接使用 SSL_read/SSL_write 包装 uv_stream_t。

## 故障排查

### 常见问题

#### 1. 握手失败

**错误**: `SSL handshake failed`

**排查步骤**:
1. 检查证书和私钥是否匹配
2. 确认客户端支持 TLS 版本
3. 查看 OpenSSL 错误队列：`ERR_print_errors_fp(stderr)`

#### 2. BIO 读写错误

**错误**: `BIO read/write failed`

**解决方案**:
- 确保 BIO 正确关联到 client_ctx_t
- 检查缓冲区是否足够大
- 验证 BIO 方法实现是否正确

#### 3. 内存泄漏

**检测方法**:
```bash
valgrind --leak-check=full ./bin/c_gateway
```

**重点检查**:
- SSL 对象是否正确释放
- BIO 是否正确释放
- 缓冲区是否正确管理

## 安全建议

1. **强制使用 TLS 1.2+**:
   ```c
   SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
   ```

2. **启用 HSTS**:
   ```c
   response_headers += "Strict-Transport-Security: max-age=31536000\r\n";
   ```

3. **证书链验证**:
   ```c
   SSL_CTX_load_verify_locations(g_ssl_ctx, ca_cert_path, NULL);
   ```

4. **前向安全性**:
   优先使用 ECDHE 密钥交换算法

## 参考资料

- [OpenSSL 官方文档](https://www.openssl.org/docs/)
- [RFC 8446 - TLS 1.3](https://tools.ietf.org/html/rfc8446)
- [libuv 文档](https://docs.libuv.org/)
- [BIO 编程指南](https://www.openssl.org/docs/man1.1.1/man7/bio.html)

## 后续改进方向

1. **异步 SSL 握手**: 完全非阻塞的握手流程
2. **TLS 1.3 支持**: 启用最新的 TLS 标准
3. **ALPN/NPN**: 支持 HTTP/2 协议协商
4. **OCSP Stapling**: 提升证书验证性能
5. **Session Ticket**: 加速会话恢复
6. **性能基准测试**: 与 nginx 等成熟方案对比

## 总结

✅ **已完成**:
- SSL 上下文初始化和清理
- 自定义 BIO 方法实现
- SSL 握手逻辑
- 加密数据读取和解密
- 加密数据写入和发送
- 完整的资源清理机制

⚠️ **待优化**:
- 减少数据复制次数
- 完全异步的 SSL 操作
- 更完善的错误处理
- 生产环境压力测试
