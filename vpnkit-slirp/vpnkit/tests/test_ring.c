#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "ring.h"

#define TEST(name, cond) do { \
    if (cond) printf("[PASS] %s\n", name); \
    else      printf("[FAIL] %s (line %d)\n", name, __LINE__); \
} while(0)

void test_basic(void)
{
    ring_t ring;
    ring_create(&ring, "/test-ring-basic");

    uint8_t frame[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t buf[64];

    TEST("read empty returns 0", ring_read(&ring, buf, sizeof(buf)) == 0);

    TEST("write succeeds", ring_write(&ring, frame, 4) == 0);
    int n = ring_read(&ring, buf, sizeof(buf));
    TEST("read returns 4", n == 4);
    TEST("data matches", memcmp(buf, frame, 4) == 0);

    TEST("read again returns 0", ring_read(&ring, buf, sizeof(buf)) == 0);

    ring_destroy(&ring, "/test-ring-basic");
}

void test_multiple_frames(void)
{
    ring_t ring;
    ring_create(&ring, "/test-ring-multi");

    uint8_t f1[] = {0x01, 0x02};
    uint8_t f2[] = {0x03, 0x04, 0x05};
    uint8_t f3[] = {0x06};
    uint8_t buf[64];

    ring_write(&ring, f1, 2);
    ring_write(&ring, f2, 3);
    ring_write(&ring, f3, 1);

    int n1 = ring_read(&ring, buf, sizeof(buf));
    TEST("frame 1 len", n1 == 2);
    TEST("frame 1 data", buf[0] == 0x01 && buf[1] == 0x02);

    int n2 = ring_read(&ring, buf, sizeof(buf));
    TEST("frame 2 len", n2 == 3);

    int n3 = ring_read(&ring, buf, sizeof(buf));
    TEST("frame 3 len", n3 == 1);

    ring_destroy(&ring, "/test-ring-multi");
}

#define STRESS_COUNT 10000

static void *stress_writer(void *arg)
{
    ring_t *ring = (ring_t *)arg;
    uint8_t frame[8];
    for (int i = 0; i < STRESS_COUNT; i++) {
        memcpy(frame, &i, sizeof(i));
        while (ring_write(ring, frame, sizeof(int)) < 0)
            usleep(1);
    }
    return NULL;
}

void test_stress(void)
{
    ring_t ring;
    ring_create(&ring, "/test-ring-stress");

    pthread_t writer;
    pthread_create(&writer, NULL, stress_writer, &ring);

    uint8_t buf[8];
    int received = 0;
    int last_seq = -1;
    int ordered = 1;

    while (received < STRESS_COUNT) {
        int n = ring_read(&ring, buf, sizeof(buf));
        if (n <= 0) { usleep(1); continue; }

        int seq;
        memcpy(&seq, buf, sizeof(int));
        if (seq != last_seq + 1) ordered = 0;
        last_seq = seq;
        received++;
    }

    pthread_join(writer, NULL);

    TEST("stress: all received", received == STRESS_COUNT);
    TEST("stress: in order", ordered);

    ring_destroy(&ring, "/test-ring-stress");
}

int main(void)
{
    printf("=== Ring Buffer Tests ===\n");
    test_basic();
    test_multiple_frames();
    test_stress();
    return 0;
}