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

static int recv_one_fd(int sock)
{
    char dummy;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
    char cbuf[CMSG_SPACE(sizeof(int) * 2)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    if (recvmsg(sock, &msg, 0) < 0) { perror("recvmsg"); return -1; }

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "ERROR: no SCM_RIGHTS\n");
        return -1;
    }
    return *(int *)CMSG_DATA(cm);
}

static uint16_t checksum16(const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *tcp, size_t tcp_len)
{
    struct { uint32_t src; uint32_t dst; uint8_t zero; uint8_t proto; uint16_t len; } pseudo;
    pseudo.src = src_ip;
    pseudo.dst = dst_ip;
    pseudo.zero = 0;
    pseudo.proto = 6;
    pseudo.len = htons((uint16_t)tcp_len);

    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) sum += p[i];

    p = (const uint16_t *)tcp;
    size_t rem = tcp_len;
    while (rem > 1) { sum += *p++; rem -= 2; }
    if (rem) sum += *(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
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

static int wait_arp_reply(uint8_t *gateway_mac)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        uint8_t buf[2048];
        int n = ring_read(&g_rx_ring, buf, sizeof(buf));
        if (n > 0) {
            struct ethhdr *eth = (struct ethhdr *)buf;
            if (ntohs(eth->h_proto) == ETH_P_ARP) {
                uint8_t *arp = buf + 14;
                if (arp[6] == 0 && arp[7] == 2) {
                    memcpy(gateway_mac, arp + 8, 6);
                    printf("ARP: gateway MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
                           gateway_mac[0], gateway_mac[1], gateway_mac[2],
                           gateway_mac[3], gateway_mac[4], gateway_mac[5]);
                    return 0;
                }
            }
        }
        usleep(200000);
    }
    return -1;
}

static size_t build_tcp_frame(uint8_t *frame, size_t sizeof_frame,
                              uint8_t *src_mac, uint8_t *dst_mac,
                              uint32_t src_ip, uint32_t dst_ip,
                              uint16_t src_port, uint16_t dst_port,
                              uint32_t seq, uint32_t ack,
                              uint16_t flags,
                              const uint8_t *payload, size_t plen)
{
    size_t tcp_hdr_len = (flags & 0x02) ? 24 : 20;
    size_t ip_len = 20 + tcp_hdr_len + plen;
    size_t total = 14 + ip_len;

    memset(frame, 0, total);

    struct ethhdr *eth = (struct ethhdr *)frame;
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (ip_len >> 8) & 0xFF; ip[3] = ip_len & 0xFF;
    ip[8] = 64; ip[9] = 6;
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    ip[10] = 0; ip[11] = 0;
    uint16_t ipc = checksum16(ip, 20);
    memcpy(ip + 10, &ipc, 2);

    uint8_t *tcp = frame + 34;
    tcp[0] = (src_port >> 8) & 0xFF; tcp[1] = src_port & 0xFF;
    tcp[2] = (dst_port >> 8) & 0xFF; tcp[3] = dst_port & 0xFF;
    tcp[4] = (seq >> 24) & 0xFF; tcp[5] = (seq >> 16) & 0xFF;
    tcp[6] = (seq >> 8) & 0xFF; tcp[7] = seq & 0xFF;
    tcp[8] = (ack >> 24) & 0xFF; tcp[9] = (ack >> 16) & 0xFF;
    tcp[10] = (ack >> 8) & 0xFF; tcp[11] = ack & 0xFF;
    tcp[12] = (tcp_hdr_len / 4) << 4;
    tcp[13] = flags & 0xFF;
    tcp[14] = 0xFF; tcp[15] = 0xFF;

    if (flags & 0x02) {
        uint8_t *opts = tcp + 20;
        opts[0] = 2; opts[1] = 4;
        uint16_t mss = htons(1460);
        memcpy(&opts[2], &mss, 2);
    }

    if (payload && plen > 0)
        memcpy(tcp + tcp_hdr_len, payload, plen);

    uint16_t tc = tcp_checksum(src_ip, dst_ip, tcp, tcp_hdr_len + plen);
    memcpy(tcp + 16, &tc, 2);

    return total;
}

