# VR 献血题库服务构建（依赖 sdk/dist 下的 libcservice）
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -g -D_GNU_SOURCE -I../sdk/include

# libcservice 已内置 uv/llhttp/cjson/curl/pthread/jansson/xml2/mariadb/oracle(odpic)，
# 这里显式列出直接依赖，确保静态链接或缺失 DT_NEEDED 时也能解析。

# Oracle 驱动（ODPI-C）链接：与 sdk 一致，探测到 dpi.h 才启用。
ORACLE_PREFIX ?= /home/qiao/open_src/odpi-26.0.0
ORACLE_LIBS :=
ifneq ($(wildcard $(ORACLE_PREFIX)/include/dpi.h),)
  CFLAGS += -DQ_ENABLE_ORACLE
  ORACLE_LIBS := -L$(ORACLE_PREFIX)/lib -lodpic
  $(info [svc] oracle 驱动链接已启用: $(ORACLE_PREFIX))
endif

LDFLAGS ?= -L../sdk/dist/lib -lcservice -luv -lllhttp -lcjson -lcurl \
           -lpthread -ljansson -lxml2 -lmariadb -lcrypto $(ORACLE_LIBS) \
           -Wl,-rpath,'$$ORIGIN/../sdk/dist/lib'

TARGET  := vr_question_service

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
