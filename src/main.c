#include "gateway.h"

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
  chdir("/");

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
} worker_context_t;

void *worker_thread(void *arg)
{
  worker_context_t *ctx = calloc(1, sizeof(worker_context_t));
  ctx->loop = uv_loop_new();
  ctx->server = malloc(sizeof(uv_tcp_t));

  uv_tcp_init(ctx->loop, ctx->server);

  // 支持 SO_REUSEPORT/SO_REUSEADDR
  int fd;
  uv_fileno((const uv_handle_t *)ctx->server, &fd);
  int opt = 1;
#ifdef _WIN32
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

  // 使用 IPv6 双栈或纯 IPv4
  if (init_tcp_server_ipv6(ctx->loop, ctx->server, "::", g_gateway_config.service_port) != 0)
  {
    // IPv6 失败则回退到 IPv4
    printf("[Network] IPv6 绑定失败，回退到 IPv4\n");
    struct sockaddr_in addr4;
    uv_ip4_addr("0.0.0.0", g_gateway_config.service_port, &addr4);
    uv_tcp_bind(ctx->server, (const struct sockaddr *)&addr4, 0);
  }

  uv_listen((uv_stream_t *)ctx->server, 128, on_new_connection);

  printf("[Thread %d] 网关正在监听 %d 端口... (IPv6: %s, HTTPS: %s)\n",
         gettid(), g_gateway_config.service_port,
         g_gateway_config.enable_ipv6 ? "enabled" : "disabled",
         g_gateway_config.enable_https ? "enabled" : "disabled");

  // 运行事件循环
  uv_run(ctx->loop, UV_RUN_DEFAULT);

  // ✅ 优雅退出：确保所有 handle 都已关闭
  // 停止接受新连接
  uv_close((uv_handle_t *)ctx->server, NULL);

  // 持续运行事件循环直到所有活跃 handle 都关闭完成
  // 注意：不能使用 uv_loop_close 检查，因为它会返回 UV_EBUSY 即使 handle 正在关闭中
  while (uv_loop_alive(ctx->loop))
  {
    uv_run(ctx->loop, UV_RUN_ONCE);
  }

  // 现在 loop 已经完全干净，可以安全删除
  free(ctx->server);
  uv_loop_delete(ctx->loop);
  free(ctx);

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

  // 初始化 CURL（支持 SSL/TLS）
  curl_global_init(CURL_GLOBAL_DEFAULT);

  service_registry_init();

  // === 初始化可观测性模块 ===
  metrics_init();         // 初始化 Prometheus 指标
  metrics_server_start(); // 启动指标服务器

  log_info(NULL, "gateway_started", "Gateway started on port %d", g_gateway_config.service_port);

  /*
  if (load_service_config("services.json") < 0) {
    fprintf(stderr, "警告：未能加载服务配置文件\n");
  }
  */

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

  // 清理 SSL BIO 方法
  if (g_gateway_config.enable_https)
  {
    cleanup_ssl_bio();
    // 清理 SSL 上下文
    cleanup_ssl_context();
  }

  // 清理 CURL
  curl_global_cleanup();

  return 0;
}
