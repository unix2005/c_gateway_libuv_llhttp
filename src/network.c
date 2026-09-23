#include "gateway.h"
#include "async_http.h"

/* 前向声明：HTTP 解析失败时回 400 并关闭连接 */
static void abort_bad_request(client_ctx_t *ctx, uv_stream_t *stream);

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

#ifdef HAVE_OPENSSL
/*
 * 统一清理 SSL 相关资源。
 * 关键点：SSL 拥有通过 SSL_set_bio 绑定的 BIO，SSL_free() 会释放它；
 * 若之后再 BIO_free() 就是双重释放（表现为 on_close 里 SIGSEGV）。
 * 因此先 SSL_set_bio(NULL, NULL) 交还所有权，再手动释放 BIO。
 */
static void ssl_cleanup(client_ctx_t *ctx)
{
  if (ctx->ssl)
  {
    /* SSL_free() 会释放通过 SSL_set_bio 绑定的 BIO（rbio==wbio 时只释放一次），
       因此绝不能再 BIO_free()，否则双重释放 → SIGSEGV。
       注意：也不要用 SSL_set_bio(ssl, NULL, NULL) “解绑”，
       该函数会立即 BIO_free_all 掉旧 BIO，同样导致双重释放。 */
    SSL_shutdown(ctx->ssl);
    SSL_free(ctx->ssl);
    ctx->ssl = NULL;
    ctx->ssl_bio = NULL; // 所有权已随 SSL 释放
  }
  else if (ctx->ssl_bio)
  {
    /* SSL 从未创建/已释放时，BIO 才由我们负责释放 */
    BIO_free(ctx->ssl_bio);
    ctx->ssl_bio = NULL;
  }

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
}
#endif

// 连接关闭回调
void on_close(uv_handle_t *handle)
{
  client_ctx_t *ctx = (client_ctx_t *)handle->data;

  /* 客户端断开：取消其所有在途上游请求，避免向已释放上下文写响应 */
  async_http_cancel_by_userdata(handle->loop, ctx);

#ifdef HAVE_OPENSSL
  ssl_cleanup(ctx);
#endif

  /* 释放内存池降级 malloc 登记的块（防泄漏） */
  pool_overflow_free(ctx);

  if (ctx->body_buffer)
    free(ctx->body_buffer);
  free(ctx);
}

// 客户端连接关闭（优雅清理）
static void on_client_close(uv_handle_t *handle)
{
  client_ctx_t *ctx = (client_ctx_t *)handle->data;

  /* 同上：释放前取消在途请求 */
  async_http_cancel_by_userdata(handle->loop, ctx);

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

#ifdef HAVE_OPENSSL
  /* 优雅关闭路径原先完全不清理 SSL，导致每个 HTTPS 连接泄漏 SSL/BIO */
  ssl_cleanup(ctx);
#endif

  // 释放内存池降级 malloc 登记的块（防泄漏）
  pool_overflow_free(ctx);

  // 释放客户端上下文
  free(ctx);
}

// 收集 Body 数据片
int on_body(llhttp_t *parser, const char *at, size_t length)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;

  /* 请求体大小上限：原实现 realloc 无上限（DoS 面）且未判空（NULL 会段错误）。
     超限返回 -1 让 llhttp 报错，上层据此回 413/400。 */
  size_t limit = g_gateway_config.max_body_size ? g_gateway_config.max_body_size
                                                : DEFAULT_MAX_BODY_SIZE;
  if (ctx->body_len + length > limit)
  {
    return -1;
  }

  char *p = realloc(ctx->body_buffer, ctx->body_len + length + 1);
  if (!p)
  {
    return -1;
  }
  ctx->body_buffer = p;
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

  // 依据 HTTP 版本与 Connection 头判断是否保活（llhttp 权威判定）
  // 必须在 route_request 之前完成，send_response 需要据此写 Connection 头
  ctx->keep_alive = llhttp_should_keep_alive(&ctx->parser);

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
    int parse_ok = 1;
#ifdef HAVE_OPENSSL
    /* HTTPS：只要 ssl 存在就必须走 SSL 层。
       原实现要求 ssl_handshake_state >= 1，导致首个 TLS ClientHello
       （state==0）被当成明文喂给 llhttp —— 握手不可能完成。 */
    if (ctx->ssl)
    {
      ssl_read_and_process(ctx, buf->base, nread);
    }
    else
