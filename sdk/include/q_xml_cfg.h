
/**
 * @file q_xml_cfg.h
 * @brief 基于 libxml2 的 XML 配置文件读取接口（键值对访问，隐藏 XML 细节）
 * @author qiao
 * @date 2026-09-28
 *
 * @details
 * 该模块提供一个轻量级配置读取器：以"点分路径"作为键（如 "gateway.port"），
 * 通过 XPath 在 XML 文档中查找对应文本节点，支持 int / string / bool / float
 * 四种类型，键缺失或解析失败时返回调用者提供的默认值。
 *
 * 配置上下文内部结构（libxml2 文档、XPath 上下文、错误信息）对外部隐藏，
 * 外部仅持有不透明句柄 config_ctx_t*，避免直接依赖 libxml2 类型。
 *
 * @par 典型用法
 * @code
 *   config_ctx_t *cfg = config_init("gateway.xml");
 *   int port = config_get_int(cfg, "gateway.port", 8080);
 *   char *host = config_get_string(cfg, "gateway.host", "127.0.0.1");
 *   config_destroy(cfg);
 *   free(host);
 * @endcode
 *
 * @defgroup q_xml_cfg XML 配置读取模块
 * @{
 */

#ifndef __Q_XML_CFG_H__
#define __Q_XML_CFG_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <libxml2/libxml/parser.h>
#include <libxml2/libxml/tree.h>
#include <libxml2/libxml/xpath.h>
#include <libxml2/libxml/xmlstring.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 不透明配置上下文句柄（内部结构隐藏，外部仅持有指针交互） */
typedef struct config_ctx config_ctx_t;

// ==================== 生命周期管理 ====================

/**
 * 初始化配置管理器
 * @param config_file XML 配置文件路径
 * @return 配置上下文，失败返回 NULL
 */
config_ctx_t* config_init(const char *config_file);

/**
 * 释放配置管理器
 * @param ctx 配置上下文
 */
void config_destroy(config_ctx_t *ctx);

// ==================== Key-Value 读取接口 ====================

/**
 * 读取整数配置
 * @param ctx 配置上下文
 * @param key 配置键（支持路径：如 "gateway.port"）
 * @param default_value 默认值
 * @return 配置值
 */
int config_get_int(config_ctx_t *ctx, const char *key, int default_value);

/**
 * 读取字符串配置（返回复制的字符串）
 * @param ctx 配置上下文
 * @param key 配置键
 * @param default_value 默认值（可为 NULL）
 * @return 指向复制字符串的指针，调用者必须 free()；键缺失或为空时返回
 *         default_value 的副本（若 default_value 为 NULL 则返回 NULL）
 * @note 返回的字符串由本函数内部 strdup 分配，调用者负责释放，否则内存泄漏。
 */
char* config_get_string(config_ctx_t *ctx, const char *key,
                        const char *default_value);

/**
 * 读取布尔配置
 * @param ctx 配置上下文
 * @param key 配置键
 * @param default_value 默认值
 * @return true/false
 */
bool config_get_bool(config_ctx_t *ctx, const char *key, bool default_value);

/**
 * 读取浮点数配置
 * @param ctx 配置上下文
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
double config_get_float(config_ctx_t *ctx, const char *key, double default_value);

// ==================== 配置验证 ====================

/**
 * 检查配置项是否存在
 * @param ctx 配置上下文
 * @param key 配置键
 * @return true=存在，false=不存在
 */
bool config_has_key(config_ctx_t *ctx, const char *key);

// ==================== 调试接口 ====================

/**
 * 打印所有配置（用于调试）
 * @param ctx 配置上下文
 */
void config_dump(config_ctx_t *ctx);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif
