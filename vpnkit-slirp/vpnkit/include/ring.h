#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

#define RING_SIZE       (1 << 20)
#define RING_MASK       (RING_SIZE - 1)
#define RING_SHM_SIZE   (sizeof(ring_header_t) + RING_SIZE)
#define CACHE_LINE_SIZE 64

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint8_t  data[];
} ring_header_t;

typedef struct {
    ring_header_t *hdr;
    int        shm_fd;
    int        event_fd;
    size_t     map_size;
} ring_t;

int ring_create(ring_t *ring, const char *shm_name);
int ring_attach(ring_t *ring, const char *shm_name, int event_fd);
int ring_set_eventfd(ring_t *ring, int event_fd);  // set eventfd after attach
int ring_write(ring_t *ring, const uint8_t *frame, uint16_t frame_len);
int ring_read(ring_t *ring, uint8_t *buf, size_t buf_len);
int ring_event_fd(ring_t *ring);
void ring_notify(ring_t *ring);
void ring_destroy(ring_t *ring, const char *shm_name);

#endif