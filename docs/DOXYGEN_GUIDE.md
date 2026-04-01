# Doxygen API 文档生成指南

## 概述

本项目使用 Doxygen 自动生成完整的 API 参考文档。Doxygen 会从源代码中的注释提取文档，生成 HTML 格式的交互式文档。

---

## 前置要求

### 安装 Doxygen

#### Linux (Ubuntu/Debian)
```bash
sudo apt-get install doxygen graphviz
```

#### Linux (CentOS/RHEL)
```bash
sudo yum install doxygen graphviz
```

#### macOS
```bash
brew install doxygen graphviz
```

#### Windows
从官网下载安装包：https://www.doxygen.nl/download.html

---

## 生成文档

### 方法 1: 使用 Makefile（推荐）

```bash
# 生成 Doxygen 文档
make docs

# 清理文档
make clean-docs
```

### 方法 2: 直接使用 Doxygen

```bash
# 使用配置文件生成
doxygen Doxyfile.in
```

---

## 查看文档

生成的文档位于：`docs/doxygen/html/index.html`

### 在 Linux 上查看
```bash
# 使用浏览器打开
xdg-open docs/doxygen/html/index.html

# 或使用 Firefox
firefox docs/doxygen/html/index.html
```

### 在 macOS 上查看
```bash
open docs/doxygen/html/index.html
```

### 在 Windows 上查看
双击 `docs\doxygen\html\index.html` 文件

---

## 文档结构

生成的文档包含以下主要部分：

### 1. 首页 (Main Page)
- 项目概述
- 版本信息
- 作者信息

### 2. 命名空间成员 (Modules)
按功能模块组织的函数和类型：
- **网络层模块** - TCP 服务器、连接管理
- **路由层模块** - 请求路由、响应发送
- **工具函数模块** - 内存池、IP 解析
- **可观测性模块** - 日志、指标、追踪
- **服务注册模块** - 服务发现、健康检查

### 3. 数据结构 (Data Structures)
详细的数据结构说明：
- `client_ctx_t` - 客户端上下文
- `service_t` - 服务定义
- `service_instance_t` - 服务实例
- `gateway_config_t` - 网关配置
- `observability_config_t` - 可观测性配置
- 等等...

### 4. 文件列表 (Files)
所有源文件和头文件的清单：
- `src/main.c` - 程序入口
- `src/network.c` - 网络层实现
- `include/gateway.h` - 核心头文件
- 等等...

### 5. 数据字段 (Data Fields)
结构体成员的详细说明

### 6. 函数原型 (Functions)
所有函数的完整声明和说明

---

## 注释规范

本项目采用 Doxygen 标准的注释格式：

### 文件头注释
```c
/**
 * @file filename.c
 * @brief 文件简要描述
 * 
 * 详细描述...
 * 
 * @author 作者名
 * @date 日期
 * @version 版本号
 */
```

### 函数注释
```c
/**
 * @brief 函数功能简述
 * 
 * 函数功能详述...
 * 
 * @param param1 参数 1 说明
 * @param param2 参数 2 说明
 * @return 返回值说明
 */
int function_name(int param1, char* param2);
```

### 结构体注释
```c
/**
 * @brief 结构体名称
 * 
 * 结构体用途说明...
 */
typedef struct {
    int field1;   ///< 字段 1 说明
    char* field2; ///< 字段 2 说明
} my_struct_t;
```

### 枚举注释
```c
/**
 * @brief 枚举名称
 * 
 * 枚举用途说明...
 */
typedef enum {
    VALUE_A,  ///< 值 A 说明
    VALUE_B,  ///< 值 B 说明
    VALUE_C   ///< 值 C 说明
} my_enum_t;
```

---

## Doxyfile.in 配置说明

关键配置项说明：

### 项目信息
```
PROJECT_NAME = "微服务网关"           # 项目名称
PROJECT_NUMBER = "1.0.0"              # 版本号
PROJECT_BRIEF = "基于 libuv 的高性能微服务网关"  # 项目简介
```

### 输入输出
```
INPUT = "src include"                 # 源文件目录
OUTPUT_DIRECTORY = "docs/doxygen"     # 输出目录
FILE_PATTERNS = "*.c *.h"             # 匹配的文件类型
```

### 文档内容
```
EXTRACT_ALL = YES                     # 提取所有文档（即使没有注释）
EXTRACT_STATIC = YES                  # 提取静态函数
SOURCE_BROWSER = YES                  # 显示源码浏览
```

### 图形生成
```
CALL_GRAPH = YES                      # 生成调用图
CALLER_GRAPH = YES                    # 生成被调用图
GRAPHICAL_HIERARCHY = YES             # 生成层次图
```

