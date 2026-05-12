#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <linux/if_ether.h>
#include "conn.h"
#include "ring.h"
#include "packet.h"
#include "arp.h"
#include "udp_orig.h"

void udp_handle(const uint8_t *frame, size_t frame_len,
              conn_table_t *ct, ring_t *tx_ring)
{
    if (frame_len < 14 + 20 + 8) return;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    const struct iphdr *ip = (const struct iphdr *)(frame + 14);
    const struct udphdr *udp = (const struct udphdr *)((uint8_t *)ip + ip->ihl * 4);

    uint32_t src_ip = ip->saddr;
    uint32_t dst_ip = ip->daddr;
    uint16_t src_port = ntohs(udp->source);
    uint16_t dst_port = ntohs(udp->dest);
    size_t payload_len = ntohs(udp->len) - 8;
    const uint8_t *payload = (const uint8_t *)udp + 8;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = dst_ip;
    dest.sin_port = htons(dst_port);

    sendto(sock, payload, payload_len, 0, (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t reply_buf[4096];
    ssize_t n = recvfrom(sock, reply_buf, sizeof(reply_buf), 0, NULL, NULL);
    close(sock);

    if (n <= 0) return;

    uint8_t reply_frame[1514];
    uint8_t guest_mac[6];
    memcpy(guest_mac, eth->h_source, 6);

    size_t rlen = build_udp_frame(
        reply_frame, sizeof(reply_frame),
        guest_mac, GATEWAY_MAC,
        dst_ip, src_ip,
        dst_port, src_port,
        reply_buf, (size_t)n
    );

    if (rlen > 0) {
        ring_write(tx_ring, reply_frame, (uint16_t)rlen);
        ring_notify(tx_ring);
    }
}