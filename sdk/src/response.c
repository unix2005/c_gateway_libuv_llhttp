/**
 * @file response.c
 * @brief 响应对象构造
 */
#include "sdk_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void cservice_res_status(cservice_res_t *res, int code)
{
    if (res) res->status = code;
}

void cservice_res_header(cservice_res_t *res, const char *name, const char *value)
{
    if (!res || !name || !value) return;
    if (res->header_count >= SDK_MAX_HEADERS) return;
    sdk_kv_t *h = &res->headers[res->header_count++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->value, sizeof(h->value), "%s", value);
}

static void res_set_body(cservice_res_t *res, const char *body, size_t len)
{
    free(res->body);
    res->body = NULL;
    res->body_len = 0;
    if (body && len > 0)
    {
        res->body = malloc(len);
        if (res->body) { memcpy(res->body, body, len); res->body_len = len; }
    }
}

void cservice_res_json(cservice_res_t *res, const char *json)
{
    if (!res) return;
    if (res->status == 0) res->status = 200;
    snprintf(res->content_type, sizeof(res->content_type), "application/json");
    res_set_body(res, json, json ? strlen(json) : 0);
}

void cservice_res_text(cservice_res_t *res, const char *text, const char *content_type)
{
    if (!res) return;
    if (res->status == 0) res->status = 200;
    snprintf(res->content_type, sizeof(res->content_type), "%s",
             content_type ? content_type : "text/plain; charset=utf-8");
    res_set_body(res, text, text ? strlen(text) : 0);
}

void cservice_res_send(cservice_res_t *res, int code,
                       const char *content_type, const char *body, size_t len)
{
    if (!res) return;
    res->status = code;
    snprintf(res->content_type, sizeof(res->content_type), "%s",
             content_type ? content_type : "application/octet-stream");
    res_set_body(res, body, len);
}

void cservice_res_printf(cservice_res_t *res, int code,
                         const char *content_type, const char *fmt, ...)
{
    if (!res) return;
    res->status = code;
    snprintf(res->content_type, sizeof(res->content_type), "%s",
             content_type ? content_type : "text/plain; charset=utf-8");

    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { res_set_body(res, NULL, 0); return; }
    if (n < (int)sizeof(buf))
        res_set_body(res, buf, (size_t)n);
    else
    {
        char *big = malloc((size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        res_set_body(res, big, (size_t)n);
        free(big);
    }
}
