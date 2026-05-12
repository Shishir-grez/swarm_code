#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "vtep.h"
#include "vxlan.h"
#include "ethernet.h"

typedef struct { vtep_t *vtep; } thread_arg_t;

/* ── Inbound: UDP → decapsulate → TAP ───────────────────────────── */

static void *inbound_loop(void *arg)
{
    vtep_t  *v = ((thread_arg_t *)arg)->vtep;
    uint8_t  buf[VTEP_BUF_SIZE];

    printf("Inbound loop started\n");

    while (1) {
        struct sockaddr_in src_addr;
        ssize_t n = udp_socket_recv(&v->sock, buf, sizeof(buf), &src_addr);
        if (n < 0) continue;

        uint32_t vni      = 0;
        size_t   frame_len = 0;
        const uint8_t *frame = vxlan_decapsulate(buf, (size_t)n,
                                                  &vni, &frame_len);
        if (!frame) continue;

        /* MAC learning: inner src MAC arrived from src_addr VTEP */
        struct ethhdr *eth = eth_parse((uint8_t *)frame, frame_len);
        if (eth)
            fdb_learn(&v->fdb, eth->h_source, &src_addr);

        tap_write(&v->tap, frame, frame_len);
    }
    return NULL;
}

/* ── Outbound: TAP → encapsulate → UDP ──────────────────────────── */

static void outbound_loop(vtep_t *v)
{
    uint8_t frame_buf[VTEP_BUF_SIZE];

    printf("Outbound loop started\n");

    while (1) {
        ssize_t n = tap_read(&v->tap, frame_buf, sizeof(frame_buf));
        if (n < 0) continue;

        struct ethhdr *eth = eth_parse(frame_buf, (size_t)n);
        if (!eth) continue;

        size_t   pkt_len = 0;
        uint8_t *pkt = vxlan_encapsulate(frame_buf, (size_t)n,
                                          v->vni, &pkt_len);
        if (!pkt) continue;

        struct sockaddr_in dst;
        if (fdb_lookup(&v->fdb, eth->h_dest, &dst) == 0) {
            /* Known MAC: unicast to one VTEP */
            udp_socket_send(&v->sock, pkt, pkt_len, &dst);
        } else {
            /* Unknown / broadcast: flood all peers (Head-End Replication) */
            for (int i = 0; i < v->peer_count; i++)
                udp_socket_send(&v->sock, pkt, pkt_len, &v->peers[i]);
        }

        free(pkt);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

int vtep_init(vtep_t *v, const char *tap_name,
              const char *tap_ip, uint32_t vni)
{
    memset(v, 0, sizeof(*v));
    v->vni = vni;

    if (tap_create(tap_name, &v->tap) < 0)           return -1;
    if (tap_bring_up(&v->tap, tap_ip) < 0)           return -1;
    if (udp_socket_create(&v->sock, VXLAN_PORT) < 0) return -1;

    fdb_init(&v->fdb);
    return 0;
}

void vtep_add_peer(vtep_t *v, const char *peer_ip)
{
    if (v->peer_count >= VTEP_MAX_PEERS) {
        fprintf(stderr, "vtep_add_peer: max peers reached\n");
        return;
    }
    struct sockaddr_in *p = &v->peers[v->peer_count++];
    memset(p, 0, sizeof(*p));
    p->sin_family = AF_INET;
    p->sin_port   = htons(VXLAN_PORT);
    inet_pton(AF_INET, peer_ip, &p->sin_addr);
    printf("Added peer: %s\n", peer_ip);
}

void vtep_run(vtep_t *v)
{
    pthread_t     tid;
    thread_arg_t  arg = { .vtep = v };

    pthread_create(&tid, NULL, inbound_loop, &arg);
    outbound_loop(v);       /* main thread blocks here */
    pthread_join(tid, NULL);
}

void vtep_destroy(vtep_t *v)
{
    tap_destroy(&v->tap);
    udp_socket_close(&v->sock);
    fdb_destroy(&v->fdb);
}
