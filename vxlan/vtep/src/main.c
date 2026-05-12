#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vtep.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --tap NAME --ip IP/PREFIX --vni VNI [--peers IP,IP,...]\n"
        "Example: %s --tap vtap0 --ip 10.0.0.1/24 --vni 42 "
        "--peers 192.168.1.2\n",
        prog, prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    const char *tap_name  = NULL;
    const char *tap_ip    = NULL;
    const char *peers_str = NULL;
    uint32_t    vni       = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--tap")   && i+1 < argc) tap_name  = argv[++i];
        else if (!strcmp(argv[i], "--ip")    && i+1 < argc) tap_ip    = argv[++i];
        else if (!strcmp(argv[i], "--vni")   && i+1 < argc) vni       = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peers") && i+1 < argc) peers_str = argv[++i];
        else usage(argv[0]);
    }

    if (!tap_name || !tap_ip || !vni) usage(argv[0]);

    vtep_t vtep;
    if (vtep_init(&vtep, tap_name, tap_ip, vni) < 0) {
        fprintf(stderr, "VTEP init failed\n");
        return 1;
    }

    if (peers_str) {
        char buf[256];
        strncpy(buf, peers_str, sizeof(buf) - 1);
        char *peer = strtok(buf, ",");
        while (peer) {
            vtep_add_peer(&vtep, peer);
            peer = strtok(NULL, ",");
        }
    }

    printf("VTEP running — VNI=%u  TAP=%s  IP=%s\n", vni, tap_name, tap_ip);
    vtep_run(&vtep);   /* blocks forever */

    vtep_destroy(&vtep);
    return 0;
}
