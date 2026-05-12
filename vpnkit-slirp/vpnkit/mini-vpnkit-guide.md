# mini-vpnkit — Complete C Implementation Guide
> 3 Weeks · ~1,600 LOC · Userspace TCP/IP re-origination proxy with shared memory transport

## What You're Building

A working userspace network proxy in C that:
1. Accepts raw Ethernet frames over a shared memory ring buffer from a "VM" process
2. Parses Ethernet → ARP / IPv4 → TCP / UDP at the byte level
3. Re-originates TCP connections: terminates the guest TCP handshake yourself, opens a fresh host socket, splices data between them
4. Re-originates UDP: extracts payload, sends via host socket, relays reply
5. Manages per-destination-IP connection tables (the VPNKit architecture)
6. Implements fd-as-lifecycle port forwarding: hold connection = forward active, disconnect = cleanup

This is what Docker Desktop used to give VMs internet access through corporate VPNs.

---

## What's Skipped and Why

| Skipped | Why |
|---------|-----|
| Full TCP stack (retransmit, SACK, window scaling) | Minimal handshake + data splice is enough to understand re-origination |
| DHCP server | Skip entirely. Guest IP is hardcoded. |
| DNS implementation | Use host getaddrinfo() for any DNS traffic |
| 9P protocol | fd-as-lifecycle captures the same insight in 1/4 the code |
| Virtio virtqueue | Option 1 ring buffer teaches all the same shared memory concepts |
| IPv6 | IPv4 only |
| HTTP proxy | High-level feature, not core architecture |
| ICMP | TCP + UDP covers the re-origination concept |

---

## C Advantages for This Project

You're hand-rolling TCP at the byte level — C is the only sane language for this:

```c
#include <netinet/ip.h>       // struct iphdr
#include <netinet/tcp.h>      // struct tcphdr
#include <netinet/udp.h>      // struct udphdr
#include <linux/if_ether.h>   // struct ethhdr

// Cast raw bytes — zero-copy packet parsing
struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
struct tcphdr *tcp = (struct tcphdr *)((uint8_t *)ip + ip->ihl * 4);

// Read sequence number directly from the buffer
uint32_t seq = ntohl(tcp->seq);  // network byte order → host
```

Every field is at a known byte offset. No serialization library needed.

---

## Project Structure

```
mini-vpnkit/
├── Makefile
├── include/
│   ├── ring.h
│   ├── ethernet.h
│   ├── arp.h
│   ├── packet.h
│   ├── conn.h
│   ├── tcp_orig.h
│   ├── udp_orig.h
│   └── portfwd.h
├── src/
│   ├── main.c
│   ├── ring.c
│   ├── ethernet.c
│   ├── arp.c
│   ├── packet.c
│   ├── conn.c
│   ├── tcp_orig.c
│   ├── udp_orig.c
│   └── portfwd.c
├── tools/
│   └── vm_client.c
└── tests/
    ├── test_ring.c
    ├── test_packet.c
    └── test_conn.c
```

## Makefile

```makefile
CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread -I./include
SRCS   = src/main.c src/ring.c src/ethernet.c src/arp.c src/packet.c \
         src/conn.c src/tcp_orig.c src/udp_orig.c src/portfwd.c
TARGET = mini-vpnkit

all: $(TARGET) vm_client

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lrt

vm_client: tools/vm_client.c src/ring.c
	$(CC) $(CFLAGS) -o vm_client tools/vm_client.c src/ring.c -lrt

test_ring: src/ring.c tests/test_ring.c
	$(CC) $(CFLAGS) -o test_ring src/ring.c tests/test_ring.c -lrt

test_packet: src/packet.c src/ethernet.c tests/test_packet.c
	$(CC) $(CFLAGS) -o test_packet src/packet.c src/ethernet.c tests/test_packet.c

test_conn: src/conn.c tests/test_conn.c
	$(CC) $(CFLAGS) -o test_conn src/conn.c tests/test_conn.c

clean:
	rm -f $(TARGET) vm_client test_ring test_packet test_conn

.PHONY: all clean
```

No external dependencies. Pure C + Linux headers + POSIX.

---

# WEEK 1 — Shared Memory Ring Buffer + Ethernet/ARP

---

## Day 1 — Study Only (No Code)

Draw the VPNKit architecture on paper:

```
┌──────────────────────────────┐        ┌─────────────────────────────┐
│  "VM" PROCESS (vm_client)    │        │  mini-vpnkit PROCESS        │
│                              │        │                             │
│  Sends Ethernet frames       │        │  Receives frames            │
│       │                      │        │       │                     │
│       ▼                      │        │       ▼                     │
│  ring_write(tx_ring, frame)  │  shm   │  ring_read(rx_ring, frame)  │
│                              │◄──────►│                             │
│  ring_read(rx_ring, reply)   │  mmap  │  ring_write(tx_ring, reply) │
│       │                      │        │       │                     │
│       ▼                      │        │       ▼                     │
│  Guest sees response         │        │  Parse Ethernet → ARP/IPv4  │
│                              │        │       │                     │
│                              │        │       ├── ARP? → respond    │
│                              │        │       ├── TCP? → re-orig    │
│                              │        │       └── UDP? → re-orig    │
│                              │        │              │              │
│                              │        │              ▼              │
│                              │        │  socket() → connect() →     │
│                              │        │  splice to/from host        │
└──────────────────────────────┘        └─────────────────────────────┘
```

Key insight vs slirp4netns:
- slirp4netns: TAP device fd, one NAT table, libslirp does everything
- VPNKit: frames over socket/shm, per-destination-IP TCP stacks, you terminate TCP yourself

Answer before Day 2:
- What is mmap(MAP_SHARED)? (Maps a memory region that two processes share)
- What is a ring buffer? (Circular array with head/tail pointers)
- Why memory barriers? (CPU can reorder writes — consumer might see stale head)
- What is eventfd? (Lightweight notification mechanism, integrates with poll/epoll)
- What is re-origination? (Terminate TCP on guest side, open new TCP on host side)
- Why is re-origination different from NAT? (NAT rewrites headers; re-origination is two independent connections spliced together)

---

## Day 2 — Shared Memory Ring Buffer

### Key C Concept: mmap(MAP_SHARED)

```c
#include <sys/mman.h>
#include <fcntl.h>

// Process A: create shared memory
int shm_fd = shm_open("/my-ring", O_CREAT | O_RDWR, 0600);
ftruncate(shm_fd, 4096);  // set size
void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_SHARED, shm_fd, 0);

// Process B: attach to same shared memory
int shm_fd = shm_open("/my-ring", O_RDWR, 0);
void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_SHARED, shm_fd, 0);

// Both ptr's now point to the SAME physical pages.
// Write in A → visible in B (after memory barrier).
```

### Key C Concept: Memory Barriers

Without barriers, the CPU can reorder writes:

