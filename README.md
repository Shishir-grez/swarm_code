# vxlan-vpnkit-slirp

Small Linux networking prototypes written in C. The repository rebuilds three
pieces of container and VM networking from first principles:

- `vpnkit-slirp/slirp-netns`: a minimal namespace-to-userspace gateway using
  TAP and `libslirp`.
- `vpnkit-slirp/vpnkit`: a minimal VPNKit-style proxy using raw Ethernet
  frames, shared-memory rings, and host socket re-origination.
- `vxlan/vtep`: a small VXLAN tunnel endpoint with a learning forwarding
  database (FDB).

This is an educational codebase, not a production network stack. The source
uses fixed-size tables, simplified protocol handling, static configuration,
and privileged Linux interfaces. Read the current limitations before running
it on a real network.

## Architecture

```text
slirp-netns
  process in network namespace
        | Ethernet frames
       TAP
        | file descriptor
  mini-slirp -> libslirp -> host sockets -> outside network

vpnkit
  vm_client <-> Unix socket handshake + SCM_RIGHTS
       |                     |
       +-- shared memory rings + eventfds -- mini-vpnkit
                                              |
                                      host TCP/UDP sockets

vtep
  TAP <-> VTEP <-> UDP/4789 <-> VTEP <-> TAP
                 VXLAN header + Ethernet frame
```

The projects are independent. `vpnkit` and `slirp-netns` demonstrate two
different ways to connect a guest to a host network; `vtep` demonstrates
Layer-2-over-Layer-3 forwarding between hosts.

## Repository Layout

```text
.
+-- vpnkit-slirp/
|   +-- common.md
|   +-- slirp-netns/
|   |   +-- include/
|   |   +-- src/
|   |   +-- Makefile
|   |   `-- mini-slirp-guide.md
|   `-- vpnkit/
|       +-- include/
|       +-- src/
|       +-- tools/vm_client.c
|       +-- tests/test_ring.c
|       +-- Makefile
|       `-- mini-vpnkit-guide.md
`-- vxlan/vtep/
    +-- include/
    +-- src/
    +-- tests/
    `-- Makefile
```

The two `mini-*guide.md` files are implementation and learning notes. This
README describes the behavior of the code currently checked into the
repository; some guide sections describe planned or incomplete work.

## Requirements

The projects target Linux. Native Windows builds are not supported because
the code depends on Linux namespaces, `/dev/net/tun`, TAP ioctls, Unix-domain
socket file descriptors, `eventfd`, and POSIX shared memory.

Install the toolchain and dependencies on Debian or Ubuntu:

```bash
sudo apt update
sudo apt install build-essential pkg-config iproute2 util-linux \
    libslirp-dev libglib2.0-dev curl python3 tcpdump
