#ifndef FLASHCHECK_SAFETY_H
#define FLASHCHECK_SAFETY_H

#include "flashcheck/common.h"
#include "flashcheck/config.h"
#include "flashcheck/device.h"

typedef struct {
    int require_root;
    int locked_fd;
} safety_state;

int safety_check(const config *c, const device_info *d, int fd, char *err, size_t errn);
int safety_is_mounted(const char *path, char *detail, size_t dn);
int safety_lock_device(int fd);
void safety_plan(const config *c, const device_info *d);

#endif