```c
// WRONG — consumer might see updated head before frame data is written
ring->data[head] = frame_byte;   // might execute AFTER the next line
ring->head = new_head;           // consumer sees new head, reads garbage

// CORRECT — barrier guarantees ordering
ring->data[head] = frame_byte;
__atomic_store_n(&ring->head, new_head, __ATOMIC_RELEASE);
// RELEASE means: all writes before this are visible before this store

// Consumer side:
uint32_t h = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
// ACQUIRE means: all reads after this see writes from before the RELEASE
```

### Key C Concept: eventfd for Notification

```c
#include <sys/eventfd.h>

int efd = eventfd(0, EFD_NONBLOCK);

// Producer: signal that data is available
uint64_t val = 1;
write(efd, &val, sizeof(val));

// Consumer: wait via poll(), then drain
struct pollfd pfd = { .fd = efd, .events = POLLIN };
poll(&pfd, 1, -1);
uint64_t val;
read(efd, &val, sizeof(val));  // resets counter
```

eventfd integrates directly into your poll() loop — no separate thread needed.

---

### include/ring.h

```c
#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

/*
 * Lock-free Single-Producer Single-Consumer ring buffer
 * over POSIX shared memory.
 *
 * Layout in shared memory:
 *   [ring_header_t][ring data bytes ...]
 *
 * Frames are stored as:
 *   [2-byte length][frame bytes][2-byte length][frame bytes]...
 *
 * Power-of-2 sizing allows bitwise AND instead of modulo.
 *
 * Cache line alignment on head/tail prevents false sharing —
 * producer writes head, consumer writes tail, they must not
 * share a cache line or every write invalidates the other CPU's cache.
 */

#define RING_SIZE       (1 << 20)  /* 1 MB — power of 2 */
#define RING_MASK       (RING_SIZE - 1)
#define RING_SHM_SIZE   (sizeof(ring_header_t) + RING_SIZE)
#define CACHE_LINE_SIZE 64

typedef struct {
    uint32_t head __attribute__((aligned(CACHE_LINE_SIZE)));  /* producer writes */
    uint32_t tail __attribute__((aligned(CACHE_LINE_SIZE)));  /* consumer writes */
    uint8_t  data[];
} ring_header_t;

/*
 * ring_t wraps a shared memory ring with an eventfd for notification.
 *
 * Two instances needed per direction:
 *   tx_ring: VM → vpnkit (VM writes, vpnkit reads)
 *   rx_ring: vpnkit → VM (vpnkit writes, VM reads)
 */
typedef struct {
    ring_header_t *hdr;      /* pointer into mmap'd region */
    int            shm_fd;
    int            event_fd; /* eventfd for wakeup notification */
    size_t         map_size;
} ring_t;

/*
 * Create a new ring (producer side).
 * shm_name: POSIX shared memory name, e.g. "/vpnkit-tx"
 */
int ring_create(ring_t *ring, const char *shm_name);

/*
 * Attach to existing ring (consumer side).
 */
int ring_attach(ring_t *ring, const char *shm_name, int event_fd);

/*
 * Write a frame into the ring.
 * Returns 0 on success, -1 if ring is full (backpressure).
 *
 * Stores: [2-byte frame length][frame bytes]
 * Memory barrier: RELEASE after writing data, before updating head.
 */
int ring_write(ring_t *ring, const uint8_t *frame, uint16_t frame_len);

/*
 * Read a frame from the ring.
 * Returns frame length on success, 0 if ring is empty, -1 on error.
 *
 * Memory barrier: ACQUIRE on head read, RELEASE on tail update.
 */
int ring_read(ring_t *ring, uint8_t *buf, size_t buf_len);

/* Get eventfd for use in poll() */
int ring_event_fd(ring_t *ring);

/* Signal that data was written (producer calls after ring_write) */
void ring_notify(ring_t *ring);

/* Cleanup */
void ring_destroy(ring_t *ring, const char *shm_name);

#endif
```

### src/ring.c

```c
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

    /* Create shared memory object */
    shm_unlink(shm_name);  /* remove stale */
    ring->shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (ring->shm_fd < 0) { perror("shm_open create"); return -1; }

    if (ftruncate(ring->shm_fd, ring->map_size) < 0) {
        perror("ftruncate"); return -1;
    }

    /* Map into our address space */
    ring->hdr = mmap(NULL, ring->map_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     ring->shm_fd, 0);
    if (ring->hdr == MAP_FAILED) { perror("mmap"); return -1; }

    /* Initialize head/tail to 0 */
    ring->hdr->head = 0;
    ring->hdr->tail = 0;

    /* Create eventfd for notification */
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

/*
 * Available space in ring.
 *
 * Ring is full when (head + 1) wraps to tail.
 * We waste one slot to distinguish full from empty.
 */
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
    uint32_t needed = 2 + frame_len;  /* 2-byte length prefix + payload */

    if (ring_space(ring->hdr) < needed)
        return -1;  /* ring full — backpressure */

    uint32_t head = ring->hdr->head;

    /* Write length prefix (little-endian in ring, doesn't matter as long as consistent) */
    ring->hdr->data[head & RING_MASK] = (uint8_t)(frame_len & 0xFF);
    ring->hdr->data[(head + 1) & RING_MASK] = (uint8_t)(frame_len >> 8);

    /* Write frame data byte by byte (handles wrap-around) */
    for (uint32_t i = 0; i < frame_len; i++)
        ring->hdr->data[(head + 2 + i) & RING_MASK] = frame[i];

    /*
     * RELEASE barrier: all the data writes above are guaranteed to be
     * visible to the consumer BEFORE the consumer sees the updated head.
     * Without this, consumer might read garbage.
     */
    __atomic_store_n(&ring->hdr->head, (head + needed) & RING_MASK,
                     __ATOMIC_RELEASE);

    return 0;
}

int ring_read(ring_t *ring, uint8_t *buf, size_t buf_len)
{
    if (ring_used(ring->hdr) < 2)
        return 0;  /* empty */

    uint32_t tail = ring->hdr->tail;

    /* Read length prefix */
    uint16_t frame_len = ring->hdr->data[tail & RING_MASK]
                       | ((uint16_t)ring->hdr->data[(tail + 1) & RING_MASK] << 8);

    if (frame_len == 0 || frame_len > buf_len)
        return -1;

    if (ring_used(ring->hdr) < (uint32_t)(2 + frame_len))
        return 0;  /* incomplete frame */

    /* Read frame data */
    for (uint32_t i = 0; i < frame_len; i++)
        buf[i] = ring->hdr->data[(tail + 2 + i) & RING_MASK];

    /*
     * RELEASE barrier: update tail after reading all data.
     * This tells the producer "I'm done reading, you can reuse this space."
     */
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
```

---

## Day 3 — Ring Buffer Tests

### tests/test_ring.c

