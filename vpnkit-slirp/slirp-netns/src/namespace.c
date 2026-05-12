#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "namespace.h"

static int send_fd(int sock, int fd)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[1] = {'F'};
    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    iov.iov_base = buf;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0) >= 0 ? 0 : -1;
}

static int recv_fd(int sock)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[1];
    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    iov.iov_base = buf;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (recvmsg(sock, &msg, 0) < 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) return -1;

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

static int enter_ns(pid_t pid, const char *type, int nstype)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, type);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return -1;
    }

    if (setns(fd, nstype) < 0) {
        fprintf(stderr, "setns(%s): %s\n", type, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int create_tap(const char *name)
{
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { perror("open /dev/net/tun"); return -1; }

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

static int configure_tap(const char *name)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "ip link set lo up");
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "ip addr add 10.0.2.100/24 dev %s", name);
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "ip link set %s up", name);
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "ip route add default via 10.0.2.2 dev %s", name);
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "cp /etc/resolv.conf /tmp/resolv.conf.tmp && "
            "echo 'nameserver 10.0.2.3' > /tmp/resolv.conf.tmp && "
            "mount --bind /tmp/resolv.conf.tmp /etc/resolv.conf");
    system(cmd);

    return 0;
}

int ns_enter(pid_t target_pid, const char *tap_name, ns_result_t *result)
{
    int ready_sv[2];
    int fd_sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ready_sv) < 0) {
        perror("socketpair ready"); return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd_sv) < 0) {
        perror("socketpair fd"); return -1;
    }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return -1; }

    if (child == 0) {
        close(ready_sv[0]);
        close(fd_sv[0]);

        if (enter_ns(target_pid, "user", CLONE_NEWUSER) < 0)
            _exit(1);

        if (enter_ns(target_pid, "net", CLONE_NEWNET) < 0)
            _exit(1);

        int tap_fd = create_tap(tap_name);
        if (tap_fd < 0) _exit(1);

        if (configure_tap(tap_name) < 0) _exit(1);

        if (send_fd(fd_sv[1], tap_fd) < 0) _exit(1);
        close(fd_sv[1]);
        close(tap_fd);

        char one = '1';
        write(ready_sv[1], &one, 1);

        for (;;) pause();
    }

    close(ready_sv[1]);
    close(fd_sv[1]);

    char buf;
    if (read(ready_sv[0], &buf, 1) != 1 || buf != '1') {
        fprintf(stderr, "Child failed to become ready\n");
        return -1;
    }

    int tap_fd = recv_fd(fd_sv[0]);
    close(fd_sv[0]);
    if (tap_fd < 0) {
        fprintf(stderr, "Failed to receive TAP fd\n");
        return -1;
    }

    result->tap_fd = tap_fd;
    result->ready_fd = ready_sv[0];
    result->exit_fd = ready_sv[0];
    result->child_pid = child;

    printf("Entered namespace of PID %d, TAP fd=%d\n", target_pid, tap_fd);
    return 0;
}