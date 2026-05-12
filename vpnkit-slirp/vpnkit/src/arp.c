#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include "arp.h"

const uint8_t GATEWAY_MAC[6] = {0xCA, 0xFE, 0x00, 0x00, 0x00, 0x01};
const uint8_t GATEWAY_IP[4]  = {10, 0, 2, 2};

int arp_handle(const uint8_t *frame, size_t frame_len,
               uint8_t *reply_buf, size_t reply_buf_len)
{
    if (frame_len < sizeof(struct ethhdr) + 28)
        return -1;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    if (ntohs(eth->h_proto) != ETH_P_ARP)
        return 0;

    const uint8_t *arp = frame + sizeof(struct ethhdr);
    uint16_t oper = arp[6] << 8 | arp[7];
    uint16_t ptype = arp[2] << 8 | arp[3];

    if (oper != ARP_REQUEST || ptype != ETH_P_IP)
        return 0;

    if (memcmp(arp + 24, GATEWAY_IP, 4) != 0)
        return 0;

    if (reply_buf_len < sizeof(struct ethhdr) + 28)
        return -1;

    struct ethhdr *reth = (struct ethhdr *)reply_buf;
    memcpy(reth->h_dest, arp + 8, 6);
    memcpy(reth->h_source, GATEWAY_MAC, 6);
    reth->h_proto = htons(ETH_P_ARP);

    uint8_t *rarp = reply_buf + sizeof(struct ethhdr);
    rarp[0] = 0; rarp[1] = 1;
    rarp[2] = 8; rarp[3] = 0;
    rarp[4] = 6; rarp[5] = 4;
    rarp[6] = 0; rarp[7] = 2;
    memcpy(rarp + 8, GATEWAY_MAC, 6);
    memcpy(rarp + 14, GATEWAY_IP, 4);
    memcpy(rarp + 18, arp + 8, 6);
    memcpy(rarp + 24, arp + 14, 4);

    printf("ARP: reply sent\n");
    return sizeof(struct ethhdr) + 28;
}