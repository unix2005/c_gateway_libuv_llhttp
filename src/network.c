#include "gateway.h"

// === SSL/TLS 全局变量 ===
#ifdef HAVE_OPENSSL
SSL_CTX *g_ssl_ctx = NULL;
BIO_METHOD *g_bio_method = NULL;  // 自定义 BIO 方法
#endif

// 内存分配回调
void alloc_buffer(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
  buf->base = malloc(suggested);
  buf->len = suggested;
}

// 连接关闭回调
void on_close(uv_handle_t *handle)
{
  client_ctx_t *ctx = (client_ctx_t *)handle->data;

#ifdef HAVE_OPENSSL
  // 清理 SSL 资源
  if (ctx->ssl)
  {
    SSL_shutdown(ctx->ssl);
    SSL_free(ctx->ssl);
    ctx->ssl = NULL;
  }

  // 清理 BIO
  if (ctx->ssl_bio)
  {
    BIO_free(ctx->ssl_bio);
    ctx->ssl_bio = NULL;
  }

  // 清理 SSL 缓冲区
  if (ctx->ssl_read_buffer)
  {
    free(ctx->ssl_read_buffer);
    ctx->ssl_read_buffer = NULL;
    ctx->ssl_read_len = 0;
    ctx->ssl_read_capacity = 0;
  }

  if (ctx->ssl_write_buffer)
  {
    free(ctx->ssl_write_buffer);
    ctx->ssl_write_buffer = NULL;
    ctx->ssl_write_len = 0;
  }
#endif

  if (ctx->body_buffer)
    free(ctx->body_buffer);
  free(ctx);
}

// 客户端连接关闭（优雅清理）
static void on_client_close(uv_handle_t *handle)
{
  client_ctx_t *ctx = (client_ctx_t *)handle->data;

  // 记录连接关闭日志
  if (g_gateway_config.observability.enable_logging)
  {
    log_debug(ctx, "connection_closed", "Client connection closed");
  }

  // 释放 body 缓冲区
  if (ctx->body_buffer)
  {
    free(ctx->body_buffer);
  }

  // 释放客户端上下文
  free(ctx);
}

// 收集 Body 数据片
int on_body(llhttp_t *parser, const char *at, size_t length)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;
  ctx->body_buffer = realloc(ctx->body_buffer, ctx->body_len + length + 1);
  memcpy(ctx->body_buffer + ctx->body_len, at, length);
  ctx->body_len += length;
  ctx->body_buffer[ctx->body_len] = '\0';
  return 0;
}

// 获取请求 URL
int on_url(llhttp_t *parser, const char *at, size_t length)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;
  snprintf(ctx->url, sizeof(ctx->url), "%.*s", (int)length, at);
  return 0;
}

#if 0
// 请求接收完毕
int on_message_complete(llhttp_t* parser) {
  client_ctx_t* ctx = (client_ctx_t*)parser->data;
  route_request(ctx); // 进入业务路由
  return 0;
}
#endif

// 请求接收完毕
int on_message_complete(llhttp_t *parser)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;

  // === 记录请求开始 ===
  if (g_gateway_config.observability.enable_logging)
  {
    log_info(ctx, "request_started", "method=%s path=%s",
             llhttp_method_name(ctx->parser.method), ctx->url);
  }

  // === 指标收集 ===
  metrics_request_start(ctx);

  // 执行业务路由
  route_request(ctx);

  // 【关键】重置内存池偏移量，下一条请求可以覆盖旧数据，实现内存复用
  ctx->pool.used = 0;

  // 清理 Body 缓冲区（如果用了 realloc）
  if (ctx->body_buffer)
  {
    free(ctx->body_buffer);
    ctx->body_buffer = NULL;
    ctx->body_len = 0;
  }
  return 0;
}

