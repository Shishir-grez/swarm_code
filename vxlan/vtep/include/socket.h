#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include <sys/types.h>

typedef struct {
    int fd;
    int port;
} udp_socket_t;

int     udp_socket_create(udp_socket_t *sock, int port);
ssize_t udp_socket_send(const udp_socket_t *sock,
                         const uint8_t *pkt, size_t len,
                         const struct sockaddr_in *dst);
ssize_t udp_socket_recv(const udp_socket_t *sock,
                         uint8_t *buf, size_t buf_len,
                         struct sockaddr_in *src);
void    udp_socket_close(udp_socket_t *sock);

#endif
