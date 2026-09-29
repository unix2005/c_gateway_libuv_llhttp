CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -g -D_GNU_SOURCE -I../include
LDFLAGS ?= -L../dist/lib -lcservice -luv -lllhttp -lcjson -lcurl -lpthread \
           -ljansson -lxml2 -lmariadb -lcrypto \
           -Wl,-rpath,'$$ORIGIN/../dist/lib'

all: order_service proxy_service service_template sm4_tool

order_service: order_service.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

proxy_service: proxy_service.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

service_template: service_template.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

sm4_tool: sm4_tool.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f order_service proxy_service service_template sm4_tool
