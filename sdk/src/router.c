/**
 * @file router.c
 * @brief 路由匹配与方法映射
 */
#include "sdk_internal.h"
#include <string.h>

cservice_method_t sdk_method_from_str(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "GET") == 0)    return CSERVICE_GET;
    if (strcmp(s, "POST") == 0)   return CSERVICE_POST;
    if (strcmp(s, "PUT") == 0)    return CSERVICE_PUT;
    if (strcmp(s, "DELETE") == 0) return CSERVICE_DELETE;
    if (strcmp(s, "HEAD") == 0)   return CSERVICE_HEAD;
    if (strcmp(s, "PATCH") == 0)  return CSERVICE_PATCH;
    return 0;
}

cservice_method_t sdk_method_from_llhttp(int lm)
{
    /* 直接使用 llhttp 真实枚举值，避免版本差异导致方法映射错位 */
    switch (lm)
    {
        case HTTP_DELETE: return CSERVICE_DELETE;
        case HTTP_GET:    return CSERVICE_GET;
        case HTTP_HEAD:   return CSERVICE_HEAD;
        case HTTP_POST:   return CSERVICE_POST;
        case HTTP_PUT:    return CSERVICE_PUT;
        case HTTP_PATCH:  return CSERVICE_PATCH;
        default:          return 0;
    }
}

sdk_route_t *sdk_route_match(cservice_t *svc, cservice_method_t m, const char *path)
{
    if (!svc || !path) return NULL;

    /* 第一遍：精确匹配优先 */
    for (int i = 0; i < svc->route_count; i++)
    {
        sdk_route_t *r = &svc->routes[i];
        if ((r->method & m) && !r->is_prefix &&
            strcmp(path, r->path) == 0)
            return r;
    }

    /* 第二遍：前缀匹配 */
    for (int i = 0; i < svc->route_count; i++)
    {
        sdk_route_t *r = &svc->routes[i];
        size_t len = strlen(r->path);
        if ((r->method & m) && r->is_prefix &&
            strncmp(path, r->path, len) == 0)
            return r;
    }
    return NULL;
}
