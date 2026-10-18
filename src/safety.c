#include "flashcheck/safety.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/wait.h>
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
    if (n <= 0) {
        snprintf(detail, dn, "cannot inspect mounted filesystems; refusing unsafe write");
        return -1;
    }
    st = calloc((size_t)n, sizeof *st);
    if (st == NULL) {
        snprintf(detail, dn, "cannot inspect mounted filesystems; refusing unsafe write");
        return -1;
    }
    n = getfsstat(st, (size_t)n * sizeof *st, MNT_NOWAIT);
    if (n < 0) {
        free(st);
        snprintf(detail, dn, "cannot inspect mounted filesystems; refusing unsafe write");
        return -1;
    }
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

static int scan_apfs_backing(const char *path, char *detail, size_t dn)
{
    char command[PATH_MAX + 64];
    char line[1024], target[128];
    char container[64] = "";
    FILE *f;
    int saw_header = 0;

    {
        const char *base = strrchr(path, '/');
        snprintf(target, sizeof target, "%s", base != NULL ? base + 1 : path);
    }
    if (strncmp(target, "rdisk", 5) == 0)
        memmove(target, target + 1, strlen(target));
    snprintf(command, sizeof command, "/usr/sbin/diskutil apfs list 2>/dev/null");
    f = popen(command, "r");
    if (f == NULL) {
        snprintf(detail, dn, "cannot inspect APFS storage relationships; refusing unsafe write");
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char *p;
        if (strstr(line, "APFS Container") != NULL)
            saw_header = 1;
        p = strstr(line, "APFS Container Reference:");
        if (p != NULL) {
            p = strchr(p, ':') + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (sscanf(p, "%63[a-zA-Z0-9]", container) != 1)
                container[0] = '\0';
        }
        p = strstr(line, "Physical Store");
        if (p != NULL) {
            char store[128];
            const char *q = strrchr(line, ' ');
            if (q != NULL && sscanf(q + 1, "%127[a-zA-Z0-9]", store) == 1 &&
                (strcmp(store, target) == 0 ||
                 safety_mount_matches(store, target))) {
                struct statfs *st;
                char cpath[96];
                int n = getfsstat(NULL, 0, MNT_NOWAIT);
                if (container[0] == '\0') {
                    pclose(f);
                    snprintf(detail, dn, "APFS container identity is unclear; refusing unsafe write");
                    return -1;
                }
                snprintf(cpath, sizeof cpath, "/dev/%.80s", container);
                if (n <= 0 || (st = calloc((size_t)n, sizeof *st)) == NULL) {
                    pclose(f);
                    snprintf(detail, dn, "cannot inspect mounted APFS volumes; refusing unsafe write");
                    return -1;
                }
                n = getfsstat(st, (size_t)n * sizeof *st, MNT_NOWAIT);
                if (n < 0) {
                    free(st);
                    pclose(f);
                    snprintf(detail, dn, "cannot inspect mounted APFS volumes; refusing unsafe write");
                    return -1;
                }
                for (int i = 0; i < n; i++) {
                    if (safety_mount_matches(st[i].f_mntfromname, cpath)) {
                        snprintf(detail, dn, "physical store backs mounted APFS container %.63s",
                                 container);
                        free(st);
                        pclose(f);
                        return 1;
                    }
                }
                free(st);
            }
        }
    }
    {
        int status = pclose(f);
        if (!saw_header || status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            snprintf(detail, dn, "cannot verify APFS storage relationships; refusing unsafe write");
            return -1;
        }
    }
    return 0;
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

#ifdef __linux__
static int add_sysfs_node(char nodes[][PATH_MAX], size_t *count, const char *path)
{
    char resolved[PATH_MAX];

    if (realpath(path, resolved) == NULL)
        return -1;
    for (size_t i = 0; i < *count; i++)
        if (strcmp(nodes[i], resolved) == 0)
            return 0;
    if (*count >= 128)
        return -1;
    snprintf(nodes[(*count)++], PATH_MAX, "%s", resolved);
    return 0;
}

static int linux_topology_in_use(dev_t id, char *detail, size_t dn)
{
    char nodes[128][PATH_MAX];
    size_t count = 0;
    char start[PATH_MAX];
    FILE *mounts;

    snprintf(start, sizeof start, "/sys/dev/block/%u:%u", major(id), minor(id));
    if (add_sysfs_node(nodes, &count, start) != 0) {
        snprintf(detail, dn, "cannot resolve device in sysfs; refusing unsafe write");
        return -1;
    }
    /* Include the parent disk and all sibling partitions for partition targets. */
    {
        char part[PATH_MAX], disk[PATH_MAX];
        FILE *f;
        snprintf(part, sizeof part, "%s/partition", nodes[0]);
        snprintf(disk, sizeof disk, "%s", nodes[0]);
        f = fopen(part, "r");
        if (f != NULL) {
            char *slash;
            fclose(f);
            slash = strrchr(disk, '/');
            if (slash == NULL || slash == disk) {
                snprintf(detail, dn, "cannot locate parent disk; refusing unsafe write");
                return -1;
            }
            *slash = '\0';
            if (add_sysfs_node(nodes, &count, disk) != 0) {
                snprintf(detail, dn, "cannot inspect parent disk; refusing unsafe write");
                return -1;
            }
        } else if (errno != ENOENT) {
            snprintf(detail, dn, "cannot inspect device partitions; refusing unsafe write");
            return -1;
        }
        {
            DIR *d = opendir(disk);
            struct dirent *ent;
            if (d == NULL) {
                snprintf(detail, dn, "cannot inspect device partitions; refusing unsafe write");
                return -1;
            }
            while ((ent = readdir(d)) != NULL) {
                char child[PATH_MAX], marker[PATH_MAX];
                if (ent->d_name[0] == '.')
                    continue;
                if (snprintf(child, sizeof child, "%s/%s", disk, ent->d_name) >=
                    (int)sizeof child)
                    continue;
                snprintf(marker, sizeof marker, "%s/partition", child);
                if (access(marker, F_OK) == 0 && add_sysfs_node(nodes, &count, child) != 0) {
                    closedir(d);
                    snprintf(detail, dn, "device topology is too large to inspect safely");
                    return -1;
                }
            }
            closedir(d);
        }
    }
    mounts = fopen("/proc/self/mountinfo", "r");
    if (mounts == NULL) {
        snprintf(detail, dn, "cannot inspect mounted block devices; refusing unsafe write");
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        char p[PATH_MAX], devnum[64];
        FILE *f;
        snprintf(p, sizeof p, "%s/dev", nodes[i]);
        f = fopen(p, "r");
        if (f == NULL || fgets(devnum, sizeof devnum, f) == NULL) {
            if (f != NULL)
                fclose(f);
            fclose(mounts);
            snprintf(detail, dn, "cannot read block-device topology; refusing unsafe write");
            return -1;
        }
        fclose(f);
        devnum[strcspn(devnum, "\r\n")] = '\0';
        rewind(mounts);
        {
            char line[4096];
            while (fgets(line, sizeof line, mounts) != NULL) {
                char *save = NULL, *tok;
                int field = 0;
                for (tok = strtok_r(line, " ", &save); tok != NULL;
                     tok = strtok_r(NULL, " ", &save), field++) {
                    if (field == 2 && strcmp(tok, devnum) == 0) {
                        snprintf(detail, dn, "device or dependent partition is mounted (%s)",
                                 devnum);
                        fclose(mounts);
                        return 1;
                    }
                }
            }
        }
        {
            unsigned node_major, node_minor;
            if (sscanf(devnum, "%u:%u", &node_major, &node_minor) != 2) {
                fclose(mounts);
                snprintf(detail, dn, "cannot parse block-device identity; refusing unsafe write");
                return -1;
            }
            FILE *swaps = fopen("/proc/swaps", "r");
            char line[1024], source[256];
            if (swaps == NULL) {
                fclose(mounts);
                snprintf(detail, dn, "cannot inspect active swap devices; refusing unsafe write");
                return -1;
            }
            while (fgets(line, sizeof line, swaps) != NULL) {
                struct stat swap_st;
                if (sscanf(line, "%255s", source) != 1 || source[0] != '/')
                    continue;
                if (stat(source, &swap_st) == 0 && S_ISBLK(swap_st.st_mode) &&
                    major(swap_st.st_rdev) == node_major &&
                    minor(swap_st.st_rdev) == node_minor) {
                    fclose(swaps);
                    fclose(mounts);
                    snprintf(detail, dn, "device or dependent holder is active swap (%s)", source);
                    return 1;
                }
            }
            fclose(swaps);
        }
        {
            const char *edges[] = { "holders", "slaves" };
            for (size_t e = 0; e < sizeof edges / sizeof edges[0]; e++) {
                DIR *d;
                struct dirent *ent;
                if (snprintf(p, sizeof p, "%s/%s", nodes[i], edges[e]) >= (int)sizeof p) {
                    fclose(mounts);
                    snprintf(detail, dn, "device topology path is too long to inspect safely");
                    return -1;
                }
                d = opendir(p);
                if (d == NULL) {
                    fclose(mounts);
                    snprintf(detail, dn, "cannot inspect block-device dependencies; refusing unsafe write");
                    return -1;
                }
                while ((ent = readdir(d)) != NULL) {
                    char dependency[PATH_MAX];
                    if (ent->d_name[0] == '.')
                        continue;
                    if (snprintf(dependency, sizeof dependency, "%s/%s", p, ent->d_name) >=
                            (int)sizeof dependency ||
                        add_sysfs_node(nodes, &count, dependency) != 0) {
                        closedir(d);
                        fclose(mounts);
                        snprintf(detail, dn, "device dependency topology is too large to inspect safely");
                        return -1;
                    }
                }
                closedir(d);
            }
        }
    }
    fclose(mounts);
    return 0;
}
#endif

int safety_is_mounted_fd(int fd, const char *path, char *detail, size_t dn)
{
    struct stat st;

    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode)) {
        snprintf(detail, dn, "cannot verify the opened block device; refusing unsafe write");
        return -1;
    }
#ifdef __linux__
    {
        int r = linux_topology_in_use(st.st_rdev, detail, dn);
        if (r != 0)
            return r;
    }
#endif
#ifdef __APPLE__
    {
        int r = scan_apfs_backing(path, detail, dn);
        if (r != 0)
            return r;
    }
#endif
    if (safety_is_mounted(path, detail, dn))
        return 1;
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

    if (!c->destructive) {
        snprintf(err, errn, "refusing to write: --destructive is required for any write test");
        return -1;
    }
    if (geteuid() != 0) {
        snprintf(err, errn, "raw block device access requires root (try: sudo)");
        return -1;
    }
    if (safety_is_mounted_fd(fd, d->path, detail, sizeof detail) != 0) {
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
        if (safety_is_mounted_fd(fd, d->path, detail, sizeof detail) != 0) {
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
