#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include "ethernet.h"

int eth_parse(const uint8_t *frame, size_t frame_len,
             uint8_t **ether_type, uint8_t **payload, size_t *payload_len)
{
    if (frame_len < sizeof(struct ethhdr))
        return -1;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    *ether_type = (uint8_t *)&eth->h_proto;
    *payload = (uint8_t *)(frame + sizeof(struct ethhdr));
    *payload_len = frame_len - sizeof(struct ethhdr));

    return 0;
}