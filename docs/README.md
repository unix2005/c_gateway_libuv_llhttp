# 项目文档归档目录

本目录包含 C 语言微服务网关项目的所有原始 Markdown 文档。

---

## 📁 目录结构

```
docs/
├── README.md                 # 本文档
├── BUGFIX_CURL_SLIST.md      # libcurl slist 释放错误修复
├── BUGFIX_C_LAMBDA.md        # C 语言 lambda 表达式修复
├── BUGFIX_C_LAMBDA_ROUND2.md # C lambda 第二轮修复
├── BUGFIX_FORWARD_DECLARATION.md  # 函数前向声明修复
├── CONFIG_ARCHITECTURE.md    # 配置架构设计
├── CONFIG_IMPLEMENTATION_SUMMARY.md  # 配置实现总结
├── CONFIG_UPGRADE_NOTES.md   # 配置升级说明
├── EXAMPLES.md               # 使用示例
├── FIX_CONFIRMATION.md       # 修复确认
├── GATEWAY_CONFIG_GUIDE.md   # 网关配置指南
├── PROJECT_DELIVERABLES.md   # 项目交付物
├── QUICK_CONFIG_REFERENCE.md # 快速配置参考
├── QUICK_REFERENCE.md        # 快速参考手册
├── TESTING.md                # 测试指南
├── UPGRADE_SUMMARY.md        # 升级总结
├── include/                  # include 目录文档
│   ├── FIX_GETTID_WARNING.md
│   └── HEADER_MERGE_NOTES.md
├── service_worker/           # Worker Service 文档
│   ├── BUILD.md
│   ├── COMPILE_SUCCESS.md
│   ├── CONFIG_GUIDE.md
│   ├── CONFIG_REGISTER_UNREGISTER.md
│   ├── FIX_GRACEFUL_SHUTDOWN.md
│   ├── FIX_USLEEP_WARNING.md
│   ├── FIX_UV_RUN_EXIT.md
│   ├── FIX_WORKER_V2_COMPILE.md
│   ├── HOST_CONFIG_FEATURE.md
│   ├── MAKEFILE_GUIDE.md
│   ├── MULTI_THREAD_ANALYSIS.md
│   ├── POOL_ALLOC_IMPLEMENTATION.md
│   ├── POOL_ALLOC_INIT_EXPLANATION.md
│   ├── README.md
│   ├── README_v3.md
│   ├── SERVICE_WORKER_SUMMARY.md
│   ├── TESTING.md
│   ├── UPGRADE_v2.md
│   ├── VERSION_NOTES.md
│   ├── WORKER_CONFIG_EXAMPLES.md
│   └── ... (其他 Worker 文档)
└── src/                      # 网关核心功能文档
    ├── DOMAIN_NAME_SUPPORT.md
    ├── FIX_INVALID_POINTER_FREE.md
    ├── FIX_NOT_FOUND_STRING.md
    ├── HEALTH_CHECK_AUTO_REMOVE.md
    ├── HEALTH_CHECK_DELETE_SERVICE.md
    └── UNIFIED_REGISTER_JSON.md
```

---

## 📖 文档分类

### Bug 修复类
- `BUGFIX_CURL_SLIST.md` - libcurl slist 释放错误修复
- `BUGFIX_C_LAMBDA.md` - C 语言 lambda 表达式修复
- `BUGFIX_C_LAMBDA_ROUND2.md` - C lambda 第二轮修复
- `BUGFIX_FORWARD_DECLARATION.md` - 函数前向声明修复
- `FIX_CONFIRMATION.md` - 修复确认
- `FIX_GRACEFUL_SHUTDOWN.md` - 优雅退出修复（Worker）
- `FIX_USLEEP_WARNING.md` - usleep 编译警告修复（Worker）
- `FIX_UV_RUN_EXIT.md` - uv_run 退出修复（Worker）
- `FIX_WORKER_V2_COMPILE.md` - v2 编译修复（Worker）
- `FIX_INVALID_POINTER_FREE.md` - free() invalid pointer 修复
- `FIX_NOT_FOUND_STRING.md` - "Not Found"字符串字面量修复

