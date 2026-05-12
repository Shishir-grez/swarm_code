CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread -I./include
SRCS   = src/main.c src/vxlan.c src/ethernet.c src/fdb.c src/tap.c src/socket.c src/vtep.c
TARGET = vtep

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_vxlan: src/vxlan.c src/ethernet.c tests/test_vxlan.c
	$(CC) $(CFLAGS) -o test_vxlan src/vxlan.c src/ethernet.c tests/test_vxlan.c

test_fdb: src/fdb.c tests/test_fdb.c
	$(CC) $(CFLAGS) -o test_fdb src/fdb.c tests/test_fdb.c

clean:
	rm -f $(TARGET) test_vxlan test_fdb

.PHONY: all clean
