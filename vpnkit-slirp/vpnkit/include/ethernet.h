#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include <stddef.h>

#define ETH_FRAME_LEN 1514

int eth_parse(const uint8_t *frame, size_t frame_len,
             uint8_t **ether_type, uint8_t **payload, size_t *payload_len);

#endif