### 配置相关类
- `CONFIG_ARCHITECTURE.md` - 配置架构设计
- `CONFIG_IMPLEMENTATION_SUMMARY.md` - 配置实现总结
- `CONFIG_UPGRADE_NOTES.md` - 配置升级说明
- `GATEWAY_CONFIG_GUIDE.md` - 网关配置指南
- `QUICK_CONFIG_REFERENCE.md` - 快速配置参考
- `HOST_CONFIG_FEATURE.md` - Host 地址配置功能（Worker）

### 使用指南类
- `EXAMPLES.md` - 使用示例
- `QUICK_REFERENCE.md` - 快速参考手册
- `TESTING.md` - 测试指南
- `BUILD.md` - 构建指南（Worker）
- `MAKEFILE_GUIDE.md` - Makefile 指南（Worker）

### 功能特性类
- `DOMAIN_NAME_SUPPORT.md` - 域名解析支持
- `UNIFIED_REGISTER_JSON.md` - 统一服务注册 JSON 格式
- `HEALTH_CHECK_AUTO_REMOVE.md` - 健康检查自动剔除机制
- `HEALTH_CHECK_DELETE_SERVICE.md` - 空服务自动删除
- `MULTI_THREAD_ANALYSIS.md` - 多线程分析（Worker）
- `POOL_ALLOC_IMPLEMENTATION.md` - 内存池实现（Worker）
- `POOL_ALLOC_INIT_EXPLANATION.md` - 内存池初始化说明（Worker）

### 版本升级类
- `UPGRADE_SUMMARY.md` - 升级总结
- `UPGRADE_v2.md` - v2 升级指南（Worker）
- `VERSION_NOTES.md` - 版本说明（Worker）
- `CONFIG_REGISTER_UNREGISTER.md` - 注册注销配置（Worker）

### 项目文档类
- `PROJECT_DELIVERABLES.md` - 项目交付物清单
- `README.md` - 项目说明
- `SERVICE_WORKER_SUMMARY.md` - Worker Service 总结
- `HEADER_MERGE_NOTES.md` - 头文件合并说明
- `COMPILE_SUCCESS.md` - 编译成功确认（Worker）

---

## 🔗 综合文档

项目的综合文档位于根目录：

- **PROJECT_DOCS.md** - 项目文档合集（根目录）
- **WORKER_DOCS.md** - Worker Service 文档合集（service_worker 目录）
- **GATEWAY_FEATURES.md** - 网关核心功能文档（src 目录）
- **DOC_MERGE_SUMMARY.md** - 文档合并说明（根目录）

**建议优先阅读综合文档**，它们包含了所有原始文档的核心内容和索引。

---

## 📝 使用说明

### 查找特定主题

1. **Bug 修复** → 查看文件名包含 `BUGFIX_` 或 `FIX_` 的文档
2. **配置相关** → 查看文件名包含 `CONFIG_` 的文档
3. **功能特性** → 查看 `src/` 和 `service_worker/` 子目录
4. **快速参考** → 阅读 `QUICK_REFERENCE.md` 或 `QUICK_CONFIG_REFERENCE.md`

### 版本追溯

如需了解某个功能的历史实现细节，可以查阅对应的原始文档。

---

## 🎯 推荐阅读顺序

### 新用户入门
1. `README.md` - 了解项目概况
2. `GATEWAY_CONFIG_GUIDE.md` - 学习网关配置
3. `QUICK_REFERENCE.md` - 快速上手

### Worker Service 开发
1. `service_worker/README.md` - Worker 概述
2. `service_worker/BUILD.md` - 构建指南
3. `service_worker/CONFIG_GUIDE.md` - 配置说明
4. `service_worker/WORKER_DOCS.md` - 综合文档

