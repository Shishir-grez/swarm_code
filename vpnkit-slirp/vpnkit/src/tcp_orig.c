#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <linux/if_ether.h>
#include "conn.h"
#include "ring.h"
#include "tcp_orig.h"
#include "packet.h"
#include "arp.h"

static uint32_t generate_isn(void)
{
    uint32_t isn;
    FILE *f = fopen("/dev/urandom", "r");
    if (f) { fread(&isn, sizeof(isn), 1, f); fclose(f); }
    else isn = (uint32_t)time(NULL);
    return isn;
}

static void send_to_guest(ring_t *tx_ring, conn_t *c,
                        uint16_t flags, const uint8_t *payload, size_t plen)
{
    uint8_t frame[1514];
    uint32_t src_ip = c->guest_dst_ip;
    uint32_t dst_ip = c->guest_src_ip;
    uint16_t src_port = c->guest_dst_port;
    uint16_t dst_port = c->guest_src_port;

    size_t len = build_tcp_frame(
        frame, sizeof(frame),
        c->guest_mac, GATEWAY_MAC,
        src_ip, dst_ip,
        src_port, dst_port,
        c->snd_nxt, c->rcv_nxt,
        flags, payload, plen
    );

    if (len > 0) {
        ring_write(tx_ring, frame, (uint16_t)len);
        ring_notify(tx_ring);
    }
}

void tcp_handle(const uint8_t *frame, size_t frame_len,
              conn_table_t *ct, ring_t *tx_ring)
{
    if (frame_len < 14 + 20 + 20) return;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    const struct iphdr *ip = (const struct iphdr *)(frame + 14);

    if (ip->protocol != IPPROTO_TCP) return;

    const struct tcphdr *tcp = (const struct tcphdr *)((uint8_t *)ip + ip->ihl * 4);

    uint32_t src_ip = ip->saddr;
    uint32_t dst_ip = ip->daddr;
    uint16_t src_port = ntohs(tcp->source);
    uint16_t dst_port = ntohs(tcp->dest);
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack_seq);

    size_t ip_hdr_len = ip->ihl * 4;
    size_t tcp_hdr_len = tcp->doff * 4;
    size_t total_ip_len = ntohs(ip->tot_len);
    size_t payload_len = total_ip_len - ip_hdr_len - tcp_hdr_len;
    const uint8_t *payload = (const uint8_t *)tcp + tcp_hdr_len;

    if (tcp->syn && !tcp->ack) {
        conn_t *c = conn_create(ct, src_ip, src_port, dst_ip, dst_port);
        if (!c) { fprintf(stderr, "Connection table full\n"); return; }

        memcpy(c->guest_mac, eth->h_source, 6);
        c->guest_isn = seq;
        c->rcv_nxt = seq + 1;

        c->host_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (c->host_fd < 0) {
            c->my_isn = generate_isn();
            c->snd_nxt = c->my_isn;
            send_to_guest(tx_ring, c, TH_RST | TH_ACK, NULL, 0);
            conn_remove(ct, c);
            return;
        }

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = dst_ip;
        dest.sin_port = htons(dst_port);

        int ret = connect(c->host_fd, (struct sockaddr *)&dest, sizeof(dest));
        if (ret < 0 && errno != EINPROGRESS) {
            c->my_isn = generate_isn();
            c->snd_nxt = c->my_isn;
            send_to_guest(tx_ring, c, TH_RST | TH_ACK, NULL, 0);
            conn_remove(ct, c);
            return;
        }

        c->my_isn = generate_isn();
        c->snd_nxt = c->my_isn + 1;
        c->state = CONN_SYN_RCVD;

        uint8_t syn_ack_frame[1514];
        size_t sa_len = build_tcp_frame(
            syn_ack_frame, sizeof(syn_ack_frame),
            c->guest_mac, GATEWAY_MAC,
            dst_ip, src_ip,
            dst_port, src_port,
            c->my_isn, c->rcv_nxt,
            TH_SYN | TH_ACK, NULL, 0
        );

        ring_write(tx_ring, syn_ack_frame, (uint16_t)sa_len);
        ring_notify(tx_ring);

        printf("TCP: SYN -> connect() -> SYN-ACK\n");
        return;
    }

    conn_t *c = conn_lookup(ct, src_ip, src_port, dst_ip, dst_port);
    if (!c) return;

    c->last_active = time(NULL);

    if (tcp->ack && c->state == CONN_SYN_RCVD) {
        c->state = CONN_ESTABLISHED;
        printf("TCP: connection ESTABLISHED\n");
    }

    if (payload_len > 0 && c->state == CONN_ESTABLISHED) {
        ssize_t sent = send(c->host_fd, payload, payload_len, MSG_NOSIGNAL);
        if (sent < 0) {
            send_to_guest(tx_ring, c, TH_RST | TH_ACK, NULL, 0);
            conn_remove(ct, c);
            return;
        }
        c->rcv_nxt += (uint32_t)payload_len;
        send_to_guest(tx_ring, c, TH_ACK, NULL, 0);
    }

    if (tcp->fin) {
        c->rcv_nxt++;
        send_to_guest(tx_ring, c, TH_ACK | TH_FIN, NULL, 0);
        c->snd_nxt++;
        if (c->host_fd >= 0) { shutdown(c->host_fd, SHUT_WR); }
        c->state = CONN_FIN_WAIT;
    }

    if (tcp->rst) {
        conn_remove(ct, c);
    }
}

void tcp_poll_host(conn_table_t *ct, ring_t *tx_ring)
{
    uint8_t buf[4096];

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        conn_t *c = &ct->entries[i];
        if (c->state != CONN_ESTABLISHED || c->host_fd < 0)
            continue;

        ssize_t n = recv(c->host_fd, buf, sizeof(buf), MSG_DONTWAIT);

        if (n > 0) {
            send_to_guest(tx_ring, c, TH_ACK | TH_PUSH, buf, (size_t)n);
            c->snd_nxt += (uint32_t)n;
            c->last_active = time(NULL);
        }
        else if (n == 0) {
            send_to_guest(tx_ring, c, TH_ACK | TH_FIN, NULL, 0);
            c->snd_nxt++;
            c->state = CONN_FIN_WAIT;
        }
    }
}