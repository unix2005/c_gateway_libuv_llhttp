@echo off
REM ========================================
REM 微服务网关 - 可观测性功能测试脚本
REM ========================================

echo.
echo === 微服务网关可观测性测试 ===
echo.

REM 1. 启动网关（后台运行）
echo [1] 启动网关...
start "Gateway" cmd /k "cd /d %~dp0.. && bin\c_gateway.exe config\gateway_config.json"
timeout /t 2 /nobreak >nul

REM 2. 发送测试请求
echo [2] 发送测试请求...
curl -s http://localhost:8080/api/users >nul
curl -s http://localhost:8080/services >nul
curl -s -X POST http://localhost:8080/api/test -d "{\"test\":\"data\"}" >nul

echo [3] 等待响应...
timeout /t 1 /nobreak >nul

REM 3. 检查 Prometheus 指标端点
echo.
echo [4] 检查 Prometheus 指标端点...
curl -s http://localhost:9090/metrics | findstr "gateway_http_requests_total"
if %ERRORLEVEL% EQU 0 (
    echo ✓ Prometheus 指标端点正常
) else (
    echo ✗ Prometheus 指标端点异常
)

echo.
echo [5] 查看完整指标...
curl -s http://localhost:9090/metrics

echo.
echo [6] 查看日志文件...
type logs\gateway.log 2>nul || echo 日志文件不存在或未生成

echo.
echo === 测试完成 ===
echo.
echo 提示:
echo - 按任意键停止网关
echo - 查看日志文件：logs\gateway.log
echo - 访问指标端点：http://localhost:9090/metrics
echo.

pause >nul

REM 停止网关（手动）
echo 正在关闭网关...
taskkill /FI "WINDOWTITLE eq Gateway*" /T /F >nul 2>&1

echo 已完成！