### 网关功能开发
1. `src/UNIFIED_REGISTER_JSON.md` - 服务注册
2. `src/DOMAIN_NAME_SUPPORT.md` - 域名支持
3. `src/HEALTH_CHECK_AUTO_REMOVE.md` - 健康检查
4. `src/GATEWAY_FEATURES.md` - 综合文档

### Bug 排查
1. 根据错误信息选择对应的 `BUGFIX_*.md` 或 `FIX_*.md` 文档
2. 查看问题原因和解决方案

---

## 📚 文档维护

### 更新原则

1. **新增功能** → 创建新的 MD 文档
2. **重大修改** → 更新原文档并标注版本
3. **综合整理** → 更新对应的综合文档（`*_DOCS.md`）

### 文档位置

- **原始文档** → 存放在 `docs/` 目录
- **综合文档** → 存放在对应模块目录（根目录、`src/`、`service_worker/`）
- **临时文档** → 可先放在 `docs/`，稳定后归类

---

## ✅ 归档清单

### 根目录已归档 (16 个文档)
- [x] BUGFIX_CURL_SLIST.md
- [x] BUGFIX_C_LAMBDA.md
- [x] BUGFIX_C_LAMBDA_ROUND2.md
- [x] BUGFIX_FORWARD_DECLARATION.md
- [x] CONFIG_ARCHITECTURE.md
- [x] CONFIG_IMPLEMENTATION_SUMMARY.md
- [x] CONFIG_UPGRADE_NOTES.md
- [x] EXAMPLES.md
- [x] FIX_CONFIRMATION.md
- [x] GATEWAY_CONFIG_GUIDE.md
- [x] PROJECT_DELIVERABLES.md
- [x] QUICK_CONFIG_REFERENCE.md
- [x] QUICK_REFERENCE.md
- [x] README.md
- [x] TESTING.md
- [x] UPGRADE_SUMMARY.md

### include 目录已归档 (2 个文档)
- [x] FIX_GETTID_WARNING.md
- [x] HEADER_MERGE_NOTES.md

### service_worker 目录已归档 (20+ 个文档)
- [x] BUILD.md
- [x] COMPILE_SUCCESS.md
- [x] CONFIG_GUIDE.md
- [x] CONFIG_REGISTER_UNREGISTER.md
- [x] FIX_GRACEFUL_SHUTDOWN.md
- [x] FIX_USLEEP_WARNING.md
- [x] FIX_UV_RUN_EXIT.md
- [x] FIX_WORKER_V2_COMPILE.md
- [x] HOST_CONFIG_FEATURE.md
- [x] MAKEFILE_GUIDE.md
- [x] MULTI_THREAD_ANALYSIS.md
- [x] POOL_ALLOC_IMPLEMENTATION.md
- [x] POOL_ALLOC_INIT_EXPLANATION.md
- [x] README.md
- [x] README_v3.md
- [x] SERVICE_WORKER_SUMMARY.md
- [x] TESTING.md
- [x] UPGRADE_v2.md
- [x] VERSION_NOTES.md
- [x] WORKER_CONFIG_EXAMPLES.md
- [x] ... (其他 Worker 文档)

### src 目录已归档 (6 个文档)
- [x] DOMAIN_NAME_SUPPORT.md
- [x] FIX_INVALID_POINTER_FREE.md
- [x] FIX_NOT_FOUND_STRING.md
- [x] HEALTH_CHECK_AUTO_REMOVE.md
- [x] HEALTH_CHECK_DELETE_SERVICE.md
- [x] UNIFIED_REGISTER_JSON.md

---

## 📋 保留在原地的综合文档

以下文档保留在原始位置，便于快速访问：

- **根目录**: 
  - `PROJECT_DOCS.md` - 项目文档合集
  - `DOC_MERGE_SUMMARY.md` - 文档合并说明

- **service_worker/**:
  - `WORKER_DOCS.md` - Worker Service 文档合集

- **src/**:
  - `GATEWAY_FEATURES.md` - 网关核心功能文档

---

**归档日期**: 2026-03-23  
**文档版本**: v1.0  
**状态**: ✅ 已完成