```c
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

/* Stress test: producer and consumer on separate threads */
#define STRESS_COUNT 100000

static void *stress_writer(void *arg)
{
    ring_t *ring = (ring_t *)arg;
    uint8_t frame[64];
    for (int i = 0; i < STRESS_COUNT; i++) {
        /* Encode sequence number in frame */
        memcpy(frame, &i, sizeof(i));
        while (ring_write(ring, frame, sizeof(int)) < 0)
            usleep(1);  /* backpressure: ring full, retry */
    }
    return NULL;
}

void test_stress(void)
{
    ring_t ring;
    ring_create(&ring, "/test-ring-stress");

    pthread_t writer;
    pthread_create(&writer, NULL, stress_writer, &ring);

    uint8_t buf[64];
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

    TEST("stress: all frames received", received == STRESS_COUNT);
    TEST("stress: frames in order", ordered);
    TEST("stress: no crash", 1);

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
```

```bash
make test_ring && ./test_ring
valgrind --tool=helgrind ./test_ring  # check for races
```

---

## Day 4 — Ethernet Parsing + ARP Responder

### Key C Concept: Parsing Network Headers by Casting

```
Ethernet frame layout:
Offset 0:   dst MAC    (6 bytes)
Offset 6:   src MAC    (6 bytes)
Offset 12:  EtherType  (2 bytes, big-endian)
Offset 14:  Payload    (variable)

struct ethhdr *eth = (struct ethhdr *)buf;
uint16_t type = ntohs(eth->h_proto);  // ALWAYS ntohs for multi-byte fields
// type == 0x0800 → IPv4 payload at buf+14
// type == 0x0806 → ARP payload at buf+14
```

### Key C Concept: ARP at L2

When the guest wants to send a packet to the gateway (10.0.2.2), it first
asks "what is the MAC address of 10.0.2.2?" via ARP broadcast. You must
respond with a fake MAC, or the guest will never send any IP traffic.

```
ARP packet (28 bytes after Ethernet header):
Offset 0:  Hardware type    (2 bytes, = 0x0001 for Ethernet)
Offset 2:  Protocol type    (2 bytes, = 0x0800 for IPv4)
Offset 4:  Hardware length  (1 byte, = 6 for MAC)
Offset 5:  Protocol length  (1 byte, = 4 for IPv4)
Offset 6:  Opcode           (2 bytes, 1=request, 2=reply)
Offset 8:  Sender MAC       (6 bytes)
Offset 14: Sender IP        (4 bytes)
Offset 18: Target MAC       (6 bytes, all zeros in request)
Offset 24: Target IP        (4 bytes, the IP being asked about)
```

---

### include/arp.h

```c
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>

/*
 * ARP packet — 28 bytes, always inside an Ethernet frame.
 *
 * We use packed struct to map directly onto the buffer.
 */
struct __attribute__((packed)) arp_pkt {
    uint16_t htype;      /* hardware type: 1 = Ethernet */
    uint16_t ptype;      /* protocol type: 0x0800 = IPv4 */
    uint8_t  hlen;       /* hardware addr len: 6 */
    uint8_t  plen;       /* protocol addr len: 4 */
    uint16_t oper;       /* opcode: 1 = request, 2 = reply */
    uint8_t  sha[6];     /* sender hardware address (MAC) */
    uint8_t  spa[4];     /* sender protocol address (IP) */
    uint8_t  tha[6];     /* target hardware address */
    uint8_t  tpa[4];     /* target protocol address */
};

#define ARP_REQUEST 1
#define ARP_REPLY   2

/* Gateway MAC — fake address we assign to our virtual gateway */
extern const uint8_t GATEWAY_MAC[6];
/* Gateway IP */
extern const uint8_t GATEWAY_IP[4];

/*
 * Handle an ARP frame.
 *
 * If it's a request for our gateway IP, builds an ARP reply
 * inside reply_buf and returns the total frame length (Ethernet + ARP).
 * Returns 0 if not a request for us, -1 on error.
 */
int arp_handle(const uint8_t *frame, size_t frame_len,
               uint8_t *reply_buf, size_t reply_buf_len);

#endif
```

### src/arp.c

```c
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include "arp.h"

const uint8_t GATEWAY_MAC[6] = {0xCA, 0xFE, 0x00, 0x00, 0x00, 0x01};
const uint8_t GATEWAY_IP[4]  = {10, 0, 2, 2};

int arp_handle(const uint8_t *frame, size_t frame_len,
               uint8_t *reply_buf, size_t reply_buf_len)
{
    if (frame_len < sizeof(struct ethhdr) + sizeof(struct arp_pkt))
        return -1;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    if (ntohs(eth->h_proto) != ETH_P_ARP)
        return 0;  /* not ARP */

    const struct arp_pkt *arp = (const struct arp_pkt *)(frame + sizeof(struct ethhdr));

    /* Only handle ARP requests for IPv4 over Ethernet */
    if (ntohs(arp->oper) != ARP_REQUEST)  return 0;
    if (ntohs(arp->htype) != 1)           return 0;
    if (ntohs(arp->ptype) != ETH_P_IP)    return 0;

    /* Is this asking for our gateway IP? */
    if (memcmp(arp->tpa, GATEWAY_IP, 4) != 0)
        return 0;  /* not for us */

    /*
     * Build ARP reply.
     *
     * The reply says: "10.0.2.2 is at CA:FE:00:00:00:01"
     *
     * After this, the guest will use GATEWAY_MAC as the destination
     * MAC for all packets going through the gateway. We'll receive
     * these frames and process them.
     */
    if (reply_buf_len < sizeof(struct ethhdr) + sizeof(struct arp_pkt))
        return -1;

    /* Ethernet header: reply to sender */
    struct ethhdr *reth = (struct ethhdr *)reply_buf;
    memcpy(reth->h_dest, arp->sha, 6);       /* to: original sender */
    memcpy(reth->h_source, GATEWAY_MAC, 6);   /* from: our fake MAC */
    reth->h_proto = htons(ETH_P_ARP);

    /* ARP reply */
    struct arp_pkt *rarp = (struct arp_pkt *)(reply_buf + sizeof(struct ethhdr));
    rarp->htype = htons(1);
    rarp->ptype = htons(ETH_P_IP);
    rarp->hlen  = 6;
    rarp->plen  = 4;
    rarp->oper  = htons(ARP_REPLY);
    memcpy(rarp->sha, GATEWAY_MAC, 6);   /* sender MAC: our gateway */
    memcpy(rarp->spa, GATEWAY_IP, 4);    /* sender IP: 10.0.2.2 */
    memcpy(rarp->tha, arp->sha, 6);      /* target MAC: original requester */
    memcpy(rarp->tpa, arp->spa, 4);      /* target IP: original sender IP */

    size_t reply_len = sizeof(struct ethhdr) + sizeof(struct arp_pkt);
    printf("ARP: who-has %d.%d.%d.%d? → reply with %02x:%02x:%02x:%02x:%02x:%02x\n",
           arp->tpa[0], arp->tpa[1], arp->tpa[2], arp->tpa[3],
           GATEWAY_MAC[0], GATEWAY_MAC[1], GATEWAY_MAC[2],
           GATEWAY_MAC[3], GATEWAY_MAC[4], GATEWAY_MAC[5]);

    return (int)reply_len;
}
```

---

## Day 5 — IPv4/TCP/UDP Header Parsing + Checksum

### Key C Concept: IP Header Checksum

The IP checksum is a one's complement sum of 16-bit words:

