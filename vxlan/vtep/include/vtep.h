#ifndef VTEP_H
#define VTEP_H

#include <stdint.h>
#include <netinet/in.h>
#include "tap.h"
#include "socket.h"
#include "fdb.h"

#define VTEP_MAX_PEERS 32
#define VTEP_BUF_SIZE  2000

typedef struct {
    tap_t          tap;
    udp_socket_t   sock;
    fdb_t          fdb;
    uint32_t       vni;
    struct sockaddr_in peers[VTEP_MAX_PEERS];
    int            peer_count;
} vtep_t;

int  vtep_init(vtep_t *v, const char *tap_name,
               const char *tap_ip, uint32_t vni);
void vtep_add_peer(vtep_t *v, const char *peer_ip);
void vtep_run(vtep_t *v);     /* blocks forever */
void vtep_destroy(vtep_t *v);

#endif
