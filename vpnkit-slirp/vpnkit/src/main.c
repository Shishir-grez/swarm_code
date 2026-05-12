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
    fprintf(stderr, "Usage: %s [--portfwd PATH] [--socket PATH] [RX_SHM TX_SHM]\n", prog);
    exit(1);
}

// Send fd via Unix socket using SCM_RIGHTS
static int send_fd(int sock, int fd)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int))];
    char dummy = '!';

    iov.iov_base = &dummy;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    struct cmsghdr *cmsg = (struct cmsghdr *)buf;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    *(int *)CMSG_DATA(cmsg) = fd;

    msg.msg_control = cmsg;
    msg.msg_controllen = cmsg->cmsg_len;

    return sendmsg(sock, &msg, 0);
}

// Receive fd via Unix socket using SCM_RIGHTS
static int recv_fd(int sock)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int))];
    char dummy;

    iov.iov_base = &dummy;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    if (recvmsg(sock, &msg, 0) < 0)
        return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        return *(int *)CMSG_DATA(cmsg);

    return -1;
}

// Wait for vm_client to connect, then share eventfds and shm names
// Returns 0 on success (should continue running), -1 on error
static int wait_for_client(int *client_fd_out, int rx_efd, int tx_efd,
                           const char *rx_shm, const char *tx_shm)
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

    printf("Waiting for VM client to connect on %s...\n", sock_path);

    struct sockaddr_un client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) { perror("accept"); close(listen_fd); return -1; }

    close(listen_fd);
    unlink(sock_path);

    // Debug: show eventfds before sending
    printf("Server: rx_ring eventfd=%d, tx_ring eventfd=%d\n",
           ring_event_fd(&g_rx_ring), ring_event_fd(&g_tx_ring));

    // Send eventfds and shm names to client
    // Protocol: [rx_efd][tx_efd][rx_path_len][rx_path][tx_path_len][tx_path]
    if (send_fd(client_fd, rx_efd) < 0) { perror("send_fd rx"); return -1; }
    if (send_fd(client_fd, tx_efd) < 0) { perror("send_fd tx"); return -1; }

    // Send shm names as strings so client can attach
    uint32_t rx_len = (uint32_t)strlen(rx_shm);
    uint32_t tx_len = (uint32_t)strlen(tx_shm);
    if (write(client_fd, &rx_len, 4) != 4 ||
        write(client_fd, rx_shm, rx_len) != (ssize_t)rx_len ||
        write(client_fd, &tx_len, 4) != 4 ||
        write(client_fd, tx_shm, tx_len) != (ssize_t)tx_len) {
        perror("write shm names"); return -1;
    }

    *client_fd_out = client_fd;
    printf("Client connected, eventfds shared.\n");
    return 0;
}

int main(int argc, char *argv[])
{
    const char *portfwd_path = NULL;
    const char *rx_shm = "/vpnkit-rx";
    const char *tx_shm = "/vpnkit-tx";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--portfwd") && i + 1 < argc)
            portfwd_path = argv[++i];
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            argv[++i]; /* accepted but unused in this server mode */
        else if (rx_shm == NULL)
            rx_shm = argv[i];
        else if (tx_shm == NULL)
            tx_shm = argv[i];
        else
            usage(argv[0]);
    }

    conn_table_init(&g_conn_table);

    // Create shared memory rings (owned by server)
    if (ring_create(&g_rx_ring, rx_shm) < 0) {
        fprintf(stderr, "Failed to create rx ring\n");
        return 1;
    }
    if (ring_create(&g_tx_ring, tx_shm) < 0) {
        fprintf(stderr, "Failed to create tx ring\n");
        return 1;
    }

    // Wait for vm_client to connect, share eventfds
    int client_fd = -1;
    if (wait_for_client(&client_fd, ring_event_fd(&g_rx_ring),
                        ring_event_fd(&g_tx_ring), rx_shm, tx_shm) < 0) {
        return 1;
    }
    close(client_fd); // client fd no longer needed after fd sharing

    if (portfwd_path) {
        portfwd_init(&g_portfwd, portfwd_path, &g_tx_ring, &g_conn_table);
    }

    printf("mini-vpnkit running. RX=%s TX=%s (pid %d)\n", rx_shm, tx_shm, getpid());

    uint8_t frame[2048];

    while (1) {
        struct pollfd pfds[4];
        int nfds = 0;

        // Poll rx ring: data from guest
        pfds[nfds].fd = ring_event_fd(&g_rx_ring);
        pfds[nfds++].events = POLLIN;

        // Poll tx ring: notifications from our own sends (used below)
        // Note: we also poll it to detect when tx is readable for portfwd clients
        pfds[nfds].fd = ring_event_fd(&g_tx_ring);
        pfds[nfds++].events = POLLIN;

        if (portfwd_path) {
            pfds[nfds].fd = portfwd_control_fd(&g_portfwd);
            pfds[nfds++].events = POLLIN;
        }

        int ret = poll(pfds, nfds, 100);
        if (ret <= 0) {
            conn_reap_expired(&g_conn_table, 60);
            continue;
        }

        // Incoming frames from guest (rx ring)
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
                        // ARP reply goes to TX ring (where guest reads)
                        ring_write(&g_tx_ring, reply, (uint16_t)rlen);
                        ring_notify(&g_tx_ring);
                    }
                }
                else if (type == 0x0800) {
                    const uint8_t *ip = payload;
                    if (ip[0] >> 4 == 4) {
                        uint8_t proto = ip[9];
                        if (proto == 6) {
                            tcp_handle(frame, n, &g_conn_table, &g_tx_ring);
                        }
                        else if (proto == 17) {
                            udp_handle(frame, n, &g_conn_table, &g_tx_ring);
                        }
                    }
                }
            }
        }

        // Flush any pending TCP host traffic
        tcp_poll_host(&g_conn_table, &g_tx_ring);

        if (portfwd_path && (pfds[2].revents & POLLIN)) {
            portfwd_handle_new(&g_portfwd);
        }
        if (portfwd_path) {
            portfwd_poll(&g_portfwd);
        }
    }

    if (portfwd_path) portfwd_destroy(&g_portfwd);
    ring_destroy(&g_rx_ring, rx_shm);
    ring_destroy(&g_tx_ring, tx_shm);
    return 0;
}