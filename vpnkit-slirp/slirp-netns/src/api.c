#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include "api.h"

#define API_BUF_SIZE 4096

#define MAX_FORWARDS 64

typedef struct {
    int id;
    int proto;
    char host_addr[16];
    int host_port;
    char guest_addr[16];
    int guest_port;
    int active;
} forward_entry_t;

static forward_entry_t forwards[MAX_FORWARDS];

int api_server_init(api_server_t *api, const char *socket_path, Slirp *slirp)
{
    memset(api, 0, sizeof(*api));
    api->slirp = slirp;
    strncpy(api->path, socket_path, sizeof(api->path) - 1);

    unlink(socket_path);

    api->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (api->listen_fd < 0) { perror("socket AF_UNIX"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(api->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind API socket"); return -1;
    }

    if (listen(api->listen_fd, 5) < 0) {
        perror("listen API socket"); return -1;
    }

    printf("API socket listening on %s\n", socket_path);
    return 0;
}

int api_server_fd(api_server_t *api)
{
    return api->listen_fd;
}

static const char *json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;

    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '"') p++;

    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < out_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return out;
}

static int json_get_int(const char *json, const char *key)
{
    char buf[32];
    if (!json_get_string(json, key, buf, sizeof(buf))) return -1;
    return atoi(buf);
}

static void handle_add_hostfwd(api_server_t *api, const char *json, int client_fd)
{
    char proto_str[8] = "tcp";
    char host_addr[16] = "0.0.0.0";
    char guest_addr[16] = "10.0.2.100";

    json_get_string(json, "proto", proto_str, sizeof(proto_str));
    json_get_string(json, "host_addr", host_addr, sizeof(host_addr));
    json_get_string(json, "guest_addr", guest_addr, sizeof(guest_addr));

    int host_port = json_get_int(json, "host_port");
    int guest_port = json_get_int(json, "guest_port");
    int is_udp = (strcmp(proto_str, "udp") == 0);

    if (host_port < 0 || guest_port < 0) {
        dprintf(client_fd, "{\"error\": \"missing host_port or guest_port\"}\n");
        return;
    }

    struct in_addr host_in, guest_in;
    inet_pton(AF_INET, host_addr, &host_in);
    inet_pton(AF_INET, guest_addr, &guest_in);

    int id = slirp_add_hostfwd(api->slirp, is_udp,
                                host_in, host_port,
                                guest_in, guest_port);

    if (id < 0) {
        dprintf(client_fd, "{\"error\": \"slirp_add_hostfwd failed\"}\n");
        return;
    }

    for (int i = 0; i < MAX_FORWARDS; i++) {
        if (!forwards[i].active) {
            forwards[i].id = id;
            forwards[i].proto = is_udp;
            strncpy(forwards[i].host_addr, host_addr, 15);
            forwards[i].host_port = host_port;
            strncpy(forwards[i].guest_addr, guest_addr, 15);
            forwards[i].guest_port = guest_port;
            forwards[i].active = 1;
            break;
        }
    }

    dprintf(client_fd, "{\"return\": {\"id\": %d}}\n", id);
    printf("Added port forward: %s %s:%d -> %s:%d (id=%d)\n",
           proto_str, host_addr, host_port, guest_addr, guest_port, id);
}

static void handle_remove_hostfwd(api_server_t *api, const char *json, int client_fd)
{
    int id = json_get_int(json, "id");
    if (id < 0) {
        dprintf(client_fd, "{\"error\": \"missing id\"}\n");
        return;
    }

    int removed = 0;
    for (int i = 0; i < MAX_FORWARDS; i++) {
        if (forwards[i].active && forwards[i].id == id) {
            forwards[i].active = 0;
            removed = 1;
            break;
        }
    }

    if (!removed) {
        dprintf(client_fd, "{\"error\": \"forward not found\"}\n");
        return;
    }

    dprintf(client_fd, "{\"return\": {}}\n");
}

static void handle_list_hostfwd(api_server_t *api, const char *json, int client_fd)
{
    (void)api; (void)json;

    dprintf(client_fd, "{\"return\": {\"entries\": [");
    int first = 1;
    for (int i = 0; i < MAX_FORWARDS; i++) {
        if (!forwards[i].active) continue;
        forward_entry_t *f = &forwards[i];
        dprintf(client_fd, "%s{\"id\": %d, \"proto\": \"%s\", "
                "\"host_addr\": \"%s\", \"host_port\": %d, "
                "\"guest_addr\": \"%s\", \"guest_port\": %d}",
                first ? "" : ", ", f->id,
                f->proto ? "udp" : "tcp",
                f->host_addr, f->host_port,
                f->guest_addr, f->guest_port);
        first = 0;
    }
    dprintf(client_fd, "]}}\n");
}

void api_server_handle(api_server_t *api)
{
    int client = accept(api->listen_fd, NULL, NULL);
    if (client < 0) { perror("accept API"); return; }

    char buf[API_BUF_SIZE];
    ssize_t total = 0;

    while (total < API_BUF_SIZE - 1) {
        ssize_t n = read(client, buf + total, API_BUF_SIZE - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    char execute[32] = {0};
    json_get_string(buf, "execute", execute, sizeof(execute));

    if (strcmp(execute, "add_hostfwd") == 0)
        handle_add_hostfwd(api, buf, client);
    else if (strcmp(execute, "remove_hostfwd") == 0)
        handle_remove_hostfwd(api, buf, client);
    else if (strcmp(execute, "list_hostfwd") == 0)
        handle_list_hostfwd(api, buf, client);
    else
        dprintf(client, "{\"error\": \"unknown command: %s\"}\n", execute);

    close(client);
}

void api_server_destroy(api_server_t *api)
{
    if (api->listen_fd >= 0) close(api->listen_fd);
    unlink(api->path);
}