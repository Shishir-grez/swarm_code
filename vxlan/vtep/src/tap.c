#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include "tap.h"

int tap_create(const char *name, tap_t *tap)
{
    struct ifreq ifr;

    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("ioctl TUNSETIFF");
        close(fd);
        return -1;
    }

    tap->fd = fd;
    strncpy(tap->name, ifr.ifr_name, sizeof(tap->name) - 1);
    printf("Created TAP: %s (fd=%d)\n", tap->name, tap->fd);
    return 0;
}

int tap_bring_up(const tap_t *tap, const char *ip_cidr)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "ip addr add %s dev %s", ip_cidr, tap->name);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed: %s\n", cmd);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "ip link set %s up", tap->name);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed: %s\n", cmd);
        return -1;
    }

    printf("Interface %s up, IP %s\n", tap->name, ip_cidr);
    return 0;
}

ssize_t tap_read(const tap_t *tap, uint8_t *buf, size_t buf_len)
{
    ssize_t n = read(tap->fd, buf, buf_len);
    if (n < 0) perror("tap_read");
    return n;
}

ssize_t tap_write(const tap_t *tap, const uint8_t *frame, size_t len)
{
    ssize_t n = write(tap->fd, frame, len);
    if (n < 0) perror("tap_write");
    return n;
}

void tap_destroy(tap_t *tap)
{
    if (tap->fd >= 0) {
        close(tap->fd);
        tap->fd = -1;
    }
}
