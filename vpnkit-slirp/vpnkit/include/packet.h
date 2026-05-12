#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

uint16_t ip_checksum(const void *data, size_t len);
uint16_t tcp_checksum(const struct iphdr *ip, const struct tcphdr *tcp, size_t tcp_len);
uint16_t udp_checksum(const struct iphdr *ip, const struct udphdr *udp, size_t udp_len);

size_t build_tcp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    uint32_t seq, uint32_t ack_seq,
    uint16_t flags,
    const uint8_t *payload, size_t payload_len
);

size_t build_udp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const uint8_t *payload, size_t payload_len
);

#endif