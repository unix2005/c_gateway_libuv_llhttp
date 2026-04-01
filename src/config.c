#include "gateway.h"

// 加载网关主配置
int load_gateway_config(const char *config_file)
{
  FILE *f = fopen(config_file, "r");
  if (!f)
  {
    fprintf(stderr, "[Config] 无法打开配置文件：%s\n", config_file);
    return -1;
  }
  printf("[Config] 正在加载网关配置文件：%s\n", config_file);

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
    fprintf(stderr, "[Config] JSON 解析失败\n");
    return -1;
  }

  // 加载网关配置
  cJSON *gateway = cJSON_GetObjectItem(root, "gateway");
  if (gateway)
  {
    cJSON *item = NULL;

    // 工作线程数
    item = cJSON_GetObjectItem(gateway, "worker_threads");
    g_gateway_config.worker_threads = item ? item->valueint : 4;

    // 服务端口
    item = cJSON_GetObjectItem(gateway, "service_port");
    g_gateway_config.service_port = item ? item->valueint : 8080;

    // IPv6 开关
    item = cJSON_GetObjectItem(gateway, "enable_ipv6");
    g_gateway_config.enable_ipv6 = item ? item->valueint : 0;

    // HTTPS 开关
    item = cJSON_GetObjectItem(gateway, "enable_https");
    g_gateway_config.enable_https = item ? item->valueint : 0;

    // 日志路径
    item = cJSON_GetObjectItem(gateway, "log_path");
    strncpy(g_gateway_config.log_path,
            item ? item->valuestring : "gateway.log",
            sizeof(g_gateway_config.log_path) - 1);

    // 健康检查间隔
    item = cJSON_GetObjectItem(gateway, "health_check_interval");
    g_gateway_config.health_check_interval = item ? item->valueint : DEFAULT_HEALTH_CHECK_INTERVAL;

    // SSL 证书路径
    item = cJSON_GetObjectItem(gateway, "ssl_cert_path");
    if (item)
    {
      strncpy(g_gateway_config.ssl_cert_path, item->valuestring, sizeof(g_gateway_config.ssl_cert_path) - 1);
    }

    // SSL 私钥路径
    item = cJSON_GetObjectItem(gateway, "ssl_key_path");
    if (item)
    {
      strncpy(g_gateway_config.ssl_key_path, item->valuestring, sizeof(g_gateway_config.ssl_key_path) - 1);
    }

    // === 可观测性配置 ===
    cJSON *observability = cJSON_GetObjectItem(gateway, "observability");
    if (observability)
    {
      // 日志配置
      item = cJSON_GetObjectItem(observability, "enable_logging");
      g_gateway_config.observability.enable_logging = item ? item->valueint : 1;

      item = cJSON_GetObjectItem(observability, "log_level");
      const char *log_level_str = item ? item->valuestring : "info";
      if (strcmp(log_level_str, "debug") == 0)
        g_gateway_config.observability.log_level = LOG_LEVEL_DEBUG;
      else if (strcmp(log_level_str, "warn") == 0)
        g_gateway_config.observability.log_level = LOG_LEVEL_WARN;
      else if (strcmp(log_level_str, "error") == 0)
        g_gateway_config.observability.log_level = LOG_LEVEL_ERROR;
      else
        g_gateway_config.observability.log_level = LOG_LEVEL_INFO;

      item = cJSON_GetObjectItem(observability, "enable_json_log");
      g_gateway_config.observability.enable_json_log = item ? item->valueint : 1;

      // 指标配置
      item = cJSON_GetObjectItem(observability, "enable_metrics");
      g_gateway_config.observability.enable_metrics = item ? item->valueint : 1;

      item = cJSON_GetObjectItem(observability, "metrics_port");
      g_gateway_config.observability.metrics_port = item ? item->valueint : 9090;

      item = cJSON_GetObjectItem(observability, "metrics_path");
      strncpy(g_gateway_config.observability.metrics_path,
              item ? item->valuestring : "/metrics",
              sizeof(g_gateway_config.observability.metrics_path) - 1);

      // 追踪配置
      item = cJSON_GetObjectItem(observability, "enable_tracing");
      g_gateway_config.observability.enable_tracing = item ? item->valueint : 1;

      item = cJSON_GetObjectItem(observability, "tracing_exporter");
      strncpy(g_gateway_config.observability.tracing_exporter,
              item ? item->valuestring : "console",
              sizeof(g_gateway_config.observability.tracing_exporter) - 1);

      item = cJSON_GetObjectItem(observability, "tracing_endpoint");
      strncpy(g_gateway_config.observability.tracing_endpoint,
              item ? item->valuestring : "http://localhost:14268/api/traces",
              sizeof(g_gateway_config.observability.tracing_endpoint) - 1);

      item = cJSON_GetObjectItem(observability, "tracing_sample_rate");
      g_gateway_config.observability.tracing_sample_rate = item ? item->valuedouble : 1.0;
    }
    else
    {
      // 默认值
      g_gateway_config.observability.enable_logging = 1;
      g_gateway_config.observability.log_level = LOG_LEVEL_INFO;
      g_gateway_config.observability.enable_json_log = 1;
      g_gateway_config.observability.enable_metrics = 1;
      g_gateway_config.observability.metrics_port = 9090;
      strcpy(g_gateway_config.observability.metrics_path, "/metrics");
      g_gateway_config.observability.enable_tracing = 1;
      strcpy(g_gateway_config.observability.tracing_exporter, "console");
      strcpy(g_gateway_config.observability.tracing_endpoint, "http://localhost:14268/api/traces");
      g_gateway_config.observability.tracing_sample_rate = 1.0;
    }

    printf("[Config] 网关配置已加载\n");
    printf("  - 工作线程：%d\n", g_gateway_config.worker_threads);
    printf("  - 服务端口：%d\n", g_gateway_config.service_port);
    printf("  - IPv6: %s\n", g_gateway_config.enable_ipv6 ? "enabled" : "disabled");
    printf("  - HTTPS: %s\n", g_gateway_config.enable_https ? "enabled" : "disabled");
    printf("  - 日志路径：%s\n", g_gateway_config.log_path);
    printf("  - 健康检查间隔：%dms\n", g_gateway_config.health_check_interval);

    // 打印可观测性配置
    printf("\n[Config] === 可观测性配置 ===\n");
    printf("  - 日志：%s (级别：%s, JSON: %s)\n",
           g_gateway_config.observability.enable_logging ? "enabled" : "disabled",
           g_gateway_config.observability.log_level == LOG_LEVEL_DEBUG ? "DEBUG" : g_gateway_config.observability.log_level == LOG_LEVEL_INFO ? "INFO"
                                                                               : g_gateway_config.observability.log_level == LOG_LEVEL_WARN   ? "WARN"
                                                                                                                                              : "ERROR",
           g_gateway_config.observability.enable_json_log ? "yes" : "no");
    printf("  - 指标：%s (端口：%d, 路径：%s)\n",
           g_gateway_config.observability.enable_metrics ? "enabled" : "disabled",
           g_gateway_config.observability.metrics_port,
           g_gateway_config.observability.metrics_path);
    printf("  - 追踪：%s (导出器：%s, 采样率：%.1f%%)\n",
           g_gateway_config.observability.enable_tracing ? "enabled" : "disabled",
           g_gateway_config.observability.tracing_exporter,
           g_gateway_config.observability.tracing_sample_rate * 100.0);

    if (g_gateway_config.enable_https)
    {
      printf("  - SSL 证书：%s\n", g_gateway_config.ssl_cert_path);
      printf("  - SSL 私钥：%s\n", g_gateway_config.ssl_key_path);
    }
  }
  else
  {
    // 使用默认配置
    printf("[Config] 未找到网关配置，使用默认值\n");
    g_gateway_config.worker_threads = 4;
    g_gateway_config.service_port = 8080;
    g_gateway_config.enable_ipv6 = 0;
    g_gateway_config.enable_https = 0;
    strcpy(g_gateway_config.log_path, "gateway.log");
    g_gateway_config.health_check_interval = DEFAULT_HEALTH_CHECK_INTERVAL;
    g_gateway_config.ssl_cert_path[0] = '\0';
    g_gateway_config.ssl_key_path[0] = '\0';
  }

  cJSON_Delete(root);
  return 0;
}

