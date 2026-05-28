#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <linux/if_ether.h>
#include "conn.h"
#include "ring.h"
#include "packet.h"
#include "arp.h"
#include "udp_orig.h"

void udp_table_init(udp_conn_table_t *ut)
{
    memset(ut, 0, sizeof(*ut));
}

static udp_conn_t *udp_conn_create(udp_conn_table_t *ut,
                                    uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port,
                                    const uint8_t *guest_mac)
{
    for (int i = 0; i < MAX_UDP_CONNS; i++) {
        if (!ut->entries[i].active) {
            udp_conn_t *u = &ut->entries[i];
            memset(u, 0, sizeof(*u));
            u->src_ip = src_ip;
            u->src_port = src_port;
            u->dst_ip = dst_ip;
            u->dst_port = dst_port;
            memcpy(u->guest_mac, guest_mac, 6);
            u->last_active = time(NULL);
            return u;
        }
    }
    return NULL;
}

static void udp_conn_remove(udp_conn_table_t *ut, udp_conn_t *u)
{
    if (u->sock_fd >= 0) close(u->sock_fd);
    u->active = 0;
    u->sock_fd = -1;
}

void udp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring,
                udp_conn_table_t *ut)
{
    (void)ct;

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

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) return;

    int val = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    udp_conn_t *u = udp_conn_create(ut, src_ip, src_port, dst_ip, dst_port,
                                     eth->h_source);
    if (!u) { close(sock); return; }

    u->sock_fd = sock;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = dst_ip;
    dest.sin_port = htons(dst_port);

    ssize_t sent = sendto(sock, payload, payload_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        fprintf(stderr, "UDP: sendto failed: %s\n", strerror(errno));
        udp_conn_remove(ut, u);
        return;
    }

    printf("UDP: query sent to %s:%d, waiting for reply...\n",
           inet_ntoa(*(struct in_addr *)&dst_ip), dst_port);
}

void udp_poll(udp_conn_table_t *ut, ring_t *tx_ring)
{
    for (int i = MAX_UDP_CONNS - 1; i >= 0; i--) {
        udp_conn_t *u = &ut->entries[i];
        if (!u->active || u->sock_fd < 0) continue;

        if (difftime(time(NULL), u->last_active) > 5.0) {
            printf("UDP: timeout for %s:%d\n",
                   inet_ntoa(*(struct in_addr *)&u->src_ip), u->src_port);
            udp_conn_remove(ut, u);
            continue;
        }

        struct pollfd pfd = { .fd = u->sock_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) <= 0) continue;

        uint8_t reply_buf[4096];
        ssize_t n = recvfrom(u->sock_fd, reply_buf, sizeof(reply_buf), 0, NULL, NULL);
        if (n <= 0) continue;

        uint8_t reply_frame[1514];
        size_t rlen = build_udp_frame(
            reply_frame, sizeof(reply_frame),
            u->guest_mac, GATEWAY_MAC,
            u->dst_ip, u->src_ip,
            u->dst_port, u->src_port,
            reply_buf, (size_t)n
        );

        if (rlen > 0) {
            ring_write(tx_ring, reply_frame, (uint16_t)rlen);
            ring_notify(tx_ring);
            printf("UDP: reply forwarded (%zd bytes)\n", n);
        }

        udp_conn_remove(ut, u);
    }
}
