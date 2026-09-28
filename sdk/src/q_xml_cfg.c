/**
 * @file q_xml_cfg.c
 * @brief 基于 libxml2 的 XML 配置读取实现
 * @author qiao
 * @date 2026-09-28
 * @ingroup q_xml_cfg
 *
 * @details
 * 实现 q_xml_cfg.h 声明的配置读取接口。内部借助 libxml2 的 XPath 引擎，
 * 将"点分键"（如 gateway.port）转换为 XPath 表达式（/gateway/port/text()）
 * 后求值。所有公开函数对 NULL 上下文/键均做防御性处理并返回默认值或 NULL。
 */
#include "sdk_internal.h"
#include "q_xml_cfg.h"

// 配置上下文结构体（隐藏 XML 细节）
struct config_ctx 
{
  xmlDocPtr doc;           // XML 文档指针
  xmlXPathContextPtr xpath; // XPath 上下文
  char *error_msg;         // 错误信息
};

// ==================== 内部辅助函数 ====================

/**
 * @brief 将点分路径转换为 XPath 文本节点表达式
 * @param key 点分键，如 "gateway.tls.cert_file"
 * @return 转换后的 XPath 表达式（如 "/gateway/tls/cert_file/text()"），
 *         由调用者 free()；key 为 NULL 或内存不足时返回 NULL
 * @note 内部假定路径长度不超过 511 字符（含终止符），超出部分会被截断。
 */
static char* key_to_xpath(const char *key) 
{
  if (!key) return NULL;

  // 分配足够空间（假设路径不超过 512 字符）
  char *xpath = malloc(512);
  if (!xpath) return NULL;

  // 复制原始路径
  char *path = strdup(key);
  if (!path) 
  {
    free(xpath);
    return NULL;
  }

  // 将点号替换为斜杠
  char *p = path;
  while ((p = strchr(p, '.')) != NULL) 
  {
    *p = '/';
    p++;
  }

  // 构建完整 XPath
  snprintf(xpath, 512, "/%s/text()", path);
  free(path);

  return xpath;
}

/**
 * @brief 按点分键查找对应的 XML 文本节点
 * @param ctx 配置上下文
 * @param key 点分键
 * @return 命中的 XML 节点指针；未命中、ctx/key 为空或 XPath 求值失败时返回 NULL
 */
static xmlNodePtr find_node(config_ctx_t *ctx, const char *key) 
{
  if (!ctx || !key) return NULL;

  char *xpath_expr = key_to_xpath(key);
  if (!xpath_expr) return NULL;

  xmlXPathObjectPtr result = xmlXPathEvalExpression(
      BAD_CAST xpath_expr, ctx->xpath);

  free(xpath_expr);

  if (!result) return NULL;

  xmlNodePtr node = NULL;
  if (result->nodesetval && result->nodesetval->nodeNr > 0) 
  {
    node = result->nodesetval->nodeTab[0];
  }

  xmlXPathFreeObject(result);
  return node;
}

/**
 * @brief 初始化配置管理器并解析 XML 文件
 * @param config_file XML 配置文件路径
 * @return 配置上下文句柄；解析失败或参数为 NULL 时返回 NULL
 * @note 返回的句柄需调用 config_destroy() 释放。内部会初始化 libxml2
 *       (LIBXML_TEST_VERSION)，多次调用安全。
 */
config_ctx_t* config_init(const char *config_file) 
{
  if (!config_file) return NULL;

  // 初始化 libxml2
  LIBXML_TEST_VERSION

  // 解析 XML 文件
  xmlDocPtr doc = xmlReadFile(config_file, NULL, 0);
  if (!doc) 
  {
    fprintf(stderr, "Failed to parse config file: %s\n", config_file);
    return NULL;
  }

  // 创建 XPath 上下文
  xmlXPathContextPtr xpath = xmlXPathNewContext(doc);
  if (!xpath) 
  {
    xmlFreeDoc(doc);
    return NULL;
  }

  // 分配配置上下文
  config_ctx_t *ctx = calloc(1, sizeof(config_ctx_t));
  if (!ctx) 
  {
    xmlXPathFreeContext(xpath);
    xmlFreeDoc(doc);
    return NULL;
  }

  ctx->doc = doc;
  ctx->xpath = xpath;

  return ctx;
}

/**
 * @brief 释放配置管理器及其内部资源
 * @param ctx 配置上下文（可为 NULL，此时直接返回）
 * @note 内部会调用 xmlCleanupParser() 清理 libxml2 全局状态。
 */
