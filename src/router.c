#include "gateway.h"

// 模拟数据库数据（保留原有功能）
static employee_t employee_db[] = {
    {1001, "张三", "技术部", 15000.5},
    {1002, "李四", "市场部", 12000.0},
    {1003, "王五", "研发部", 18000.7}};
static int db_count = 3;

void route_request(client_ctx_t *client)
{
  printf("[Router] 收到请求：%s %s\n",
         client->parser.method == HTTP_POST ? "POST" : "GET",
         client->url);

  // 1. 网关健康检查（本地处理）
  if (strcmp(client->url, "/health") == 0)
  {
    send_response(client, 200, "application/json", strdup("{\"status\":\"UP\",\"type\":\"gateway\"}"));
    return;
  }

  // 2. 查看已注册服务列表（本地处理）
  if (strcmp(client->url, "/api/services") == 0 && client->parser.method == HTTP_GET)
  {
    handle_get_services(client);
    return;
  }

  // 2.1 服务注册接口
  if (strcmp(client->url, "/api/services/register") == 0 && client->parser.method == HTTP_POST)
  {
    handle_service_register(client);
    return;
  }

  // 2.2 服务注销接口
  if (strcmp(client->url, "/api/services/unregister") == 0 && client->parser.method == HTTP_DELETE)
  {
    handle_service_unregister(client);
    return;
  }

  // 3. 查找匹配的服务并转发请求
  service_t *target = service_find_by_path(client->url);

  if (target)
  {
    // 找到匹配的服务，选择健康的实例
    service_instance_t *instance = service_select_instance(target);

    if (instance)
    {
      // 使用 proxy.c 中的转发功能
      forward_to_service(client, instance);
      return;
    }

    printf("[Router] No healthy instances for %s\n", target->name);

    // 没有健康的实例
    send_response(client, 503, "application/json",
                  strdup("{\"error\":\"No healthy service instance available\"}"));
    return;
  }
  else
  {
    send_response(client, 404, "text/plain", strdup("Not Found"));
  }

  // 4. 本地业务逻辑（保留原有功能作为后备）
  /*
  if (strcmp(client->url, "/api/data") == 0 && client->parser.method == HTTP_POST)
  {
    printf("收到 POST 数据：%s\n", client->body_buffer ? client->body_buffer : "空");
    send_response(client, 201, "application/json", strdup("{\"message\":\"Created\"}"));
  }
  else if (strncmp(client->url, "/api/employees",14) == 0 && client->parser.method == HTTP_GET)
  {
    handle_get_employees(client);
  }
  else
  {
    send_response(client, 404, "text/plain", strdup("Not Found"));
  }
  */
}

void send_response(client_ctx_t *client, int status_code, const char *content_type, char *body_to_send)
{
  // 1. 创建写入上下文
  write_ctx_t *wctx = malloc(sizeof(write_ctx_t));
  memset(wctx, 0, sizeof(write_ctx_t));

  // 2. 准备 Header
  // 我们必须 malloc Header，因为 send_response 返回后栈上的局部变量会失效
  wctx->header_ptr = malloc(512);
  int header_len = sprintf(wctx->header_ptr,
                           "HTTP/1.1 %d OK\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n",
                           status_code, content_type, body_to_send ? strlen(body_to_send) : 0);

  // 3. 关联 Body
  // 假设 body_to_send 是由 cJSON_Print 生成的堆指针
  wctx->body_ptr = body_to_send;

  // 4. 设置 uv_buf_t 指向这些堆内存
  wctx->bufs[0] = uv_buf_init(wctx->header_ptr, header_len);
  int nbufs = 1;

  if (wctx->body_ptr)
  {
    wctx->bufs[1] = uv_buf_init(wctx->body_ptr, strlen(wctx->body_ptr));
    nbufs = 2;
  }

  // 5. 启动异步写入，绑定回调
  int r = uv_write(&wctx->req, (uv_stream_t *)&client->handle, wctx->bufs, nbufs, on_write_completed);

  if (r < 0)
  {
    fprintf(stderr, "uv_write failed immediately: %s\n", uv_strerror(r));
    // 如果启动失败，需要手动清理，因为回调不会被触发
    free(wctx->header_ptr);
    if (wctx->body_ptr)
      free(wctx->body_ptr);
    free(wctx);
  }
}

/**
 * 获取已注册服务列表
 */
