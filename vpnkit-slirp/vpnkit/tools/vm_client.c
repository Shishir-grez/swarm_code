#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <linux/if_ether.h>
#include "ring.h"

static const uint8_t GATEWAY_IP[4] = {10, 0, 2, 2};

static ring_t g_rx_ring;
static ring_t g_tx_ring;

/*
 * Ring direction map:
 *   /vpnkit-rx: server READS here, client WRITES here (g_tx_ring)
 *   /vpnkit-tx: server WRITES here, client READS here (g_rx_ring)
 *
 * Server sends FDs one at a time:
 *   FD #1 = rx ring's eventfd (used by client when writing to rx ring)
 *   FD #2 = tx ring's eventfd (used by server when writing to tx ring)
 *
 * Client receives them in same order and attaches accordingly.
 */

/* Receive one FD from server via SCM_RIGHTS on a connected Unix socket */
static int recv_one_fd(int sock)
{
    char dummy;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

    /*
     * Use CMSG_SPACE(sizeof(int)*2) for buffer to handle kernel
     * alignment padding — some kernels use 16-byte alignment for
     * the cmsg buffer even for a single int.
     */
    char cbuf[CMSG_SPACE(sizeof(int) * 2)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    if (recvmsg(sock, &msg, 0) < 0) {
        perror("recvmsg");
        return -1;
    }

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "ERROR: no SCM_RIGHTS in message\n");
        return -1;
    }

    int *fds = (int *)CMSG_DATA(cm);
    return fds[0];
}

static void send_arp_request(uint8_t *frame, uint8_t *src_mac, uint8_t *target_ip)
{
    memset(frame, 0, 60);
    struct ethhdr *eth = (struct ethhdr *)frame;
    memset(eth->h_dest, 0xFF, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_ARP);

    uint8_t *arp = frame + 14;
    arp[0] = 0; arp[1] = 1;
    arp[2] = 8; arp[3] = 0;
    arp[4] = 6; arp[5] = 4;
    arp[6] = 0; arp[7] = 1;
    memcpy(arp + 8, src_mac, 6);
    memset(arp + 14, 0, 4);
    memset(arp + 18, 0, 6);
    memcpy(arp + 24, target_ip, 4);
}

static void send_tcp_syn(uint8_t *frame, uint8_t *src_mac, uint8_t *dst_mac,
                       uint32_t src_ip, uint32_t dst_ip,
                       uint16_t src_port, uint32_t seq)
{
    memset(frame, 0, 54);
    struct ethhdr *eth = (struct ethhdr *)frame;
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[1] = 0; ip[2] = 0; ip[3] = 0;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 6; ip[10] = 0; ip[11] = 0;
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    uint16_t ip_tot = htons(40);
    memcpy(ip + 2, &ip_tot, 2);

    uint8_t *tcp = frame + 34;
    tcp[0] = (src_port >> 8) & 0xFF;
    tcp[1] = src_port & 0xFF;
    tcp[2] = 0; tcp[3] = 80;
    tcp[4] = (seq >> 24) & 0xFF;
    tcp[5] = (seq >> 16) & 0xFF;
    tcp[6] = (seq >> 8) & 0xFF;
    tcp[7] = seq & 0xFF;
    tcp[12] = 0x50; tcp[13] = 0x02;
    tcp[14] = 0xFF; tcp[15] = 0xFF;
}

int main(int argc, char *argv[])
{
    srand((unsigned)time(NULL));

    printf("VM client starting...\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/vpnkit.sock", sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(sock); return 1;
    }
    printf("Connected to mini-vpnkit\n");

    /* Receive two eventfds from server (order: rx_efd then tx_efd) */
    int rx_efd = recv_one_fd(sock);
    int tx_efd = recv_one_fd(sock);
    printf("Received eventfds: rx=%d tx=%d\n", rx_efd, tx_efd);

    if (rx_efd < 0 || tx_efd < 0) {
        fprintf(stderr, "Failed to receive eventfds\n");
        close(sock); return 1;
    }

    close(sock);

    /*
     * Attach to rings:
     *   g_tx_ring → /vpnkit-rx (client writes requests here, eventfd=rx_efd)
     *   g_rx_ring → /vpnkit-tx (client reads replies here, eventfd=tx_efd)
     */
    ring_attach(&g_tx_ring, "/vpnkit-rx", rx_efd);
    ring_attach(&g_rx_ring, "/vpnkit-tx", tx_efd);

    printf("g_tx_ring → /vpnkit-rx (efd=%d), g_rx_ring → /vpnkit-tx (efd=%d)\n",
           ring_event_fd(&g_tx_ring), ring_event_fd(&g_rx_ring));

    uint8_t src_mac[6] = {0x02, rand()&0xFF, rand()&0xFF, rand()&0xFF, rand()&0xFF, 1};
    uint32_t src_ip = (10 << 24) | (0 << 16) | (2 << 8) | (100);

    uint8_t frame[128];
    send_arp_request(frame, src_mac, (uint8_t *)GATEWAY_IP);
    ring_write(&g_tx_ring, frame, 60);
    ring_notify(&g_tx_ring);
    printf("Sent ARP request (notified via /vpnkit-rx eventfd %d)\n",
           ring_event_fd(&g_tx_ring));

    usleep(200000);

    int replies = 0;
    while (replies < 3) {
        uint8_t buf[2048];
        int n = ring_read(&g_rx_ring, buf, sizeof(buf));
        if (n > 0) {
            replies++;
            struct ethhdr *eth = (struct ethhdr *)buf;
            printf("Reply #%d: type=0x%04x\n", replies, ntohs(eth->h_proto));
        }
        sleep(1);
    }

    printf("Test complete, %d replies received\n", replies);

    ring_destroy(&g_rx_ring, "/vpnkit-tx");
    ring_destroy(&g_tx_ring, "/vpnkit-rx");
    return 0;
}