#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "fdb.h"

#define TEST(name, cond) do {                              \
    if (cond) printf("[PASS] %s\n", name);                 \
    else      printf("[FAIL] %s  (line %d)\n", name, __LINE__); \
} while(0)

static uint8_t mac1[6] = {0x00,0x11,0x22,0x33,0x44,0x55};
static uint8_t mac2[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};

static struct sockaddr_in make_vtep(const char *ip)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(4789);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

/* ── Functional tests ─────────────────────────────────────────────── */

void test_basic(void)
{
    fdb_t fdb; fdb_init(&fdb);
    struct sockaddr_in vtep1 = make_vtep("192.168.1.1");
    struct sockaddr_in result;

    TEST("miss on empty",   fdb_lookup(&fdb, mac1, &result) == -1);
    fdb_learn(&fdb, mac1, &vtep1);
    TEST("hit after learn", fdb_lookup(&fdb, mac1, &result) == 0);
    TEST("correct vtep",
         result.sin_addr.s_addr == vtep1.sin_addr.s_addr);

    fdb_destroy(&fdb);
}

void test_mac_move(void)
{
    fdb_t fdb; fdb_init(&fdb);
    struct sockaddr_in vtep1 = make_vtep("192.168.1.1");
    struct sockaddr_in vtep2 = make_vtep("192.168.1.2");
    struct sockaddr_in result;

    fdb_learn(&fdb, mac1, &vtep1);
    fdb_learn(&fdb, mac1, &vtep2);   /* MAC moved */
    fdb_lookup(&fdb, mac1, &result);
    TEST("mac move: vtep updated",
         result.sin_addr.s_addr == vtep2.sin_addr.s_addr);

    fdb_destroy(&fdb);
}

void test_two_macs(void)
{
    fdb_t fdb; fdb_init(&fdb);
    struct sockaddr_in vtep1 = make_vtep("192.168.1.1");
    struct sockaddr_in vtep2 = make_vtep("192.168.1.2");
    struct sockaddr_in result;

    fdb_learn(&fdb, mac1, &vtep1);
    fdb_learn(&fdb, mac2, &vtep2);

    fdb_lookup(&fdb, mac1, &result);
    TEST("mac1 → vtep1", result.sin_addr.s_addr == vtep1.sin_addr.s_addr);

    fdb_lookup(&fdb, mac2, &result);
    TEST("mac2 → vtep2", result.sin_addr.s_addr == vtep2.sin_addr.s_addr);

    fdb_destroy(&fdb);
}

/* ── Concurrent stress test ───────────────────────────────────────── */

#define NTHREADS 8
#define NITERS   5000

typedef struct { fdb_t *fdb; int id; } stress_arg_t;

static void *writer_fn(void *arg)
{
    stress_arg_t *a = (stress_arg_t *)arg;
    uint8_t mac[6]  = {0,0,0,0,0,(uint8_t)a->id};
    struct sockaddr_in vtep = make_vtep("10.0.0.1");
    for (int i = 0; i < NITERS; i++)
        fdb_learn(a->fdb, mac, &vtep);
    return NULL;
}

static void *reader_fn(void *arg)
{
    stress_arg_t *a = (stress_arg_t *)arg;
    uint8_t mac[6]  = {0,0,0,0,0,(uint8_t)a->id};
    struct sockaddr_in result;
    for (int i = 0; i < NITERS; i++)
        fdb_lookup(a->fdb, mac, &result);
    return NULL;
}

void test_concurrent(void)
{
    fdb_t fdb; fdb_init(&fdb);

    pthread_t    wt[NTHREADS], rt[NTHREADS];
    stress_arg_t args[NTHREADS];

    for (int i = 0; i < NTHREADS; i++) {
        args[i].fdb = &fdb;
        args[i].id  = i;
        pthread_create(&wt[i], NULL, writer_fn, &args[i]);
        pthread_create(&rt[i], NULL, reader_fn, &args[i]);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(wt[i], NULL);
        pthread_join(rt[i], NULL);
    }

    TEST("concurrent stress: no crash", 1);
    fdb_destroy(&fdb);
}

int main(void)
{
    printf("=== FDB Tests ===\n");
    test_basic();
    test_mac_move();
    test_two_macs();
    test_concurrent();
    printf("Done.\n");
    return 0;
}
