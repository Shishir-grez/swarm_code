CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread -I./include
SRCS   = src/main.c src/ring.c src/ethernet.c src/arp.c src/packet.c \
         src/conn.c src/tcp_orig.c src/udp_orig.c src/portfwd.c
TARGET = mini-vpnkit

all: $(TARGET) vm_client

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lrt

vm_client: tools/vm_client.c src/ring.c
	$(CC) $(CFLAGS) -o vm_client tools/vm_client.c src/ring.c -lrt

test_ring: src/ring.c tests/test_ring.c
	$(CC) $(CFLAGS) -o test_ring src/ring.c tests/test_ring.c -lrt

test_packet: src/packet.c src/ethernet.c tests/test_packet.c
	$(CC) $(CFLAGS) -o test_packet src/packet.c src/ethernet.c tests/test_packet.c

test_conn: src/conn.c tests/test_conn.c
	$(CC) $(CFLAGS) -o test_conn src/conn.c tests/test_conn.c

clean:
	rm -f $(TARGET) vm_client test_ring test_packet test_conn

.PHONY: all clean