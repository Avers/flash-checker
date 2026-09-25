#include "flashcheck/device.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/disk.h>
#endif

#include "flashcheck/io.h"
#include "flashcheck/util.h"

#ifdef __linux__
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
#define BLKSSZGET _IO(0x12, 104)
#endif
#ifndef BLKPBSZGET
#define BLKPBSZGET _IO(0x12, 123)
#endif
#endif

const char *device_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void sysfs_path(char *buf, size_t n, const char *name, const char *rel)
{
    if (rel != NULL && *rel != '\0')
        snprintf(buf, n, "/sys/block/%s/%s", name, rel);
    else
        snprintf(buf, n, "/sys/block/%s", name);
}

static void read_sysfs_str(const char *name, const char *rel, const char *alt, char *out,
                           size_t outn)
{
    char p[PATH_MAX];
    sysfs_path(p, sizeof p, name, rel);
    if (read_file(p, out, outn) != 0 && alt != NULL) {
        snprintf(p, sizeof p, "/sys/block/%s/%s", name, alt);
        if (read_file(p, out, outn) != 0)
            out[0] = '\0';
    }
}

static void find_usb(const char *name, device_info *d)
{
    char base[PATH_MAX];
    char cur[PATH_MAX];
    int depth;

    snprintf(base, sizeof base, "/sys/block/%s", name);
    if (realpath(base, cur) == NULL)
        return;
    for (depth = 0; depth < 8; depth++) {
        char p[PATH_MAX + 32];
        char v[16];
        char pid[16];
        char *slash;

        snprintf(p, sizeof p, "%s/idVendor", cur);
        if (read_file(p, v, sizeof v) == 0) {
            snprintf(p, sizeof p, "%s/idProduct", cur);
            if (read_file(p, pid, sizeof pid) == 0)
                snprintf(d->usb_vidpid, sizeof d->usb_vidpid, "%.15s:%.15s", v, pid);
            snprintf(p, sizeof p, "%s/product", cur);
            if (read_file(p, d->usb_product, sizeof d->usb_product) != 0)
                d->usb_product[0] = '\0';
        }
        snprintf(p, sizeof p, "%s/driver", cur);
        if (read_file(p, v, sizeof v) == 0 && v[0] != '\0' && d->transport[0] == '\0')
            snprintf(d->transport, sizeof d->transport, "%.15s", v);

        slash = strrchr(cur, '/');
        if (slash == NULL || slash == cur)
            break;
        *slash = '\0';
    }
}

int device_probe(const char *path, device_info *d, char *err, size_t errn)
{
    struct stat st;
    int fd = -1;
    int e = 0;
    char p[PATH_MAX];
    uint64_t v = 0;

    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    snprintf(d->name, sizeof d->name, "%s", device_basename(path));

    if (stat(path, &st) != 0) {
        snprintf(err, errn, "cannot stat %s: %s", path, strerror(errno));
        return -1;
    }
    if (!S_ISBLK(st.st_mode)) {
        snprintf(err, errn, "%s is not a block device", path);
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errn, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    d->capacity = 0;
    d->logical_sector = 0;
    d->physical_sector = 0;
#ifdef __APPLE__
    {
        uint32_t block_size = 512;
        uint64_t block_count = 0;
        if (ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) != 0)
            block_size = 512;
        d->logical_sector = d->physical_sector = block_size;
        if (ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count) == 0)
            d->capacity = block_count * (uint64_t)block_size;
    }
#else
    if (ioctl(fd, BLKGETSIZE64, &d->capacity) != 0)
        d->capacity = 0;
    if (ioctl(fd, BLKSSZGET, &d->logical_sector) != 0 || d->logical_sector == 0)
        d->logical_sector = 0;
    if (ioctl(fd, BLKPBSZGET, &d->physical_sector) != 0)
        d->physical_sector = 0;