void on_read(uv_stream_t *client_stream, ssize_t nread, const uv_buf_t *buf)
{
  client_ctx_t *ctx = (client_ctx_t *)client_stream->data;

  if (nread > 0)
  {
#ifdef HAVE_OPENSSL
    if (ctx->ssl && ctx->ssl_handshake_state >= 1)
    {
      // HTTPS: 先处理 SSL 握手或解密
      ssl_read_and_process(ctx, buf->base, nread);
    }
    else
    {
      // HTTP: 直接处理明文
      llhttp_execute(&ctx->parser, buf->base, nread);
    }
#else
    llhttp_execute(&ctx->parser, buf->base, nread);
#endif
  }
  else if (nread < 0)
  {
    if (nread != UV_EOF)
    {
      fprintf(stderr, "[Read] 错误：%s\n", uv_strerror(nread));
    }

#ifdef HAVE_OPENSSL
    // 清理 SSL 资源
    if (ctx->ssl)
    {
      SSL_shutdown(ctx->ssl);
    }
#endif

    uv_close((uv_handle_t *)client_stream, on_close);
  }

  if (buf->base)
    free(buf->base);
}

void on_write_completed(uv_write_t *req, int status)
{
  // 通过强转找回包裹我们的上下文
  write_ctx_t *wctx = (write_ctx_t *)req;
  client_ctx_t *ctx = (client_ctx_t *)req->handle->data;

  if (status < 0)
  {
    fprintf(stderr, "Write error: %s\n", uv_strerror(status));
  }

  // === 记录请求完成指标和日志 ===
  uint64_t now = get_time_nanoseconds();
  double duration_sec = (double)(now - ctx->request_start_time) / 1000000000.0;

  if (g_gateway_config.observability.enable_logging)
  {
    log_info(ctx, "request_completed", "duration=%.3fms status=%d",
             duration_sec * 1000.0, 200); // TODO: 从响应中获取真实状态码
  }

  metrics_request_end(ctx, 200, duration_sec);

  // === 导出追踪数据 ===
  if (g_gateway_config.observability.enable_tracing)
  {
    tracing_export_span(ctx, "http_request", duration_sec * 1000.0);
  }

  // 释放 Header 和 Body 占用的堆内存
  if (wctx->header_ptr)
    free(wctx->header_ptr);
  if (wctx->body_ptr)
    free(wctx->body_ptr);

  // 最后释放写入上下文结构体本身
  free(wctx);

  // ✅ 关键修复：如果不是 Keep-Alive，关闭客户端连接
  // 这确保每个请求处理后都能正确清理
  uv_close((uv_handle_t *)&ctx->handle, on_client_close);
}

void on_new_connection(uv_stream_t *server, int status)
{
  if (status < 0)
    return;

  client_ctx_t *ctx = calloc(1, sizeof(client_ctx_t));

  uv_tcp_init(server->loop, &ctx->handle);
  ctx->handle.data = ctx;

  llhttp_settings_init(&ctx->settings);
  ctx->settings.on_url = on_url;
  ctx->settings.on_body = on_body;
  ctx->settings.on_message_complete = on_message_complete;
  llhttp_init(&ctx->parser, HTTP_REQUEST, &ctx->settings);
  ctx->parser.data = ctx;

  // === 初始化可观测性字段 ===
  ctx->request_start_time = get_time_nanoseconds();
  ctx->request_id[0] = '\0';
  ctx->trace_id[0] = '\0';
  ctx->span_id[0] = '\0';
  ctx->is_sampled = 0;

  // 初始化追踪上下文（从请求头读取 traceparent）
  tracing_init_context(ctx, NULL); // TODO: 从请求头解析

#ifdef HAVE_OPENSSL
  // 如果启用了 HTTPS，创建 SSL 连接
  if (g_gateway_config.enable_https && g_ssl_ctx)
  {
    ctx->ssl = SSL_new(g_ssl_ctx);
    if (!ctx->ssl)
    {
      fprintf(stderr, "[SSL] 创建 SSL 连接失败\n");
      ERR_print_errors_fp(stderr);
      free(ctx);
      return;
    }

    // 初始化 BIO
    if (init_ssl_bio() != 0)
    {
      SSL_free(ctx->ssl);
      ctx->ssl = NULL;
      free(ctx);
      return;
    }

    ctx->ssl_bio = BIO_new(g_bio_method);
    BIO_set_data(ctx->ssl_bio, ctx);
    SSL_set_bio(ctx->ssl, ctx->ssl_bio, ctx->ssl_bio);

    // 设置为服务器模式并接受连接
    SSL_set_accept_state(ctx->ssl);
    ctx->ssl_handshake_state = 1; // 握手中

    printf("[SSL] 新的 TLS 连接建立，开始握手...\n");
  }
  else
  {
    ctx->ssl = NULL;
    ctx->ssl_bio = NULL;
    ctx->ssl_handshake_state = 0;
  }
#else
  ctx->ssl_handshake_state = 0;
#endif

  uv_accept(server, (uv_stream_t *)&ctx->handle);

  // 开始读取数据（加密或明文）
  uv_read_start((uv_stream_t *)&ctx->handle, alloc_buffer, on_read);
}

