@echo off
REM Windows 下注册服务工具编译脚本

echo === 编译服务注册工具 ===
echo.

set CC=gcc
set LIBS=-lcurl -lcjson

if not exist bin mkdir bin

echo [编译] register_service.c ...

%CC% -o bin\register_service.exe tools\register_service.c %LIBS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [成功] 编译完成！
    echo 可执行文件：bin\register_service.exe
    echo.
    echo 用法示例:
    echo   bin\register_service.exe localhost 8080 my-service /api/myservice localhost 8081 http /health false false
    echo.
) else (
    echo.
    echo [错误] 编译失败！
    echo 请确保已安装 libcurl 和 cJSON 库。
    echo.
)

pause