```

You also need:

- a working `/dev/net/tun` device;
- permission to create and configure TAP interfaces (`root` or
  `CAP_NET_ADMIN`);
- permission to enter the target process's user and network namespaces for
  `mini-slirp`;
- a reachable underlay network and UDP port 4789 for a multi-host VTEP test.

WSL2 may work if its kernel exposes the required devices and namespace
features, but this repository does not configure WSL2 for you.

## Build

Run each Makefile from the repository root:

```bash
make -C vpnkit-slirp/slirp-netns
make -C vpnkit-slirp/vpnkit
make -C vxlan/vtep
```

The resulting programs are:

| Directory | Program | Purpose |
| --- | --- | --- |
| `vpnkit-slirp/slirp-netns` | `mini-slirp` | TAP-backed `libslirp` gateway |
| `vpnkit-slirp/vpnkit` | `mini-vpnkit` | Shared-memory Ethernet proxy |
| `vpnkit-slirp/vpnkit` | `vm_client` | Test client that simulates a VM NIC |
| `vxlan/vtep` | `vtep` | VXLAN tunnel endpoint |

Clean build outputs with:

```bash
make -C vpnkit-slirp/slirp-netns clean
make -C vpnkit-slirp/vpnkit clean
make -C vxlan/vtep clean
```

The VTEP clean target also removes the tracked prebuilt files `vtep`,
`test_vxlan`, and `test_fdb` that are present in this checkout. Regenerate them
with the build/test targets above if you use `make clean`.

## Tests

The VXLAN and FDB tests are available in the repository:

```bash
make -C vxlan/vtep test_vxlan test_fdb
./vxlan/vtep/test_vxlan
./vxlan/vtep/test_fdb
```

The VPNKit ring-buffer test is also available:

```bash
make -C vpnkit-slirp/vpnkit test_ring
./vpnkit-slirp/vpnkit/test_ring
```

The `slirp-netns` Makefile contains `test_tap` and `test_api` targets, but the
corresponding test source files are not currently present. The VPNKit
Makefile contains `test_packet` and `test_conn` targets, but those test source
files are also absent. Those targets should be treated as planned test hooks,
not as currently runnable tests.

## `mini-slirp`

### What It Does

`mini-slirp` models the basic architecture of rootless container networking:

1. Fork a helper process.
2. Enter the target process's user namespace, then its network namespace with
   `setns(2)`.
3. Create an `IFF_TAP | IFF_NO_PI` interface in that namespace.
4. Configure the TAP interface and default route.
5. Pass the TAP file descriptor back to the parent with `SCM_RIGHTS`.
6. Feed frames from the TAP into `libslirp`.
7. Write frames emitted by `libslirp` back to the TAP interface.

The parent runs a `poll(2)` loop over the TAP descriptor and host-side
descriptors requested by `libslirp`. `libslirp` supplies the userspace IPv4,
TCP, UDP, and DNS/NAT behavior; this project does not implement those
protocols itself.

### Virtual Network

The current source uses these fixed values:

| Value | Address |
| --- | --- |
| Guest/TAP address | `10.0.2.100/24` |
| Virtual gateway | `10.0.2.2` |
| Virtual DNS server | `10.0.2.3` |
| Virtual network | `10.0.2.0/24` |

The namespace helper brings up loopback, assigns the TAP address, adds the
default route through `10.0.2.2`, and attempts to bind-mount a resolver
configuration that names `10.0.2.3`. The helper does not enter the target
process's mount namespace and ignores failure from the resolver mount command,
so this step is not safely isolated by the current implementation.

### Command Line

```text
mini-slirp [--api-socket PATH] [--configure] PID TAPNAME
```

- `PID` identifies a live process whose user and network namespaces are used.
- `TAPNAME` is the interface name to create inside that namespace.
- `--api-socket PATH` creates the Unix-domain control socket described below.
- `--configure` is accepted for compatibility, but configuration is always
  performed by the current implementation.

### Basic Experiment

Create an isolated namespace in one terminal:

```bash
unshare --user --map-root-user --net --mount sh
ip link show
```

For the resolver mount to remain inside the isolated mount namespace, start
the gateway from that shell (or otherwise isolate the mount namespace of the
process that runs it):

```bash
./vpnkit-slirp/slirp-netns/mini-slirp $$ tap0 &
```

Back in the namespace, inspect the interface and try outbound connectivity:

```bash
ip addr show tap0
ip route
curl -I https://example.com
```

The `--mount` option is important for this experiment. Verify
`/etc/resolv.conf` before relying on DNS, because the helper's bind-mount
command is best-effort.

### Control API Status

`src/api.c` contains a small JSON-over-Unix-socket handler with these intended
operations:

- `add_hostfwd`: add a TCP or UDP host-to-guest forward;
- `remove_hostfwd`: remove a tracked forward by ID;
- `list_hostfwd`: list tracked forwards.

The bookkeeping array can hold up to 64 forwards.

Requests are read until the client half-closes its write side with
`shutdown(SHUT_WR)`, then the server writes one JSON response and closes the
connection.

This API is not end-to-end usable in the current tree. `main.c` creates the
API listener, but `slirp_ctx_run()` does not add `api_server_fd()` to its poll
set or call `api_server_handle()`. Also, the current remove handler only
updates its bookkeeping array and does not call `slirp_remove_hostfwd()`.

## `mini-vpnkit`

### What It Does

`mini-vpnkit` models the user-space networking approach historically used by
VPNKit-style systems. It receives complete Ethernet frames, handles a small
subset of ARP/IPv4/TCP/UDP, and opens independent host sockets. It therefore
re-originates connections instead of rewriting packets in place:

```text
Guest TCP connection  <->  mini-vpnkit  <->  independent host TCP connection
```

The TCP connection table stores the guest 4-tuple, guest MAC, sequence state,
host socket, and last-activity time. Host-to-guest data is converted back into
Ethernet/IP/TCP frames. UDP uses a short-lived host socket and a five-second
entry timeout.

### Shared-Memory Transport

The server creates two one-megabyte POSIX shared-memory rings and listens on
`/tmp/vpnkit.sock`. A client connects to that Unix socket and receives the two
eventfds through separate `SCM_RIGHTS` messages.

| Shared-memory object | Writer | Reader | Server-side variable |
| --- | --- | --- | --- |
| `/vpnkit-rx` | VM client | `mini-vpnkit` | `g_rx_ring` |
| `/vpnkit-tx` | `mini-vpnkit` | VM client | `g_tx_ring` |

Each ring stores frames as:

```text
[2-byte little-endian frame length][frame bytes]...
```

The ring is single-producer/single-consumer. Atomic acquire/release
operations publish the head and tail indexes; eventfds are only wake-up
notifications and do not carry frame data. The current loops do not drain the
eventfds after notification, and `vm_client` polls the ring with sleeps rather
than waiting on its eventfd, so the notification path is only partially wired.

### Command Line

```text
mini-vpnkit [--portfwd PATH]
```

Without `--portfwd`, the daemon waits for one client on the fixed socket
`/tmp/vpnkit.sock`. The shared-memory names are also fixed, so only one
instance should use them at a time. With `--portfwd PATH`, the daemon still
waits for the VM handshake first; the `PATH` control socket is created only
after a client connects to `/tmp/vpnkit.sock`.

### VM Client Experiment

Start a host service on the address hard-coded by `vm_client`:

```bash
python3 -m http.server 8080
```

In another terminal, start the proxy, then start its test client:

```bash
./vpnkit-slirp/vpnkit/mini-vpnkit
./vpnkit-slirp/vpnkit/vm_client
```

The client connects to `/tmp/vpnkit.sock`, receives the eventfds, attaches to
the fixed rings, sends an ARP request for `10.0.2.2`, performs a TCP handshake,
and requests `http://127.0.0.1:8080/`. It is a protocol test harness, not a
general VM or TAP backend.

