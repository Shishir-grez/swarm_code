## Updated Complete Plan

```
Project 1: mini-slirp (link libslirp)
============================================

Day 1-2: Namespace entry + TAP setup (~200 LOC)
  - fork()
  - Child: open /proc/<pid>/ns/user, setns()
  - Child: open /proc/<pid>/ns/net, setns()
  - Child: open /dev/net/tun, TUNSETIFF with IFF_TAP
  - Child: configure interface via ioctl
  - Child: bring up interface + loopback
  - socketpair(AF_UNIX) for ready-fd signaling
  - Parent: poll() on exit-fd for child death

Day 3: libslirp integration (~150 LOC)
  - SlirpConfig: vnetwork, vhost, vdnssearch, MTU
  - slirp_new() with callbacks (send_packet, timer)
  - Main loop: poll TAP fd → slirp_input()
  - slirp_pollfds_fill/poll/pollfds_poll cycle

Day 4: DNS + connectivity test (~50 LOC)
  - Configure resolv.conf to 10.0.2.3
  - Test: curl from inside namespace works

Day 5-6: JSON port-forward API (~200 LOC)
  - Listen on Unix socket
  - Parse add_hostfwd / remove_hostfwd / list_hostfwd
  - Call slirp_add_hostfwd() / slirp_remove_hostfwd()
  - SHUT_WR per-request protocol
  - Write JSON response

Day 7: Testing + polish (~100 LOC)
  - Smoke tests: outbound TCP/UDP/DNS
  - Port forward round-trip

Subtotal: ~700 LOC, 7 days


Project 2: mini-vpnkit (hand-rolled re-origination)
=====================================================

Day 8: Unix socket frame transport (~150 LOC)
  - Server: listen on Unix socket
  - Accept one client (the "VM")
  - Read loop: 2-byte length prefix + Ethernet frame
  - Write: same framing for replies
  - EtherType dispatch: ARP / IPv4

Day 9: ARP responder (~100 LOC)
  - Parse ARP request (28 bytes)
  - Synthesize ARP reply for gateway MAC
  - Frame into Ethernet, send back

Day 10-12: TCP re-origination (~400 LOC)
  - Parse IPv4 header, TCP header
  - Connection table (hash map, 5-tuple key):
      struct: guest IP/port, host fd, ISN,
      snd_nxt, rcv_nxt, state enum
  - On guest SYN:
      connect() to real destination
      Success → build SYN-ACK (random ISN, MSS option)
      Fail → build RST
  - On ESTABLISHED + guest data:
      recv from TAP → send to host socket
      Build ACK back to guest
  - On host socket readable:
      recv → build TCP data packet to guest
  - On FIN/EOF: teardown
  - IP checksum + TCP pseudo-header checksum

Day 13: UDP re-origination (~100 LOC)
  - Parse UDP header
  - host SOCK_DGRAM → sendto → relay reply
  - Timeout-based cleanup

Day 14: Per-destination-IP multiplexing (~100 LOC)
  - Hash map: dst_ip → connection set
  - New IP = new virtual endpoint
  - This is the VPNKit model vs NAT model

Day 15: fd-as-lifecycle port forward (~100 LOC)
  - Control Unix socket
  - Client connects = add port forward
  - Bind host port, on accept → craft SYN into guest
  - Client disconnects = automatic cleanup

Day 16: Testing (~100 LOC)
  - Tiny C client that simulates a VM NIC
  - Verify outbound TCP, UDP, port forward inbound

Subtotal: ~1,050 LOC, 9 days


Project 3: Shared memory frame transport
==========================================

Replace Project 2's Unix socket transport with
a shared memory ring buffer between the "VM" process
and the mini-vpnkit process.

Day 17: Shared memory region setup (~100 LOC)
  - Create shared region: shm_open() + ftruncate() + mmap()
  - MAP_SHARED flag so both processes see same pages
  - Layout in shared memory:
      struct ring_header {
          uint32_t head;        // producer writes here
          uint32_t tail;        // consumer writes here
          uint32_t size;        // ring capacity
          uint8_t  data[];      // the actual ring buffer
      };
  - Two rings: one TX (VM→host), one RX (host→VM)
  - Total shared region: header + (2 × ring_size)
  - Second process attaches: shm_open() + mmap() same name

Day 18: Ring buffer operations (~150 LOC)
  - ring_write(ring, frame, frame_len):
      Check if enough space: (head - tail) < size
      If full: return EAGAIN (backpressure)
      Write 2-byte length prefix at head offset
      Write frame bytes after it
      Memory barrier: __atomic_store_n(&ring->head, new_head, __ATOMIC_RELEASE)
  - ring_read(ring, buf, buf_len):
      Check if data available: head != tail
      If empty: return EAGAIN
      Read 2-byte length at tail offset
      Read frame bytes
      Memory barrier: __atomic_store_n(&ring->tail, new_tail, __ATOMIC_RELEASE)
  - Handle wrap-around: ring is circular,
      use modulo arithmetic for offsets

Day 19: Notification mechanism (~100 LOC)
  - When ring is empty, consumer must sleep
  - Option A: eventfd — producer writes 1 byte
      to wake consumer, consumer polls on eventfd
      alongside other fds in the event loop
  - Option B: futex — consumer calls
      futex(FUTEX_WAIT) on head, producer calls
      futex(FUTEX_WAKE) after writing
  - eventfd is simpler and integrates with your
      existing poll()/epoll() loop — use this
  - Create two eventfds: one for each direction
  - Producer: write to eventfd after ring_write
  - Consumer: epoll on eventfd, call ring_read when triggered

Day 20: Integration with mini-vpnkit (~100 LOC)
  - Replace the Unix socket read/write path:
      Old: recv(unix_sock) → parse frame
      New: ring_read(rx_ring) → parse frame
      Old: send(unix_sock, frame)
      New: ring_write(tx_ring, frame) + eventfd notify
  - Everything above this layer stays the same:
      ARP responder, TCP re-origination,
      UDP, connection table, port forwards
  - The "VM client" test program also switches
      to ring_write/ring_read instead of socket send/recv

Day 21: Verification + benchmarking (~100 LOC)
  - Correctness test:
      Send 1000 frames through ring buffer
      Verify all arrive, in order, data intact
      Verify curl from simulated guest still works
  - Race condition test:
      Run producer and consumer on separate cores
      Blast frames at max speed
      Check for torn reads, duplicates, corruption
  - Benchmark: measure frames/sec and latency
      Compare: Unix socket transport vs shared memory
      You want real numbers for interview discussion
  - Edge cases:
      Ring full → verify backpressure works
      Producer crashes → verify consumer doesn't hang
      Consumer slow → verify no data loss

Subtotal: ~550 LOC, 5 days
```

