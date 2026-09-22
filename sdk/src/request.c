/**
 * @file request.c
 * @brief 请求对象访问器
 */
#include "sdk_internal.h"
#include <string.h>
#include <strings.h>

const char *cservice_req_path(cservice_req_t *req)
{
    return req ? req->path : NULL;
}

const char *cservice_req_method(cservice_req_t *req)
{
    return req ? req->method_str : NULL;
}

const char *cservice_req_header(cservice_req_t *req, const char *name)
{
    if (!req || !name) return NULL;
    for (int i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

const char *cservice_req_query(cservice_req_t *req, const char *key)
{
    if (!req || !key || !req->conn) return NULL;
    const char *q = strchr(req->conn->url, '?');
    if (!q) return NULL;
    q++;
    size_t klen = strlen(key);
    while (*q)
    {
        const char *p = strchr(q, '=');
        size_t cur = p ? (size_t)(p - q) : strlen(q);
        if (cur == klen && strncmp(q, key, klen) == 0)
            return p ? p + 1 : "";
        /* 跳到下一个 & */
        const char *amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return NULL;
}

const char *cservice_req_body(cservice_req_t *req, size_t *len)
{
    if (!req) { if (len) *len = 0; return NULL; }
    if (len) *len = req->body_len;
    return req->body;
}
