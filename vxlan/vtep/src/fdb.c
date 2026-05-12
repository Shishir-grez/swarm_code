#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fdb.h"

/* ── Private helpers ──────────────────────────────────────────────── */

static int shard_of(const uint8_t *mac)
{
    return (int)mac[5];   /* 0–255, direct array index */
}

static int find_entry(fdb_shard_t *shard, const uint8_t *mac)
{
    for (int i = 0; i < FDB_MAX_ENTRIES; i++) {
        if (shard->entries[i].valid &&
            memcmp(shard->entries[i].mac, mac, ETH_ADDR_LEN) == 0)
            return i;
    }
    return -1;
}

static int find_empty(fdb_shard_t *shard)
{
    for (int i = 0; i < FDB_MAX_ENTRIES; i++)
        if (!shard->entries[i].valid)
            return i;
    return -1;
}

/* ── Age thread: evicts expired entries every 30 seconds ─────────── */

static void *age_thread_fn(void *arg)
{
    fdb_t *fdb = (fdb_t *)arg;

    while (fdb->running) {
        sleep(30);
        time_t now = time(NULL);

        for (int s = 0; s < FDB_NUM_SHARDS; s++) {
            fdb_shard_t *shard = &fdb->shards[s];
            pthread_rwlock_wrlock(&shard->lock);

            for (int i = 0; i < FDB_MAX_ENTRIES; i++) {
                fdb_entry_t *e = &shard->entries[i];
                if (e->valid &&
                    difftime(now, e->learned_at) > fdb->ttl_seconds) {
                    e->valid = 0;
                    shard->count--;
                }
            }
            pthread_rwlock_unlock(&shard->lock);
        }
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────── */

void fdb_init(fdb_t *fdb)
{
    memset(fdb, 0, sizeof(*fdb));
    fdb->ttl_seconds = FDB_DEFAULT_TTL;
    fdb->running     = 1;

    for (int i = 0; i < FDB_NUM_SHARDS; i++)
        pthread_rwlock_init(&fdb->shards[i].lock, NULL);

    pthread_create(&fdb->age_thread, NULL, age_thread_fn, fdb);
}

void fdb_destroy(fdb_t *fdb)
{
    fdb->running = 0;
    pthread_join(fdb->age_thread, NULL);

    for (int i = 0; i < FDB_NUM_SHARDS; i++)
        pthread_rwlock_destroy(&fdb->shards[i].lock);
}

void fdb_learn(fdb_t *fdb, const uint8_t *mac,
               const struct sockaddr_in *vtep)
{
    int s = shard_of(mac);
    fdb_shard_t *shard = &fdb->shards[s];   /* pointer, never copy */

    pthread_rwlock_wrlock(&shard->lock);

    int idx = find_entry(shard, mac);

    if (idx < 0) {
        idx = find_empty(shard);
        if (idx < 0) {
            /* Shard full — evict oldest entry */
            time_t oldest = time(NULL);
            int    oldest_idx = 0;
            for (int i = 0; i < FDB_MAX_ENTRIES; i++) {
                if (shard->entries[i].learned_at < oldest) {
                    oldest     = shard->entries[i].learned_at;
                    oldest_idx = i;
                }
            }
            idx = oldest_idx;
        } else {
            shard->count++;
        }
    }

    /* Overwrite: covers new entry, MAC move, and TTL refresh */
    memcpy(shard->entries[idx].mac, mac, ETH_ADDR_LEN);
    memcpy(&shard->entries[idx].vtep, vtep, sizeof(*vtep));
    shard->entries[idx].learned_at = time(NULL);
    shard->entries[idx].valid      = 1;

    pthread_rwlock_unlock(&shard->lock);
}

int fdb_lookup(fdb_t *fdb, const uint8_t *mac,
               struct sockaddr_in *vtep)
{
    int s = shard_of(mac);
    fdb_shard_t *shard = &fdb->shards[s];

    pthread_rwlock_rdlock(&shard->lock);

    int result = -1;
    int idx    = find_entry(shard, mac);

    if (idx >= 0) {
        fdb_entry_t *e = &shard->entries[idx];
        if (difftime(time(NULL), e->learned_at) <= fdb->ttl_seconds) {
            memcpy(vtep, &e->vtep, sizeof(*vtep));  /* copy under lock */
            result = 0;
        }
        /* Expired: return -1, let age_thread handle deletion */
    }

    pthread_rwlock_unlock(&shard->lock);
    return result;
}
