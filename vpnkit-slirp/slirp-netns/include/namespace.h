#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sys/types.h>

typedef struct {
    int tap_fd;
    int ready_fd;
    int exit_fd;
    pid_t child_pid;
} ns_result_t;

int ns_enter(pid_t target_pid, const char *tap_name, ns_result_t *result);

#endif