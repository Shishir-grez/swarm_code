#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "socket.h"

int udp_socket_create(udp_socket_t *sock, int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    sock->fd   = fd;
    sock->port = port;
    printf("UDP socket bound on port %d\n", port);
    return 0;
}

ssize_t udp_socket_send(const udp_socket_t *sock,
                         const uint8_t *pkt, size_t len,
                         const struct sockaddr_in *dst)
{
    ssize_t n = sendto(sock->fd, pkt, len, 0,
                       (const struct sockaddr *)dst, sizeof(*dst));
    if (n < 0) perror("sendto");
    return n;
}

ssize_t udp_socket_recv(const udp_socket_t *sock,
                         uint8_t *buf, size_t buf_len,
                         struct sockaddr_in *src)
{
    socklen_t src_len = sizeof(*src);
    ssize_t n = recvfrom(sock->fd, buf, buf_len, 0,
                          (struct sockaddr *)src, &src_len);
    if (n < 0) perror("recvfrom");
    return n;
}

void udp_socket_close(udp_socket_t *sock)
{
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
}