void config_destroy(config_ctx_t *ctx) 
{
  if (!ctx) return;

  if (ctx->xpath) xmlXPathFreeContext(ctx->xpath);
  if (ctx->doc) xmlFreeDoc(ctx->doc);
  if (ctx->error_msg) free(ctx->error_msg);
  free(ctx);

  // 清理 libxml2
  xmlCleanupParser();
}

/**
 * @brief 读取整数配置
 * @param ctx 配置上下文
 * @param key 点分键（如 "gateway.port"）
 * @param default_value 键缺失 / 节点为空 / ctx 或 key 为 NULL 时返回的默认值
 * @return 解析得到的整数；失败返回 default_value
 */
int config_get_int(config_ctx_t *ctx, const char *key, int default_value) 
{
  if (!ctx || !key) return default_value;

  xmlNodePtr node = find_node(ctx, key);
  if (!node || !node->content) 
  {
    return default_value;
  }

  return atoi((const char*)node->content);
}

/**
 * @brief 读取字符串配置（返回内部 strdup 的副本）
 * @param ctx 配置上下文
 * @param key 点分键
 * @param default_value 默认值（可为 NULL）
 * @return 复制字符串指针，调用者须 free()；键缺失或节点为空时返回
 *         default_value 的副本（default_value 为 NULL 则返回 NULL）
 */
char* config_get_string(config_ctx_t *ctx, const char *key,
                        const char *default_value) 
{
  if (!ctx || !key) 
  {
    return default_value ? strdup(default_value) : NULL;
  }

  xmlNodePtr node = find_node(ctx, key);
  if (!node || !node->content) 
  {
    return default_value ? strdup(default_value) : NULL;
  }

  return strdup((const char*)node->content);
}

/**
 * @brief 读取布尔配置
 * @param ctx 配置上下文
 * @param key 点分键
 * @param default_value 默认值
 * @return true 表示真；仅当节点文本为 "true"/"yes"/"1"（大小写不敏感）时为真，
 *         其余情况（含键缺失、ctx/key 为 NULL）均返回 default_value
 */
bool config_get_bool(config_ctx_t *ctx, const char *key, bool default_value) 
{
  if (!ctx || !key) return default_value;

  xmlNodePtr node = find_node(ctx, key);
  if (!node || !node->content) 
  {
    return default_value;
  }

  const char *str = (const char*)node->content;

  // 支持多种布尔表示
  if (strcasecmp(str, "true") == 0 ||
      strcasecmp(str, "yes") == 0 ||
      strcmp(str, "1") == 0) 
  {
    return true;
  }

  return false;
}
/**
 * @brief 读取浮点数配置
 * @param ctx 配置上下文
 * @param key 点分键
 * @param default_value 默认值
 * @return 解析得到的双精度浮点；键缺失、节点为空或解析失败时返回 default_value
 */
double config_get_float(config_ctx_t *ctx, const char *key,
                        double default_value) 
{
  if (!ctx || !key) return default_value;

  xmlNodePtr node = find_node(ctx, key);
  if (!node || !node->content) 
  {
    return default_value;
  }

  return atof((const char*)node->content);
}

/**
 * @brief 检查配置项是否存在
 * @param ctx 配置上下文
 * @param key 点分键
 * @return true=存在，false=不存在（ctx 或 key 为 NULL 也返回 false）
 */
bool config_has_key(config_ctx_t *ctx, const char *key) 
{
  if (!ctx || !key) return false;

  xmlNodePtr node = find_node(ctx, key);
  return (node != NULL);
}

/**
 * @brief 打印所有叶子配置项（调试用）
 * @param ctx 配置上下文（可为 NULL，此时直接返回）
 * @note 输出格式为 "节点名 = 值"，逐行打印到 stdout；仅用于调试，不属于稳定接口。
 */
void config_dump(config_ctx_t *ctx) 
{
  if (!ctx) return;

  // 获取根节点
  xmlNodePtr root = xmlDocGetRootElement(ctx->doc);
  if (!root) return;

  // 简单遍历所有叶子节点
  xmlXPathObjectPtr result = xmlXPathEvalExpression(
      BAD_CAST "//*/*/text()", ctx->xpath);

  if (result && result->nodesetval) 
  {
    for (int i = 0; i < result->nodesetval->nodeNr; i++) 
    {
      xmlNodePtr node = result->nodesetval->nodeTab[i];
      printf("%s = %s\n", node->parent->name, node->content);
    }
  }

  xmlXPathFreeObject(result);
}
