#ifndef SLIRP_WRAP_H
#define SLIRP_WRAP_H

#include <slirp/libslirp.h>

typedef struct {
    Slirp *slirp;
    int tap_fd;
    int running;
} slirp_ctx_t;

int  slirp_ctx_init(slirp_ctx_t *ctx, int tap_fd);
void slirp_ctx_run(slirp_ctx_t *ctx);
void slirp_ctx_stop(slirp_ctx_t *ctx);
Slirp *slirp_ctx_get_slirp(slirp_ctx_t *ctx);

#endif