#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>

#define ARP_REQUEST 1
#define ARP_REPLY   2

extern const uint8_t GATEWAY_MAC[6];
extern const uint8_t GATEWAY_IP[4];

int arp_handle(const uint8_t *frame, size_t frame_len,
               uint8_t *reply_buf, size_t reply_buf_len);

#endif