# VR 献血题库服务构建（依赖 sdk/dist 下的 libcservice）
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -g -D_GNU_SOURCE -I../sdk/include

# libcservice 已内置 uv/llhttp/cjson/curl/pthread/jansson/xml2/mariadb，
# 这里显式列出直接依赖，确保静态链接或缺失 DT_NEEDED 时也能解析。
LDFLAGS ?= -L../sdk/dist/lib -lcservice -luv -lllhttp -lcjson -lcurl \
           -lpthread -ljansson -lxml2 -lmariadb -lcrypto \
           -Wl,-rpath,'$$ORIGIN/../sdk/dist/lib'

TARGET  := vr_question_service

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