#endif
    {
      parse_ok = (llhttp_execute(&ctx->parser, buf->base, nread) == HPE_OK);
    }

    if (!parse_ok)
    {
      /* 原实现忽略 llhttp_execute 返回值，解析失败静默吞掉且不回 400 */
      abort_bad_request(ctx, client_stream);
    }
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
             duration_sec * 1000.0, wctx->status_code);
  }

  metrics_request_end(ctx, wctx->status_code, duration_sec);

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

  // === 真正的 Keep-Alive：保活则重置上下文并继续读下一条请求 ===
  // 否则关闭连接（ graceful 清理由 on_client_close 完成）
  if (ctx->keep_alive && status >= 0)
  {
    llhttp_init(&ctx->parser, HTTP_REQUEST, &ctx->settings);
    ctx->parser.data = ctx;
    ctx->pool.used = 0;
    ctx->url[0] = '\0';
    ctx->body_buffer = NULL;
    ctx->body_len = 0;
    ctx->request_start_time = get_time_nanoseconds();
    ctx->request_id[0] = '\0';
    ctx->trace_id[0] = '\0';
    ctx->span_id[0] = '\0';
    ctx->is_sampled = 0;
    uv_read_start((uv_stream_t *)&ctx->handle, alloc_buffer, on_read);
  }
  else
  {
    uv_close((uv_handle_t *)&ctx->handle, on_client_close);
  }
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
// 前向声明：握手阶段待发数据的写完成回调
static void on_ssl_handshake_write_done(uv_write_t *req, int status);

/*
 * 将 bio_write 累积的待发密文（ssl_write_buffer）交给 uv_write 发送。
 * 握手与加解密过程中都可能产生待发密文，必须在每次 SSL 调用后冲刷，
 * 否则对端收不到 ServerHello/Certificate，握手永远无法完成。
 */
static void ssl_flush_out(client_ctx_t *ctx)
{
  if (!ctx->ssl_write_buffer || ctx->ssl_write_len == 0)
    return;

  char  *data = ctx->ssl_write_buffer;
  size_t len  = ctx->ssl_write_len;
  ctx->ssl_write_buffer = NULL;
  ctx->ssl_write_len = 0;

  uv_buf_t   buffer = uv_buf_init(data, len);
  uv_write_t *req   = malloc(sizeof(uv_write_t));
  if (!req)
  {
    free(data);
    return;
  }
  req->data = data;
  uv_write(req, (uv_stream_t *)&ctx->handle, &buffer, 1, on_ssl_handshake_write_done);
}

/*
 * 入站密文缓存：SSL_read 经自定义 BIO 的 bio_read 消费本缓冲。
 * 修复要点：原实现用 BIO_write() 写入站数据，而 bio_write 写的是
 * ssl_write_buffer（出站），ssl_read_buffer 全仓库从未被写入，
 * 导致 SSL_read 永远拿不到数据 —— 握手在数学上不可能完成。
 */
static int ssl_read_buffer_append(client_ctx_t *ctx, const char *data, size_t len)
{
  if (ctx->ssl_read_len + len > ctx->ssl_read_capacity)
  {
    size_t need = ctx->ssl_read_len + len;
    size_t cap  = ctx->ssl_read_capacity ? ctx->ssl_read_capacity : 4096;
    while (cap < need)
      cap *= 2;
    char *p = realloc(ctx->ssl_read_buffer, cap);
    if (!p)
      return -1;
    ctx->ssl_read_buffer   = p;
    ctx->ssl_read_capacity = cap;
  }
  memcpy(ctx->ssl_read_buffer + ctx->ssl_read_len, data, len);
  ctx->ssl_read_len += len;
  return 0;
}