### C 语言优化
```
OPTIMIZE_OUTPUT_FOR_C = YES           # 针对 C 语言优化
TYPEDEF_HIDES_STRUCT = NO             # 显示 typedef 和 struct
```

---

## 文档示例

### 函数文档示例

以 `route_request` 函数为例：

**源码注释**：
```c
/**
 * @brief 路由请求处理函数
 * 
 * 根据请求 URL 和方法进行路由匹配，分发到对应的处理函数。
 * 支持内置端点和后端服务代理。
 * 
 * @param client 客户端上下文
 */
void route_request(client_ctx_t* client);
```

**生成的文档包含**：
- 函数原型
- 功能描述
- 参数说明
- 所在文件位置
- 被哪些函数调用（调用图）
- 调用了哪些函数（被调用图）

### 结构体文档示例

以 `client_ctx_t` 为例：

**生成的文档包含**：
- 结构体定义
- 每个字段的详细说明
- 字段类型和大小
- 使用示例
- 相关结构体引用

---

## 高级功能

### 1. 搜索功能

Doxygen 生成的文档支持全文搜索：
- 点击右上角的搜索框
- 输入函数名、类型、关键字
- 快速定位到相关文档

### 2. 调用关系图

启用 Graphviz 后，文档会显示：
- **Caller Graph**: 哪些函数调用了当前函数
- **Call Graph**: 当前函数调用了哪些函数

### 3. 源码关联

开启 `SOURCE_BROWSER` 后：
- 可以点击函数名跳转到源码
- 源码中显示对应的文档注释
- 支持交叉引用

---

## 故障排查

### 问题 1: 提示找不到 Doxygen

**解决**：
```bash
# 检查是否安装
which doxygen

# 如果未安装，参考"前置要求"部分进行安装
```

### 问题 2: 生成的文档为空

**可能原因**：
- 源代码中没有 Doxygen 格式的注释
- `EXTRACT_ALL` 设置为 NO

**解决**：
- 确保源代码中有 `/** ... */` 格式的注释
- 检查 `Doxyfile.in` 中 `EXTRACT_ALL = YES`

### 问题 3: 调用图无法显示

**可能原因**：
- 未安装 Graphviz

**解决**：
```bash
# Ubuntu/Debian
sudo apt-get install graphviz

# CentOS/RHEL
sudo yum install graphviz

# macOS
brew install graphviz
```

### 问题 4: 中文显示乱码

**解决**：
- 确保源文件使用 UTF-8 编码
- 检查 `Doxyfile.in` 中 `INPUT_ENCODING = "UTF-8"`
- 浏览器设置为 UTF-8 编码

---

## 最佳实践

### 1. 及时更新文档

每次修改代码时，同步更新注释：
- 修改函数功能时，更新描述
- 新增参数时，添加 `@param` 说明
- 修改返回值时，更新 `@return` 说明

### 2. 保持注释简洁

- `@brief` 要简明扼要
- 详细描述可以分段
- 避免冗余信息

### 3. 使用分组

将相关的函数组织在一起：
```c
// === 网络层函数 ===
/** @{ */

/** 函数 1 说明 */
void func1(void);

/** 函数 2 说明 */
void func2(void);

/** @} */
```

### 4. 添加示例代码

对于复杂的函数，可以添加使用示例：
```c
/**
 * @brief 函数说明
 * 
 * 示例:
 * @code
 * client_ctx_t ctx;
 * route_request(&ctx);
 * @endcode
 */
```

---

## 持续集成

可以在 CI/CD 流程中自动更新文档：

### GitLab CI 示例
```yaml
pages:
  stage: deploy
  script:
    - make docs
    - mv docs/doxygen/html public/
  artifacts:
    paths:
      - public
  only:
    - master
```

### GitHub Actions 示例
```yaml
name: Deploy Docs
on:
  push:
    branches: [ master ]
jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install Doxygen
        run: sudo apt-get install doxygen graphviz
      - name: Build Docs
        run: make docs
      - name: Deploy to GitHub Pages
        uses: peaceiris/actions-gh-pages@v3
        with:
          github_token: ${{ secrets.GITHUB_TOKEN }}
          publish_dir: ./docs/doxygen/html
```

---

## 参考资料

- [Doxygen 官网](https://www.doxygen.nl/)
- [Doxygen 手册](https://www.doxygen.nl/manual/index.html)
- [Graphviz 官网](https://graphviz.org/)
- [Doxygen 示例](https://github.com/doxygen/examples)

---

**最后更新**: 2026-03-25  
**版本**: v1.0.0
