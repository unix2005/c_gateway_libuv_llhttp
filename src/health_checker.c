#include "gateway.h"

static uv_timer_t health_timer;
static uv_loop_t *health_loop;
#define MAX_FAILURE_COUNT 3 // 最大失败次数，超过后移除服务

size_t health_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  (void)contents;
  (void)userp;
  return realsize;
}

// 从注册表中移除服务实例
static void remove_unhealthy_service(service_t *svc, int instance_index)
{
  printf("[Health] ✗ 服务 %s:%d 连续 %d 次检查失败，从注册表移除\n",
         svc->instances[instance_index].host,
         svc->instances[instance_index].port,
         MAX_FAILURE_COUNT);

  // 将该实例之后的所有实例前移
  for (int i = instance_index; i < svc->instance_count - 1; i++)
  {
    svc->instances[i] = svc->instances[i + 1];
  }

  // 减少实例数量
  svc->instance_count--;

  // 重置轮询索引
  if (svc->current_instance >= svc->instance_count)
  {
    svc->current_instance = 0;
  }

  printf("[Health] 当前服务 %s 剩余实例数：%d\n", svc->name, svc->instance_count);
}

// 从注册表中删除整个服务定义
static void remove_service_from_registry(service_registry_t *registry, int service_index)
{
  printf("[Health] ✗ 服务 %s 已无健康实例，从注册表删除整个服务\n",
         registry->services[service_index].name);

  // 将该服务之后的所有服务前移
  for (int i = service_index; i < registry->service_count - 1; i++)
  {
    registry->services[i] = registry->services[i + 1];
  }

  // 减少服务总数
  registry->service_count--;

  printf("[Health] 注册表剩余服务数：%d\n", registry->service_count);
}

void check_service_health(service_instance_t *instance)
{
  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  char url[1024];
  const char *proto = (instance->protocol == PROTOCOL_HTTPS) ? "https" : "http";

  memset(url, 0, sizeof(url));
  // 支持 IPv6 地址格式
  if (instance->ip_addr.is_ipv6)
  {
    snprintf(url, sizeof(url), "%s://[%s]:%d%s",
             proto, instance->host, instance->port, instance->endpoint);
  }
  else
  {
    snprintf(url, sizeof(url), "%s://%s:%d%s",
             proto, instance->host, instance->port, instance->endpoint);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, health_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);

  // HTTPS SSL 验证配置
  if (instance->protocol == PROTOCOL_HTTPS)
  {
    if (!instance->verify_ssl)
    {
      // 不验证证书（用于开发/测试）
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    else
    {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
      if (strlen(instance->ca_cert_path) > 0)
      {
        curl_easy_setopt(curl, CURLOPT_CAINFO, instance->ca_cert_path);
      }
    }
  }

  printf("[Health] 检查：%s\n", url);
  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  if (res == CURLE_OK && http_code == 200)
  {
    instance->health = SERVICE_HEALTHY;
    instance->failure_count = 0; // ✓ 重置失败计数
    printf("[Health] ✓ %s:%d (%s) 健康 [HTTP %ld]\n",
           instance->host, instance->port,
           (instance->protocol == PROTOCOL_HTTPS) ? "HTTPS" : "HTTP",
           http_code);
  }
  else
  {
    instance->health = SERVICE_UNHEALTHY;
    instance->failure_count++; // ✓ 增加失败计数
    printf("[Health] ✗ %s:%d (%s) 不健康 [HTTP %ld, error: %s] (失败次数：%d/%d)\n",
           instance->host, instance->port,
           (instance->protocol == PROTOCOL_HTTPS) ? "HTTPS" : "HTTP",
           http_code, curl_easy_strerror(res),
           instance->failure_count, MAX_FAILURE_COUNT);
  }

  instance->last_check_time = uv_now(health_loop);
  curl_easy_cleanup(curl);
}

void health_check_callback(uv_timer_t *handle)
{
  (void)handle;

  pthread_mutex_lock(&g_registry.lock);

  for (int i = 0; i < g_registry.service_count; i++)
  {
    service_t *svc = &g_registry.services[i];

    pthread_mutex_lock(&svc->lock);
    for (int j = 0; j < svc->instance_count; j++)
    {
      check_service_health(&svc->instances[j]);

      // ✓ 检查是否达到移除条件
      if (svc->instances[j].failure_count >= MAX_FAILURE_COUNT)
      {
        remove_unhealthy_service(svc, j);
        j--; // 回退索引，因为后面的元素前移了

        // ✓ 检查是否所有实例都被移除
        if (svc->instance_count == 0)
        {
          pthread_mutex_unlock(&svc->lock);
          remove_service_from_registry(&g_registry, i);
          i--;   // 回退索引，因为后面的服务前移了
          break; // 跳出内层循环，处理下一个服务
        }
      }
    }
    pthread_mutex_unlock(&svc->lock);
  }

  pthread_mutex_unlock(&g_registry.lock);
}

void start_health_checker()
{
  health_loop = uv_loop_new();
  uv_timer_init(health_loop, &health_timer);

  printf("[Health] 启动健康检查器，间隔 %d ms\n", g_gateway_config.health_check_interval);

  uv_timer_start(&health_timer, health_check_callback,
                 g_gateway_config.health_check_interval, g_gateway_config.health_check_interval);

  uv_run(health_loop, UV_RUN_DEFAULT);
}