### Port Forwarding Status

`--portfwd PATH` enables a Unix control listener implemented in
`src/portfwd.c`. The current implementation accepts a control connection and
uses the connection lifetime as the resource lifetime, but it does not create
the host listener or inject a guest-side TCP flow. The host port and guest
destination are currently hard-coded to `8080` and `10.0.2.100:80`.

Treat this option as an unfinished demonstration of fd-as-lifecycle cleanup,
not as a working port-forward service.

The current VPNKit capacities are 256 TCP connection entries, 64 UDP entries,
32 port-forward slots, and 1 MiB per shared-memory ring. Packet builders use
1514-byte output buffers, so larger host-to-guest TCP or UDP frames cannot be
constructed by the current implementation.

## `vtep`

### What It Does

`vtep` connects a TAP interface to a VXLAN-over-UDP data plane:

```text
TAP frame
  -> prepend 8-byte VXLAN header
  -> send UDP datagram to a peer on port 4789

UDP datagram
  -> validate VXLAN I flag
  -> remove the VXLAN header without copying
  -> inject the inner Ethernet frame into TAP
```

The outer IP and UDP headers are created by the kernel UDP socket. The project
builds only the VXLAN header and the inner Ethernet payload.

### Command Line

```text
vtep --tap NAME --ip IP/PREFIX --vni VNI [--peers IP,IP,...]
```

Example configuration for two hosts with reachable underlay addresses:

```bash
# Host A
sudo ./vxlan/vtep/vtep \
    --tap vtap0 --ip 10.0.0.1/24 --vni 42 --peers 192.168.1.20

# Host B
sudo ./vxlan/vtep/vtep \
    --tap vtap0 --ip 10.0.0.2/24 --vni 42 --peers 192.168.1.10
```

Use the same VNI and an overlay address range appropriate for both TAP
interfaces. The VNI is encoded as a 24-bit big-endian value. The command-line
program rejects zero but does not reject values above `0xFFFFFF`; the encoder
silently keeps only the low 24 bits. Use a value from `1` through `0xFFFFFF`.

The process binds UDP port 4789 on all local IPv4 addresses. Every peer in
`--peers` is assigned port 4789. A maximum of 32 outbound peers is accepted.
The peer list is not an inbound allow-list: the VTEP accepts valid VXLAN
datagrams from any UDP source and can learn that source in the FDB.

### Forwarding Database

The FDB learns the source MAC of every accepted inbound frame and associates
it with the sender's UDP address. Outbound behavior is:

