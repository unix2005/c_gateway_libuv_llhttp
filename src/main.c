#include "gateway.h"
#include "async_http.h"

// 全局网关配置
gateway_config_t g_gateway_config;


// 守护进程化
void daemonize()
{
  pid_t pid;

  // 1. 创建子进程，父进程退出
  pid = fork();
  if (pid < 0)
  {
    perror("fork failed");
    exit(1);
  }
  if (pid > 0)
  {
    exit(0); // 父进程直接退出
  }

  // 2. 创建新会话
  setsid();

  // 3. 再次fork，防止进程重新打开终端
  pid = fork();
  if (pid < 0)
  {
    perror("fork 2 failed");
    exit(1);
  }
  if (pid > 0)
  {
    exit(0);
  }

  // 4. 设置文件权限掩码
  umask(0);

  // 5. 切换工作目录
  int ret=0;
  ret = chdir("/");
  if ( ret < 0 )
  {
    perror("chdir failed");
  }

  // 6. 关闭标准输入输出
  close(0);
  close(1);
  close(2);

  // 重新打开到 /dev/null
  open("/dev/null", O_RDONLY);
  open("/dev/null", O_RDWR);
  open("/dev/null", O_RDWR);
}

// TCP 服务器初始化（支持 IPv6）
int init_tcp_server_ipv6(uv_loop_t *loop, uv_tcp_t *server, const char *addr, int port)
{
  struct sockaddr_in6 addr6;
  struct sockaddr_in addr4;
  int r;

  if (g_gateway_config.enable_ipv6)
  {
    // IPv6 模式
    uv_ip6_addr(addr, port, &addr6);
    r = uv_tcp_bind(server, (const struct sockaddr *)&addr6, 0);
    if (r == 0)
    {
      printf("[Network] IPv6 服务器绑定：%s:%d\n", addr, port);
    }
  }
  else
  {
    // IPv4 模式
    uv_ip4_addr(addr, port, &addr4);
    r = uv_tcp_bind(server, (const struct sockaddr *)&addr4, 0);
    if (r == 0)
    {
      printf("[Network] IPv4 服务器绑定：%s:%d\n", addr, port);
    }
  }

  return r;
}

typedef struct
{
  uv_tcp_t *server;
  uv_loop_t *loop;
  uv_async_t stop_async;   /* 供信号处理器跨线程唤醒本 loop */
} worker_context_t;

/* 所有 worker 事件循环，供信号处理优雅停止 */
#define MAX_WORKER_LOOPS 64
static uv_loop_t *g_worker_loops[MAX_WORKER_LOOPS];
static uv_async_t *g_worker_async[MAX_WORKER_LOOPS];
static int        g_worker_loop_count = 0;

/* 异步唤醒回调：在目标 loop 所属线程中执行，打断其阻塞在 epoll_wait 的事件循环 */
static void on_worker_async_stop(uv_async_t *a)
{
  uv_stop(a->loop);
}

/* SIGINT/SIGTERM：通过 uv_async_send 唤醒各 worker 循环。
   注意：uv_stop 无法唤醒一个阻塞在 epoll_wait 且没有定时器的 loop（stop_flag
   要等 epoll 返回才被检查），必须用 async 写 eventfd/pipe 触发其立即返回。 */
static void on_signal_stop(int signum)
{
  (void)signum;
  for (int i = 0; i < g_worker_loop_count; i++)
  {
    if (g_worker_async[i])
      uv_async_send(g_worker_async[i]);
  }
  health_checker_stop();
}

/* 关闭 loop 内所有残留 handle（监听 + 长连接），再删除 loop，避免 uv_loop_delete 断言 */
static void close_handle_walk(uv_handle_t *h, void *arg)
{
  (void)arg;
  if (h) uv_close(h, NULL);
}

static void worker_done(worker_context_t *ctx)
{
  /* 被 SIGTERM 关闭的服务器不等待 keep-alive 客户端：
     直接 uv_walk 关闭所有 handle，跑一轮 NOWAIT 处理可立即完成的关闭，
     再用 uv_loop_close（忽略 UV_EBUSY，进程退出由 OS 回收资源）。
     避免 UV_RUN_DEFAULT 在长连接上永久阻塞，也避免 uv_loop_delete 断言。 */
  uv_walk(ctx->loop, close_handle_walk, NULL);
  uv_run(ctx->loop, UV_RUN_NOWAIT);
  uv_loop_close(ctx->loop);
  free(ctx->server);
  free(ctx);
}

