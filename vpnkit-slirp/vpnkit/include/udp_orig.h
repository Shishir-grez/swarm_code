#ifndef UDP_ORIG_H
#define UDP_ORIG_H

#include <stdint.h>
#include <time.h>
#include "conn.h"
#include "ring.h"

#define MAX_UDP_CONNS 64

typedef struct {
    int      active;
    int      sock_fd;
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint8_t  guest_mac[6];
    time_t   last_active;
} udp_conn_t;

typedef struct {
    udp_conn_t entries[MAX_UDP_CONNS];
} udp_conn_table_t;

void udp_table_init(udp_conn_table_t *ut);

void udp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring,
                udp_conn_table_t *ut);

void udp_poll(udp_conn_table_t *ut, ring_t *tx_ring);

#endif
