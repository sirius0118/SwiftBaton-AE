CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pthread
LDLIBS += -lrdmacm -libverbs -pthread

all: rsocket_relay

rsocket_relay: rsocket_relay.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f rsocket_relay

.PHONY: all clean
