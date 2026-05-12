#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "conn.h"

void conn_table_init(conn_table_t *ct)
{
    memset(ct, 0, sizeof(*ct));
}

static uint32_t hash_5tuple(uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port)
{
    uint32_t h = 2166136261u;
    uint8_t *p = (uint8_t *)&src_ip;
    for (int i = 0; i < 4; i++) { h ^= p[i]; h *= 16777619u; }
    p = (uint8_t *)&src_port;
    for (int i = 0; i < 2; i++) { h ^= p[i]; h *= 16777619u; }
    p = (uint8_t *)&dst_ip;
    for (int i = 0; i < 4; i++) { h ^= p[i]; h *= 16777619u; }
    p = (uint8_t *)&dst_port;
    for (int i = 0; i < 2; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

conn_t *conn_lookup(conn_table_t *ct, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port)
{
    uint32_t idx = hash_5tuple(src_ip, src_port, dst_ip, dst_port) % MAX_CONNECTIONS;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        uint32_t slot = (idx + i) % MAX_CONNECTIONS;
        conn_t *c = &ct->entries[slot];
        if (c->state == CONN_FREE) return NULL;
        if (c->guest_src_ip == src_ip && c->guest_src_port == src_port &&
            c->guest_dst_ip == dst_ip && c->guest_dst_port == dst_port &&
            c->state != CONN_CLOSED)
            return c;
    }
    return NULL;
}

conn_t *conn_create(conn_table_t *ct, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port)
{
    uint32_t idx = hash_5tuple(src_ip, src_port, dst_ip, dst_port) % MAX_CONNECTIONS;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        uint32_t slot = (idx + i) % MAX_CONNECTIONS;
        conn_t *c = &ct->entries[slot];
        if (c->state == CONN_FREE || c->state == CONN_CLOSED) {
            memset(c, 0, sizeof(*c));
            c->guest_src_ip = src_ip;
            c->guest_src_port = src_port;
            c->guest_dst_ip = dst_ip;
            c->guest_dst_port = dst_port;
            c->host_fd = -1;
            c->last_active = time(NULL);
            return c;
        }
    }
    return NULL;
}

void conn_remove(conn_table_t *ct, conn_t *c)
{
    (void)ct;
    if (c->host_fd >= 0) close(c->host_fd);
    c->state = CONN_FREE;
    c->host_fd = -1;
}

void conn_reap_expired(conn_table_t *ct, int timeout_secs)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        conn_t *c = &ct->entries[i];
        if (c->state != CONN_FREE && c->state != CONN_CLOSED) {
            if (difftime(now, c->last_active) > timeout_secs)
                conn_remove(ct, c);
        }
    }
}