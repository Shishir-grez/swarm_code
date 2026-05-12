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

// Receive a file descriptor via Unix socket using SCM_RIGHTS
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

// Read exact N bytes from socket
static int recv_exact(int sock, void *buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        ssize_t n = read(sock, (char *)buf + received, len - received);
        if (n <= 0) return -1;
        received += (size_t)n;
    }
    return 0;
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

    uint8_t *tcp = frame + 34;
    tcp[0] = (src_port >> 8) & 0xFF;
    tcp[1] = src_port & 0xFF;
    tcp[4] = (seq >> 24) & 0xFF;
    tcp[5] = (seq >> 16) & 0xFF;
    tcp[6] = (seq >> 8) & 0xFF;
    tcp[7] = seq & 0xFF;
    tcp[12] = 0x50; tcp[13] = 0x02;
    tcp[14] = 0xFF; tcp[15] = 0xFF;
    tcp[8] = 0x02;
}

int main(int argc, char *argv[])
{
    srand(time(NULL));

    printf("VM client starting...\n");

    // Connect to mini-vpnkit's Unix socket
    const char *sock_path = "/tmp/vpnkit.sock";
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(sock); return 1;
    }
    printf("Connected to mini-vpnkit\n");

    // Receive eventfds from server via SCM_RIGHTS
    int rx_efd = recv_fd(sock);
    int tx_efd = recv_fd(sock);
    if (rx_efd < 0 || tx_efd < 0) {
        fprintf(stderr, "Failed to receive eventfds\n");
        return 1;
    }
    printf("Received eventfds: rx=%d tx=%d\n", rx_efd, tx_efd);

    // Receive shared memory names
    uint32_t rx_len, tx_len;
    char rx_shm[256], tx_shm[256];
    if (recv_exact(sock, &rx_len, 4) < 0 ||
        recv_exact(sock, rx_shm, rx_len) < 0 ||
        recv_exact(sock, &tx_len, 4) < 0 ||
        recv_exact(sock, tx_shm, tx_len) < 0) {
        fprintf(stderr, "Failed to receive shm names\n");
        return 1;
    }
    rx_shm[rx_len] = '\0';
    tx_shm[tx_len] = '\0';
    printf("RX ring: %s, TX ring: %s\n", rx_shm, tx_shm);

    close(sock); // No longer needed after receiving everything

    // Attach to shared memory rings with the server's eventfds
    // vm_client READS from rx_ring (= /vpnkit-tx, where server writes replies)
    // vm_client WRITES to tx_ring (= /vpnkit-rx, where server reads requests)
    ring_attach(&g_rx_ring, tx_shm, rx_efd);  // read server replies here
    ring_attach(&g_tx_ring, rx_shm, tx_efd);  // write requests here

    uint8_t src_mac[6] = {0x02, rand()&0xFF, rand()&0xFF, rand()&0xFF, rand()&0xFF, 1};
    uint32_t src_ip = (10 << 24) | (0 << 16) | (2 << 8) | (100);

    uint8_t frame[128];
    send_arp_request(frame, src_mac, (uint8_t *)GATEWAY_IP);
    ring_write(&g_tx_ring, frame, 60);
    ring_notify(&g_tx_ring);
    printf("Sent ARP request (notified via eventfd %d)\n", tx_efd);

    usleep(100000);

    while (1) {
        uint8_t buf[2048];
        int n = ring_read(&g_rx_ring, buf, sizeof(buf));
        if (n > 0) {
            struct ethhdr *eth = (struct ethhdr *)buf;
            printf("Received frame: type=0x%04x\n", ntohs(eth->h_proto));
        }
        sleep(1);
    }

    ring_destroy(&g_rx_ring, tx_shm);
    ring_destroy(&g_tx_ring, rx_shm);
    return 0;
}