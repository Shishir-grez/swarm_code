#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "ring.h"
#include "conn.h"
#include "tcp_orig.h"
#include "udp_orig.h"
#include "portfwd.h"
#include "ethernet.h"
#include "arp.h"

static ring_t g_rx_ring;
static ring_t g_tx_ring;
static conn_table_t g_conn_table;
static portfwd_server_t g_portfwd;

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--portfwd PATH]\n", prog);
    exit(1);
}

/* Send one fd via Unix socket using SCM_RIGHTS */
static int send_fd(int sock, int fd)
{
    char dummy = '!';
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

    /*
     * CMSG_SPACE(2*sizeof(int)) ensures room for 2 FDs even with alignment padding.
     * We actually only send 1 FD per call, but use the larger buffer to be safe.
     */
    char cbuf[CMSG_SPACE(2 * sizeof(int))];
    struct cmsghdr *cm = (struct cmsghdr *)cbuf;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    *(int *)CMSG_DATA(cm) = fd;

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cm;
    msg.msg_controllen = cm->cmsg_len;

    return sendmsg(sock, &msg, 0);
}

/* Wait for client, send both FDs, then shm names */
static int wait_for_client(int *client_fd_out, int rx_efd, int tx_efd)
{
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return -1; }

    const char *sock_path = "/tmp/vpnkit.sock";
    unlink(sock_path);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return -1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen"); close(listen_fd); return -1;
    }

    printf("Waiting for VM client on %s...\n", sock_path);

    struct sockaddr_un client_addr;
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
    if (client_fd < 0) { perror("accept"); close(listen_fd); return -1; }
    close(listen_fd);
    unlink(sock_path);

    /* Send FDs: rx_efd first, then tx_efd — both as separate SCM_RIGHTS msgs */
    if (send_fd(client_fd, rx_efd) < 0) { perror("send rx_efd"); return -1; }
    if (send_fd(client_fd, tx_efd) < 0) { perror("send tx_efd"); return -1; }

    *client_fd_out = client_fd;
    return 0;
}

int main(int argc, char *argv[])
{
    const char *portfwd_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--portfwd") && i + 1 < argc)
            portfwd_path = argv[++i];
        else
            usage(argv[0]);
    }

    conn_table_init(&g_conn_table);

    if (ring_create(&g_rx_ring, "/vpnkit-rx") < 0) {
        fprintf(stderr, "Failed to create rx ring\n");
        return 1;
    }
    if (ring_create(&g_tx_ring, "/vpnkit-tx") < 0) {
        fprintf(stderr, "Failed to create tx ring\n");
        return 1;
    }

    int client_fd = -1;
    if (wait_for_client(&client_fd,
                        ring_event_fd(&g_rx_ring),
                        ring_event_fd(&g_tx_ring)) < 0) {
        return 1;
    }
    close(client_fd);

    if (portfwd_path)
        portfwd_init(&g_portfwd, portfwd_path, &g_tx_ring, &g_conn_table);

    printf("mini-vpnkit running. RX=/vpnkit-rx TX=/vpnkit-tx\n");

    uint8_t frame[2048];
    while (1) {
        struct pollfd pfds[4];
        int nfds = 0;
        pfds[nfds].fd = ring_event_fd(&g_rx_ring);
        pfds[nfds++].events = POLLIN;
        if (portfwd_path) {
            pfds[nfds].fd = ring_event_fd(&g_tx_ring);
            pfds[nfds++].events = POLLIN;
            pfds[nfds].fd = portfwd_control_fd(&g_portfwd);
            pfds[nfds++].events = POLLIN;
        }
        int ret = poll(pfds, nfds, 100);
        if (ret <= 0) {
            conn_reap_expired(&g_conn_table, 60);
            continue;
        }
        if (pfds[0].revents & POLLIN) {
            int n = ring_read(&g_rx_ring, frame, sizeof(frame));
            if (n > 0) {
                uint8_t *eth_type, *payload;
                size_t payload_len;
                eth_parse(frame, n, &eth_type, &payload, &payload_len);
                uint16_t type = (eth_type[0] << 8) | eth_type[1];
                if (type == 0x0806) {
                    uint8_t reply[128];
                    int rlen = arp_handle(frame, n, reply, sizeof(reply));
                    if (rlen > 0) {
                        ring_write(&g_tx_ring, reply, (uint16_t)rlen);
                        ring_notify(&g_tx_ring);
                    }
                }
                else if (type == 0x0800) {
                    const uint8_t *ip = payload;
                    if (ip[0] >> 4 == 4) {
                        uint8_t proto = ip[9];
                        if (proto == 6)
                            tcp_handle(frame, n, &g_conn_table, &g_tx_ring);
                        else if (proto == 17)
                            udp_handle(frame, n, &g_conn_table, &g_tx_ring);
                    }
                }
            }
        }
        tcp_poll_host(&g_conn_table, &g_tx_ring);
        if (portfwd_path && (pfds[2].revents & POLLIN))
            portfwd_handle_new(&g_portfwd);
        if (portfwd_path)
            portfwd_poll(&g_portfwd);
    }

    if (portfwd_path) portfwd_destroy(&g_portfwd);
    ring_destroy(&g_rx_ring, "/vpnkit-rx");
    ring_destroy(&g_tx_ring, "/vpnkit-tx");
    return 0;
}