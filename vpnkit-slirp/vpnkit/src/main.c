#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include "ring.h"
#include "conn.h"
#include "tcp_orig.h"
#include "udp_orig.h"
#include "portfwd.h"
#include "ethernet.h"
#include "arp.h"

static ring_t g_rx_ring;
static ring_t g_tx_ring;
static conn_table_t g_conn_table;
static portfwd_server_t g_portfwd;

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--portfwd PATH] RX_SHM TX_SHM\n", prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    const char *portfwd_path = NULL;
    const char *rx_shm = "/vpnkit-rx";
    const char *tx_shm = "/vpnkit-tx";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--portfwd") && i + 1 < argc)
            portfwd_path = argv[++i];
        else if (rx_shm == NULL)
            rx_shm = argv[i];
        else if (tx_shm == NULL)
            tx_shm = argv[i];
        else
            usage(argv[0]);
    }

    conn_table_init(&g_conn_table);

    ring_create(&g_rx_ring, rx_shm);
    ring_create(&g_tx_ring, tx_shm);

    if (portfwd_path) {
        portfwd_init(&g_portfwd, portfwd_path, &g_tx_ring, &g_conn_table);
    }

    printf("mini-vpnkit running. RX=%s TX=%s\n", rx_shm, tx_shm);

    uint8_t frame[2048];

    while (1) {
        struct pollfd pfds[4];
        int nfds = 0;

        pfds[nfds].fd = ring_event_fd(&g_rx_ring);
        pfds[nfds++].events = POLLIN;

        if (portfwd_path) {
            pfds[nfds].fd = ring_event_fd(&g_tx_ring);
            pfds[nfds++].events = POLLIN;
            pfds[nfds].fd = portfwd_control_fd(&g_portfwd);
            pfds[nfds++].events = POLLIN;
        }

        int ret = poll(pfds, nfds, 100);
        if (ret <= 0) {
            conn_reap_expired(&g_conn_table, 60);
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            int n = ring_read(&g_rx_ring, frame, sizeof(frame));
            if (n > 0) {
                uint8_t *eth_type, *payload;
                size_t payload_len;
                eth_parse(frame, n, &eth_type, &payload, &payload_len);

                uint16_t type = (eth_type[0] << 8) | eth_type[1];

                if (type == 0x0806) {
                    uint8_t reply[128];
                    int rlen = arp_handle(frame, n, reply, sizeof(reply));
                    if (rlen > 0) {
                        ring_write(&g_rx_ring, reply, (uint16_t)rlen);
                        ring_notify(&g_rx_ring);
                    }
                }
                else if (type == 0x0800) {
                    const uint8_t *ip = payload;
                    if (ip[0] >> 4 == 4) {
                        uint8_t proto = ip[9];
                        if (proto == 6) {
                            tcp_handle(frame, n, &g_conn_table, &g_tx_ring);
                        }
                        else if (proto == 17) {
                            udp_handle(frame, n, &g_conn_table, &g_tx_ring);
                        }
                    }
                }
            }
        }

        tcp_poll_host(&g_conn_table, &g_tx_ring);

        if (portfwd_path && (pfds[2].revents & POLLIN)) {
            portfwd_handle_new(&g_portfwd);
        }
        if (portfwd_path) {
            portfwd_poll(&g_portfwd);
        }
    }

    if (portfwd_path) portfwd_destroy(&g_portfwd);
    ring_destroy(&g_rx_ring, rx_shm);
    ring_destroy(&g_tx_ring, tx_shm);
    return 0;
}