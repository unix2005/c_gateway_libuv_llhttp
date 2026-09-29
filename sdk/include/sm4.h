#ifndef Q_SM4_H
#define Q_SM4_H

#include <stddef.h>
#include <stdint.h>

#include "q_log.h"   /* Q_OK / Q_ERR_* 作为本模块返回码约定 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SM4-CBC 对称加解密（基于 OpenSSL EVP）。
 *
 * 密钥与 IV 取自环境变量（部署期注入，不落盘）：
 *   SM4_KEY  16 字节密钥，十六进制字符串（32 个十六进制字符）
 *   SM4_IV   16 字节初始化向量，十六进制字符串（32 个十六进制字符）
 *
 * 密文统一以十六进制文本表示，便于写进配置文件 / 数据库字段。
 * 填充采用 PKCS#7（OpenSSL EVP 默认）。
 */

#define Q_SM4_KEY_LEN  16
#define Q_SM4_IV_LEN   16
#define Q_SM4_BLOCK    16

/* 一次性从环境变量读取并校验密钥/IV，之后 encrypt/decrypt 不再读环境。
 * 重复调用会刷新。返回 Q_OK / Q_ERR_*。 */
int q_sm4_init(void);

/* 密钥/IV 是否已就绪（q_sm4_init 成功过） */
int q_sm4_ready(void);

/*
 * 加密：plain[0..plen) -> 输出十六进制密文到 out（含 '\0'）。
 * out_cap 需 >= plen*2 + Q_SM4_BLOCK*2 + 1（含 PKCS7 填充 + 终止符余量）。
 * 返回 Q_OK / Q_ERR_*。
 */
int q_sm4_encrypt(const char *plain, size_t plen,
                 char *out, size_t out_cap, char *err, size_t errlen);

/* 解密：hex 密文 -> 明文到 out（含 '\0'）。out_cap 需 >= 密文字节数 + 1。 */
int q_sm4_decrypt(const char *hex, char *out, size_t out_cap,
                 char *err, size_t errlen);

/* 便捷：以 NUL 结尾字符串形态（自动按 strlen 取长度） */
int q_sm4_encrypt_str(const char *plain, char *out, size_t out_cap,
                      char *err, size_t errlen);
int q_sm4_decrypt_str(const char *hex,   char *out, size_t out_cap,
                      char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* Q_SM4_H */
