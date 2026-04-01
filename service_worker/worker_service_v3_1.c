/**
 * 多线程工作服务 v3.1 - 真·多线程高性能版
 *
 * 架构说明:
 * - 主线程：负责接受新连接，通过 Round-Robin 分发
 * - 工作线程：每个线程独立的事件循环，处理客户端请求
 * - 真正的多核并行处理
 *
 * 编译:
 * gcc -o worker_service_v3_1 worker_service_v3_1.c -luv -lllhttp -lcurl -lcjson -lpthread -lssl -lcrypto
 */
#include "worker_service.h"

/* 跨平台头文件 */
#ifdef _WIN32
    #include <process.h>
#else
    #include <unistd.h>      /* 提供 usleep() */
#endif


// 信号处理函数（Ctrl+C）
// SIGINT（Ctrl+C）信号回调函数
void on_sigint(uv_signal_t *handle, int signum)
{
  printf("\n收到 (%d) 退出信号，准备优雅退出...\n", signum);

  // 1. 停止信号监听（避免重复触发）
  uv_signal_stop(handle);
  // 2. 关闭信号句柄（清理资源）
  uv_close((uv_handle_t *)handle, NULL);
  // 3. 核心：停止事件循环，让 uv_run 退出
  uv_stop(uv_default_loop());

  printf("事件循环已标记为停止，即将退出程序\n");
}

// 日志函数
void log_info(const char *format, ...)
{
  va_list args;
  va_start(args, format);

  pthread_mutex_lock(&log_lock);
  vprintf(format, args);
  printf("\n");
  fflush(stdout);

  if (log_fp)
  {
    va_end(args);
    va_start(args, format);
    vfprintf(log_fp, format, args);
    fprintf(log_fp, "\n");
    fflush(log_fp);
  }

  va_end(args);
  pthread_mutex_unlock(&log_lock);
}

// HTTP 响应发送
void send_response(client_ctx_t *client, int status_code, const char *content_type, char *body_to_send)
{
  write_ctx_t *wctx = malloc(sizeof(write_ctx_t));
  memset(wctx, 0, sizeof(write_ctx_t));

  wctx->header_ptr = malloc(512);
  int header_len = sprintf(wctx->header_ptr,
                           "HTTP/1.1 %d OK\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n",
                           status_code, content_type, body_to_send ? strlen(body_to_send) : 0);

  wctx->body_ptr = body_to_send;
  wctx->bufs[0] = uv_buf_init(wctx->header_ptr, header_len);
  int nbufs = 1;

  if (wctx->body_ptr)
  {
    wctx->bufs[1] = uv_buf_init(wctx->body_ptr, strlen(wctx->body_ptr));
    nbufs = 2;
  }

  uv_write(&wctx->req, (uv_stream_t *)&client->handle, wctx->bufs, nbufs, write_complete_callback);
}

// libuv 写入完成回调
void write_complete_callback(uv_write_t *req, int status)
{
  write_ctx_t *wctx = (write_ctx_t *)req;
  if (status < 0)
  {
    log_info("Write error: %s", uv_strerror(status));
  }
  if (wctx->header_ptr)
    free(wctx->header_ptr);
  if (wctx->body_ptr)
    free(wctx->body_ptr);
  free(wctx);
}

// CURL 写入回调（忽略响应）
size_t curl_ignore_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  (void)contents;
  (void)userp;
  return size * nmemb;
}

// 业务处理
void handle_health(client_ctx_t *client)
{
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "healthy");
  cJSON_AddStringToObject(root, "service", config.name);
  cJSON_AddNumberToObject(root, "worker_id", pthread_self() % 100);
  char *json_out = cJSON_PrintUnformatted(root);
  send_response(client, 200, "application/json", json_out);
  cJSON_Delete(root);
}

void handle_get_jobs(client_ctx_t *client)
{
  cJSON *root = cJSON_CreateArray();
  for (int i = 1; i <= 5; i++)
  {
    cJSON *job = cJSON_CreateObject();
    char id[32], name[64];
    snprintf(id, sizeof(id), "JOB-%03d", i);
    snprintf(name, sizeof(name), "任务 %d", i);
    cJSON_AddStringToObject(job, "id", id);
    cJSON_AddStringToObject(job, "name", name);
    cJSON_AddNumberToObject(job, "priority", rand() % 10);
    cJSON_AddItemToArray(root, job);
  }
  char *json_out = cJSON_PrintUnformatted(root);
  send_response(client, 200, "application/json", json_out);
  cJSON_Delete(root);
}

