#ifndef FLASHCHECK_DEVICE_H
#define FLASHCHECK_DEVICE_H

#include "flashcheck/common.h"

#define DEV_STR 128

typedef struct {
    char path[DEV_STR];
    char name[64];
    uint64_t capacity;
    uint32_t logical_sector;
    uint32_t physical_sector;
    int removable;
    int direct_supported;
    char vendor[DEV_STR];
    char model[DEV_STR];
    char serial[DEV_STR];
    char rev[DEV_STR];
    char usb_vidpid[32];
    char usb_product[DEV_STR];
    char transport[64];
} device_info;

int device_probe(const char *path, device_info *out, char *err, size_t errn);
const char *device_basename(const char *path);
void device_print_info(const device_info *d);
void device_json(const device_info *d, void *ctx, int indent);

#endif
