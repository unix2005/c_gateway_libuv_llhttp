# Makefile for Worker Service v1, v2, v3
# 支持编译所有版本的工作服务

# 编译器设置
CC = gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -DHAVE_OPENSSL -I include -g
LDFLAGS = 

# 依赖库
LIBS = -luv -lllhttp -lcurl -lcjson -lpthread -lssl -lcrypto

# 目录结构
SRC_DIR = .
BIN_DIR = .
OBJ_DIR = .

# 目标文件（排除 v1，因为它使用不同的架构）
TARGETS = $(BIN_DIR)/worker_service_v2 \
          $(BIN_DIR)/worker_service_v3 \
          $(BIN_DIR)/worker_service_v3_1

# 源文件
V2_SRC = worker_service_v2.c
V3_SRC = worker_service_v3.c
V3_1_SRC = worker_service_v3_1.c

# 对象文件
V2_OBJ = $(OBJ_DIR)/worker_service_v2.o
V3_OBJ = $(OBJ_DIR)/worker_service_v3.o
V3_1_OBJ = $(OBJ_DIR)/worker_service_v3_1.o

# 默认目标
all: dirs $(TARGETS)
	@echo ""
	@echo "✅ 编译完成！"
	@echo "生成的可执行文件:"
	@ls -lh $(BIN_DIR)/

# 创建目录
dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)

# ============================================================================
# Worker Service v2 (配置版 - socket + JSON 配置)
# ============================================================================
$(BIN_DIR)/worker_service_v2: $(V2_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "[LINK] $@"

$(OBJ_DIR)/worker_service_v2.o: $(V2_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<
	@echo "[CC] $<"

# ============================================================================
# Worker Service v3 (高性能版 - libuv + llhttp, 单线程)
# ============================================================================
$(BIN_DIR)/worker_service_v3: $(V3_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "[LINK] $@"

$(OBJ_DIR)/worker_service_v3.o: $(V3_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<
	@echo "[CC] $<"

# ============================================================================
# Worker Service v3.1 (真·多线程版 - libuv + llhttp + pthread)
# ============================================================================
$(BIN_DIR)/worker_service_v3_1: $(V3_1_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "[LINK] $@"

$(OBJ_DIR)/worker_service_v3_1.o: $(V3_1_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<
	@echo "[CC] $<"

# ============================================================================
# 清理
# ============================================================================
clean:
	@echo "正在清理..."
	#rm -rf $(BIN_DIR) $(OBJ_DIR)
	rm -f *.o
	@echo "✅ 清理完成"

# ============================================================================
# 安装依赖检查
# ============================================================================
check-deps:
	@echo "检查依赖库..."
	@pkg-config --exists libuv && echo "✓ libuv 已安装" || echo "✗ libuv 未安装"
	@pkg-config --exists libcurl && echo "✓ libcurl 已安装" || echo "✗ libcurl 未安装"
	@pkg-config --exists libcjson && echo "✓ libcjson 已安装" || echo "✗ libcjson 未安装"
	@echo ""
	@echo "如需安装依赖，请运行:"
	@echo "  Ubuntu/Debian:"
	@echo "    sudo apt-get install libuv1-dev libcurl4-openssl-dev libcjson-dev libssl-dev"
	@echo ""
	@echo "  CentOS/RHEL:"
	@echo "    sudo yum install libuv-devel libcurl-devel cjson-devel openssl-devel"
	@echo ""
	@echo "  macOS:"
	@echo "    brew install libuv curl cjson openssl"
	@echo ""

# ============================================================================
# 帮助信息
# ============================================================================
help:
	@echo "Worker Service Makefile 使用说明"
	@echo "================================="
	@echo ""
	@echo "可用目标:"
	@echo "  all              - 编译所有版本 (默认)"
	@echo "  worker_service   - 编译 v1 版本 (基础版)"
	@echo "  worker_service_v2 - 编译 v2 版本 (配置版)"
	@echo "  worker_service_v3 - 编译 v3 版本 (libuv 单线程)"
	@echo "  worker_service_v3_1 - 编译 v3.1 版本 (libuv 多线程)"
	@echo "  clean            - 清理编译产物"
	@echo "  check-deps       - 检查依赖库"
	@echo "  help             - 显示帮助信息"
	@echo ""
	@echo "示例:"
	@echo "  make                     # 编译所有版本"
	@echo "  make worker_service_v3_1 # 只编译 v3.1"
	@echo "  make clean               # 清理"
	@echo "  make check-deps          # 检查依赖"
	@echo ""

# ============================================================================
# 快捷目标
# ============================================================================
v2: $(BIN_DIR)/worker_service_v2
v3: $(BIN_DIR)/worker_service_v3
v3_1: $(BIN_DIR)/worker_service_v3_1

# 声明伪目标
.PHONY: all dirs clean check-deps help v2 v3 v3_1