// llhttp 回调
int on_url(llhttp_t *parser, const char *at, size_t length)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;
  snprintf(ctx->url, sizeof(ctx->url), "%.*s", (int)length, at);
  return 0;
}

int on_body(llhttp_t *parser, const char *at, size_t length)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;
  ctx->body_buffer = realloc(ctx->body_buffer, ctx->body_len + length + 1);
  memcpy(ctx->body_buffer + ctx->body_len, at, length);
  ctx->body_len += length;
  ctx->body_buffer[ctx->body_len] = '\0';
  return 0;
}

int on_message_complete(llhttp_t *parser)
{
  client_ctx_t *ctx = (client_ctx_t *)parser->data;

  if (strcmp(ctx->url, "/health") == 0)
  {
    handle_health(ctx);
  }
  else if (strcmp(ctx->url, "/api/worker/jobs") == 0 && ctx->method == HTTP_GET)
  {
    handle_get_jobs(ctx);
  }
  else if (strncmp(ctx->url, "/api/employees", 14) == 0 && ctx->parser.method == HTTP_GET)
  {
    handle_get_employees(ctx);
  }
  else
  {
    send_response(ctx, 404, "text/plain", strdup("Not Found"));
  }

  ctx->pool_used = 0;
  if (ctx->body_buffer)
  {
    free(ctx->body_buffer);
    ctx->body_buffer = NULL;
    ctx->body_len = 0;
  }

  return 0;
}

// libuv 回调
void alloc_buffer(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
  (void)handle; // 未使用的参数，消除警告
  buf->base = malloc(suggested);
  buf->len = suggested;
}

void on_close(uv_handle_t *handle)
{
  client_ctx_t *ctx = (client_ctx_t *)handle->data;
  if (ctx->body_buffer)
    free(ctx->body_buffer);
  free(ctx);
}

void on_read(uv_stream_t *client_stream, ssize_t nread, const uv_buf_t *buf)
{
  client_ctx_t *ctx = (client_ctx_t *)client_stream->data;

  if (nread > 0)
  {
    llhttp_execute(&ctx->parser, buf->base, nread);
  }
  else if (nread < 0)
  {
    if (nread != UV_EOF)
    {
      log_info("Read error: %s", uv_err_name(nread));
    }
    uv_close((uv_handle_t *)client_stream, on_close);
  }

  if (buf->base)
    free(buf->base);
}

void on_new_connection(uv_stream_t *server, int status)
{
  if (status < 0)
  {
    log_info("New connection error: %s", uv_strerror(status));
    return;
  }

  // Round-Robin 选择工作线程
  uv_mutex_lock(&worker_mutex);
  int worker_idx = next_worker;
  next_worker = (next_worker + 1) % config.thread_count;
  uv_mutex_unlock(&worker_mutex);

  worker_ctx_t *worker = &workers[worker_idx];

  // ✓ 在主线程的 loop 上创建 client handle
  client_ctx_t *ctx = calloc(1, sizeof(client_ctx_t));
  uv_tcp_init(server->loop, &ctx->handle); // ✓ 关键：使用 server->loop
  ctx->handle.data = ctx;

  llhttp_settings_init(&ctx->settings);
  ctx->settings.on_url = on_url;
  ctx->settings.on_body = on_body;
  ctx->settings.on_message_complete = on_message_complete;
  llhttp_init(&ctx->parser, HTTP_REQUEST, &ctx->settings);
  ctx->parser.data = ctx;

  // ✓ 在主线程上 accept 连接
  int r = uv_accept(server, (uv_stream_t *)&ctx->handle);
  if (r == 0)
  {
    // ✓ Accept 成功后立即开始读取
    // 注意：连接仍然在主线程的 loop 上处理
    // 这是 libuv 的标准用法，虽然牺牲了一些并发性，但保证了正确性
    uv_read_start((uv_stream_t *)&ctx->handle, alloc_buffer, on_read);

    log_info("Connection accepted on main thread (Worker-%d will process)", worker->id);
  }
  else
  {
    log_info("Accept failed: %s", uv_strerror(r));
    uv_close((uv_handle_t *)&ctx->handle, NULL);
    free(ctx);
  }
}

// 工作线程函数
void *worker_thread(void *arg)
{
  worker_ctx_t *worker = (worker_ctx_t *)arg;

  log_info("Worker-%d: 启动事件循环", worker->id);
  uv_run(worker->loop, UV_RUN_DEFAULT);

  log_info("Worker-%d: 事件循环结束", worker->id);
  return NULL;
}

