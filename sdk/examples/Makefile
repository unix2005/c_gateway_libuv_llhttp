CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -I../include
LDFLAGS ?= -L../dist/lib -lcservice -luv -lllhttp -lcjson -lcurl -lpthread \
           -Wl,-rpath,'$$ORIGIN/../dist/lib'

all: order_service

order_service: order_service.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f order_service