void handle_get_services(client_ctx_t *client)
{
  cJSON *root = cJSON_CreateArray();

  pthread_mutex_lock(&g_registry.lock);
  for (int i = 0; i < g_registry.service_count; i++)
  {
    service_t *svc = &g_registry.services[i];
    cJSON *svc_obj = cJSON_CreateObject();

    // ✓ 服务基本信息（统一格式）
    cJSON_AddStringToObject(svc_obj, "name", svc->name);
    cJSON_AddStringToObject(svc_obj, "description", svc->description);
    cJSON_AddStringToObject(svc_obj, "path_prefix", svc->path_prefix);
    cJSON_AddStringToObject(svc_obj, "health_endpoint", svc->health_endpoint);

    // ✓ 实例信息
    cJSON *instances = cJSON_CreateArray();
    pthread_mutex_lock(&svc->lock);
    for (int j = 0; j < svc->instance_count; j++)
    {
      cJSON *inst = cJSON_CreateObject();
      
      // ✓ 实例详细信息（统一格式）
      cJSON_AddStringToObject(inst, "host", svc->instances[j].host);
      cJSON_AddNumberToObject(inst, "port", svc->instances[j].port);
      
      const char *proto_str = (svc->instances[j].protocol == PROTOCOL_HTTPS) ? "https" : "http";
      cJSON_AddStringToObject(inst, "protocol", proto_str);
      
      cJSON_AddBoolToObject(inst, "ipv6", svc->instances[j].ip_addr.is_ipv6);
      
      const char *health_str = "unknown";
      if (svc->instances[j].health == SERVICE_HEALTHY)
        health_str = "healthy";
      else if (svc->instances[j].health == SERVICE_UNHEALTHY)
        health_str = "unhealthy";
      cJSON_AddStringToObject(inst, "health", health_str);
      
      cJSON_AddNumberToObject(inst, "requests", svc->instances[j].request_count);
      cJSON_AddBoolToObject(inst, "verify_ssl", svc->instances[j].verify_ssl);
      
      cJSON_AddItemToArray(instances, inst);
    }
    pthread_mutex_unlock(&svc->lock);

    cJSON_AddItemToObject(svc_obj, "instances", instances);
    cJSON_AddItemToArray(root, svc_obj);
  }
  pthread_mutex_unlock(&g_registry.lock);

  char *json_out = cJSON_PrintUnformatted(root);
  send_response(client, 200, "application/json", json_out);
  cJSON_Delete(root);
}

/**
 * 处理服务注册请求
 * POST /api/services/register
 */
void handle_service_register(client_ctx_t *client)
{
  printf("[Router] 收到服务注册请求\n");

  if (!client->body_buffer)
  {
    send_response(client, 400, "application/json", strdup("{\"error\":\"Missing request body\"}"));
    return;
  }

  // 解析 JSON 请求体
  cJSON *root = cJSON_Parse(client->body_buffer);
  if (!root)
  {
    send_response(client, 400, "application/json", strdup("{\"error\":\"Invalid JSON\"}"));
    return;
  }

  cJSON *name_obj = cJSON_GetObjectItem(root, "name");
  cJSON *desc_obj = cJSON_GetObjectItem(root, "description");
  cJSON *path_obj = cJSON_GetObjectItem(root, "path_prefix");
  cJSON *host_obj = cJSON_GetObjectItem(root, "host");
  cJSON *port_obj = cJSON_GetObjectItem(root, "port");
  cJSON *protocol_obj = cJSON_GetObjectItem(root, "protocol");
  cJSON *health_obj = cJSON_GetObjectItem(root, "health_endpoint");
  cJSON *verify_ssl_obj = cJSON_GetObjectItem(root, "verify_ssl");
  cJSON *ipv6_obj = cJSON_GetObjectItem(root, "ipv6");

  if (!name_obj || !path_obj || !host_obj || !port_obj)
  {
    cJSON_Delete(root);
    send_response(client, 400, "application/json", strdup("{\"error\":\"Missing required fields\"}"));
    return;
  }

  // 协议类型（默认 HTTP）
  protocol_t protocol = PROTOCOL_HTTP;
  if (protocol_obj && strcmp(protocol_obj->valuestring, "https") == 0)
  {
    protocol = PROTOCOL_HTTPS;
  }

  // SSL 验证（默认不验证）
  int verify_ssl = verify_ssl_obj ? verify_ssl_obj->valueint : 0;

  // IPv6 支持
  int is_ipv6 = ipv6_obj ? ipv6_obj->valueint : 0;

  // 注册服务
  int result = service_register_with_ipv6(
      name_obj->valuestring,
      desc_obj ? desc_obj->valuestring : NULL,
      path_obj->valuestring,
      host_obj->valuestring,
      port_obj->valueint,
      protocol,
      health_obj ? health_obj->valuestring : NULL,
      verify_ssl,
      is_ipv6);

  cJSON_Delete(root);

  if (result == 0)
  {
    send_response(client, 200, "application/json", strdup("{\"status\":\"registered\",\"message\":\"Service registered successfully\"}"));
  }
  else
  {
    send_response(client, 500, "application/json", strdup("{\"error\":\"Failed to register service\"}"));
  }
}

/**
 * 处理服务注销请求
 * DELETE /api/services/unregister
 */
void handle_service_unregister(client_ctx_t *client)
{
  printf("[Router] 收到服务注销请求\n");

  if (!client->body_buffer)
  {
    send_response(client, 400, "application/json", strdup("{\"error\":\"Missing request body\"}"));
    return;
  }

  // 解析 JSON 请求体
  cJSON *root = cJSON_Parse(client->body_buffer);
  if (!root)
  {
    send_response(client, 400, "application/json", strdup("{\"error\":\"Invalid JSON\"}"));
    return;
  }

  cJSON *name_obj = cJSON_GetObjectItem(root, "name");
  cJSON *host_obj = cJSON_GetObjectItem(root, "host");
  cJSON *port_obj = cJSON_GetObjectItem(root, "port");

  if (!name_obj || !host_obj || !port_obj)
  {
    cJSON_Delete(root);
    send_response(client, 400, "application/json", strdup("{\"error\":\"Missing required fields\"}"));
    return;
  }

  // 注销服务
  int result = service_deregister(
      name_obj->valuestring,
      host_obj->valuestring,
      port_obj->valueint);

  cJSON_Delete(root);

  if (result == 0)
  {
    send_response(client, 200, "application/json", strdup("{\"status\":\"unregistered\",\"message\":\"Service unregistered successfully\"}"));
  }
  else
  {
    send_response(client, 404, "application/json", strdup("{\"error\":\"Service instance not found\"}"));
  }
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
