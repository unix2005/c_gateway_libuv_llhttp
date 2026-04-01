#!/bin/bash
# 多线程工作服务 v2 - Linux/macOS编译脚本

echo "=== 编译多线程工作服务 v2 (支持 IPv6/HTTPS) ==="
echo

CC=gcc
CFLAGS="-DHAVE_OPENSSL"
LIBS="-lcurl -lcjson -lpthread -lssl -lcrypto"

mkdir -p bin

echo "[编译] worker_service_v2.c ..."

$CC $CFLAGS -o bin/worker_service worker_service_v2.c $LIBS

if [ $? -eq 0 ]; then
    echo
    echo "[成功] 编译完成！"
    echo "可执行文件：bin/worker_service"
    echo
    echo "使用方法:"
    echo "  ./bin/worker_service worker_config.json"
    echo
    echo "配置文件示例:"
    echo "  worker_config.json - 基本 HTTP 配置"
    echo "  worker_config_https.json - HTTPS 配置"
    echo "  worker_config_ipv6.json - IPv6 配置"
    echo
else
    echo
    echo "[错误] 编译失败！"
    echo "请确保已安装以下库:"
    echo
    echo "Ubuntu/Debian:"
    echo "  sudo apt-get install libcurl4-openssl-dev libcjson-dev libssl-dev"
    echo
    echo "CentOS/RHEL:"
    echo "  sudo yum install libcurl-devel cjson-devel openssl-devel"
    echo
    echo "macOS:"
    echo "  brew install curl cjson openssl"
    echo
fi