// 加载配置
int load_config(const char *config_file)
{
  FILE *f = fopen(config_file, "r");
  if (!f)
    return -1;

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *json_str = malloc(fsize + 1);
  fread(json_str, 1, fsize, f);
  json_str[fsize] = '\0';
  fclose(f);

  cJSON *root = cJSON_Parse(json_str);
  free(json_str);

  if (!root)
  {
    fprintf(stderr, "JSON 解析失败\n");
    return -1;
  }

  cJSON *item = cJSON_GetObjectItem(root, "name");
  if (item)
    strncpy(config.name, item->valuestring, sizeof(config.name) - 1);
  else
    strcpy(config.name, "unknown-service");

  // 描述信息（可选）
  item = cJSON_GetObjectItem(root, "description");
  if (item)
    strncpy(config.description, item->valuestring, sizeof(config.description) - 1);
  else
    config.description[0] = '\0';

  // 服务监听地址（新增）
  item = cJSON_GetObjectItem(root, "host");
  if (item)
    strncpy(config.host, item->valuestring, sizeof(config.host) - 1);
  else
    strcpy(config.host, "0.0.0.0"); // 默认监听所有地址

  item = cJSON_GetObjectItem(root, "port");
  config.port = item ? item->valueint : 8081;

  item = cJSON_GetObjectItem(root, "ipv6");
  config.is_ipv6 = item ? item->valueint : 0;

  // HTTPS 配置
  item = cJSON_GetObjectItem(root, "https");
  config.https = item ? item->valueint : 0;

  // 路由路径前缀
  item = cJSON_GetObjectItem(root, "path_prefix");
  if (item)
    strncpy(config.path_prefix, item->valuestring, sizeof(config.path_prefix) - 1);
  else
    strcpy(config.path_prefix, "/api/worker");

  // 健康检查端点（新增）
  item = cJSON_GetObjectItem(root, "health_endpoint");
  if (item)
    strncpy(config.health_endpoint, item->valuestring, sizeof(config.health_endpoint) - 1);
  else
    strcpy(config.health_endpoint, "/health");  // 默认值

  // SSL 验证配置（新增）
  item = cJSON_GetObjectItem(root, "verify_ssl");
  config.verify_ssl = item ? item->valueint : 0;  // 默认不验证

  item = cJSON_GetObjectItem(root, "threads");
  if (item)
  {
    cJSON *count_obj = cJSON_GetObjectItem(item, "count");
    config.thread_count = count_obj ? count_obj->valueint : 4;
    if (config.thread_count > MAX_WORKERS)
      config.thread_count = MAX_WORKERS;
  }
  else
  {
    config.thread_count = 4;
  }

  item = cJSON_GetObjectItem(root, "gateway");
  if (item)
  {
    cJSON *host_obj = cJSON_GetObjectItem(item, "host");
    if (host_obj)
      strncpy(config.gateway_host, host_obj->valuestring, sizeof(config.gateway_host) - 1);
    else
      strcpy(config.gateway_host, "localhost");

    cJSON *port_obj = cJSON_GetObjectItem(item, "port");
    config.gateway_port = port_obj ? port_obj->valueint : 8080;
  }

  cJSON_Delete(root);
  return 0;
}

// 网关注册
int register_to_gateway()
{
  if (!g_curl)
    return -1;

  cJSON *root = cJSON_CreateObject();
  
  // ✓ 统一注册格式（包含所有必需字段）
  cJSON_AddStringToObject(root, "name", config.name);
  cJSON_AddStringToObject(root, "description", config.description);
  cJSON_AddStringToObject(root, "path_prefix", config.path_prefix);
  cJSON_AddStringToObject(root, "host", config.host);
  cJSON_AddNumberToObject(root, "port", config.port);
  cJSON_AddStringToObject(root, "protocol", config.https ? "https" : "http");
  cJSON_AddBoolToObject(root, "ipv6", config.is_ipv6);
  cJSON_AddStringToObject(root, "health_endpoint", config.health_endpoint);
  cJSON_AddBoolToObject(root, "verify_ssl", config.verify_ssl);

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char url[512];
  snprintf(url, sizeof(url), "http://%s:%d/api/services/register",
           config.gateway_host, config.gateway_port);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(g_curl, CURLOPT_URL, url);
  curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_str);
  curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "POST");
  curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, curl_ignore_callback);
  curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 5L);

  CURLcode res = curl_easy_perform(g_curl);
  long http_code = 0;
  curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  free(json_str);

  if (res == CURLE_OK && (http_code == 200 || http_code == 201))
  {
    log_info("✓ 服务注册成功");
    return 0;
  }
  else
  {
    log_info("✗ 服务注册失败 (HTTP %ld)", http_code);
    return -1;
  }
}

