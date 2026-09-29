/**
 * @file sm4_tool.c
 * @brief SDK SM4-CBC 加解密小工具（用于生成配置文件里加密后的数据库密码等）
 *
 * 密钥/IV 来自环境变量（与运行时一致）：
 *   export SM4_KEY=00112233445566778899aabbccddeeff
 *   export SM4_IV =102030405060708090a0b0c0d0e0f00
 *
 * 用法：
 *   ./sm4_tool enc <明文>       -> 打印十六进制密文（写进配置 password 字段）
 *   ./sm4_tool dec <密文hex>    -> 打印明文
 *
 * 编译：make -C sdk examples
 */
#include <sm4.h>
#include <q_log.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    q_log_init("logs", "sm4_tool", Q_LOG_INFO);

    if (argc < 3) {
        fprintf(stderr,
            "用法:\n"
            "  %s enc <明文>      用 SM4_KEY/SM4_IV(SM4-CBC) 加密，输出十六进制密文\n"
            "  %s dec <密文hex>   解密\n",
            argv[0], argv[0]);
        return 2;
    }

    int rc = q_sm4_init();
    if (rc == Q_ERR) {
        fprintf(stderr,
            "SM4 初始化失败：未设置环境变量 SM4_KEY / SM4_IV\n");
        return 1;
    } else if (rc != Q_OK) {
        fprintf(stderr,
            "SM4 初始化失败：SM4_KEY / SM4_IV 需为 32 位十六进制（16 字节）\n");
        return 1;
    }

    char buf[8192];
    char err[256];

    if (strcmp(argv[1], "enc") == 0) {
        if (q_sm4_encrypt_str(argv[2], buf, sizeof(buf), err, sizeof(err)) != Q_OK) {
            fprintf(stderr, "加密失败: %s\n", err);
            return 1;
        }
        printf("%s\n", buf);
    } else if (strcmp(argv[1], "dec") == 0) {
        if (q_sm4_decrypt_str(argv[2], buf, sizeof(buf), err, sizeof(err)) != Q_OK) {
            fprintf(stderr, "解密失败: %s\n", err);
            return 1;
        }
        printf("%s\n", buf);
    } else {
        fprintf(stderr, "未知子命令: %s（请用 enc / dec）\n", argv[1]);
        return 2;
    }
    return 0;
}
