@echo off
REM 多线程工作服务 v2 - Windows 编译脚本

echo === 编译多线程工作服务 v2 (支持 IPv6/HTTPS) ===
echo.

set CC=gcc
set CFLAGS=-DHAVE_OPENSSL
set LIBS=-lcurl -lcjson -lpthread -lssl -lcrypto

if not exist bin mkdir bin

echo [编译] worker_service_v2.c ...

%CC% %CFLAGS% -o bin\worker_service.exe worker_service_v2.c %LIBS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [成功] 编译完成！
    echo 可执行文件：bin\worker_service.exe
    echo.
    echo 使用方法:
    echo   bin\worker_service.exe worker_config.json
    echo.
    echo 配置文件示例:
    echo   worker_config.json - 基本 HTTP 配置
    echo   worker_config_https.json - HTTPS 配置
    echo   worker_config_ipv6.json - IPv6 配置
    echo.
) else (
    echo.
    echo [错误] 编译失败！
    echo 请确保已安装以下库:
    echo   - libcurl (with SSL support)
    echo   - cJSON
    echo   - pthread-w32
    echo   - OpenSSL
    echo.
)

pause
