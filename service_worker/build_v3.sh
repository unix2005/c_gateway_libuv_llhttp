#!/bin/bash
# 多线程工作服务 v3 - Linux/macOS编译脚本 (libuv + llhttp)

echo "=== 编译多线程工作服务 v3 (libuv + llhttp) ==="
echo

CC=gcc
CFLAGS="-DHAVE_OPENSSL -O2"
LIBS="-luv -lllhttp -lcurl -lcjson -lpthread -lssl -lcrypto"

mkdir -p bin

echo "[编译] worker_service_v3_1.c ..."

$CC $CFLAGS -o bin/worker_service_v3_1 worker_service_v3_1.c $LIBS

if [ $? -eq 0 ]; then
    echo
    echo "[成功] 编译完成！"
    echo "可执行文件：bin/worker_service_v3"
    echo
    echo "使用方法:"
    echo "  ./bin/worker_service_v3 worker_config.json"
    echo
    echo "技术栈:"
    echo "  - 网络层：libuv (异步事件驱动)"
    echo "  - HTTP 解析：llhttp (高性能)"
    echo "  - 配置加载：cJSON"
    echo "  - HTTP 客户端：libcurl"
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
    echo "  sudo apt-get install libuv1-dev libcurl4-openssl-dev libcjson-dev libssl-dev"
    echo
    echo "  # llhttp 需要手动编译安装:"
    echo "  git clone https://github.com/nodejs/llhttp.git"
    echo "  cd llhttp"
    echo "  npm install"
    echo "  npm run build"
    echo "  sudo make install"
    echo
    echo "CentOS/RHEL:"
    echo "  sudo yum install libuv-devel libcurl-devel cjson-devel openssl-devel"
    echo
    echo "macOS:"
    echo "  brew install libuv curl cjson openssl"
    echo "  # llhttp:"
    echo "  git clone https://github.com/nodejs/llhttp.git"
    echo "  cd llhttp && npm install && npm run build && sudo make install"
    echo
fi