// ============================================================================
// SSL/TLS 初始化（仅当启用 HTTPS 时）
// ============================================================================

#ifdef HAVE_OPENSSL

// BIO 自定义方法（连接 libuv 和 OpenSSL）
// 注意：OpenSSL 1.1+ 使用不透明结构体，需使用 accessor 函数
// BIO_set_num 在 OpenSSL 1.1+ 中已移除，改用 BIO_clear_flags

static int bio_create(BIO *b)
{
  BIO_set_data(b, NULL);    // 设置自定义数据指针
  BIO_set_init(b, 1);       // 标记为已初始化
  BIO_clear_flags(b, ~0);   // 清除所有标志（~0 表示清除所有位）
  return 1;
}

static int bio_destroy(BIO *b)
{
  if (b == NULL)
    return 0;
  BIO_set_data(b, NULL);
  BIO_set_init(b, 0);
  BIO_clear_flags(b, ~0);
  return 1;
}

static int bio_read(BIO *b, char *out, int outl)
{
  client_ctx_t *ctx = (client_ctx_t *)BIO_get_data(b);

  if (!ctx || !ctx->ssl_read_buffer || ctx->ssl_read_len == 0)
  {
    BIO_set_retry_read(b);
    return -1;
  }

  int to_read = (outl < ctx->ssl_read_len) ? outl : ctx->ssl_read_len;
  memcpy(out, ctx->ssl_read_buffer, to_read);

  // 移动剩余数据
  if (to_read < ctx->ssl_read_len)
  {
    memmove(ctx->ssl_read_buffer, ctx->ssl_read_buffer + to_read,
            ctx->ssl_read_len - to_read);
  }
  ctx->ssl_read_len -= to_read;

  return to_read;
}

static int bio_write(BIO *b, const char *in, int inl)
{
  client_ctx_t *ctx = (client_ctx_t *)BIO_get_data(b);

  if (!ctx)
    return -1;

  // 将加密数据写入发送缓冲区
  // 这里需要实现一个发送队列，简化处理：直接调用 uv_write
  // 为了简单起见，我们使用一个临时缓冲区
  if (!ctx->ssl_write_buffer)
  {
    ctx->ssl_write_buffer = malloc(8192);
    ctx->ssl_write_len = 0;
  }

  if (ctx->ssl_write_len + inl > 8192)
  {
    // 缓冲区满，需要扩容
    ctx->ssl_write_buffer = realloc(ctx->ssl_write_buffer, ctx->ssl_write_len + inl);
  }

  memcpy(ctx->ssl_write_buffer + ctx->ssl_write_len, in, inl);
  ctx->ssl_write_len += inl;

  return inl;
}

static long bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
  switch (cmd)
  {
  case BIO_CTRL_FLUSH:
    return 1;
  default:
    return 0;
  }
}

static BIO_METHOD *create_bio_method()
{
  BIO_METHOD *method = BIO_meth_new(BIO_TYPE_SOCKET, "libuv BIO");
  BIO_meth_set_write(method, bio_write);
  BIO_meth_set_read(method, bio_read);
  BIO_meth_set_puts(method, NULL);
  BIO_meth_set_gets(method, NULL);
  BIO_meth_set_ctrl(method, bio_ctrl);
  BIO_meth_set_create(method, bio_create);
  BIO_meth_set_destroy(method, bio_destroy);
  return method;
}

// 初始化 SSL BIO 方法
int init_ssl_bio()
{
  if (!g_bio_method)
  {
    g_bio_method = create_bio_method();
    if (!g_bio_method)
    {
      fprintf(stderr, "[SSL] 创建 BIO 方法失败\n");
      return -1;
    }
  }
  return 0;
}

// 清理 SSL BIO 方法
void cleanup_ssl_bio()
{
  if (g_bio_method)
  {
    BIO_meth_free(g_bio_method);
    g_bio_method = NULL;
  }
}