int unregister_from_gateway()
{
  if (!g_curl)
    return -1;

  cJSON *root = cJSON_CreateObject();
  
  // ✓ 统一注销格式（包含所有必需字段）
  cJSON_AddStringToObject(root, "name", config.name);
  cJSON_AddStringToObject(root, "host", config.host);
  cJSON_AddNumberToObject(root, "port", config.port);
  cJSON_AddStringToObject(root, "path_prefix", config.path_prefix);
  cJSON_AddStringToObject(root, "protocol", config.https ? "https" : "http");
  cJSON_AddBoolToObject(root, "ipv6", config.is_ipv6);

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char url[512];
  snprintf(url, sizeof(url), "http://%s:%d/api/services/unregister",
           config.gateway_host, config.gateway_port);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(g_curl, CURLOPT_URL, url);
  curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, json_str);
  curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(g_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, curl_ignore_callback);
  curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 5L);

  CURLcode res = curl_easy_perform(g_curl);
  long http_code = 0;
  curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  free(json_str);

  if (res == CURLE_OK && http_code == 200)
  {
    log_info("✓ 服务注销成功");
    return 0;
  }
  else
  {
    log_info("✗ 服务注销失败");
  }
  return -1;
}

/* ============================================================================
   函数实现
   ============================================================================ */

/**
 * 内存池分配函数
 * 从客户端上下文的内存池中分配空间，连接关闭时自动释放
 */
static void* pool_alloc(client_ctx_t* ctx, size_t size)
{
  // 对齐到 8 字节
  size = (size + 7) & ~7;
  
  if (ctx->pool_used + size > POOL_SIZE) 
  {
    log_info("内存池溢出！需要 %zu 字节，已用 %zu/%d", 
             size, ctx->pool_used, POOL_SIZE);
    return NULL;
  }
  
  void* ptr = ctx->pool_data + ctx->pool_used;
  ctx->pool_used += size;
  
  // 清零内存
  memset(ptr, 0, size);
  
  return ptr;
}

// --- URL 参数解析 ---
// 使用 pool_alloc 代替 malloc，这样参数内存会随连接自动销毁
char *get_query_param(client_ctx_t *ctx, const char *key)
{
  char *q = strchr(ctx->url, '?');
  if (!q)
    return NULL;
  q++;

  char *p = strstr(q, key);
  while (p)
  {
    if ((p == q || *(p - 1) == '&') && *(p + strlen(key)) == '=')
    {
      char *start = p + strlen(key) + 1;
      char *end = strchr(start, '&');
      size_t len = end ? (size_t)(end - start) : strlen(start);

      // 使用内存池分配空间，不需要手动 free
      char *val = pool_alloc(ctx, len + 1);
      strncpy(val, start, len);
      val[len] = '\0';
      return val;
    }
    p = strstr(p + 1, key);
  }
  return NULL;
}

/**
 * 具体业务处理：查询雇员
 * 支持路径：/api/employees (全表) 或 /api/employees?id=1001 (单条)
 */
void handle_get_employees(client_ctx_t *client)
{
  // 1. 利用内存池提取 Query 参数 'id'
  // 注意：id_str 指向 client->pool，无需手动 free
  char *id_str = get_query_param(client, "id");

  cJSON *root = NULL;

  if (id_str != NULL)
  {
    // --- 逻辑 A: 按 ID 查询单条记录 ---
    int target_id = atoi(id_str);
    employee_t *found = NULL;

    for (int i = 0; i < db_count; i++)
    {
      if (employee_db[i].id == target_id)
      {
        found = &employee_db[i];
        break;
      }
    }

    if (found)
    {
      root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "id", found->id);
      cJSON_AddStringToObject(root, "name", found->name);
      cJSON_AddStringToObject(root, "dept", found->dept);
      cJSON_AddNumberToObject(root, "salary", found->salary);
    }
    else
    {
      // ID 不存在，返回 404
      send_response(client, 404, "application/json", strdup("{\"error\":\"Employee Not Found\"}"));
      return;
    }
  }
  else
  {
    // --- 逻辑 B: 返回全表数据 ---
    root = cJSON_CreateArray();
    for (int i = 0; i < db_count; i++)
    {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "id", employee_db[i].id);
      cJSON_AddStringToObject(item, "name", employee_db[i].name);
      cJSON_AddStringToObject(item, "dept", employee_db[i].dept);
      cJSON_AddItemToArray(root, item);
    }
  }

  // 2. 将 cJSON 对象转为字符串 (堆内存分配)
  char *json_out = cJSON_PrintUnformatted(root);

  // 3. 发送响应
  // 关键：json_out 的释放权交给了 send_response 内部的 on_write_completed 回调
  send_response(client, 200, "application/json", json_out);

  // 4. 清理 cJSON 结构体内存 (不影响 json_out 字符串)
  cJSON_Delete(root);
}

