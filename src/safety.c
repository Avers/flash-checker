#include "flashcheck/safety.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h>
#endif

#include "flashcheck/util.h"

static int suffix_matches(const char *s)
{
    if (*s == '\0')
        return 1;
    while (*s == 's') {
        s++;
        if (!isdigit((unsigned char)*s))
            return 0;
        while (isdigit((unsigned char)*s))
            s++;
    }
    if (*s == '\0')
        return 1;
    if (*s == 'p') {
        s++;
        if (!isdigit((unsigned char)*s))
            return 0;
        while (isdigit((unsigned char)*s))
            s++;
        return *s == '\0';
    }
    return 0;
}

int safety_mount_matches(const char *source, const char *path)
{
    const char *s = source;
    const char *name, *tail, *t;
    size_t n;

    while (*s == ' ' || *s == '\t')
        s++;
    if (strcmp(s, path) == 0)
        return 1;
    n = strlen(path);
    if (strncmp(s, path, n) != 0)
        return 0;
    tail = s + n;
    if (suffix_matches(tail))
        return 1;
    for (t = tail; *t != '\0'; t++)
        if (!isdigit((unsigned char)*t))
            return 0;
    name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    n = strlen(name);
    return n > 0 && !isdigit((unsigned char)name[n - 1]);
}

