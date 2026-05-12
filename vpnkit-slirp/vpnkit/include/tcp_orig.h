#ifndef TCP_ORIG_H
#define TCP_ORIG_H

#include "conn.h"
#include "ring.h"

void tcp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring);

void tcp_poll_host(conn_table_t *ct, ring_t *tx_ring);

#endif