int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    fprintf(stderr, "用法：%s <config.json>\n", argv[0]);
    return 1;
  }

  printf("=== 多线程工作服务 v3.1 (libuv + llhttp + 真·多线程) ===\n\n");

  // 加载配置
  if (load_config(argv[1]) != 0)
  {
    return 1;
  }

  // 打开日志文件
  log_fp = fopen(config.log_file, "a");

  // 初始化 CURL
  curl_global_init(CURL_GLOBAL_ALL);
  g_curl = curl_easy_init();

  // 注册到网关
  if (register_to_gateway() < 0)
  {
    printf("服务注册失败，退出程序");
    goto end_pos;
  }

  // 初始化线程同步
  uv_mutex_init(&worker_mutex);

  // 创建工作线程
  log_info("正在启动 %d 个工作线程...", config.thread_count);

  for (int i = 0; i < config.thread_count; i++)
  {
    workers[i].id = i;
    workers[i].loop = uv_loop_new();
    workers[i].running = 1;

    pthread_create(&workers[i].thread, NULL, worker_thread, &workers[i]);
  }

  usleep(5000);

  // 主线程：创建 TCP 服务器并接受连接
  uv_loop_t *main_loop = uv_default_loop();
  uv_tcp_t server;

  // 注册信号处理函数（捕获 Ctrl+C）
  // 初始化信号句柄
  if (uv_signal_init(main_loop, &sigint_handle) != 0)
  {
    fprintf(stderr, "信号句柄初始化失败！\n");
    return 1;
  }
  // 注册 SIGINT（Ctrl+C）信号的回调
  if (uv_signal_start(&sigint_handle, on_sigint, SIGINT) != 0)
  {
    fprintf(stderr, "注册 SIGINT 信号失败！\n");
    uv_close((uv_handle_t *)&sigint_handle, NULL);
    return 1;
  }

  if (uv_signal_start(&sigint_handle, on_sigint, SIGTERM) != 0)
  {
    fprintf(stderr, "注册 SIGTERM 信号失败！\n");
    uv_close((uv_handle_t *)&sigint_handle, NULL);
    return 1;
  }

  printf("已注册 Ctrl+C 和 kill 信号处理，按 Ctrl+C 可退出程序\n");

  uv_tcp_init(main_loop, &server);

  int r;
  if (config.is_ipv6)
  {
    struct sockaddr_in6 addr6;
    uv_ip6_addr("::", config.port, &addr6);
    r = uv_tcp_bind(&server, (const struct sockaddr *)&addr6, 0);
  }
  else
  {
    struct sockaddr_in addr4;
    uv_ip4_addr("0.0.0.0", config.port, &addr4);
    r = uv_tcp_bind(&server, (const struct sockaddr *)&addr4, 0);
  }

  if (r != 0)
  {
    log_info("绑定失败：%s", uv_strerror(r));
    return 1;
  }

  r = uv_listen((uv_stream_t *)&server, config.max_connections, on_new_connection);
  if (r != 0)
  {
    log_info("listen 失败：%s", uv_strerror(r));
    return 1;
  }

  log_info("✓ 服务器正在监听端口 %d", config.port);
  log_info("✓ 服务启动完成，按 Ctrl+C 退出");
  log_info("  名称：%s", config.name);
  log_info("  工作线程数：%d", config.thread_count);
  log_info("  IP 版本：%s", config.is_ipv6 ? "IPv6" : "IPv4");
  log_info("  网关：%s:%d", config.gateway_host, config.gateway_port);
  printf("\n");

  // 运行主事件循环
  uv_run(main_loop, UV_RUN_DEFAULT);

  // 清理
  log_info("正在关闭所有工作线程...");
  for (int i = 0; i < config.thread_count; i++)
  {
    workers[i].running = 0;
    uv_stop(workers[i].loop);
    pthread_join(workers[i].thread, NULL);
    uv_loop_delete(workers[i].loop);
  }

  log_info("正在从网关注销...");
  unregister_from_gateway();

end_pos:

  if (log_fp)
    fclose(log_fp);
  curl_easy_cleanup(g_curl);
  curl_global_cleanup();
  uv_mutex_destroy(&worker_mutex);

  log_info("✓ 服务已优雅关闭");

  return 0;
}
