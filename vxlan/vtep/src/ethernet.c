#include <stdio.h>
#include <string.h>
#include "ethernet.h"

const uint8_t ETH_BROADCAST[ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

struct ethhdr *eth_parse(uint8_t *buf, size_t len)
{
    if (len < ETH_HLEN) {
        fprintf(stderr, "eth_parse: frame too short (%zu bytes)\n", len);
        return NULL;
    }
    return (struct ethhdr *)buf;   /* cast — no copy */
}

int eth_is_broadcast(const uint8_t *mac)
{
    return memcmp(mac, ETH_BROADCAST, ETH_ALEN) == 0;
}

int eth_mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, ETH_ALEN) == 0;
}

void eth_mac_copy(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, ETH_ALEN);
}

void eth_print_mac(const uint8_t *mac)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
