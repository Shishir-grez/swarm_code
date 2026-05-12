#ifndef TAP_H
#define TAP_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define TAP_MAX_FRAME 1500

typedef struct {
    int  fd;
    char name[16];
} tap_t;

/* Creates TAP interface. Requires sudo / CAP_NET_ADMIN. */
int tap_create(const char *name, tap_t *tap);

/* Assigns ip_cidr (e.g. "10.0.0.1/24") and brings interface UP. */
int tap_bring_up(const tap_t *tap, const char *ip_cidr);

/* Blocks until one Ethernet frame is available. */
ssize_t tap_read(const tap_t *tap, uint8_t *buf, size_t buf_len);

/* Injects one Ethernet frame into the kernel network stack. */
ssize_t tap_write(const tap_t *tap, const uint8_t *frame, size_t len);

void tap_destroy(tap_t *tap);

#endif