static int wait_frame(uint8_t *buf, size_t buflen, uint16_t expected_proto, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int n = ring_read(&g_rx_ring, buf, buflen);
        if (n > 0) {
            struct ethhdr *eth = (struct ethhdr *)buf;
            if (ntohs(eth->h_proto) == expected_proto)
                return n;
        }
        usleep(10000);
        elapsed += 10;
    }
    return -1;
}

static int test_tcp(uint8_t *src_mac, uint32_t src_ip, uint8_t *dst_mac,
                    uint32_t dst_ip, uint16_t dst_port)
{
    uint16_t src_port = 40000 + (rand() % 10000);
    uint32_t seq = 1000;
    uint32_t ack = 0;
    uint8_t frame[1514];
    uint8_t buf[2048];

    printf("\n=== TCP test: %s:%d ===\n", inet_ntoa(*(struct in_addr *)&dst_ip), dst_port);

    /* SYN */
    printf("1. Sending SYN...\n");
    size_t len = build_tcp_frame(frame, sizeof(frame),
                                 src_mac, dst_mac, src_ip, dst_ip,
                                 src_port, dst_port, seq, 0,
                                 0x02, NULL, 0);
    ring_write(&g_tx_ring, frame, (uint16_t)len);
    ring_notify(&g_tx_ring);
    seq++;

    /* SYN-ACK */
    printf("2. Waiting for SYN-ACK...\n");
    int n = wait_frame(buf, sizeof(buf), ETH_P_IP, 5000);
    if (n < 0) { printf("   TIMEOUT\n"); return -1; }

    uint8_t *ip = buf + 14;
    uint8_t *tcp = ip + 20;
    uint16_t sport = (tcp[0] << 8) | tcp[1];
    uint32_t srv_seq = ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16)
                     | ((uint32_t)tcp[6] << 8) | tcp[7];
    uint32_t srv_ack = ((uint32_t)tcp[8] << 24) | ((uint32_t)tcp[9] << 16)
                     | ((uint32_t)tcp[10] << 8) | tcp[11];
    uint8_t flags = tcp[13];

    printf("   sport=0x%04x flags=0x%02x seq=0x%08x ack=0x%08x\n",
           sport, flags, srv_seq, srv_ack);

    if ((flags & 0x12) != 0x12) { printf("   Not SYN-ACK (flags=0x%02x)\n", flags); return -1; }
    ack = srv_seq + 1;
    printf("   SYN-ACK received, ack=0x%08x\n", ack);

    /* ACK */
    printf("3. Sending ACK...\n");
    len = build_tcp_frame(frame, sizeof(frame),
                          src_mac, dst_mac, src_ip, dst_ip,
                          src_port, dst_port, seq, ack,
                          0x10, NULL, 0);
    ring_write(&g_tx_ring, frame, (uint16_t)len);
    ring_notify(&g_tx_ring);
    printf("   Connection ESTABLISHED\n");

    /* HTTP GET */
    const char *http = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    size_t http_len = strlen(http);
    printf("4. Sending HTTP GET (%zu bytes)...\n", http_len);
    len = build_tcp_frame(frame, sizeof(frame),
                          src_mac, dst_mac, src_ip, dst_ip,
                          src_port, dst_port, seq, ack,
                          0x18, (const uint8_t *)http, http_len);
    ring_write(&g_tx_ring, frame, (uint16_t)len);
    ring_notify(&g_tx_ring);
    seq += http_len;

    /* Read response */
    printf("5. Waiting for response...\n");
    int total = 0;
    for (int i = 0; i < 50; i++) {
        n = ring_read(&g_rx_ring, buf, sizeof(buf));
        if (n > 0) {
            struct ethhdr *eth = (struct ethhdr *)buf;
            if (ntohs(eth->h_proto) != ETH_P_IP) continue;

            uint8_t *tip = buf + 14;
            uint8_t *ttcp = tip + 20;
            uint8_t tflags = ttcp[13];
            size_t tcp_hlen = (ttcp[12] >> 4) * 4;
            size_t ip_totlen = (tip[2] << 8) | tip[3];
            size_t payload_len = ip_totlen - 20 - tcp_hlen;

            if (tflags & 0x01) {
                printf("   FIN received\n");
                uint32_t fin_seq = ((uint32_t)ttcp[4] << 24) | ((uint32_t)ttcp[5] << 16)
                                 | ((uint32_t)ttcp[6] << 8) | ttcp[7];
                uint32_t fin_ack = ((uint32_t)ttcp[8] << 24) | ((uint32_t)ttcp[9] << 16)
                                 | ((uint32_t)ttcp[10] << 8) | ttcp[11];
                len = build_tcp_frame(frame, sizeof(frame),
                                      src_mac, dst_mac, src_ip, dst_ip,
                                      src_port, dst_port, seq, fin_seq + 1,
                                      0x10, NULL, 0);
                ring_write(&g_tx_ring, frame, (uint16_t)len);
                ring_notify(&g_tx_ring);
                break;
            }

            if (payload_len > 0) {
                uint8_t *payload = ttcp + tcp_hlen;
                if (total == 0) {
                    printf("   Got %zu bytes of data:\n", payload_len);
                    size_t show = payload_len < 200 ? payload_len : 200;
                    printf("   ---\n   %.*s\n   ---\n", (int)show, payload);
                }
                total += payload_len;
                uint32_t fin_seq = ((uint32_t)ttcp[4] << 24) | ((uint32_t)ttcp[5] << 16)
                                 | ((uint32_t)ttcp[6] << 8) | ttcp[7];
                len = build_tcp_frame(frame, sizeof(frame),
                                      src_mac, dst_mac, src_ip, dst_ip,
                                      src_port, dst_port, seq, fin_seq + payload_len,
                                      0x10, NULL, 0);
                ring_write(&g_tx_ring, frame, (uint16_t)len);
                ring_notify(&g_tx_ring);
            }
        }
        usleep(50000);
    }

    printf("   Total received: %d bytes\n", total);
    printf("=== TCP test %s ===\n", total > 0 ? "PASSED" : "FAILED");
    return total > 0 ? 0 : -1;
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

    int rx_efd = recv_one_fd(sock);
    int tx_efd = recv_one_fd(sock);
    printf("Received eventfds: rx=%d tx=%d\n", rx_efd, tx_efd);

    if (rx_efd < 0 || tx_efd < 0) {
        fprintf(stderr, "Failed to receive eventfds\n");
        close(sock); return 1;
    }
    close(sock);

    ring_attach(&g_tx_ring, "/vpnkit-rx", rx_efd);
    ring_attach(&g_rx_ring, "/vpnkit-tx", tx_efd);

    uint8_t src_mac[6] = {0x02, rand()&0xFF, rand()&0xFF, rand()&0xFF, rand()&0xFF, 1};
    uint32_t src_ip = htonl((10 << 24) | (0 << 16) | (2 << 8) | 100);

    /* ARP: resolve gateway MAC */
    printf("\n--- ARP ---\n");
    uint8_t frame[1514];
    uint8_t gateway_mac[6];

    send_arp_request(frame, src_mac, (uint8_t *)GATEWAY_IP);
    ring_write(&g_tx_ring, frame, 60);
    ring_notify(&g_tx_ring);
    printf("Sent ARP request\n");

    if (wait_arp_reply(gateway_mac) < 0) {
        fprintf(stderr, "ARP failed\n");
        ring_destroy(&g_rx_ring, "/vpnkit-tx");
        ring_destroy(&g_tx_ring, "/vpnkit-rx");
        return 1;
    }

    /* TCP: connect to localhost:8080 (simple HTTP server) */
    uint32_t dst_ip = inet_addr("127.0.0.1");
    test_tcp(src_mac, src_ip, gateway_mac, dst_ip, 8080);

    ring_destroy(&g_rx_ring, "/vpnkit-tx");
    ring_destroy(&g_tx_ring, "/vpnkit-rx");
    return 0;
}