## Topic + Subtopic Additions for Shared Memory

### Block 6 — Shared Memory IPC (Project 3)

**6.1 mmap and Shared Regions**
- `shm_open()` — creates a named POSIX shared memory object in `/dev/shm`
- `ftruncate()` — set the size of the shared region
- `mmap()` — map the region into process virtual address space
- `MAP_SHARED` vs `MAP_PRIVATE` — shared means writes visible to other process, private means copy-on-write
- `munmap()` + `shm_unlink()` — cleanup
- **Interview depth**: Explain what happens in the page table when two processes mmap the same region. How does the kernel ensure both see the same physical pages?

**6.2 Memory Ordering and Barriers**
- Why barriers are needed — CPU and compiler can reorder reads/writes for performance
- Without barriers: consumer might read stale head value, see garbage data
- `__atomic_store_n` with `__ATOMIC_RELEASE` — all writes before this are visible before the store
- `__atomic_load_n` with `__ATOMIC_ACQUIRE` — all reads after this see writes from before the release
- Release/acquire pair — producer releases head update, consumer acquires it, guarantees frame data is visible
- `__ATOMIC_SEQ_CST` — strongest ordering, simpler but slower, fine for learning
- Compiler barriers vs CPU barriers — `volatile` is not enough, you need atomic operations
- **Interview depth**: This is asked at Google/Microsoft systems rounds. Explain what happens without a memory barrier on x86 vs ARM. Draw the happens-before relationship between producer and consumer.

