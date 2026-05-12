#ifndef FDB_H
#define FDB_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <netinet/in.h>

#define FDB_NUM_SHARDS   256
#define FDB_MAX_ENTRIES  128    /* per shard — 32,768 total */
#define FDB_DEFAULT_TTL  300    /* seconds (5 min) */
#define ETH_ADDR_LEN       6

typedef struct {
    uint8_t            mac[ETH_ADDR_LEN];
    struct sockaddr_in vtep;
    time_t             learned_at;
    int                valid;    /* 0 = empty slot */
} fdb_entry_t;

typedef struct {
    pthread_rwlock_t lock;
    fdb_entry_t      entries[FDB_MAX_ENTRIES];
    int              count;
} fdb_shard_t;

typedef struct {
    fdb_shard_t shards[FDB_NUM_SHARDS];
    int         ttl_seconds;
    pthread_t   age_thread;
    volatile int running;
} fdb_t;

void fdb_init(fdb_t *fdb);
void fdb_destroy(fdb_t *fdb);

/* Learn: mac is reachable via vtep. Handles MAC moves automatically. */
void fdb_learn(fdb_t *fdb, const uint8_t *mac,
               const struct sockaddr_in *vtep);

/* Lookup: returns 0 + fills vtep on hit. Returns -1 on miss or expired. */
int  fdb_lookup(fdb_t *fdb, const uint8_t *mac,
                struct sockaddr_in *vtep);

#endif