int init_ssl_context()
{
  if (!g_gateway_config.enable_https)
  {
    return 0; // 未启用 HTTPS，直接返回成功
  }

  printf("[SSL] 初始化 OpenSSL 库...\n");

  // 初始化 OpenSSL 库
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  // 创建 SSL 上下文
  g_ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!g_ssl_ctx)
  {
    fprintf(stderr, "[SSL] 创建 SSL 上下文失败\n");
    ERR_print_errors_fp(stderr);
    return -1;
  }

  // 设置 SSL 选项（禁用不安全的协议版本）
  SSL_CTX_set_options(g_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

  // 加载证书文件
  if (strlen(g_gateway_config.ssl_cert_path) > 0)
  {
    printf("[SSL] 加载证书文件：%s\n", g_gateway_config.ssl_cert_path);
    if (SSL_CTX_use_certificate_file(g_ssl_ctx, g_gateway_config.ssl_cert_path, SSL_FILETYPE_PEM) <= 0)
    {
      fprintf(stderr, "[SSL] 加载证书失败：%s\n", g_gateway_config.ssl_cert_path);
      ERR_print_errors_fp(stderr);
      SSL_CTX_free(g_ssl_ctx);
      g_ssl_ctx = NULL;
      return -1;
    }
  }
  else
  {
    fprintf(stderr, "[SSL] 错误：未配置 SSL 证书路径\n");
    SSL_CTX_free(g_ssl_ctx);
    g_ssl_ctx = NULL;
    return -1;
  }

  // 加载私钥文件
  if (strlen(g_gateway_config.ssl_key_path) > 0)
  {
    printf("[SSL] 加载私钥文件：%s\n", g_gateway_config.ssl_key_path);
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, g_gateway_config.ssl_key_path, SSL_FILETYPE_PEM) <= 0)
    {
      fprintf(stderr, "[SSL] 加载私钥失败：%s\n", g_gateway_config.ssl_key_path);
      ERR_print_errors_fp(stderr);
      SSL_CTX_free(g_ssl_ctx);
      g_ssl_ctx = NULL;
      return -1;
    }
  }
  else
  {
    fprintf(stderr, "[SSL] 错误：未配置 SSL 私钥路径\n");
    SSL_CTX_free(g_ssl_ctx);
    g_ssl_ctx = NULL;
    return -1;
  }

  // 验证私钥与证书是否匹配
  if (!SSL_CTX_check_private_key(g_ssl_ctx))
  {
    fprintf(stderr, "[SSL] 错误：私钥与证书不匹配\n");
    ERR_print_errors_fp(stderr);
    SSL_CTX_free(g_ssl_ctx);
    g_ssl_ctx = NULL;
    return -1;
  }

  printf("[SSL] ✓ SSL/TLS 初始化成功完成\n");
  return 0;
}

// 清理 SSL 上下文
void cleanup_ssl_context()
{
  if (g_ssl_ctx)
  {
    SSL_CTX_free(g_ssl_ctx);
    g_ssl_ctx = NULL;
    EVP_cleanup();
    ERR_free_strings();
    printf("[SSL] SSL 上下文已清理\n");
  }
}
#else
// 未编译 OpenSSL 支持时的占位函数
int init_ssl_context()
{
  if (g_gateway_config.enable_https)
  {
    fprintf(stderr, "[SSL] 警告：启用了 HTTPS 但未编译 OpenSSL 支持，将使用普通 HTTP\n");
    g_gateway_config.enable_https = 0;
  }
  return 0;
}

void cleanup_ssl_context()
{
  // 无需清理
}
#endif

// ============================================================================
// SSL 握手处理
// ============================================================================

#ifdef HAVE_OPENSSL