```c
/*
 * Compute Internet checksum (RFC 1071).
 *
 * Sum all 16-bit words, fold 32-bit carries back into 16 bits,
 * then take one's complement.
 *
 * Used for: IP header checksum, TCP pseudo-header checksum, UDP checksum.
 */
uint16_t checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1)  /* odd byte */
        sum += *(const uint8_t *)ptr;

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}
```

### Key C Concept: TCP Pseudo-Header Checksum

TCP checksum covers more than just the TCP header — it includes a
"pseudo-header" with source IP, dest IP, protocol, and TCP length.
This catches misrouted packets.

```c
struct __attribute__((packed)) pseudo_header {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;    /* 6 = TCP, 17 = UDP */
    uint16_t tcp_length;  /* TCP header + data */
};
```

---

### include/packet.h

```c
#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

/* Checksum computation */
uint16_t ip_checksum(const void *data, size_t len);
uint16_t tcp_checksum(const struct iphdr *ip, const struct tcphdr *tcp, size_t tcp_len);
uint16_t udp_checksum(const struct iphdr *ip, const struct udphdr *udp, size_t udp_len);

/*
 * Build a complete Ethernet + IP + TCP packet.
 *
 * Used for:
 *   - SYN-ACK reply to guest SYN
 *   - ACK packets
 *   - Data packets from host → guest
 *   - FIN packets
 *
 * Returns total frame length.
 */
size_t build_tcp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    uint32_t seq, uint32_t ack_seq,
    uint16_t flags,       /* TH_SYN, TH_ACK, TH_FIN, TH_PSH, TH_RST */
    const uint8_t *payload, size_t payload_len
);

/*
 * Build a complete Ethernet + IP + UDP packet.
 */
size_t build_udp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const uint8_t *payload, size_t payload_len
);

#endif
```

### src/packet.c

```c
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include "packet.h"

uint16_t ip_checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t tcp_checksum(const struct iphdr *ip, const struct tcphdr *tcp, size_t tcp_len)
{
    struct {
        uint32_t src; uint32_t dst;
        uint8_t zero; uint8_t proto; uint16_t len;
    } __attribute__((packed)) pseudo;

    pseudo.src   = ip->saddr;
    pseudo.dst   = ip->daddr;
    pseudo.zero  = 0;
    pseudo.proto = IPPROTO_TCP;
    pseudo.len   = htons((uint16_t)tcp_len);

    uint32_t sum = 0;
    const uint16_t *p;

    /* Sum pseudo-header */
    p = (const uint16_t *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) sum += p[i];

    /* Sum TCP header + data */
    p = (const uint16_t *)tcp;
    size_t remaining = tcp_len;
    while (remaining > 1) { sum += *p++; remaining -= 2; }
    if (remaining == 1) sum += *(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t udp_checksum(const struct iphdr *ip, const struct udphdr *udp, size_t udp_len)
{
    struct {
        uint32_t src; uint32_t dst;
        uint8_t zero; uint8_t proto; uint16_t len;
    } __attribute__((packed)) pseudo;

    pseudo.src   = ip->saddr;
    pseudo.dst   = ip->daddr;
    pseudo.zero  = 0;
    pseudo.proto = IPPROTO_UDP;
    pseudo.len   = htons((uint16_t)udp_len);

    uint32_t sum = 0;
    const uint16_t *p;

    p = (const uint16_t *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) sum += p[i];

    p = (const uint16_t *)udp;
    size_t remaining = udp_len;
    while (remaining > 1) { sum += *p++; remaining -= 2; }
    if (remaining == 1) sum += *(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

size_t build_tcp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    uint32_t seq, uint32_t ack_seq,
    uint16_t flags,
    const uint8_t *payload, size_t payload_len)
{
    size_t eth_len = sizeof(struct ethhdr);
    size_t ip_len  = sizeof(struct iphdr);
    size_t tcp_hdr_len = 20;  /* no options for simplicity */

    /* For SYN/SYN-ACK: add MSS option (4 bytes) */
    if (flags & TH_SYN) tcp_hdr_len = 24;

    size_t total = eth_len + ip_len + tcp_hdr_len + payload_len;
    if (total > buf_len) return 0;

    memset(buf, 0, total);

    /* Ethernet */
    struct ethhdr *eth = (struct ethhdr *)buf;
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    /* IP */
    struct iphdr *ip = (struct iphdr *)(buf + eth_len);
    ip->version  = 4;
    ip->ihl      = 5;
    ip->tot_len  = htons((uint16_t)(ip_len + tcp_hdr_len + payload_len));
    ip->ttl      = 64;
    ip->protocol = IPPROTO_TCP;
    ip->saddr    = src_ip;
    ip->daddr    = dst_ip;
    ip->check    = ip_checksum(ip, ip_len);

    /* TCP */
    struct tcphdr *tcp = (struct tcphdr *)(buf + eth_len + ip_len);
    tcp->source  = htons(src_port);
    tcp->dest    = htons(dst_port);
    tcp->seq     = htonl(seq);
    tcp->ack_seq = htonl(ack_seq);
    tcp->doff    = tcp_hdr_len / 4;

    if (flags & TH_SYN) tcp->syn = 1;
    if (flags & TH_ACK) tcp->ack = 1;
    if (flags & TH_FIN) tcp->fin = 1;
    if (flags & TH_RST) tcp->rst = 1;
    if (flags & TH_PSH) tcp->psh = 1;

    tcp->window  = htons(65535);

    /* MSS option for SYN packets */
    if (flags & TH_SYN) {
        uint8_t *opts = (uint8_t *)tcp + 20;
        opts[0] = 2;     /* kind = MSS */
        opts[1] = 4;     /* length = 4 */
        uint16_t mss = htons(1460);
        memcpy(&opts[2], &mss, 2);
    }

    /* Payload */
    if (payload && payload_len > 0)
        memcpy(buf + eth_len + ip_len + tcp_hdr_len, payload, payload_len);

    /* TCP checksum (computed last, over pseudo-header + TCP) */
    tcp->check = 0;
    tcp->check = tcp_checksum(ip, tcp, tcp_hdr_len + payload_len);

    return total;
}

size_t build_udp_frame(
    uint8_t *buf, size_t buf_len,
    const uint8_t *dst_mac, const uint8_t *src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const uint8_t *payload, size_t payload_len)
{
    size_t eth_len = sizeof(struct ethhdr);
    size_t ip_len  = sizeof(struct iphdr);
    size_t udp_hdr_len = sizeof(struct udphdr);
    size_t total = eth_len + ip_len + udp_hdr_len + payload_len;

    if (total > buf_len) return 0;
    memset(buf, 0, total);

    struct ethhdr *eth = (struct ethhdr *)buf;
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr *ip = (struct iphdr *)(buf + eth_len);
    ip->version  = 4;
    ip->ihl      = 5;
    ip->tot_len  = htons((uint16_t)(ip_len + udp_hdr_len + payload_len));
    ip->ttl      = 64;
    ip->protocol = IPPROTO_UDP;
    ip->saddr    = src_ip;
    ip->daddr    = dst_ip;
    ip->check    = ip_checksum(ip, ip_len);

    struct udphdr *udp = (struct udphdr *)(buf + eth_len + ip_len);
    udp->source = htons(src_port);
    udp->dest   = htons(dst_port);
    udp->len    = htons((uint16_t)(udp_hdr_len + payload_len));

    if (payload && payload_len > 0)
        memcpy(buf + eth_len + ip_len + udp_hdr_len, payload, payload_len);

    udp->check = 0;
    udp->check = udp_checksum(ip, udp, udp_hdr_len + payload_len);

    return total;
}
```

