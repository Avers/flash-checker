#include "flashcheck/safety.h"

#include <ctype.h>
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
    FILE *f = fopen("/proc/mounts", "r");
    int found = 0;

    if (f == NULL)
        f = fopen("/proc/self/mounts", "r");
    if (f == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (sscanf(line, "%255s %255s", src, mnt) != 2)
            continue;
        if (safety_mount_matches(src, path)) {
            snprintf(detail, dn, "mounted at: %.200s (%.64s)", mnt, src);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int scan_swaps(const char *path, char *detail, size_t dn)
{
    char line[1024];
    FILE *f = fopen("/proc/swaps", "r");

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, path) != NULL) {
            snprintf(detail, dn, "active swap: %.200s", line);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int scan_holders(const char *path, char *detail, size_t dn)
{
    char p[PATH_MAX];
    char ent[256];
    int fd;
    ssize_t n;

    snprintf(p, sizeof p, "/sys/block/%s/holders", device_basename(path));
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, ent, sizeof ent - 1);
    close(fd);
    if (n <= 0)
        return 0;
    ent[n] = '\0';
    snprintf(detail, dn, "device in use by holder: %.200s", ent);
    return 1;
}

#ifdef __APPLE__
static int scan_mounts_bsd(const char *path, char *detail, size_t dn)
{
    struct statfs *st;
    int n, found = 0;

    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    st = calloc((size_t)n, sizeof *st);
    if (st == NULL)
        return 0;
    n = getfsstat(st, (size_t)n * sizeof *st, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        if (safety_mount_matches(st[i].f_mntfromname, path)) {
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
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        return -1;
    return 0;
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
        char ans[16];
        log_out("");
        log_out("DESTRUCTIVE TEST: all data on %s will be destroyed.", d->path);
        log_out("Type 'yes' to continue: ");
        fflush(stdout);
        if (fgets(ans, sizeof ans, stdin) == NULL || strncmp(ans, "yes", 3) != 0) {
            snprintf(err, errn, "aborted by user");
            return -1;
        }
    }
    return 0;
}