1. Known destination MAC: send one VXLAN datagram to the learned VTEP.
2. Unknown or broadcast destination MAC: send to every configured peer
   (head-end replication).

The FDB has 256 shards selected by the last MAC byte. Each shard has 128
entries protected by a read/write lock. Entries expire after 300 seconds and
an age thread scans every 30 seconds. If a shard is full, the oldest entry is
replaced.

This implementation has no multicast control plane, EVPN, peer discovery,
encryption, or replay protection. Inbound packets are checked for the VXLAN I
flag, but the parsed VNI is not compared with `vtep->vni`; a received packet
with another valid VNI can therefore be injected into TAP.

## Interface Reference

| Component | Interface | Current value |
| --- | --- | --- |
| `mini-slirp` | TAP network | `10.0.2.0/24` |
| `mini-slirp` | Gateway | `10.0.2.2` |
| `mini-slirp` | DNS | `10.0.2.3` |
| `mini-vpnkit` | Handshake socket | `/tmp/vpnkit.sock` |
| `mini-vpnkit` | RX ring | `/vpnkit-rx` |
| `mini-vpnkit` | TX ring | `/vpnkit-tx` |
| `vtep` | VXLAN UDP port | `4789` |
| `vtep` | Maximum static peers | `32` |

## Current Limitations

- Linux only; no Windows, macOS, or portable networking backend.
- IPv4-focused examples and parsers; no complete IPv6, DHCP, or ICMP
  implementation.
- `mini-vpnkit` implements a teaching subset of TCP rather than a complete TCP
  stack. Retransmission, window management, and many edge cases are absent.
- TCP connection establishment performs a blocking `connect(2)` in the main
  packet-processing path; the intended asynchronous connect path is not used.
- Several packet parsers trust header lengths supplied by the input frame.
  Do not expose these programs to untrusted traffic.
- TAP setup uses shell commands through `system(3)` and requires suitable
  privileges.
- The slirp API and VPNKit port-forward path are incomplete as described above.
- VTEP peers are static and unauthenticated; VXLAN traffic is not encrypted.
- The event loops run indefinitely and have limited shutdown/error cleanup.
- Stopping `mini-slirp` does not kill or reap its paused namespace helper, and
  the helper does not clean up the resolver mount.
- There is currently no `LICENSE` file in the repository. Check the project
  owner's intended licensing before redistributing the code.

## Troubleshooting

### `open /dev/net/tun` fails

Check that the TUN device exists and that the process has permission to use
it:

```bash
ls -l /dev/net/tun
sudo modprobe tun
```

### Namespace entry fails

Check that the PID is still alive, that `/proc/<PID>/ns/user` and
`/proc/<PID>/ns/net` are readable, and that the process was created with the
namespace features required by the example. Use `--mount` with `unshare` so
the resolver bind mount is isolated.

### VTEPs cannot exchange frames

Check all of the following between hosts:

```bash
ping <peer-underlay-ip>
sudo ss -lunp | grep 4789
sudo tcpdump -ni any udp port 4789
```

The peers must use reachable underlay addresses, UDP 4789 must be allowed by
the host and network firewalls, and both TAP interfaces must use the same VNI
and compatible overlay addressing.

### `vm_client` cannot connect

Start `mini-vpnkit` first. The client expects the fixed path `/tmp/vpnkit.sock`
and fixed shared-memory objects. Remove stale processes, then start a single
fresh server and client pair. If the TCP test times out,
confirm that a service is listening on `127.0.0.1:8080`.

The small C test programs print `[PASS]` and `[FAIL]` lines but currently
return status zero from `main()` even when a check fails. Inspect their output
instead of relying only on the shell exit status. FDB tests may also wait for
the 30-second aging thread interval while `fdb_destroy()` joins the thread.

## Further Reading

- [`vpnkit-slirp/common.md`](vpnkit-slirp/common.md): project plan and shared
  concepts.
- [`mini-slirp` guide](vpnkit-slirp/slirp-netns/mini-slirp-guide.md):
  namespace, TAP, libslirp, and control-plane notes.
- [`mini-vpnkit` guide](vpnkit-slirp/vpnkit/mini-vpnkit-guide.md): shared
  memory, packet parsing, and re-origination notes.
- [RFC 7348: Virtual eXtensible Local Area Network](https://www.rfc-editor.org/rfc/rfc7348)
- [libslirp](https://gitlab.freedesktop.org/slirp/libslirp)
