@echo off
chcp 65001 >nul
REM ========================================
REM 微服务网关 - Doxygen 文档生成脚本
REM 
REM 功能：检查环境、安装依赖（可选）、生成文档
REM ========================================

echo ============================================================
echo           微服务网关 API 文档生成工具
echo ============================================================
echo.

REM 检查当前目录
if not exist "Doxyfile.in" (
    echo [错误] 未找到 Doxyfile.in 配置文件！
    echo 请确保在正确的目录运行此脚本。
    echo 当前目录：%CD%
    pause
    exit /b 1
)

echo [检查] 正在检查 Doxygen 安装状态...
echo.

REM 检查 Doxygen 是否已安装
where doxygen >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [警告] 未找到 Doxygen！
    echo.
    echo 请选择操作:
    echo   1. 打开 Doxygen 下载页面（需要手动安装）
    echo   2. 退出
    echo.
    set /p choice=请输入选项 (1/2): 
    
    if "%choice%"=="1" (
        echo.
        echo [信息] 正在打开浏览器...
        start https://www.doxygen.nl/download.html
        echo.
        echo 请在打开的页面下载并安装 Doxygen for Windows
        echo 安装完成后，重新运行此脚本。
        echo.
        echo 安装说明:
        echo   1. 下载安装包
        echo   2. 运行安装程序
        echo   3. 将 C:\Program Files\doxygen\bin 添加到 PATH 环境变量
        echo   4. 重新打开命令提示符
        echo.
        pause
        exit /b 1
    ) else (
        echo.
        echo [操作已取消]
        pause
        exit /b 1
    )
)

REM Doxygen 已安装，显示版本
echo [成功] Doxygen 已安装
for /f "tokens=*" %%i in ('doxygen --version') do set DOXYGEN_VERSION=%%i
echo [版本] %DOXYGEN_VERSION%
echo.

REM 检查 Graphviz（可选）
echo [检查] 正在检查 Graphviz 安装状态...
where dot >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo [成功] Graphviz 已安装
    for /f "tokens=*" %%i in ('dot -V 2^>^&1 ^| findstr "version"') do set GRAPHVIZ_VERSION=%%i
    echo [版本] %GRAPHVIZ_VERSION%
) else (
    echo [警告] Graphviz 未安装（调用图将无法生成）
    echo.
    echo 如需安装 Graphviz，请访问:
    echo https://graphviz.org/download/
)
echo.

REM 询问是否继续
echo 准备生成文档...
echo   输出目录：docs\doxygen\html\
echo.
set /p confirm=是否继续？(Y/N): 
if /i not "%confirm%"=="Y" (
    echo.
    echo [操作已取消]
    pause
    exit /b 0
)

echo.
echo ============================================================
echo 正在生成文档...
echo ============================================================
echo.

REM 清理旧文档
if exist "docs\doxygen\html" (
    echo [清理] 删除旧的文档...
    rmdir /s /q docs\doxygen\html
)

REM 创建输出目录
if not exist "docs\doxygen" (
    echo [准备] 创建输出目录...
    mkdir docs\doxygen
)

REM 执行 Doxygen
echo [生成] 正在生成 API 文档...
echo.
doxygen Doxyfile.in

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ============================================================
    echo [成功] API 文档已生成！
    echo ============================================================
    echo.
    echo 文档路径：docs\doxygen\html\index.html
    echo.
    
    REM 询问是否立即打开
    set /p open_doc=是否立即打开文档？(Y/N): 
    if /i "%open_doc%"=="Y" (
        echo.
        echo [打开] 正在启动浏览器查看文档...
        start docs\doxygen\html\index.html
    )
    
    echo.
    echo ============================================================
    echo 提示:
    echo   - 文档已保存到：docs\doxygen\html\index.html
    echo   - 可以使用任何现代浏览器查看
    echo   - 支持全文搜索和交叉引用
    echo ============================================================
    echo.
) else (
    echo.
    echo ============================================================
    echo [错误] 文档生成失败！
    echo ============================================================
    echo.
    echo 可能的原因:
    echo   1. Doxyfile.in 配置有误
    echo   2. 源文件路径不正确
    echo   3. Doxygen 版本过旧
    echo.
    echo 建议:
    echo   - 检查 Doxyfile.in 中的 INPUT 路径是否正确
    echo   - 确保 src 和 include 目录存在
    echo   - 查看详细错误信息 above
    echo.
)

pause
