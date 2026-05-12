#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include "ring.h"

int ring_create(ring_t *ring, const char *shm_name)
{
    memset(ring, 0, sizeof(*ring));
    ring->map_size = RING_SHM_SIZE;

    shm_unlink(shm_name);
    ring->shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (ring->shm_fd < 0) { perror("shm_open create"); return -1; }

    if (ftruncate(ring->shm_fd, ring->map_size) < 0) {
        perror("ftruncate"); return -1;
    }

    ring->hdr = mmap(NULL, ring->map_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     ring->shm_fd, 0);
    if (ring->hdr == MAP_FAILED) { perror("mmap"); return -1; }

    ring->hdr->head = 0;
    ring->hdr->tail = 0;

    ring->event_fd = eventfd(0, EFD_NONBLOCK);
    if (ring->event_fd < 0) { perror("eventfd"); return -1; }

    return 0;
}

int ring_attach(ring_t *ring, const char *shm_name, int event_fd)
{
    memset(ring, 0, sizeof(*ring));
    ring->map_size = RING_SHM_SIZE;
    ring->event_fd = event_fd;

    ring->shm_fd = shm_open(shm_name, O_RDWR, 0);
    if (ring->shm_fd < 0) { perror("shm_open attach"); return -1; }

    ring->hdr = mmap(NULL, ring->map_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     ring->shm_fd, 0);
    if (ring->hdr == MAP_FAILED) { perror("mmap attach"); return -1; }

    return 0;
}

static inline uint32_t ring_space(ring_header_t *h)
{
    uint32_t head = __atomic_load_n(&h->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
    return (tail - head - 1) & RING_MASK;
}

static inline uint32_t ring_used(ring_header_t *h)
{
    uint32_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&h->tail, __ATOMIC_RELAXED);
    return (head - tail) & RING_MASK;
}

int ring_write(ring_t *ring, const uint8_t *frame, uint16_t frame_len)
{
    uint32_t needed = 2 + frame_len;

    if (ring_space(ring->hdr) < needed)
        return -1;

    uint32_t head = ring->hdr->head;

    ring->hdr->data[head & RING_MASK] = (uint8_t)(frame_len & 0xFF);
    ring->hdr->data[(head + 1) & RING_MASK] = (uint8_t)(frame_len >> 8);

    for (uint32_t i = 0; i < frame_len; i++)
        ring->hdr->data[(head + 2 + i) & RING_MASK] = frame[i];

    __atomic_store_n(&ring->hdr->head, (head + needed) & RING_MASK,
                     __ATOMIC_RELEASE);

    return 0;
}

int ring_read(ring_t *ring, uint8_t *buf, size_t buf_len)
{
    if (ring_used(ring->hdr) < 2)
        return 0;

    uint32_t tail = ring->hdr->tail;

    uint16_t frame_len = ring->hdr->data[tail & RING_MASK]
                       | ((uint16_t)ring->hdr->data[(tail + 1) & RING_MASK] << 8);

    if (frame_len == 0 || frame_len > buf_len)
        return -1;

    if (ring_used(ring->hdr) < (uint32_t)(2 + frame_len))
        return 0;

    for (uint32_t i = 0; i < frame_len; i++)
        buf[i] = ring->hdr->data[(tail + 2 + i) & RING_MASK];

    __atomic_store_n(&ring->hdr->tail, (tail + 2 + frame_len) & RING_MASK,
                     __ATOMIC_RELEASE);

    return frame_len;
}

int ring_event_fd(ring_t *ring) { return ring->event_fd; }

void ring_notify(ring_t *ring)
{
    uint64_t val = 1;
    write(ring->event_fd, &val, sizeof(val));
}

void ring_destroy(ring_t *ring, const char *shm_name)
{
    if (ring->hdr && ring->hdr != MAP_FAILED)
        munmap(ring->hdr, ring->map_size);
    if (ring->shm_fd >= 0) close(ring->shm_fd);
    if (ring->event_fd >= 0) close(ring->event_fd);
    if (shm_name) shm_unlink(shm_name);
}