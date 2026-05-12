#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "namespace.h"
#include "slirp_wrap.h"
#include "api.h"

static slirp_ctx_t g_ctx;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--api-socket PATH] [--configure] PID TAPNAME\n"
        "Example: %s --api-socket /tmp/api.sock $(cat /tmp/pid) tap0\n",
        prog, prog);
    exit(1);
}

static void handle_sigint(int sig)
{
    (void)sig;
    slirp_ctx_stop(&g_ctx);
}

int main(int argc, char *argv[])
{
    const char *api_path = NULL;
    const char *tap_name = NULL;
    pid_t target_pid = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--api-socket") && i + 1 < argc)
            api_path = argv[++i];
        else if (!strcmp(argv[i], "--configure"))
            ;
        else if (target_pid == 0)
            target_pid = (pid_t)atoi(argv[i]);
        else if (!tap_name)
            tap_name = argv[i];
        else
            usage(argv[0]);
    }

    if (target_pid <= 0 || !tap_name) usage(argv[0]);

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    ns_result_t ns;
    if (ns_enter(target_pid, tap_name, &ns) < 0) {
        fprintf(stderr, "Failed to enter namespace of PID %d\n", target_pid);
        return 1;
    }

    if (slirp_ctx_init(&g_ctx, ns.tap_fd) < 0) {
        fprintf(stderr, "Failed to initialize libslirp\n");
        return 1;
    }

    api_server_t api;
    int has_api = 0;
    if (api_path) {
        if (api_server_init(&api, api_path, slirp_ctx_get_slirp(&g_ctx)) < 0) {
            fprintf(stderr, "Failed to initialize API server\n");
            return 1;
        }
        has_api = 1;
    }

    printf("mini-slirp running. PID=%d TAP=%s\n", target_pid, tap_name);
    if (has_api) printf("API socket: %s\n", api_path);

    slirp_ctx_run(&g_ctx);

    if (has_api) api_server_destroy(&api);
    return 0;
}