#ifndef API_H
#define API_H

#include <slirp/libslirp.h>

typedef struct {
    int listen_fd;
    char path[256];
    Slirp *slirp;
} api_server_t;

int  api_server_init(api_server_t *api, const char *socket_path, Slirp *slirp);
int  api_server_fd(api_server_t *api);
void api_server_handle(api_server_t *api);
void api_server_destroy(api_server_t *api);

#endif