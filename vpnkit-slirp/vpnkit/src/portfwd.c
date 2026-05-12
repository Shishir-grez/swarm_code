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

    if (bind(pf->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return -1;
    }
    if (listen(pf->listen_fd, 5) < 0) { perror("listen"); return -1; }

    printf("Port forward server listening on %s\n", socket_path);
    return 0;
}

int portfwd_control_fd(portfwd_server_t *pf) { return pf->listen_fd; }

void portfwd_handle_new(portfwd_server_t *pf)
{
    int client = accept(pf->listen_fd, NULL, NULL);
    if (client < 0) return;

    for (int i = 0; i < MAX_PORT_FORWARDS; i++) {
        if (!pf->fwds[i].active) {
            pf->fwds[i].active = 1;
            pf->fwds[i].control_fd = client;
            pf->fwds[i].host_port = 8080;
            pf->fwds[i].guest_ip = 0x6400020A; 
            pf->fwds[i].guest_port = 80;
            printf("Port forward added: host port %d -> guest\n", pf->fwds[i].host_port);
            return;
        }
    }
    close(client);
}

void portfwd_poll(portfwd_server_t *pf)
{
    for (int i = 0; i < MAX_PORT_FORWARDS; i++) {
        if (!pf->fwds[i].active) continue;

        struct pollfd pfd = { .fd = pf->fwds[i].control_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) <= 0) continue;

        char buf[1];
        ssize_t n = read(pf->fwds[i].control_fd, buf, sizeof(buf));
        if (n <= 0) {
            printf("Port forward removed\n");
            close(pf->fwds[i].control_fd);
            pf->fwds[i].active = 0;
        }
    }
}

void portfwd_destroy(portfwd_server_t *pf)
{
    for (int i = 0; i < MAX_PORT_FORWARDS; i++) {
        if (pf->fwds[i].active) close(pf->fwds[i].control_fd);
    }
    if (pf->listen_fd >= 0) close(pf->listen_fd);
    unlink(pf->path);
}