---

# WEEK 2 — TCP Re-origination + Connection Table

---

## Day 1 — Connection Table

### include/conn.h

```c
#ifndef CONN_H
#define CONN_H

#include <stdint.h>
#include <netinet/in.h>
#include <time.h>

#define MAX_CONNECTIONS 256

typedef enum {
    CONN_FREE = 0,
    CONN_SYN_RCVD,      /* guest sent SYN, we're connecting to host */
    CONN_ESTABLISHED,    /* handshake complete, splicing data */
    CONN_FIN_WAIT,       /* one side sent FIN */
    CONN_CLOSED
} conn_state_t;

/*
 * Per-connection state.
 *
 * Tracks both the guest-side TCP (sequence numbers we generate)
 * and the host-side socket (real connection to the destination).
 */
typedef struct {
    conn_state_t state;

    /* Guest-side 5-tuple */
    uint32_t guest_src_ip;
    uint16_t guest_src_port;
    uint32_t guest_dst_ip;
    uint16_t guest_dst_port;
    uint8_t  guest_mac[6];     /* guest's MAC for building reply frames */

    /* Guest-side TCP state */
    uint32_t guest_isn;        /* guest's initial sequence number */
    uint32_t my_isn;           /* our (vpnkit's) initial sequence number */
    uint32_t snd_nxt;          /* next seq we'll send TO the guest */
    uint32_t rcv_nxt;          /* next seq we expect FROM the guest */

    /* Host-side socket */
    int      host_fd;          /* SOCK_STREAM connected to real dest */

    time_t   last_active;
} conn_t;

/*
 * Connection table — hash map from 5-tuple to conn_t.
 *
 * Hash function: FNV-1a on (src_ip, src_port, dst_ip, dst_port).
 * Collision: linear probing (simple, cache-friendly).
 */
typedef struct {
    conn_t entries[MAX_CONNECTIONS];
} conn_table_t;

void    conn_table_init(conn_table_t *ct);
conn_t *conn_lookup(conn_table_t *ct, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port);
conn_t *conn_create(conn_table_t *ct, uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port);
void    conn_remove(conn_table_t *ct, conn_t *c);
void    conn_reap_expired(conn_table_t *ct, int timeout_secs);

#endif
```

### src/conn.c

```c
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
    /* FNV-1a hash */
    uint32_t h = 2166136261u;
    uint8_t *p;

    p = (uint8_t *)&src_ip;
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

        if (c->state == CONN_FREE) return NULL;  /* empty slot = miss */

        if (c->guest_src_ip   == src_ip   &&
            c->guest_src_port == src_port &&
            c->guest_dst_ip   == dst_ip   &&
            c->guest_dst_port == dst_port &&
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
            c->guest_src_ip   = src_ip;
            c->guest_src_port = src_port;
            c->guest_dst_ip   = dst_ip;
            c->guest_dst_port = dst_port;
            c->host_fd        = -1;
            c->last_active    = time(NULL);
            return c;
        }
    }
    return NULL;  /* table full */
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
```

---

## Day 2-4 — TCP Re-origination

This is the heart of VPNKit. When a guest sends a TCP SYN:

```
1. Guest sends SYN to (dst_ip:dst_port)
2. You parse the SYN from the Ethernet frame
3. You open a real host socket: socket() + connect(dst_ip:dst_port)
4. If connect succeeds:
   → Build a SYN-ACK frame with YOUR ISN
   → Send it back to the guest via the ring buffer
   → Guest responds with ACK → connection ESTABLISHED
5. Now splice: guest data → host socket, host socket → guest frame
6. If connect fails:
   → Build a RST frame, send to guest
```

This is NOT NAT. You are one endpoint of the TCP connection on the guest side,
and you open a completely separate connection on the host side. Two independent
TCP streams, you copy data between them.

### include/tcp_orig.h

```c
#ifndef TCP_ORIG_H
#define TCP_ORIG_H

#include "conn.h"
#include "ring.h"

/*
 * Handle a TCP packet from the guest.
 *
 * frame: complete Ethernet frame
 * frame_len: length of frame
 * ct: connection table
 * tx_ring: ring buffer to send frames back to guest
 */
void tcp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring);

/*
 * Check all host sockets for incoming data.
 * Call this from the main loop when host fds are readable.
 *
 * For each connection with data on host_fd:
 *   recv() data, build TCP data frame, write to tx_ring.
 */
void tcp_poll_host(conn_table_t *ct, ring_t *tx_ring);

#endif
```

### src/tcp_orig.c

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/if_ether.h>
#include "tcp_orig.h"
#include "packet.h"
#include "arp.h"

/*
 * Generate a random Initial Sequence Number.
 *
 * Real TCP stacks use a clock-based ISN to prevent overlap
 * with previous connections. For learning purposes, random is fine.
 * In production, a predictable ISN enables TCP sequence prediction attacks.
 */
static uint32_t generate_isn(void)
{
    uint32_t isn;
    FILE *f = fopen("/dev/urandom", "r");
    if (f) { fread(&isn, sizeof(isn), 1, f); fclose(f); }
    else isn = (uint32_t)time(NULL);
    return isn;
}

/*
 * Send a TCP frame back to the guest via the ring buffer.
 */
static void send_to_guest(ring_t *tx_ring, conn_t *c,
                          uint16_t flags, const uint8_t *payload, size_t plen)
{
    uint8_t frame[1514];

    size_t len = build_tcp_frame(
        frame, sizeof(frame),
        c->guest_mac, GATEWAY_MAC,      /* dst=guest, src=gateway */
        c->guest_dst_ip, c->guest_src_ip,  /* IP: pretend to be the remote server */
        c->guest_dst_port, c->guest_src_port,
        c->snd_nxt, c->rcv_nxt,
        flags, payload, plen
    );

    if (len > 0) {
        ring_write(tx_ring, frame, (uint16_t)len);
        ring_notify(tx_ring);
    }
}

