#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include <linux/if_ether.h>   /* struct ethhdr, ETH_ALEN=6, ETH_HLEN=14 */

#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV6  0x86DD

extern const uint8_t ETH_BROADCAST[ETH_ALEN];

/* Returns pointer INTO buf (no copy). NULL if too short. */
struct ethhdr *eth_parse(uint8_t *buf, size_t len);

int  eth_is_broadcast(const uint8_t *mac);
int  eth_mac_equal(const uint8_t *a, const uint8_t *b);
void eth_mac_copy(uint8_t *dst, const uint8_t *src);
void eth_print_mac(const uint8_t *mac);

#endif
