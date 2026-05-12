#ifndef UDP_ORIG_H
#define UDP_ORIG_H

#include "conn.h"
#include "ring.h"

void udp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring);

#endif