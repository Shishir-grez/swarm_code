CC     = gcc
CFLAGS = -Wall -Wextra -g -I./include \
         $(shell pkg-config --cflags slirp glib-2.0 2>/dev/null || echo "")
LDFLAGS = $(shell pkg-config --libs slirp glib-2.0 2>/dev/null || echo "-lslirp -lglib-2.0")
SRCS   = src/main.c src/namespace.c src/tap.c src/slirp_wrap.c src/api.c
TARGET = mini-slirp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_tap: src/tap.c tests/test_tap.c
	$(CC) $(CFLAGS) -o test_tap src/tap.c tests/test_tap.c

test_api: src/api.c tests/test_api.c
	$(CC) $(CFLAGS) -o test_api src/api.c tests/test_api.c

clean:
	rm -f $(TARGET) test_tap test_api

.PHONY: all clean