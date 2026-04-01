@echo off
REM 多线程工作服务 v3 - Windows 编译脚本 (libuv + llhttp)

echo === 编译多线程工作服务 v3 (支持 libuv + llhttp) ===
echo.

set CC=gcc
set CFLAGS=-DHAVE_OPENSSL -O2
set LIBS=-luv -lllhttp -lcurl -lcjson -lpthread -lssl -lcrypto

if not exist bin mkdir bin

echo [编译] worker_service_v3.c ...

%CC% %CFLAGS% -o bin\worker_service_v3.exe worker_service_v3.c %LIBS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [成功] 编译完成！
    echo 可执行文件：bin\worker_service_v3.exe
    echo.
    echo 使用方法:
    echo   bin\worker_service_v3.exe worker_config.json
    echo.
    echo 技术栈:
    echo   - 网络层：libuv ^(异步事件驱动^)
    echo   - HTTP 解析：llhttp ^(高性能^)
    echo   - 配置加载：cJSON
    echo   - HTTP 客户端：libcurl
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
    echo   - libuv
    echo   - llhttp
    echo   - libcurl (with SSL support)
    echo   - cJSON
    echo   - pthread-w32
    echo   - OpenSSL
    echo.
    echo Windows 安装提示:
    echo   1. 使用 MSYS2: pacman -S mingw-w64-x86_64-libuv mingw-w64-x86_64-curl
    echo   2. llhttp 需要手动编译：https://github.com/nodejs/llhttp
    echo.
)

pause
