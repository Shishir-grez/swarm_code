#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "packet.h"
#include "arp.h"

uint16_t ip_checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t tcp_checksum(const struct iphdr *ip, const struct tcphdr *tcp, size_t tcp_len)
{
    struct { uint32_t src; uint32_t dst; uint8_t zero; uint8_t proto; uint16_t len; } pseudo;
    pseudo.src = ip->saddr;
    pseudo.dst = ip->daddr;
    pseudo.zero = 0;
    pseudo.proto = IPPROTO_TCP;
    pseudo.len = htons((uint16_t)tcp_len);

    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) sum += p[i];

    p = (const uint16_t *)tcp;
    size_t remaining = tcp_len;
    while (remaining > 1) { sum += *p++; remaining -= 2; }
    if (remaining == 1) sum += *(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t udp_checksum(const struct iphdr *ip, const struct udphdr *udp, size_t udp_len)
{
    struct { uint32_t src; uint32_t dst; uint8_t zero; uint8_t proto; uint16_t len; } pseudo;
    pseudo.src = ip->saddr;
    pseudo.dst = ip->daddr;
    pseudo.zero = 0;
    pseudo.proto = IPPROTO_UDP;
    pseudo.len = htons((uint16_t)udp_len);

    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) sum += p[i];

    p = (const uint16_t *)udp;
    size_t remaining = udp_len;
    while (remaining > 1) { sum += *p++; remaining -= 2; }
    if (remaining == 1) sum += *(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

size_t build_tcp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    uint32_t seq, uint32_t ack_seq,
    uint16_t flags,
    const uint8_t *payload, size_t payload_len)
{
    size_t eth_len = 14;
    size_t ip_len = 20;
    size_t tcp_hdr_len = (flags & TH_SYN) ? 24 : 20;
    size_t total = eth_len + ip_len + tcp_hdr_len + payload_len;
    if (total > buf_len) return 0;

    memset(buf, 0, total);

    struct ethhdr *eth = (struct ethhdr *)buf;
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr *ip2 = (struct iphdr *)(buf + eth_len);
    ip2->version = 4;
    ip2->ihl = 5;
    ip2->tot_len = htons((uint16_t)(ip_len + tcp_hdr_len + payload_len));
    ip2->ttl = 64;
    ip2->protocol = IPPROTO_TCP;
    ip2->saddr = src_ip;
    ip2->daddr = dst_ip;
    ip2->check = ip_checksum(ip2, ip_len);

    struct tcphdr *tcp = (struct tcphdr *)(buf + eth_len + ip_len);
    tcp->source = htons(src_port);
    tcp->dest = htons(dst_port);
    tcp->seq = htonl(seq);
    tcp->ack_seq = htonl(ack_seq);
    tcp->doff = tcp_hdr_len / 4;
    tcp->syn = (flags & TH_SYN) ? 1 : 0;
    tcp->ack = (flags & TH_ACK) ? 1 : 0;
    tcp->fin = (flags & TH_FIN) ? 1 : 0;
    tcp->rst = (flags & TH_RST) ? 1 : 0;
    tcp->psh = (flags & TH_PSH) ? 1 : 0;
    tcp->window = htons(65535);

    if (flags & TH_SYN) {
        uint8_t *opts = (uint8_t *)tcp + 20;
        opts[0] = 2; opts[1] = 4;
        uint16_t mss = htons(1460);
        memcpy(&opts[2], &mss, 2);
    }

    if (payload && payload_len > 0)
        memcpy(buf + eth_len + ip_len + tcp_hdr_len, payload, payload_len);

    tcp->check = 0;
    tcp->check = tcp_checksum(ip2, tcp, tcp_hdr_len + payload_len);

    return total;
}

size_t build_udp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const uint8_t *payload, size_t payload_len)
{
    size_t eth_len = 14;
    size_t ip_len = 20;
    size_t udp_hdr_len = 8;
    size_t total = eth_len + ip_len + udp_hdr_len + payload_len;
    if (total > buf_len) return 0;
    memset(buf, 0, total);

    struct ethhdr *eth = (struct ethhdr *)buf;
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr *ip2 = (struct iphdr *)(buf + eth_len);
    ip2->version = 4;
    ip2->ihl = 5;
    ip2->tot_len = htons((uint16_t)(ip_len + udp_hdr_len + payload_len));
    ip2->ttl = 64;
    ip2->protocol = IPPROTO_UDP;
    ip2->saddr = src_ip;
    ip2->daddr = dst_ip;
    ip2->check = ip_checksum(ip2, ip_len);

    struct udphdr *udp = (struct udphdr *)(buf + eth_len + ip_len);
    udp->source = htons(src_port);
    udp->dest = htons(dst_port);
    udp->len = htons((uint16_t)(udp_hdr_len + payload_len));

    if (payload && payload_len > 0)
        memcpy(buf + eth_len + ip_len + udp_hdr_len, payload, payload_len);

    udp->check = 0;
    udp->check = udp_checksum(ip2, udp, udp_hdr_len + payload_len);

    return total;
}