#endif
    close(fd);

    if (d->capacity == 0) {
        if (stat(path, &st) == 0)
            d->capacity = (uint64_t)st.st_blocks * 512u;
    }
    if (d->capacity == 0) {
        sysfs_path(p, sizeof p, d->name, "size");
        if (read_file(p, err, errn) == 0 && parse_u64(err, &v) == 0)
            d->capacity = v * 512u;
    }
    if (d->capacity == 0) {
        snprintf(err, errn, "cannot determine capacity of %s", path);
        return -1;
    }
    if (d->logical_sector == 0) {
        sysfs_path(p, sizeof p, d->name, "queue/logical_block_size");
        if (read_file(p, err, errn) != 0 || parse_u64(err, &v) != 0)
            v = 512;
        d->logical_sector = (uint32_t)v;
    }
    if (d->physical_sector == 0) {
        sysfs_path(p, sizeof p, d->name, "queue/physical_block_size");
        if (read_file(p, err, errn) != 0 || parse_u64(err, &v) != 0)
            v = d->logical_sector;
        d->physical_sector = (uint32_t)v;
    }

    sysfs_path(p, sizeof p, d->name, "removable");
    if (read_file(p, err, errn) == 0 && parse_u64(err, &v) == 0)
        d->removable = (int)v;
    else
        d->removable = -1;

    read_sysfs_str(d->name, "device/vendor", "device/vendor", d->vendor, sizeof d->vendor);
    read_sysfs_str(d->name, "device/model", "device/model", d->model, sizeof d->model);
    if (d->model[0] == '\0')
        read_sysfs_str(d->name, "device/name", NULL, d->model, sizeof d->model);
    read_sysfs_str(d->name, "device/../serial", NULL, d->serial, sizeof d->serial);
    if (d->serial[0] == '\0')
        read_sysfs_str(d->name, "device/rev", NULL, d->serial, sizeof d->serial);
    find_usb(d->name, d);
    if (d->transport[0] == '\0')
        snprintf(d->transport, sizeof d->transport, "unknown");

    io_sync_probe_direct(path, &d->direct_supported, &e);
    return 0;
}

static const char *na(const char *s) { return (s != NULL && s[0] != '\0') ? s : "n/a"; }

void device_print_info(const device_info *d)
{
    char cap[64], sec[64];

    fmt_size(cap, sizeof cap, d->capacity);
    snprintf(sec, sizeof sec, "%u B", d->logical_sector);

    log_out("Device:");
    log_out("  path:           %s", d->path);
    if (d->usb_vidpid[0] != '\0')
        log_out("  usb:            %s  %s", d->usb_vidpid, na(d->usb_product));
    if (d->vendor[0] != '\0' || d->model[0] != '\0')
        log_out("  model:          %s %s", na(d->vendor), na(d->model));
    if (d->serial[0] != '\0')
        log_out("  serial:         %s", d->serial);
    log_out("  reported size:  %s (%llu bytes)", cap, (unsigned long long)d->capacity);
    log_out("  logical block:  %s", sec);
    log_out("  transport:      %s", na(d->transport));
    log_out("  removable:      %s", d->removable == 1 ? "yes" : (d->removable == 0 ? "no" : "?"));
    log_out("  O_DIRECT:       %s", d->direct_supported ? "supported" : "not supported");
}

void device_json(const device_info *d, void *ctx, int indent)
{
    char cap[64];
    FILE *o = ctx;

    fmt_size(cap, sizeof cap, d->capacity);
    fprintf(o, "%*s\"device\": {\n", indent, "");
    fprintf(o, "%*s  \"path\": \"%s\",\n", indent, "", d->path);
    fprintf(o, "%*s  \"usb_vid_pid\": \"%s\",\n", indent, "", na(d->usb_vidpid));
    fprintf(o, "%*s  \"usb_product\": \"%s\",\n", indent, "", na(d->usb_product));
    fprintf(o, "%*s  \"vendor\": \"%s\",\n", indent, "", na(d->vendor));
    fprintf(o, "%*s  \"model\": \"%s\",\n", indent, "", na(d->model));
    fprintf(o, "%*s  \"serial\": \"%s\",\n", indent, "", na(d->serial));
    fprintf(o, "%*s  \"transport\": \"%s\",\n", indent, "", na(d->transport));
    fprintf(o, "%*s  \"removable\": %d,\n", indent, "", d->removable);
    fprintf(o, "%*s  \"logical_block_size\": %u,\n", indent, "", d->logical_sector);
    fprintf(o, "%*s  \"physical_block_size\": %u,\n", indent, "", d->physical_sector);
    fprintf(o, "%*s  \"o_direct_supported\": %s,\n", indent, "",
            d->direct_supported ? "true" : "false");
    fprintf(o, "%*s  \"reported_capacity_bytes\": %llu,\n", indent, "",
            (unsigned long long)d->capacity);
    fprintf(o, "%*s  \"reported_capacity\": \"%s\"\n", indent, "", cap);
    fprintf(o, "%*s}", indent, "");
}