// 执行 SSL 握手
int do_ssl_handshake(client_ctx_t *ctx)
{
  if (!ctx->ssl || ctx->ssl_handshake_state != 1)
  {
    return -1;
  }

  int ret = SSL_do_handshake(ctx->ssl);

  if (ret == 1)
  {
    // 握手成功
    ctx->ssl_handshake_state = 2;
    printf("[SSL] ✓ TLS 握手成功：%s\n", SSL_get_cipher(ctx->ssl));

    // 发送任何待处理的加密数据
    if (ctx->ssl_write_buffer && ctx->ssl_write_len > 0)
    {
      uv_buf_t buffer = uv_buf_init(ctx->ssl_write_buffer, ctx->ssl_write_len);
      uv_write_t *req = malloc(sizeof(uv_write_t));
      uv_write(req, (uv_stream_t *)&ctx->handle, &buffer, 1, NULL);
      ctx->ssl_write_buffer = NULL;
      ctx->ssl_write_len = 0;
    }

    return 0;
  }

  int err = SSL_get_error(ctx->ssl, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
  {
    // 需要更多数据，继续读取
    return 1; // 继续
  }

  // 错误
  fprintf(stderr, "[SSL] 握手失败，错误码：%d\n", err);
  ERR_print_errors_fp(stderr);
  return -1;
}

// SSL 加密读取（解密数据并传递给 llhttp）
int ssl_read_and_process(client_ctx_t *ctx, const char *data, size_t len)
{
  if (!ctx->ssl || ctx->ssl_handshake_state != 2)
  {
    // 未启用 HTTPS 或握手未完成，直接处理明文
    llhttp_execute(&ctx->parser, data, len);
    return 0;
  }

  // 将加密数据写入 BIO
  BIO_write(ctx->ssl_bio, data, len);

  // 如果正在握手，先完成握手
  if (ctx->ssl_handshake_state == 1)
  {
    do_ssl_handshake(ctx);
    if (ctx->ssl_handshake_state != 2)
    {
      return 0; // 握手未完成，等待更多数据
    }
  }

  // 从 SSL 读取解密后的数据
  char decrypted[8192];
  int decrypted_len;

  while ((decrypted_len = SSL_read(ctx->ssl, decrypted, sizeof(decrypted))) > 0)
  {
    // 将解密后的数据传递给 llhttp
    llhttp_execute(&ctx->parser, decrypted, decrypted_len);
  }

  int err = SSL_get_error(ctx->ssl, decrypted_len);
  if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_ZERO_RETURN)
  {
    fprintf(stderr, "[SSL] 读取失败：%d\n", err);
    return -1;
  }

  return 0;
}

// SSL 写入完成回调
void on_ssl_write_completed(uv_write_t *req, int status)
{
  if (status < 0)
  {
    fprintf(stderr, "[SSL] 写入完成错误：%s\n", uv_strerror(status));
  }
  free(req);
}

// SSL 加密写入（简化版本，适合小数据）
int ssl_write_encrypted_response(client_ctx_t *ctx, const char *data, size_t len)
{
#ifdef HAVE_OPENSSL
  if (!ctx->ssl || ctx->ssl_handshake_state != 2)
  {
    return -1;
  }

  // 使用 SSL_write 加密数据
  int encrypted_len = SSL_write(ctx->ssl, data, len);
  if (encrypted_len <= 0)
  {
    int err = SSL_get_error(ctx->ssl, encrypted_len);
    fprintf(stderr, "[SSL] 加密写入失败：%d\n", err);
    return -1;
  }

  // 从 BIO 读取加密后的数据并发送
  unsigned char bio_data[16384]; // 足够大的缓冲区
  int bio_len;

  while ((bio_len = BIO_read(ctx->ssl_bio, bio_data, sizeof(bio_data))) > 0)
  {
    uv_buf_t buffer = uv_buf_init((char *)bio_data, bio_len);
    uv_write_t *req = malloc(sizeof(uv_write_t));
    memset(req, 0, sizeof(uv_write_t));

    int r = uv_write(req, (uv_stream_t *)&ctx->handle, &buffer, 1, on_ssl_write_completed);
    if (r < 0)
    {
      fprintf(stderr, "[SSL] uv_write 失败：%s\n", uv_strerror(r));
      free(req);
      return -1;
    }
  }

  return 0;
#else
  return -1;
#endif
}

// SSL 加密写入（保留原有函数，内部调用新函数）
int ssl_write_data(client_ctx_t *ctx, const char *data, size_t len)
{
  if (!ctx->ssl || ctx->ssl_handshake_state != 2)
  {
    // 未启用 HTTPS，直接发送明文
    uv_buf_t buffer = uv_buf_init((char *)data, len);
    uv_write_t *req = malloc(sizeof(uv_write_t));
    memset(req, 0, sizeof(uv_write_t));
    return uv_write(req, (uv_stream_t *)&ctx->handle, &buffer, 1, NULL);
  }

  // 调用新的加密写入函数
  return ssl_write_encrypted_response(ctx, data, len);
}

#endif

