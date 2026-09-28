CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -g -I../include
LDFLAGS ?= -L../dist/lib -lcservice -luv -lllhttp -lcjson -lcurl -lpthread \
           -Wl,-rpath,'$$ORIGIN/../dist/lib'

all: order_service proxy_service service_template

order_service: order_service.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

proxy_service: proxy_service.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

service_template: service_template.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f order_service proxy_service service_template
