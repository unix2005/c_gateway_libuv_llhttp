@echo off
REM 微服务网关 - Windows 编译脚本 (支持 HTTPS + IPv6)

echo === 微服务网关编译脚本 (Windows) ===
echo.

REM 设置编译器
set CC=gcc

REM 设置包含目录和库目录（根据你的实际安装路径修改）
set INCLUDE_DIRS=-I./include
set LIB_DIRS=

REM 需要链接的库
set LIBS=-luv -lllhttp -lcurl -lcjson -lpthread

REM 源文件
set SRCS=src\main.c src\network.c src\service_registry.c src\health_checker.c src\proxy.c src\config.c src\router.c src\utils.c src\logger.c src\metrics.c src\tracer.c

REM 创建输出目录
if not exist bin mkdir bin

echo [编译] 正在编译微服务网关...
echo.

%CC% %INCLUDE_DIRS% -o bin\c_gateway.exe %SRCS% %LIBS% %LIB_DIRS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [成功] 编译完成！
    echo 可执行文件：bin\c_gateway.exe
    echo.
    echo 运行网关：bin\c_gateway.exe
    echo.
) else (
    echo.
    echo [错误] 编译失败！
    echo 请确保已安装以下依赖库:
    echo   - libuv
    echo   - llhttp
    echo   - libcurl
    echo   - cJSON
    echo   - pthread-w32
    echo.
    echo 并检查这些库的头文件和库文件路径是否正确配置。
    echo.
)

pause
