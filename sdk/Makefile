# libcservice SDK Makefile
# 产出：libcservice.a（静态） libcservice.so（动态） 以及 make install 到 dist/

CC      ?= gcc
# _GNU_SOURCE：q_log.c 用到 syscall(2)/sched_yield 等 GNU 扩展
CFLAGS  ?= -std=c99 -Wall -Wextra -g -O2 -fPIC -D_GNU_SOURCE
INCS    := -Iinclude $(shell pkg-config --cflags libxml-2.0 2>/dev/null) $(shell xml2-config --cflags 2>/dev/null)
# libxml2：优先 pkg-config，macOS 上常缺失，回退到 xml2-config
LIBS    := -luv -lllhttp -lcjson -lcurl -lpthread -ljansson \
           $(shell pkg-config --libs libxml-2.0 2>/dev/null) $(shell xml2-config --libs 2>/dev/null)

SRC_DIR := src
# 业务模块 + SDK 内置的 q_log 无锁异步日志（含最小 q 核心依赖 time.c/types.c）
# + db（连接池/驱动抽象）+ mapper（XML SQL Mapper），均复用 q_log.h / q_core 基础件
SRCS    := $(SRC_DIR)/core.c $(SRC_DIR)/server.c $(SRC_DIR)/router.c \
           $(SRC_DIR)/request.c $(SRC_DIR)/response.c $(SRC_DIR)/registry.c \
           $(SRC_DIR)/config.c $(SRC_DIR)/logger.c $(SRC_DIR)/q_xml_cfg.c \
           $(SRC_DIR)/q_log.c $(SRC_DIR)/q_time.c $(SRC_DIR)/q_types.c \
           $(SRC_DIR)/q_ds.c $(SRC_DIR)/q_str.c \
           $(SRC_DIR)/async.c \
           $(SRC_DIR)/http_client.c \
           $(SRC_DIR)/db/db.c $(SRC_DIR)/db/driver/mock/q_db_mock.c \
           $(SRC_DIR)/mapper/mapper.c $(SRC_DIR)/mapper/loader.c $(SRC_DIR)/mapper/expr.c

# MySQL 驱动（mariadb-connector-c，可选）：探测到头文件才编入，否则自动跳过。
# 这样 SDK 在没装驱动的环境里也能构建（mock 驱动始终可用）。
#MARIADB_PREFIX ?= /usr/local/opt/mariadb-connector-c
MARIADB_PREFIX ?= /usr
ifneq ($(wildcard $(MARIADB_PREFIX)/include/mariadb/mysql.h),)
  SRCS += $(SRC_DIR)/db/driver/mysql/q_db_mysql.c
  INCS += -I$(MARIADB_PREFIX)/include/mariadb
  LIBS += -L$(MARIADB_PREFIX)/lib -lmariadb
  $(info [db] mysql 驱动已启用: $(MARIADB_PREFIX))
else
  $(info [db] 未找到 mariadb-connector-c，跳过 mysql 驱动（mock 仍可用）)
endif

OBJS    := $(SRCS:.c=.o)

DIST    := dist
LIBDIR  := $(DIST)/lib
INCDIR  := $(DIST)/include

STATIC  := $(LIBDIR)/libcservice.a
SHARED  := $(LIBDIR)/libcservice.so

.PHONY: all static shared install clean examples

all: static shared

static: $(STATIC)
shared: $(SHARED)

$(OBJS): $(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

$(STATIC): $(OBJS)
	@mkdir -p $(LIBDIR)
	ar rcs $@ $(OBJS)

$(SHARED): $(OBJS)
	@mkdir -p $(LIBDIR)
	$(CC) -shared -o $@ $(OBJS) $(LIBS)

install: all
	@mkdir -p $(LIBDIR) $(INCDIR)
	cp include/cservice.h $(INCDIR)/
	cp include/q_log.h $(INCDIR)/
	cp include/q_xml_cfg.h $(INCDIR)/
	cp include/q_ds.h $(INCDIR)/
	cp include/q_str.h $(INCDIR)/
	cp include/db.h $(INCDIR)/
	cp include/db_mock.h $(INCDIR)/
	cp include/mapper.h $(INCDIR)/
	@echo "已安装到 $(DIST)/{include,lib}（库由构建步骤直接产出至 dist/lib）"

examples: shared
	$(MAKE) -C examples

clean:
	find $(SRC_DIR) -name '*.o' -delete 2>/dev/null || true
	rm -f $(STATIC) $(SHARED)
	rm -rf $(DIST)
	$(MAKE) -C examples clean 2>/dev/null || true