**6.3 Ring Buffer Design**
- Circular buffer with power-of-2 size — allows bitwise AND for modulo instead of division
- Head and tail in separate cache lines — avoid false sharing where producer and consumer contend on the same cache line
- `__attribute__((aligned(64)))` — align to cache line boundary (64 bytes on most x86)
- Single producer, single consumer — no locks needed, just atomic head/tail updates
- Multi-producer or multi-consumer — would need CAS (compare-and-swap), not needed here
- Backpressure — when ring is full, producer either blocks or drops, you choose the policy
- **Interview depth**: Implement a lock-free SPSC ring buffer. Explain false sharing. Explain why power-of-2 sizing helps. This is a very common interview question.

**6.4 Notification Mechanisms**
- Polling (busy-wait) — check head != tail in a loop, lowest latency, wastes CPU
- `eventfd` — lightweight kernel notification, integrates with epoll, one write = one wakeup
- `futex` — userspace fast path (no syscall if not contended), kernel slow path for sleeping
- Adaptive — busy-spin for N iterations, then fall back to eventfd/futex sleep
- **Interview depth**: Compare polling vs interrupt-driven notification. When is busy-wait acceptable? Explain futex fast path vs slow path.

**6.5 Benchmarking and Verification**
- Throughput — frames per second, measure with `clock_gettime(CLOCK_MONOTONIC)`
- Latency — timestamp in shared memory before write, read after read, take difference
- Correctness — sequence numbers in frames, verify at consumer side
- Stress test — producer at max speed, verify no data loss or corruption
- **Interview depth**: How would you benchmark IPC mechanisms? What metrics matter? How do you detect a torn read?

---

## Revised Grand Total

```
Project 1 (slirp, libslirp):          ~700 LOC,   7 days
Project 2 (VPNKit, hand-rolled):     ~1,050 LOC,  9 days
Project 3 (shared memory transport):   ~550 LOC,   5 days
                                     ----------   ------
Total:                               ~2,300 LOC,  21 days
```

## Revised Resume Line

```
mini-slirp: Userspace network gateway for unprivileged Linux containers
- Implemented Linux namespace entry (setns/clone), TAP device management,
  and cross-namespace IPC via socketpair in C
- Integrated libslirp for TCP/IP NAT with custom JSON control API
  over Unix domain sockets for dynamic port forwarding
- Technologies: C, Linux namespaces, TAP/TUN, libslirp, epoll

mini-vpnkit: Userspace TCP/IP re-origination proxy with shared memory transport
- Built TCP handshake synthesis (SYN-ACK generation, sequence tracking)
  and connection splicing from raw Ethernet frames in C
- Replaced Unix socket IPC with lock-free shared memory ring buffer
  using mmap, atomic operations, and eventfd notification,
  achieving Nx throughput improvement over socket baseline
- Implemented per-destination-IP connection multiplexing and
  fd-lifecycle-based port forwarding with automatic crash cleanup
- Technologies: C, mmap, lock-free ring buffer, memory barriers,
  TCP/IP internals, Unix domain sockets
```

## Dependency Order

```
Project 1 (slirp) is fully independent
Project 2 (VPNKit) is fully independent of Project 1
Project 3 (shared memory) depends on Project 2
  — it replaces Project 2's transport layer

Build order: 1 → 2 → 3
  or:        2 → 3 → 1

Either works. Project 1 is easiest (libslirp does the heavy lifting).
Project 2 is hardest (hand-rolled TCP). Project 3 is most
interview-relevant for systems design questions.
```