static int scan_mounts_proc(const char *path, char *detail, size_t dn)
{
    char line[1024], src[256], mnt[256];
    char rpath[PATH_MAX];
    const char *target = path;
    FILE *f = fopen("/proc/mounts", "r");
    int found = 0;

    if (realpath(path, rpath) != NULL)
        target = rpath;

    if (f == NULL)
        f = fopen("/proc/self/mounts", "r");
    if (f == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char rsrc[PATH_MAX];
        if (sscanf(line, "%255s %255s", src, mnt) != 2)
            continue;
        if (safety_mount_matches(src, path) || safety_mount_matches(src, target)) {
            snprintf(detail, dn, "mounted at: %.200s (%.64s)", mnt, src);
            found = 1;
            break;
        }
        if (realpath(src, rsrc) != NULL) {
            if (safety_mount_matches(rsrc, path) || safety_mount_matches(rsrc, target)) {
                snprintf(detail, dn, "mounted at: %.200s (%.64s)", mnt, src);
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

static int scan_swaps(const char *path, char *detail, size_t dn)
{
    char line[1024], src[256];
    char rpath[PATH_MAX];
    const char *target = path;
    FILE *f = fopen("/proc/swaps", "r");

    if (realpath(path, rpath) != NULL)
        target = rpath;

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char rsrc[PATH_MAX];
        if (sscanf(line, "%255s", src) != 1)
            continue;
        if (safety_mount_matches(src, path) || safety_mount_matches(src, target)) {
            snprintf(detail, dn, "active swap: %.200s", src);
            fclose(f);
            return 1;
        }
        if (realpath(src, rsrc) != NULL) {
            if (safety_mount_matches(rsrc, path) || safety_mount_matches(rsrc, target)) {
                snprintf(detail, dn, "active swap: %.200s", src);
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

static int scan_holders(const char *path, char *detail, size_t dn)
{
#ifdef __linux__
    char p[PATH_MAX];
    char rpath[PATH_MAX];
    const char *target = path;
    DIR *d;
    struct dirent *ent;

    if (realpath(path, rpath) != NULL)
        target = rpath;

    snprintf(p, sizeof p, "/sys/block/%s/holders", device_basename(target));
    d = opendir(p);
    if (d == NULL)
        return 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(detail, dn, "device in use by holder: %.200s", ent->d_name);
        closedir(d);
        return 1;
    }
    closedir(d);
#else
    (void)path;
    (void)detail;
    (void)dn;
#endif
    return 0;
}

#ifdef __APPLE__
static int scan_mounts_bsd(const char *path, char *detail, size_t dn)
{
    struct statfs *st;
    int n, found = 0;
    char rpath[PATH_MAX];
    const char *target = path;

    if (realpath(path, rpath) != NULL)
        target = rpath;

    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    st = calloc((size_t)n, sizeof *st);
    if (st == NULL)
        return 0;
    n = getfsstat(st, (size_t)n * sizeof *st, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        if (safety_mount_matches(st[i].f_mntfromname, path) ||
            safety_mount_matches(st[i].f_mntfromname, target)) {
            snprintf(detail, dn, "mounted at: %.200s (%.64s)", st[i].f_mntonname,
                     st[i].f_mntfromname);
            found = 1;
            break;
        }
    }
    free(st);
    return found;
}
#endif

int safety_is_mounted(const char *path, char *detail, size_t dn)
{
#ifdef __APPLE__
    if (scan_mounts_bsd(path, detail, dn))
        return 1;
#endif
    if (scan_mounts_proc(path, detail, dn))
        return 1;
    if (scan_swaps(path, detail, dn))
        return 1;
    if (scan_holders(path, detail, dn))
        return 1;
    detail[0] = '\0';
    return 0;
}

int safety_lock_device(int fd)
{
    unsigned i;

    if (fd < 0)
        return -1;
    /* udev re-probes a device right after it is written and holds a shared
       flock for a moment; a plain LOCK_NB would fail spuriously then. */
    for (i = 0; i < 10; i++) {
        int e;

        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return 0;
        e = errno;
        if (e != EWOULDBLOCK && e != EAGAIN)
            return -1;
        sleep_ms(200u);
        errno = e;
    }
    return -1;
}

void safety_plan(const config *c, const device_info *d)
{
    uint64_t end = c->limit != 0 ? FC_MIN(c->limit, d->capacity) : d->capacity;
    char a[64], b[64];

    fmt_size(a, sizeof a, c->offset);
    fmt_size(b, sizeof b, end);
    log_out("Plan:");
    log_out("  mode:           %s", mode_str(c->mode));
    log_out("  range:          %s .. %s (of %llu bytes)", a, b,
            (unsigned long long)d->capacity);
    log_out("  chunk size:     %s", (fmt_size(a, sizeof a, c->chunk_size), a));
    log_out("  window:         %s", (fmt_size(b, sizeof b, c->window_size), b));
    log_out("  pattern:        %s", pattern_kind_str(c->pattern));
    log_out("  passes:         %llu", (unsigned long long)c->passes);
}

int safety_check(const config *c, const device_info *d, int fd, char *err, size_t errn)
{
    char detail[512];

    (void)fd;
    if (!c->destructive) {
        snprintf(err, errn, "refusing to write: --destructive is required for any write test");
        return -1;
    }
    if (geteuid() != 0) {
        snprintf(err, errn, "raw block device access requires root (try: sudo)");
        return -1;
    }
    if (safety_is_mounted(d->path, detail, sizeof detail)) {
#ifdef __APPLE__
        snprintf(err, errn, "refusing to write: device is in use (%s); run: "
                            "diskutil unmountDisk %s", detail, d->path);
#else
        snprintf(err, errn, "refusing to write: device is in use (%s); unmount it first",
                 detail);
#endif
        return -1;
    }
    if (d->removable == 0 && !c->assume_yes) {
        snprintf(err, errn,
                 "refusing to write: %s is reported as a non-removable internal device; "
                 "re-run with --yes if this is intended",
                 d->path);
        return -1;
    }
    if (!c->assume_yes && !c->dry_run) {
        char ans[32];
        char cap[64];
        size_t len;

        fmt_size(cap, sizeof cap, d->capacity);
        log_out("");
        log_out("DESTRUCTIVE TEST: all data on %s will be destroyed.", d->path);
        if (d->vendor[0] || d->model[0])
            log_out("Target Device:    %s %s (%s, %s)",
                    d->vendor[0] ? d->vendor : "",
                    d->model[0] ? d->model : "",
                    cap,
                    d->removable == 1 ? "removable" : (d->removable == 0 ? "non-removable" : "unknown"));
        else
            log_out("Target Device:    %s (%s)", cap,
                    d->removable == 1 ? "removable" : (d->removable == 0 ? "non-removable" : "unknown"));
        log_out("Type 'yes' to continue: ");
        fflush(stdout);
        if (fgets(ans, sizeof ans, stdin) == NULL) {
            snprintf(err, errn, "aborted by user");
            return -1;
        }
        len = strlen(ans);
        while (len > 0 && (ans[len - 1] == '\n' || ans[len - 1] == '\r' || ans[len - 1] == ' ' || ans[len - 1] == '\t'))
            ans[--len] = '\0';
        if (strcasecmp(ans, "yes") != 0) {
            snprintf(err, errn, "aborted by user");
            return -1;
        }
        if (safety_is_mounted(d->path, detail, sizeof detail)) {
#ifdef __APPLE__
            snprintf(err, errn, "refusing to write: device was mounted during confirmation (%s); run: "
                                "diskutil unmountDisk %s", detail, d->path);
#else
            snprintf(err, errn, "refusing to write: device was mounted during confirmation (%s); unmount it first",
                     detail);
#endif
            return -1;
        }
    }
    return 0;
}
