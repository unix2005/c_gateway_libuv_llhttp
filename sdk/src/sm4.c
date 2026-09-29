/*
 * SM4-CBC 对称加解密（OpenSSL EVP 实现）。
 * 密钥/IV 来自环境变量 SM4_KEY / SM4_IV（十六进制），用于运行时解密
 * 配置文件中以十六进制存放的敏感字段（如数据库密码）。
 */
#include "sm4.h"

#include <openssl/evp.h>
#include <openssl/err.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_key[Q_SM4_KEY_LEN];
static unsigned char g_iv[Q_SM4_IV_LEN];
static int           g_ready = 0;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 十六进制 -> 二进制；长度须为偶数，且全为合法十六进制字符 */
static int hex2bin(const char *hex, unsigned char *out, size_t *outlen)
{
    size_t n = strlen(hex);
    if (n == 0 || (n % 2) != 0) return -1;
    size_t bytes = n / 2;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    *outlen = bytes;
    return 0;
}

static void bin2hex(const unsigned char *bin, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = d[bin[i] >> 4];
        out[2 * i + 1] = d[bin[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

int q_sm4_init(void)
{
    const char *ks = getenv("SM4_KEY");
    const char *ivs = getenv("SM4_IV");
    g_ready = 0;
    if (ks == NULL || ivs == NULL) {
        q_warn("sm4: 未设置环境变量 SM4_KEY / SM4_IV，解密不可用");
        return Q_ERR;
    }
    size_t kl = 0, vl = 0;
    if (hex2bin(ks, g_key, &kl) != 0 || kl != Q_SM4_KEY_LEN) {
        q_error("sm4: SM4_KEY 应为 %d 字节十六进制（%d 字符）",
                Q_SM4_KEY_LEN, Q_SM4_KEY_LEN * 2);
        return Q_ERR_INVAL;
    }
    if (hex2bin(ivs, g_iv, &vl) != 0 || vl != Q_SM4_IV_LEN) {
        q_error("sm4: SM4_IV 应为 %d 字节十六进制（%d 字符）",
                Q_SM4_IV_LEN, Q_SM4_IV_LEN * 2);
        return Q_ERR_INVAL;
    }
    g_ready = 1;
    return Q_OK;
}

int q_sm4_ready(void) { return g_ready; }

static void ossl_err(char *err, size_t errlen)
{
    unsigned long e = ERR_get_error();
    if (e != 0 && err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", ERR_error_string(e, NULL));
}

int q_sm4_encrypt(const char *plain, size_t plen,
                  char *out, size_t out_cap, char *err, size_t errlen)
{
    if (!g_ready) {
        if (err) snprintf(err, errlen, "sm4 未初始化（缺少 SM4_KEY / SM4_IV）");
        return Q_ERR;
    }
    if (plain == NULL || out == NULL) {
        if (err) snprintf(err, errlen, "参数空");
        return Q_ERR_INVAL;
    }
    /* 密文字节上限 = plen + 一个分组的 PKCS7 填充 */
    size_t ct_max = plen + Q_SM4_BLOCK;
    if (out_cap < ct_max * 2 + 1) {
        if (err) snprintf(err, errlen, "out 缓冲不足");
        return Q_ERR_INVAL;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        if (err) snprintf(err, errlen, "EVP_CTX 分配失败");
        return Q_ERR_NOMEM;
    }

    int rc = Q_OK;
    if (EVP_EncryptInit_ex(ctx, EVP_sm4_cbc(), NULL, g_key, g_iv) != 1) {
        ossl_err(err, errlen); rc = Q_ERR; goto cleanup;
    }
    unsigned char *buf = malloc(ct_max + Q_SM4_BLOCK);
    if (buf == NULL) {
        if (err) snprintf(err, errlen, "内存不足");
        rc = Q_ERR_NOMEM; goto cleanup;
    }
    int outl = 0, tmpl = 0;
    if (EVP_EncryptUpdate(ctx, buf, &outl, (const unsigned char *)plain, (int)plen) != 1) {
        ossl_err(err, errlen); rc = Q_ERR; free(buf); goto cleanup;
    }
    if (EVP_EncryptFinal_ex(ctx, buf + outl, &tmpl) != 1) {
        ossl_err(err, errlen); rc = Q_ERR; free(buf); goto cleanup;
    }
    bin2hex(buf, (size_t)(outl + tmpl), out);
    free(buf);

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int q_sm4_decrypt(const char *hex, char *out, size_t out_cap,
                  char *err, size_t errlen)
{
    if (!g_ready) {
        if (err) snprintf(err, errlen, "sm4 未初始化（缺少 SM4_KEY / SM4_IV）");
        return Q_ERR;
    }
    if (hex == NULL || out == NULL) {
        if (err) snprintf(err, errlen, "参数空");
        return Q_ERR_INVAL;
    }

    unsigned char *bin = malloc(strlen(hex) / 2 + 1);
    if (bin == NULL) {
        if (err) snprintf(err, errlen, "内存不足");
        return Q_ERR_NOMEM;
    }
    size_t blen = 0;
    if (hex2bin(hex, bin, &blen) != 0) {
        free(bin);
        if (err) snprintf(err, errlen, "非法十六进制密文");
        return Q_ERR_INVAL;
    }
    if (blen == 0 || (blen % Q_SM4_BLOCK) != 0) {
        free(bin);
        if (err) snprintf(err, errlen, "密文长度非法（必须为分组整数倍）");
        return Q_ERR_INVAL;
    }
    if (out_cap < blen + 1) {
        free(bin);
        if (err) snprintf(err, errlen, "out 缓冲不足");
        return Q_ERR_INVAL;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        free(bin);
        if (err) snprintf(err, errlen, "EVP_CTX 分配失败");
        return Q_ERR_NOMEM;
    }

    int rc = Q_OK;
    if (EVP_DecryptInit_ex(ctx, EVP_sm4_cbc(), NULL, g_key, g_iv) != 1) {
        ossl_err(err, errlen); rc = Q_ERR; goto cleanup;
    }
    int outl = 0, tmpl = 0;
    if (EVP_DecryptUpdate(ctx, (unsigned char *)out, &outl, bin, (int)blen) != 1) {
        ossl_err(err, errlen); rc = Q_ERR; goto cleanup;
    }
    if (EVP_DecryptFinal_ex(ctx, (unsigned char *)out + outl, &tmpl) != 1) {
        /* 密钥/IV 不符或密文被篡改时此处失败 */
        ossl_err(err, errlen); rc = Q_ERR; goto cleanup;
    }

    int total = outl + tmpl;
    if (total > 0) {                       /* 去除 PKCS#7 填充 */
        int pad = (unsigned char)out[total - 1];
        if (pad >= 1 && pad <= Q_SM4_BLOCK && pad <= total) total -= pad;
    }
    out[total] = '\0';

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    free(bin);
    return rc;
}

int q_sm4_encrypt_str(const char *plain, char *out, size_t out_cap,
                      char *err, size_t errlen)
{
    if (plain == NULL) return Q_ERR_INVAL;
    return q_sm4_encrypt(plain, strlen(plain), out, out_cap, err, errlen);
}

int q_sm4_decrypt_str(const char *hex, char *out, size_t out_cap,
                      char *err, size_t errlen)
{
    return q_sm4_decrypt(hex, out, out_cap, err, errlen);
}
