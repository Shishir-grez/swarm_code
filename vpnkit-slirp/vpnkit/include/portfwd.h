#ifndef PORTFWD_H
#define PORTFWD_H

#include "ring.h"
#include "conn.h"

#define MAX_PORT_FORWARDS 32

typedef struct {
    int active;
    int control_fd;
    int listen_fd;
    uint16_t host_port;
    uint32_t guest_ip;
    uint16_t guest_port;
} port_forward_t;

typedef struct {
    int listen_fd;
    char path[256];
    port_forward_t fwds[MAX_PORT_FORWARDS];
    ring_t *tx_ring;
    conn_table_t *ct;
} portfwd_server_t;

int  portfwd_init(portfwd_server_t *pf, const char *socket_path,
                  ring_t *tx_ring, conn_table_t *ct);
int  portfwd_control_fd(portfwd_server_t *pf);
void portfwd_handle_new(portfwd_server_t *pf);
void portfwd_poll(portfwd_server_t *pf);
void portfwd_destroy(portfwd_server_t *pf);

#endif