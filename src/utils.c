#include "gateway.h"

// --- 内存池分配器 ---
// 在 client_ctx_t 预分配的 8KB 空间内移动指针，实现零系统调用分配
void* pool_alloc(client_ctx_t* ctx, size_t size) 
{
  // 8字节对齐
  size = (size + 7) & ~7; 

  if (ctx->pool.used + size > POOL_SIZE) 
  {
    /* 如果 8KB 不够用，降级为普通 malloc。
       修复：原实现直接返回 malloc 结果且从不释放，造成内存泄漏。
       这里登记溢出块，由 pool_overflow_free() 在连接关闭时统一释放。 */
    void *p = malloc(size);
    if (!p) return NULL;

    if (ctx->pool_overflow_n == ctx->pool_overflow_cap)
    {
      size_t cap = ctx->pool_overflow_cap ? ctx->pool_overflow_cap * 2 : 8;
      void **np = realloc(ctx->pool_overflow, cap * sizeof(void *));
      if (!np)
      {
        free(p);
        return NULL;
      }
      ctx->pool_overflow = np;
      ctx->pool_overflow_cap = cap;
    }
    ctx->pool_overflow[ctx->pool_overflow_n++] = p;
    return p;
  }

  void* ptr = ctx->pool.data + ctx->pool.used;
  ctx->pool.used += size;
  return ptr;
}

// 释放内存池降级 malloc 登记的所有块（连接关闭时调用）
void pool_overflow_free(client_ctx_t* ctx)
{
  if (!ctx) return;
  for (size_t i = 0; i < ctx->pool_overflow_n; i++)
    free(ctx->pool_overflow[i]);
  free(ctx->pool_overflow);
  ctx->pool_overflow = NULL;
  ctx->pool_overflow_n = 0;
  ctx->pool_overflow_cap = 0;
}

// --- URL 参数解析 ---
// 使用 pool_alloc 代替 malloc，这样参数内存会随连接自动销毁
char* get_query_param(client_ctx_t* ctx, const char* key) 
{
  char *q = strchr(ctx->url, '?');
  if (!q) return NULL;
  q++;

  char *p = strstr(q, key);
  while (p) 
  {
    if ((p == q || *(p - 1) == '&') && *(p + strlen(key)) == '=') 
    {
      char *start = p + strlen(key) + 1;
      char *end = strchr(start, '&');
      size_t len = end ? (size_t)(end - start) : strlen(start);

      // 使用内存池分配空间，不需要手动 free
      char *val = pool_alloc(ctx, len + 1);
      strncpy(val, start, len);
      val[len] = '\0';
      return val;
    }
    p = strstr(p + 1, key);
  }
  return NULL;
}