int load_service_config(const char *config_file)
{
  FILE *f = fopen(config_file, "r");
  if (!f)
  {
    fprintf(stderr, "[Config] 无法打开配置文件：%s\n", config_file);
    return -1;
  }

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
    fprintf(stderr, "[Config] JSON 解析失败\n");
    return -1;
  }

  cJSON *services = cJSON_GetObjectItem(root, "services");
  if (!services)
  {
    cJSON_Delete(root);
    return -1;
  }

  int count = 0;
  cJSON *svc = NULL;
  cJSON_ArrayForEach(svc, services)
  {
    cJSON *name_obj = cJSON_GetObjectItem(svc, "name");
    cJSON *path_obj = cJSON_GetObjectItem(svc, "path_prefix");
    cJSON *host_obj = cJSON_GetObjectItem(svc, "host");
    cJSON *port_obj = cJSON_GetObjectItem(svc, "port");
    cJSON *protocol_obj = cJSON_GetObjectItem(svc, "protocol");
    cJSON *health_obj = cJSON_GetObjectItem(svc, "health_endpoint");
    cJSON *verify_ssl_obj = cJSON_GetObjectItem(svc, "verify_ssl");
    cJSON *ipv6_obj = cJSON_GetObjectItem(svc, "ipv6");

    if (name_obj && path_obj && host_obj && port_obj)
    {
      // 协议类型（默认 HTTP）
      protocol_t protocol = PROTOCOL_HTTP;
      if (protocol_obj && strcmp(protocol_obj->valuestring, "https") == 0)
      {
        protocol = PROTOCOL_HTTPS;
      }

      // SSL 验证（默认验证）
      int verify_ssl = verify_ssl_obj ? verify_ssl_obj->valueint : 1;

      // IPv6 支持
      int is_ipv6 = ipv6_obj ? ipv6_obj->valueint : 0;

      service_register_with_ipv6(name_obj->valuestring,
                                 NULL, // description 从配置文件读取时为 NULL
                                 path_obj->valuestring,
                                 host_obj->valuestring,
                                 port_obj->valueint,
                                 protocol,
                                 health_obj ? health_obj->valuestring : NULL,
                                 verify_ssl,
                                 is_ipv6);
      count++;
    }
  }

  cJSON_Delete(root);

  printf("[Config] 加载了 %d 个服务配置\n", count);
  return count;
}
