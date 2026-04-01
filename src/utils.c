#include "gateway.h"

// --- 内存池分配器 ---
// 在 client_ctx_t 预分配的 8KB 空间内移动指针，实现零系统调用分配
void* pool_alloc(client_ctx_t* ctx, size_t size) 
{
  // 8字节对齐
  size = (size + 7) & ~7; 

  if (ctx->pool.used + size > POOL_SIZE) 
  {
    // 如果 8KB 不够用，降级为普通 malloc
    // 生产环境通常会记录一个 log，提醒调大 POOL_SIZE
    return malloc(size); 
  }

  void* ptr = ctx->pool.data + ctx->pool.used;
  ctx->pool.used += size;
  return ptr;
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
