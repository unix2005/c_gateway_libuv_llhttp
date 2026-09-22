# libcservice SDK Makefile
# 产出：libcservice.a（静态） libcservice.so（动态） 以及 make install 到 dist/

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -fPIC
INCS    := -Iinclude
LIBS    := -luv -lllhttp -lcjson -lcurl -lpthread

SRC_DIR := src
SRCS    := $(SRC_DIR)/core.c $(SRC_DIR)/server.c $(SRC_DIR)/router.c \
           $(SRC_DIR)/request.c $(SRC_DIR)/response.c $(SRC_DIR)/registry.c \
           $(SRC_DIR)/config.c $(SRC_DIR)/logger.c
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
	@echo "已安装到 $(DIST)/{include,lib}（库由构建步骤直接产出至 dist/lib）"

examples: shared
	$(MAKE) -C examples

clean:
	rm -f $(SRC_DIR)/*.o $(STATIC) $(SHARED)
	rm -rf $(DIST)
	$(MAKE) -C examples clean 2>/dev/null || true
