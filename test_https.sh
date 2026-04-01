#!/bin/bash
# HTTPS 功能测试脚本

echo "=== 网关 HTTPS 功能测试 ==="
echo ""

# 1. 检查 OpenSSL
echo "[1] 检查 OpenSSL 安装..."
if command -v openssl &> /dev/null; then
    echo "✓ OpenSSL 已安装: $(openssl version)"
else
    echo "✗ OpenSSL 未安装，请先安装 OpenSSL"
    exit 1
fi

# 2. 生成测试证书
echo ""
echo "[2] 生成自签名测试证书..."
if [ ! -f server.crt ] || [ ! -f server.key ]; then
    openssl req -x509 -newkey rsa:2048 \
        -keyout server.key \
        -out server.crt \
        -days 365 \
        -nodes \
        -subj "/C=CN/ST=Test/L=Test/O=Test/CN=localhost"
    
    if [ $? -eq 0 ]; then
        echo "✓ 证书生成成功"
    else
        echo "✗ 证书生成失败"
        exit 1
    fi
else
    echo "✓ 证书文件已存在，跳过生成"
fi

# 3. 创建测试配置文件
echo ""
echo "[3] 创建 HTTPS 配置文件..."
cat > gateway_config_https.json <<EOF
{
  "gateway": {
    "worker_threads": 2,
    "service_port": 8443,
    "enable_ipv6": 0,
    "enable_https": 1,
    "ssl_cert_path": "$(pwd)/server.crt",
    "ssl_key_path": "$(pwd)/server.key",
    "log_path": "logs/gateway.log",
    "health_check_interval": 5000,
    "observability": {
      "enable_logging": 1,
      "enable_metrics": 1,
      "enable_tracing": 0,
      "log_format": "text",
      "log_level": "info",
      "metrics_port": 9090,
      "tracing_sample_rate": 0.1
    }
  }
}
EOF

if [ $? -eq 0 ]; then
    echo "✓ 配置文件创建成功"
else
    echo "✗ 配置文件创建失败"
    exit 1
fi

# 4. 编译网关
echo ""
echo "[4] 编译网关..."
make clean > /dev/null 2>&1
make

if [ $? -eq 0 ]; then
    echo "✓ 编译成功"
else
    echo "✗ 编译失败，请检查依赖库是否安装"
    echo "提示：sudo apt-get install libuv1-dev libllhttp-dev libcurl4-openssl-dev libcjson-dev"
    exit 1
fi

# 5. 启动网关（后台运行）
echo ""
echo "[5] 启动 HTTPS 网关（后台）..."
./bin/c_gateway gateway_config_https.json &
GATEWAY_PID=$!
echo "✓ 网关已启动，PID: $GATEWAY_PID"

# 等待网关完全启动
sleep 2

# 6. 测试 HTTPS 连接
echo ""
echo "[6] 测试 HTTPS 连接..."

# 测试健康检查端点
echo "测试 /api/employees 端点..."
curl -k -s https://localhost:8443/api/employees | head -c 200

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ HTTPS 连接测试成功"
else
    echo ""
    echo "✗ HTTPS 连接测试失败"
fi

# 测试服务注册端点
echo ""
echo "测试 /services 端点..."
curl -k -s https://localhost:8443/services

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ 服务列表获取成功"
else
    echo ""
    echo "✗ 服务列表获取失败"
fi

# 7. 使用 openssl 客户端测试
echo ""
echo "[7] 使用 OpenSSL 客户端测试..."
echo | timeout 5 openssl s_client -connect localhost:8443 2>/dev/null | grep -E "(Protocol|Cipher|Verify)"

# 8. 停止网关
echo ""
echo "[8] 停止网关..."
kill $GATEWAY_PID 2>/dev/null

if [ $? -eq 0 ]; then
    echo "✓ 网关已停止"
else
    echo "✗ 网关停止失败"
fi

# 9. 清理
echo ""
echo "[9] 清理临时文件..."
# 保留证书和配置文件，方便下次测试
# rm -f server.crt server.key gateway_config_https.json
echo "✓ 测试完成！"
echo ""
echo "提示："
echo "  - 证书文件：server.crt, server.key"
echo "  - 配置文件：gateway_config_https.json"
echo "  - 日志文件：logs/gateway.log"
echo ""
echo "如需手动启动："
echo "  ./bin/c_gateway gateway_config_https.json"
echo ""
