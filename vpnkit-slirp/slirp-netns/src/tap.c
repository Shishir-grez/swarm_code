#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include "tap.h"

int tap_create(const char *name)
{
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("ioctl TUNSETIFF");
        close(fd);
        return -1;
    }

    return fd;
}

int tap_set_ip(const char *name, const char *ip, const char *mask)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev %s", ip, mask, name);
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "ip link set %s up", name);
    if (system(cmd) != 0) return -1;

    return 0;
}