// 解析失败回 400 并关闭连接
static void abort_bad_request(client_ctx_t *ctx, uv_stream_t *stream)
{
  send_response(ctx, 400, "text/plain", strdup("Bad Request\n"));
  uv_close((uv_handle_t *)stream, on_client_close);
}

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
    ssl_flush_out(ctx);

    return 0;
  }

  int err = SSL_get_error(ctx->ssl, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
  {
    /* 关键：握手中间产物（ServerHello / Certificate 等）必须立刻发出，
       否则对端收不到，握手会永远卡在 WANT_READ */
    ssl_flush_out(ctx);
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
  client_ctx_t *owner = ctx;
  uv_stream_t *stream = (uv_stream_t *)&ctx->handle;

  if (!ctx->ssl)
  {
    /* 明文 HTTP：直接解析，失败回 400（原实现忽略返回值） */
    if (llhttp_execute(&ctx->parser, data, len) != HPE_OK)
    {
      abort_bad_request(owner, stream);
      return -1;
    }
    return 0;
  }

  /* 入站密文 -> ssl_read_buffer（供 bio_read 消费）
     修复：原实现 BIO_write() 写到了出站 ssl_write_buffer，SSL_read 永远读不到数据 */
  if (ssl_read_buffer_append(ctx, data, len) != 0)
  {
    uv_close((uv_handle_t *)stream, on_client_close);
    return -1;
  }

  /* 握手未完成：只做握手，绝不把 TLS 记录喂给 llhttp */
  if (ctx->ssl_handshake_state < 2)
  {
    int r = do_ssl_handshake(ctx);
    if (r < 0)
    {
      uv_close((uv_handle_t *)stream, on_client_close);
      return -1;
    }
    if (ctx->ssl_handshake_state != 2)
      return 0; // 等待更多数据
  }

  // 从 SSL 读取解密后的数据
  char decrypted[8192];
  int decrypted_len;

  while ((decrypted_len = SSL_read(ctx->ssl, decrypted, sizeof(decrypted))) > 0)
  {
    if (llhttp_execute(&ctx->parser, decrypted, decrypted_len) != HPE_OK)
    {
      abort_bad_request(owner, stream);
      return -1;
    }
  }

  int err = SSL_get_error(ctx->ssl, decrypted_len);
  if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_ZERO_RETURN)
  {
    fprintf(stderr, "[SSL] 读取失败：%d\n", err);
    uv_close((uv_handle_t *)stream, on_client_close);
    return -1;
  }

  ssl_flush_out(ctx); // 冲刷可能的告警/会话票据等出站密文
  return 0;
}

// 堆拥有的 SSL 写上下文：data 与 req 生命周期一致，回调中释放
typedef struct {
    uv_write_t req;
    char *data;
} ssl_write_ctx_t;

// SSL 写完成回调：释放堆缓冲与请求结构体（与 gateway.h 声明一致，非 static）
void on_ssl_write_completed(uv_write_t *req, int status)
{
    ssl_write_ctx_t *w = (ssl_write_ctx_t *)req;
    if (status < 0)
        fprintf(stderr, "[SSL] 写入完成错误：%s\n", uv_strerror(status));
    free(w->data);
    free(w);
}

// 握手/应用数据阶段待发密文的写完成回调：释放堆缓冲与请求结构体
static void on_ssl_handshake_write_done(uv_write_t *req, int status)
{
    (void)status;
    free(req->data);   // 由 ssl_flush_out 交出的堆缓冲区
    free(req);
}

/*
 * 说明：HTTPS 响应完成后暂不执行明文路径 on_write_completed 的收尾逻辑
 * （指标/日志/keep-alive 复位）。实测在该回调里做这些操作会引入
 * 多次请求后的 SIGSEGV 竞态（gdb 附加时因时序变化不复现），
 * 为避免引入回归，保留简单稳定的“直接发送密文”实现。
 * TODO: 单独排查该竞态后再启用 HTTPS 的指标与复位逻辑。
 */

// SSL 加密写入（修复：数据拷贝到堆，避免栈内存被异步 uv_write 引用）
int ssl_write_encrypted_response(client_ctx_t *ctx, const char *data, size_t len, int status_code)
{
#ifdef HAVE_OPENSSL
    if (!ctx->ssl || ctx->ssl_handshake_state != 2)
        return -1;

    int encrypted_len = SSL_write(ctx->ssl, data, (int)len);
    if (encrypted_len <= 0)
    {
        int err = SSL_get_error(ctx->ssl, encrypted_len);
        fprintf(stderr, "[SSL] 加密写入失败：%d\n", err);
        return -1;
    }

    /* SSL_write 产生的出站密文由 bio_write 写入 ssl_write_buffer，
       必须直接发送该缓冲。原实现用 BIO_read() 读取，而 bio_read 走的是
       入站 ssl_read_buffer —— 方向完全相反，响应永远发不出去。 */
    (void)status_code; /* TODO: 竞态修复后再用于 HTTPS 指标 */
    ssl_flush_out(ctx);
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

  // 调用新的加密写入函数（状态码未知，按 200 计）
  return ssl_write_encrypted_response(ctx, data, len, 200);
}

#endif