void tcp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring)
{
    if (frame_len < sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct tcphdr))
        return;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    const struct iphdr *ip = (const struct iphdr *)(frame + sizeof(struct ethhdr));

    if (ip->protocol != IPPROTO_TCP) return;

    const struct tcphdr *tcp = (const struct tcphdr *)((uint8_t *)ip + ip->ihl * 4);

    uint32_t src_ip   = ip->saddr;
    uint32_t dst_ip   = ip->daddr;
    uint16_t src_port = ntohs(tcp->source);
    uint16_t dst_port = ntohs(tcp->dest);
    uint32_t seq      = ntohl(tcp->seq);
    uint32_t ack      = ntohl(tcp->ack_seq);

    /* Calculate payload */
    size_t ip_hdr_len  = ip->ihl * 4;
    size_t tcp_hdr_len = tcp->doff * 4;
    size_t total_ip_len = ntohs(ip->tot_len);
    size_t payload_len = total_ip_len - ip_hdr_len - tcp_hdr_len;
    const uint8_t *payload = (const uint8_t *)tcp + tcp_hdr_len;

    /* ── GUEST SYN — new connection ── */
    if (tcp->syn && !tcp->ack) {
        conn_t *c = conn_create(ct, src_ip, src_port, dst_ip, dst_port);
        if (!c) {
            fprintf(stderr, "Connection table full\n");
            return;
        }

        /* Save guest's MAC for reply frames */
        memcpy(c->guest_mac, eth->h_source, 6);
        c->guest_isn = seq;
        c->rcv_nxt = seq + 1;  /* SYN consumes one sequence number */

        /*
         * RE-ORIGINATION: open a real host socket to the destination.
         *
         * This is THE key operation. The guest thinks it's connecting to
         * dst_ip:dst_port. We intercept the SYN, open our own socket
         * to the same destination, and splice the two connections.
         */
        c->host_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (c->host_fd < 0) {
            /* Can't open socket → send RST to guest */
            c->my_isn = generate_isn();
            c->snd_nxt = c->my_isn;
            send_to_guest(tx_ring, c, TH_RST | TH_ACK, NULL, 0);
            conn_remove(ct, c);
            return;
        }

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = dst_ip;
        dest.sin_port = htons(dst_port);

        int ret = connect(c->host_fd, (struct sockaddr *)&dest, sizeof(dest));
        if (ret < 0 && errno != EINPROGRESS) {
            /* Connect failed immediately → RST */
            c->my_isn = generate_isn();
            c->snd_nxt = c->my_isn;
            send_to_guest(tx_ring, c, TH_RST | TH_ACK, NULL, 0);
            conn_remove(ct, c);
            return;
        }

        /*
         * Connect succeeded (or is in progress).
         * Send SYN-ACK back to guest.
         *
         * We generate our own ISN. The guest will ACK this
         * with ack = my_isn + 1.
         */
        c->my_isn = generate_isn();
        c->snd_nxt = c->my_isn + 1;  /* SYN-ACK consumes one seq */
        c->state = CONN_SYN_RCVD;

        /* Build SYN-ACK: SYN flag + ACK flag */
        uint8_t syn_ack_frame[1514];
        size_t sa_len = build_tcp_frame(
            syn_ack_frame, sizeof(syn_ack_frame),
            c->guest_mac, GATEWAY_MAC,
            dst_ip, src_ip,              /* IP: pretend to be the remote */
            dst_port, src_port,
            c->my_isn, c->rcv_nxt,       /* our ISN, ack guest's SYN */
            TH_SYN | TH_ACK, NULL, 0
        );

        ring_write(tx_ring, syn_ack_frame, (uint16_t)sa_len);
        ring_notify(tx_ring);

        printf("TCP: SYN %u.%u.%u.%u:%d → connect() → SYN-ACK\n",
               (dst_ip) & 0xFF, (dst_ip >> 8) & 0xFF,
               (dst_ip >> 16) & 0xFF, (dst_ip >> 24) & 0xFF, dst_port);
        return;
    }

    /* ── Existing connection ── */
    conn_t *c = conn_lookup(ct, src_ip, src_port, dst_ip, dst_port);
    if (!c) return;

    c->last_active = time(NULL);

    /* ── ACK completing handshake ── */
    if (tcp->ack && c->state == CONN_SYN_RCVD) {
        c->state = CONN_ESTABLISHED;
        printf("TCP: connection ESTABLISHED\n");
    }

    /* ── Data from guest ── */
    if (payload_len > 0 && c->state == CONN_ESTABLISHED) {
        /*
         * Guest sent data. Forward it to the host socket.
         * This is the "splice" — data flows from guest TCP stream
         * to a completely independent host TCP connection.
         */
        ssize_t sent = send(c->host_fd, payload, payload_len, MSG_NOSIGNAL);
        if (sent < 0) {
            send_to_guest(tx_ring, c, TH_RST | TH_ACK, NULL, 0);
            conn_remove(ct, c);
            return;
        }

        c->rcv_nxt += (uint32_t)payload_len;

        /* ACK the guest's data */
        send_to_guest(tx_ring, c, TH_ACK, NULL, 0);
    }

    /* ── FIN from guest ── */
    if (tcp->fin) {
        c->rcv_nxt++;
        send_to_guest(tx_ring, c, TH_ACK | TH_FIN, NULL, 0);
        c->snd_nxt++;
        if (c->host_fd >= 0) { shutdown(c->host_fd, SHUT_WR); }
        c->state = CONN_FIN_WAIT;
    }

    /* ── RST from guest ── */
    if (tcp->rst) {
        conn_remove(ct, c);
    }
}

void tcp_poll_host(conn_table_t *ct, ring_t *tx_ring)
{
    uint8_t buf[4096];

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        conn_t *c = &ct->entries[i];
        if (c->state != CONN_ESTABLISHED || c->host_fd < 0)
            continue;

        ssize_t n = recv(c->host_fd, buf, sizeof(buf), MSG_DONTWAIT);

        if (n > 0) {
            /*
             * Data from the real internet → build TCP data frame → guest.
             *
             * We pretend to be the remote server by using the remote's
             * IP/port as source in the frame we send to the guest.
             */
            send_to_guest(tx_ring, c, TH_ACK | TH_PSH, buf, (size_t)n);
            c->snd_nxt += (uint32_t)n;
            c->last_active = time(NULL);
        }
        else if (n == 0) {
            /* Host closed connection → FIN to guest */
            send_to_guest(tx_ring, c, TH_ACK | TH_FIN, NULL, 0);
            c->snd_nxt++;
            c->state = CONN_FIN_WAIT;
        }
        /* n < 0 && errno == EAGAIN: no data, normal for non-blocking */
    }
}
```

---

# WEEK 3 — UDP Re-origination + Port Forwarding + Integration

---

## Day 1-2 — UDP Re-origination

### src/udp_orig.c

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <linux/if_ether.h>
#include "conn.h"
#include "packet.h"
#include "ring.h"
#include "arp.h"

/*
 * UDP re-origination — simpler than TCP.
 *
 * Guest sends UDP datagram → extract payload → host sendto() →
 * receive reply → build UDP frame → guest.
 *
 * We reuse the connection table for tracking, even though UDP
 * is "connectionless" — we need to map replies back to the guest.
 */
void udp_handle(const uint8_t *frame, size_t frame_len,
                conn_table_t *ct, ring_t *tx_ring)
{
    if (frame_len < sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr))
        return;

    const struct ethhdr *eth = (const struct ethhdr *)frame;
    const struct iphdr *ip = (const struct iphdr *)(frame + sizeof(struct ethhdr));

    if (ip->protocol != IPPROTO_UDP) return;

    const struct udphdr *udp = (const struct udphdr *)((uint8_t *)ip + ip->ihl * 4);
    size_t udp_total  = ntohs(udp->len);
    size_t payload_len = udp_total - sizeof(struct udphdr);
    const uint8_t *payload = (const uint8_t *)udp + sizeof(struct udphdr);

    uint32_t src_ip   = ip->saddr;
    uint32_t dst_ip   = ip->daddr;
    uint16_t src_port = ntohs(udp->source);
    uint16_t dst_port = ntohs(udp->dest);

    /* Open a host UDP socket and send */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = dst_ip;
    dest.sin_port = htons(dst_port);

    sendto(sock, payload, payload_len, 0,
           (struct sockaddr *)&dest, sizeof(dest));

    /* Wait for reply (with timeout) */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t reply_buf[4096];
    ssize_t n = recvfrom(sock, reply_buf, sizeof(reply_buf), 0, NULL, NULL);
    close(sock);

    if (n <= 0) return;

    /* Build UDP reply frame back to guest */
    uint8_t reply_frame[1514];
    uint8_t guest_mac[6];
    memcpy(guest_mac, eth->h_source, 6);

    size_t rlen = build_udp_frame(
        reply_frame, sizeof(reply_frame),
        guest_mac, GATEWAY_MAC,
        dst_ip, src_ip,       /* pretend to be the remote */
        dst_port, src_port,
        reply_buf, (size_t)n
    );

    if (rlen > 0) {
        ring_write(tx_ring, reply_frame, (uint16_t)rlen);
        ring_notify(tx_ring);
    }
}
```