void *worker_thread(void *arg)
{
  (void)arg;
  worker_context_t *ctx = calloc(1, sizeof(worker_context_t));
  ctx->loop = uv_loop_new();
  ctx->server = malloc(sizeof(uv_tcp_t));
  uv_tcp_init(ctx->loop, ctx->server);

  /* 注册异步停止句柄，使信号处理器能从其它线程唤醒本 loop（见 on_signal_stop） */
  uv_async_init(ctx->loop, &ctx->stop_async, on_worker_async_stop);

  int port = g_gateway_config.service_port;
  int af = g_gateway_config.enable_ipv6 ? AF_INET6 : AF_INET;

  /* 关键修复：必须在 bind 之前创建 socket 并设置 SO_REUSEPORT/SO_REUSEADDR，
     再通过 uv_tcp_open 交给 libuv（原先在 uv_fileno 时 socket 尚未创建，
     setsockopt 作用在垃圾 fd 上，导致多 worker 端口复用失效）。 */
  int fd = socket(af, SOCK_STREAM, 0);
  if (fd < 0)
  {
    perror("[Network] socket() 失败");
    worker_done(ctx);
    return NULL;
  }

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

  int r;
  if (g_gateway_config.enable_ipv6)
  {
    struct sockaddr_in6 a;
    uv_ip6_addr("::", port, &a);
    r = bind(fd, (struct sockaddr *)&a, sizeof(a));
  }
  else
  {
    struct sockaddr_in a;
    uv_ip4_addr("0.0.0.0", port, &a);
    r = bind(fd, (struct sockaddr *)&a, sizeof(a));
  }

  if (r < 0)
  {
    perror("[Network] bind() 失败");
    close(fd);
    worker_done(ctx);
    return NULL;
  }

  /* 将已绑定 socket 移交给 libuv（非阻塞由 uv_tcp_open 设置） */
  r = uv_tcp_open(ctx->server, fd);
  if (r != 0)
  {
    fprintf(stderr, "[Network] uv_tcp_open 失败：%s\n", uv_strerror(r));
    close(fd);
    worker_done(ctx);
    return NULL;
  }

  r = uv_listen((uv_stream_t *)ctx->server, 128, on_new_connection);
  if (r != 0)
  {
    fprintf(stderr, "[Network] uv_listen 失败：%s\n", uv_strerror(r));
    close(fd);
    worker_done(ctx);
    return NULL;
  }

  /* 记录 loop 供信号处理器停止（原子自增分配下标，避免多 worker 线程竞争） */
  int idx = (int)__sync_fetch_and_add(&g_worker_loop_count, 1);
  if (idx < MAX_WORKER_LOOPS)
  {
    g_worker_loops[idx] = ctx->loop;
    g_worker_async[idx] = &ctx->stop_async;
  }

  printf("[Thread %d] 网关正在监听 %d 端口... (IPv6: %s, HTTPS: %s)\n",
         gettid(), port,
         g_gateway_config.enable_ipv6 ? "enabled" : "disabled",
         g_gateway_config.enable_https ? "enabled" : "disabled");

  /* 运行事件循环（被 uv_stop 停止后退出） */
  uv_run(ctx->loop, UV_RUN_DEFAULT);

  /* 优雅退出：关闭所有残留 handle（监听 + 长连接），再删除 loop */
  worker_done(ctx);

  return NULL;
}

// 健康检查线程函数
void *health_check_thread(void *arg)
{
  (void)arg;
  start_health_checker();
  return NULL;
}

int main(int argc, char *argv[])
{
  // 忽略 SIGPIPE 防止客户端断连导致进程退出
  signal(SIGPIPE, SIG_IGN);

  // SIGINT/SIGTERM：优雅停止所有 worker 事件循环
  signal(SIGINT, on_signal_stop);
  signal(SIGTERM, on_signal_stop);

  printf("=== 微服务网关启动 (HTTPS + IPv6 支持) ===\n");

  // 加载网关配置
  const char *config_file = "gateway_config.json";
  if (argc == 2)
  {
    config_file = argv[1];
  }
  else
  {
    printf("请使用配置文件启动：%s %s\n", argv[0], config_file);
    exit(1);
  }

  if (load_gateway_config(config_file) < 0)
  {
    fprintf(stderr, "警告：未能加载网关配置文件，使用默认配置\n");
  }

  // === 初始化 SSL/TLS（如果启用了 HTTPS） ===
  if (g_gateway_config.enable_https)
  {
    if (init_ssl_context() != 0)
    {
      fprintf(stderr, "错误：SSL 初始化失败，无法启动\n");
      return 1;
    }

    // 初始化 SSL BIO 方法
    if (init_ssl_bio() != 0)
    {
      fprintf(stderr, "错误：SSL BIO 初始化失败\n");
      cleanup_ssl_context();
      return 1;
    }

    printf("[SSL] ✓ SSL/TLS 和 BIO 初始化完成\n");
  }

  // 初始化异步转发模块（内部调用 curl_global_init）
  async_http_global_init();

  service_registry_init();

  // === 初始化可观测性模块 ===
  metrics_init();         // 初始化 Prometheus 指标
  metrics_server_start(); // 启动指标服务器

  log_info(NULL, "gateway_started", "Gateway started on port %d", g_gateway_config.service_port);

  /* 加载静态服务配置（此前被注释掉，导致启动后注册表为空，
     只能依赖运行时 POST 注册且重启即丢失） */
  if (load_service_config("services.json") < 0)
  {
    fprintf(stderr, "警告：未能加载服务配置文件 services.json\n");
  }

  // 启动健康检查线程
  pthread_t health_thread;
  pthread_create(&health_thread, NULL, health_check_thread, NULL);

  pthread_t threads[g_gateway_config.worker_threads];
  for (int i = 0; i < g_gateway_config.worker_threads; i++)
  {
    pthread_create(&threads[i], NULL, worker_thread, NULL);
  }

  for (int i = 0; i < g_gateway_config.worker_threads; i++)
  {
    pthread_join(threads[i], NULL);
  }

  pthread_join(health_thread, NULL);

  // 停止并等待指标服务器线程退出，避免进程退出时撕裂仍在运行的 libuv 线程
  metrics_server_stop();
  metrics_server_join();

  // 清理 SSL BIO 方法
  if (g_gateway_config.enable_https)
  {
    cleanup_ssl_bio();
    // 清理 SSL 上下文
    cleanup_ssl_context();
  }

  // 清理异步转发模块
  async_http_global_cleanup();

  return 0;
}
