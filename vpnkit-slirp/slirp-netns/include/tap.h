#ifndef TAP_H
#define TAP_H

#include <sys/types.h>

int tap_create(const char *name);
int tap_set_ip(const char *name, const char *ip, const char *mask);

#endif