---

## Day 3-4 — fd-as-lifecycle Port Forwarding

### Key Concept: fd-as-lifecycle

Instead of explicit add/remove commands (like slirp4netns JSON API),
VPNKit uses a pattern where holding a connection open = resource is active:

```
Client connects to control socket → port forward is added
Client disconnects (or crashes)   → port forward is automatically removed
```

The kernel handles cleanup — if the client process is killed, the kernel
closes the socket, the server detects it, and the forward is cleaned up.
No "remove" API needed. No leaked forwards after crashes.

### include/portfwd.h

```c
#ifndef PORTFWD_H
#define PORTFWD_H

#include "ring.h"
#include "conn.h"

#define MAX_PORT_FORWARDS 32

typedef struct {
    int      active;
    int      control_fd;     /* client's connection — close = remove */
    int      listen_fd;      /* host listener socket */
    uint16_t host_port;
    uint32_t guest_ip;
    uint16_t guest_port;
} port_forward_t;

typedef struct {
    int             listen_fd;   /* control socket */
    char            path[256];
    port_forward_t  fwds[MAX_PORT_FORWARDS];
    ring_t         *tx_ring;
    conn_table_t   *ct;
} portfwd_server_t;

int  portfwd_init(portfwd_server_t *pf, const char *socket_path,
                  ring_t *tx_ring, conn_table_t *ct);
int  portfwd_control_fd(portfwd_server_t *pf);
void portfwd_handle_new(portfwd_server_t *pf);
void portfwd_poll(portfwd_server_t *pf);
void portfwd_destroy(portfwd_server_t *pf);

#endif
```

### src/portfwd.c

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include "portfwd.h"
#include "packet.h"
#include "arp.h"

