#ifndef CONN_H
#define CONN_H

#include <stdint.h>
#include <netinet/in.h>
#include <time.h>

#define MAX_CONNECTIONS 256

typedef enum {
    CONN_FREE = 0,
    CONN_SYN_RCVD,
    CONN_ESTABLISHED,
    CONN_FIN_WAIT,
    CONN_CLOSED
} conn_state_t;

typedef struct {
    conn_state_t state;
    uint32_t guest_src_ip;
    uint16_t guest_src_port;
    uint32_t guest_dst_ip;
    uint16_t guest_dst_port;
    uint8_t  guest_mac[6];
    uint32_t guest_isn;
    uint32_t my_isn;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    int      host_fd;
    time_t   last_active;
} conn_t;

typedef struct {
    conn_t entries[MAX_CONNECTIONS];
} conn_table_t;

void    conn_table_init(conn_table_t *ct);
conn_t *conn_lookup(conn_table_t *ct, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port);
conn_t *conn_create(conn_table_t *ct, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port);
void    conn_remove(conn_table_t *ct, conn_t *c);
void    conn_reap_expired(conn_table_t *ct, int timeout_secs);

#endif