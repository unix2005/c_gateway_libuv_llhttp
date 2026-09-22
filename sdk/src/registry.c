/**
 * @file registry.c
 * @brief 向网关注册 / 注销 / 周期心跳（使用 libcurl）
 */
#include "sdk_internal.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 注册时若绑定地址为 0.0.0.0，网关无法回连，改用 127.0.0.1 */
static void reg_host_of(cservice_t *svc, char *out, size_t n)
{
    if (strcmp(svc->host, "0.0.0.0") == 0)
        snprintf(out, n, "127.0.0.1");
    else
        snprintf(out, n, "%s", svc->host);
}

static char *build_payload(cservice_t *svc)
{
    char h[64];
    reg_host_of(svc, h, sizeof(h));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", svc->name);
    cJSON_AddStringToObject(root, "host", h);
    cJSON_AddNumberToObject(root, "port", svc->port);
    cJSON_AddStringToObject(root, "protocol", "http");
    cJSON_AddStringToObject(root, "path_prefix", svc->path_prefix);
    cJSON_AddStringToObject(root, "health_check_url", svc->health_path);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static int post_json(const char *url, const char *payload)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return (rc == CURLE_OK && (code == 200 || code == 201)) ? 0 : -1;
}

int sdk_registry_register(cservice_t *svc)
{
    char *payload = build_payload(svc);
    int r = post_json(svc->reg_url, payload);
    free(payload);
    return r;
}

int sdk_registry_unregister(cservice_t *svc)
{
    char *payload = build_payload(svc);
    /* 网关取消注册接口为 POST /api/services/unregister */
    int r = post_json(svc->unreg_url, payload);
    free(payload);
    return r;
}

void *sdk_registry_heartbeat(void *arg)
{
    cservice_t *svc = (cservice_t *)arg;
    while (svc->hb_running)
    {
        sleep(5);
        if (!svc->hb_running) break;
        if (svc->register_enabled)
            sdk_registry_register(svc); /* 幂等刷新，网关侧按 name+host+port 去重 */
    }
    return NULL;
}