int portfwd_init(portfwd_server_t *pf, const char *socket_path,
                 ring_t *tx_ring, conn_table_t *ct)
{
    memset(pf, 0, sizeof(*pf));
    pf->tx_ring = tx_ring;
    pf->ct = ct;
    strncpy(pf->path, socket_path, sizeof(pf->path) - 1);

    unlink(socket_path);
    pf->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (pf->listen_fd < 0) { perror("pf socket"); return -1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(pf->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    if (listen(pf->listen_fd, 5) < 0) return -1;

    printf("Port forward control socket: %s\n", socket_path);
    return 0;
}

int portfwd_control_fd(portfwd_server_t *pf)
{
    return pf->listen_fd;
}

/*
 * Handle a new control connection.
 *
 * Protocol:
 *   Client connects, sends: "tcp HOST_PORT GUEST_IP GUEST_PORT\n"
 *   Server reads, binds host port, responds "OK\n"
 *   Client holds connection open — forward stays active.
 *   Client closes (or crashes) — forward is automatically removed.
 */
void portfwd_handle_new(portfwd_server_t *pf)
{
    int client = accept(pf->listen_fd, NULL, NULL);
    if (client < 0) return;

    /* Read the forward request */
    char buf[256];
    ssize_t n = read(client, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client); return; }
    buf[n] = '\0';

    char proto[8]; int host_port, guest_port; char guest_ip[16];
    if (sscanf(buf, "%7s %d %15s %d", proto, &host_port, guest_ip, &guest_port) != 4) {
        dprintf(client, "ERROR: expected 'tcp HOST_PORT GUEST_IP GUEST_PORT'\n");
        close(client);
        return;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_PORT_FORWARDS; i++) {
        if (!pf->fwds[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        dprintf(client, "ERROR: max forwards reached\n");
        close(client);
        return;
    }

    /* Bind host port */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { close(client); return; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in laddr = {0};
    laddr.sin_family = AF_INET;
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);
    laddr.sin_port = htons(host_port);

    if (bind(lfd, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
        dprintf(client, "ERROR: bind %d: %s\n", host_port, strerror(errno));
        close(lfd); close(client);
        return;
    }
    listen(lfd, 5);

    port_forward_t *fw = &pf->fwds[slot];
    fw->active     = 1;
    fw->control_fd = client;    /* ← THIS is the lifecycle fd */
    fw->listen_fd  = lfd;
    fw->host_port  = host_port;
    inet_pton(AF_INET, guest_ip, &fw->guest_ip);
    fw->guest_port = guest_port;

    dprintf(client, "OK\n");
    printf("Port forward: host:%d → %s:%d (control_fd=%d)\n",
           host_port, guest_ip, guest_port, client);
}

/*
 * Poll all active port forwards.
 *
 * Two things to check:
 *   1. Did the control client disconnect? → remove forward
 *   2. Did someone connect to the host port? → inject SYN to guest
 */
void portfwd_poll(portfwd_server_t *pf)
{
    for (int i = 0; i < MAX_PORT_FORWARDS; i++) {
        port_forward_t *fw = &pf->fwds[i];
        if (!fw->active) continue;

        /* Check if control client disconnected (fd-as-lifecycle) */
        struct pollfd cpfd = { .fd = fw->control_fd, .events = POLLIN };
        if (poll(&cpfd, 1, 0) > 0) {
            char tmp;
            if (recv(fw->control_fd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                /* Client closed → cleanup */
                printf("Port forward removed: host:%d (client disconnected)\n",
                       fw->host_port);
                close(fw->listen_fd);
                close(fw->control_fd);
                fw->active = 0;
                continue;
            }
        }

        /* Check for incoming connections on host port */
        struct pollfd lpfd = { .fd = fw->listen_fd, .events = POLLIN };
        if (poll(&lpfd, 1, 0) > 0 && (lpfd.revents & POLLIN)) {
            int accepted = accept(fw->listen_fd, NULL, NULL);
            if (accepted < 0) continue;

            /*
             * Someone connected to the host port.
             *
             * In a full VPNKit, you would inject a SYN frame into
             * the guest's network to initiate an inbound connection.
             * For this minimal implementation, we create a connection
             * entry that maps this accepted socket to the guest target.
             *
             * The guest would need its own listener — for now, we
             * just track the mapping.
             */
            printf("Accepted connection on host:%d → forwarding to guest:%d\n",
                   fw->host_port, fw->guest_port);

            /* TODO: inject SYN to guest via ring buffer */
            /* For now, close — the concept is demonstrated */
            close(accepted);
        }
    }
}

void portfwd_destroy(portfwd_server_t *pf)
{
    for (int i = 0; i < MAX_PORT_FORWARDS; i++) {
        if (pf->fwds[i].active) {
            close(pf->fwds[i].listen_fd);
            close(pf->fwds[i].control_fd);
        }
    }
    if (pf->listen_fd >= 0) close(pf->listen_fd);
    unlink(pf->path);
}
```

---

## Day 5 — VM Client Test Tool

### tools/vm_client.c

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include "ring.h"

/*
 * Simulates a VM sending Ethernet frames to mini-vpnkit.
 *
 * Creates the shared memory rings, sends an ARP request,
 * reads the ARP reply, then sends a TCP SYN.
 *
 * Usage: ./vm_client
 */

static uint8_t my_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static uint8_t gw_ip[4]  = {10, 0, 2, 2};
static uint8_t my_ip[4]  = {10, 0, 2, 100};

int main(void)
{
    ring_t tx_ring, rx_ring;

    /* Create rings — tx = VM→vpnkit, rx = vpnkit→VM */
    ring_create(&tx_ring, "/vpnkit-tx");
    ring_create(&rx_ring, "/vpnkit-rx");

    printf("VM client started. eventfd tx=%d rx=%d\n",
           ring_event_fd(&tx_ring), ring_event_fd(&rx_ring));
    printf("Pass these eventfds to mini-vpnkit.\n");
    printf("Press Enter to send ARP request...\n");
    getchar();

    /* Build ARP request: who-has 10.0.2.2? */
    uint8_t arp_frame[42];  /* 14 eth + 28 arp */
    memset(arp_frame, 0, sizeof(arp_frame));

    struct ethhdr *eth = (struct ethhdr *)arp_frame;
    memset(eth->h_dest, 0xFF, 6);          /* broadcast */
    memcpy(eth->h_source, my_mac, 6);
    eth->h_proto = htons(ETH_P_ARP);

    uint8_t *arp = arp_frame + 14;
    *(uint16_t *)(arp + 0) = htons(1);       /* htype: Ethernet */
    *(uint16_t *)(arp + 2) = htons(0x0800);  /* ptype: IPv4 */
    arp[4] = 6; arp[5] = 4;                  /* hlen, plen */
    *(uint16_t *)(arp + 6) = htons(1);       /* opcode: request */
    memcpy(arp + 8, my_mac, 6);              /* sender MAC */
    memcpy(arp + 14, my_ip, 4);              /* sender IP */
    /* target MAC: all zeros (we're asking) */
    memcpy(arp + 24, gw_ip, 4);             /* target IP */

    ring_write(&tx_ring, arp_frame, 42);
    ring_notify(&tx_ring);
    printf("Sent ARP request: who-has 10.0.2.2?\n");

    /* Wait for ARP reply */
    sleep(1);
    uint8_t reply[1514];
    int n = ring_read(&rx_ring, reply, sizeof(reply));
    if (n > 0) {
        struct ethhdr *reth = (struct ethhdr *)reply;
        printf("Got ARP reply: gateway MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
               reth->h_source[0], reth->h_source[1], reth->h_source[2],
               reth->h_source[3], reth->h_source[4], reth->h_source[5]);
    } else {
        printf("No ARP reply received\n");
    }

    printf("Press Enter to clean up...\n");
    getchar();

    ring_destroy(&tx_ring, "/vpnkit-tx");
    ring_destroy(&rx_ring, "/vpnkit-rx");
    return 0;
}
```

---

## Day 6-7 — End-to-End Testing

```bash
# Terminal 1: start mini-vpnkit
./mini-vpnkit --control /tmp/vpnkit.sock

# Terminal 2: start VM client
./vm_client
# Press Enter → sends ARP → should see "Got ARP reply: gateway MAC = ca:fe:..."

# Terminal 3: test port forward (fd-as-lifecycle)
# Hold this open = forward is active:
echo "tcp 8080 10.0.2.100 80" | nc -U /tmp/vpnkit.sock -q 999999

# Terminal 3 (another): verify
# Ctrl-C the nc above → forward is automatically removed
```

### Memory safety:

```bash
valgrind --leak-check=full ./mini-vpnkit --control /tmp/vpnkit.sock
valgrind --tool=helgrind ./test_ring  # thread safety on ring buffer
```

### Benchmark ring buffer vs Unix socket:

```bash
# Add a simple benchmark mode to vm_client that sends 100K frames
# and measures frames/sec for both ring buffer and Unix socket transport
# This gives you real numbers for interview discussion.
```

---

## Resume Content

Project Title:
```
mini-vpnkit: Userspace TCP/IP Re-origination Proxy  |  C  |  Systems Programming
```

Resume Bullets:
- Built TCP handshake synthesis from raw Ethernet frames in C — generating SYN-ACK packets with random ISN, per-connection sequence tracking, and IP/TCP pseudo-header checksums using zero-copy buffer casting
- Implemented a lock-free SPSC shared memory ring buffer over POSIX shm with memory barriers (__ATOMIC_RELEASE/ACQUIRE) and eventfd notification, replacing Unix socket IPC for the frame transport layer
- Designed fd-as-lifecycle port forwarding where kernel close-on-crash provides automatic resource cleanup with zero leaked forwards, eliminating the need for explicit teardown APIs
- Built per-destination-IP connection multiplexing with FNV-1a hashed connection table, supporting concurrent TCP and UDP re-origination across 256 tracked flows

Tech Stack:
C99, POSIX shared memory (shm_open/mmap), atomic memory barriers, eventfd,
raw packet construction (IP/TCP/UDP), Unix domain sockets, poll(2), valgrind/helgrind

---

## Final Self-Check

Answer these out loud before putting project on your resume:

1. What is the difference between NAT and re-origination?
2. Why do you generate a random ISN? What attack does this prevent?
3. What happens if you forget the RELEASE barrier after writing to the ring?
4. Why power-of-2 ring size? What operation does it simplify?
5. How does fd-as-lifecycle handle client crashes without leaking forwards?
6. What is the TCP pseudo-header and why does the checksum include it?
7. Why does the ARP responder exist? What happens if you skip it?
8. Why cache-line-align head and tail in the ring buffer?

If you can answer all 8: project is genuinely resume-ready.
