/**
 * @file config.c
 * @brief 从 JSON 配置文件加载服务参数（可选）
 */
#include "sdk_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *get_str(cJSON *root, const char *k)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(root, k);
    return (j && cJSON_IsString(j)) ? j->valuestring : NULL;
}

static int get_int(cJSON *root, const char *k)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(root, k);
    return (j && cJSON_IsNumber(j)) ? j->valueint : -1;
}

int sdk_config_load(cservice_t *svc, const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f) { sdk_log("WARN", "配置文件 %s 无法打开", file); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { sdk_log("WARN", "配置文件 JSON 解析失败"); return -1; }

    const char *v;
    if ((v = get_str(root, "service_name"))) snprintf(svc->name, sizeof(svc->name), "%s", v);
    if ((v = get_str(root, "host")))         snprintf(svc->host, sizeof(svc->host), "%s", v);
    int p = get_int(root, "port");           if (p > 0) svc->port = p;
    if ((v = get_str(root, "gateway_host"))) snprintf(svc->gw_host, sizeof(svc->gw_host), "%s", v);
    p = get_int(root, "gateway_port");       if (p > 0) svc->gw_port = p;
    p = get_int(root, "threads");            if (p > 0) svc->thread_count = p;
    if ((v = get_str(root, "health_path")))  snprintf(svc->health_path, sizeof(svc->health_path), "%s", v);

    cJSON_Delete(root);
    sdk_log("INFO", "已加载配置 %s", file);
    return 0;
}
