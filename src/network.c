#include "gateway.h"

// 内存分配回调
void alloc_buffer(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) 
{
  buf->base = malloc(suggested);
  buf->len = suggested;
}

// 连接关闭回调
void on_close(uv_handle_t* handle) 
{
  client_ctx_t* ctx = (client_ctx_t*)handle->data;
  if (ctx->body_buffer) free(ctx->body_buffer);
  free(ctx);
}

// 客户端连接关闭（优雅清理）
static void on_client_close(uv_handle_t* handle)
{
  client_ctx_t* ctx = (client_ctx_t*)handle->data;
  
  // 记录连接关闭日志
  if (g_gateway_config.observability.enable_logging) {
      log_debug(ctx, "connection_closed", "Client connection closed");
  }
  
  // 释放 body 缓冲区
  if (ctx->body_buffer) {
      free(ctx->body_buffer);
  }
  
  // 释放客户端上下文
  free(ctx);
}

// 收集 Body 数据片
int on_body(llhttp_t* parser, const char* at, size_t length) 
{
  client_ctx_t* ctx = (client_ctx_t*)parser->data;
  ctx->body_buffer = realloc(ctx->body_buffer, ctx->body_len + length + 1);
  memcpy(ctx->body_buffer + ctx->body_len, at, length);
  ctx->body_len += length;
  ctx->body_buffer[ctx->body_len] = '\0';
  return 0;
}

// 获取请求 URL
int on_url(llhttp_t* parser, const char* at, size_t length) 
{
  client_ctx_t* ctx = (client_ctx_t*)parser->data;
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
int on_message_complete(llhttp_t* parser) 
{
  client_ctx_t* ctx = (client_ctx_t*)parser->data;
  
  // === 记录请求开始 ===
  if (g_gateway_config.observability.enable_logging) {
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

void on_read(uv_stream_t* client_stream, ssize_t nread, const uv_buf_t* buf) 
{
  client_ctx_t* ctx = (client_ctx_t*)client_stream->data;
  if (nread > 0) 
  {
    llhttp_execute(&ctx->parser, buf->base, nread);
  } 
  else if (nread < 0) 
  {
    uv_close((uv_handle_t*)client_stream, on_close);
  }
  if (buf->base) free(buf->base);
}

void on_write_completed(uv_write_t *req, int status) 
{
  // 通过强转找回包裹我们的上下文
  write_ctx_t *wctx = (write_ctx_t*)req;
  client_ctx_t* ctx = (client_ctx_t*)req->handle->data;

  if (status < 0) {
    fprintf(stderr, "Write error: %s\n", uv_strerror(status));
  }
  
  // === 记录请求完成指标和日志 ===
  uint64_t now = get_time_nanoseconds();
  double duration_sec = (double)(now - ctx->request_start_time) / 1000000000.0;
  
  if (g_gateway_config.observability.enable_logging) {
      log_info(ctx, "request_completed", "duration=%.3fms status=%d",
               duration_sec * 1000.0, 200);  // TODO: 从响应中获取真实状态码
  }
  
  metrics_request_end(ctx, 200, duration_sec);
  
  // === 导出追踪数据 ===
  if (g_gateway_config.observability.enable_tracing) {
      tracing_export_span(ctx, "http_request", duration_sec * 1000.0);
  }

  // 释放 Header 和 Body 占用的堆内存
  if (wctx->header_ptr) free(wctx->header_ptr);
  if (wctx->body_ptr) free(wctx->body_ptr);

  // 最后释放写入上下文结构体本身
  free(wctx);

  // ✅ 关键修复：如果不是 Keep-Alive，关闭客户端连接
  // 这确保每个请求处理后都能正确清理
  uv_close((uv_handle_t*)&ctx->handle, on_client_close);
}

void on_new_connection(uv_stream_t *server, int status) 
{
  if (status < 0) return;
  client_ctx_t* ctx = calloc(1, sizeof(client_ctx_t));
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
  tracing_init_context(ctx, NULL);  // TODO: 从请求头解析

  uv_accept(server, (uv_stream_t*)&ctx->handle);
  uv_read_start((uv_stream_t*)&ctx->handle, alloc_buffer, on_read);
}
