#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <glib.h>
#include <slirp/libslirp.h>
#include "slirp_wrap.h"

typedef struct {
    SlirpTimerId id;
    void *cb_opaque;
} timer_data_t;

static ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
    slirp_ctx_t *ctx = (slirp_ctx_t *)opaque;
    return write(ctx->tap_fd, buf, len);
}

static void cb_guest_error(const char *msg, void *opaque)
{
    (void)opaque;
    fprintf(stderr, "libslirp guest error: %s\n", msg);
}

static int64_t cb_clock_get_ns(void *opaque)
{
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *cb_timer_new(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    (void)opaque;
    timer_data_t *t = calloc(1, sizeof(*t));
    t->id = id;
    t->cb_opaque = cb_opaque;
    return t;
}

static void cb_timer_free(void *timer, void *opaque)
{
    (void)opaque;
    free(timer);
}

static void cb_timer_mod(void *timer, int64_t expire_time, void *opaque)
{
    (void)timer; (void)expire_time; (void)opaque;
}

static void cb_notify(void *opaque)
{
    (void)opaque;
}

static SlirpCb slirp_callbacks = {
    .send_packet       = cb_send_packet,
    .guest_error       = cb_guest_error,
    .clock_get_ns      = cb_clock_get_ns,
    .timer_new         = cb_timer_new,
    .timer_free        = cb_timer_free,
    .timer_mod         = cb_timer_mod,
    .notify            = cb_notify,
};

int slirp_ctx_init(slirp_ctx_t *ctx, int tap_fd)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->tap_fd = tap_fd;
    ctx->running = 1;

    SlirpConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 4;
    cfg.restricted = 0;
    cfg.in_enabled = 1;

    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);

    ctx->slirp = slirp_new(&cfg, &slirp_callbacks, ctx);
    if (!ctx->slirp) {
        fprintf(stderr, "slirp_new failed\n");
        return -1;
    }

    printf("libslirp initialized: network=10.0.2.0/24 gw=10.0.2.2 dns=10.0.2.3\n");
    return 0;
}

static GPollFD g_pollfds[256];
static int g_pollfds_count = 0;
static int g_pollfds_capacity = 256;

static int add_poll_cb(int fd, int events, void *opaque)
{
    (void)opaque;
    if (g_pollfds_count >= g_pollfds_capacity) return -1;
    g_pollfds[g_pollfds_count].fd = fd;
    g_pollfds[g_pollfds_count].events = events;
    g_pollfds[g_pollfds_count].revents = 0;
    return g_pollfds_count++;
}

static int get_revents_cb(int idx, void *opaque)
{
    (void)opaque;
    if (idx < 0 || idx >= g_pollfds_count) return 0;
    return g_pollfds[idx].revents;
}

void slirp_ctx_run(slirp_ctx_t *ctx)
{
    uint8_t buf[65536];

    while (ctx->running) {
        uint32_t timeout_ms = 0;
        g_pollfds_count = 0;

        slirp_pollfds_fill(ctx->slirp, &timeout_ms, add_poll_cb, NULL);

        int tap_idx = g_pollfds_count;
        if (g_pollfds_count < g_pollfds_capacity) {
            g_pollfds[g_pollfds_count].fd = ctx->tap_fd;
            g_pollfds[g_pollfds_count].events = POLLIN;
            g_pollfds[g_pollfds_count].revents = 0;
            g_pollfds_count++;
        }

        int ret = g_poll(g_pollfds, g_pollfds_count, timeout_ms ? timeout_ms : 100);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (tap_idx < g_pollfds_count && (g_pollfds[tap_idx].revents & POLLIN)) {
            ssize_t n = read(ctx->tap_fd, buf, sizeof(buf));
            if (n > 0) {
                slirp_input(ctx->slirp, buf, (int)n);
            }
        }

        slirp_pollfds_poll(ctx->slirp, ret < 0, get_revents_cb, NULL);
    }
}

void slirp_ctx_stop(slirp_ctx_t *ctx)
{
    ctx->running = 0;
}

Slirp *slirp_ctx_get_slirp(slirp_ctx_t *ctx)
{
    return ctx->slirp;
}