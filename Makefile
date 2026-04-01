CC = gcc
CFLAGS = -O3 -Wall -I./include -g
LIBS = -luv -lllhttp -lcurl -lpthread -lcjson 

SRCS = src/main.c src/network.c src/service_registry.c src/health_checker.c \
       src/proxy.c src/config.c src/router.c src/utils.c \
       src/logger.c src/metrics.c src/tracer.c
TARGET = bin/c_gateway


$(TARGET): $(SRCS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -rf bin/*

.PHONY: all clean run
