#include "flashcheck/safety.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flashcheck/util.h"

static int line_is_device(const char *line, const char *path)
{
    const char *p = line;
    const char *name = device_basename(path);
    int hit = 0;

    while (*p == ' ')
        p++;
    if (strncmp(p, path, strlen(path)) == 0) {
        char after = p[strlen(path)];
        if (after == '\0' || after == ' ' || after == '\t')
            return 1;
    }
    if (strcmp(name, "device") == 0)
        return 0;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "/dev/", 5) != 0)
        return 0;
    p += 5;
    if (strncmp(p, name, strlen(name)) != 0)
        return 0;
    p += strlen(name);
    if (*p == '\0')
        hit = 1;
    else if (isdigit((unsigned char)*p))
        hit = 1;
    return hit;
}

int safety_is_mounted(const char *path, char *detail, size_t dn)
{
    char buf[65536];
    FILE *f = fopen("/proc/self/mountinfo", "r");
    size_t r;
    int found = 0;
    int fd;

    if (f != NULL) {
        r = fread(buf, 1, sizeof buf - 1, f);
        buf[r] = '\0';
        fclose(f);
        for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
            if (line_is_device(line, path)) {
                snprintf(detail, dn, "mounted at: %.200s", line);
                found = 1;
                break;
            }
        }
    }
    if (found)
        return 1;

    f = fopen("/proc/swaps", "r");
    if (f != NULL) {
        r = fread(buf, 1, sizeof buf - 1, f);
        buf[r] = '\0';
        fclose(f);
        f = NULL;
        for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
            if (strstr(line, path) != NULL) {
                snprintf(detail, dn, "active swap: %.200s", line);
                return 1;
            }
        }
    }

    {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/sys/block/%s/holders", device_basename(path));
        fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char ent[256];
            ssize_t n = read(fd, ent, sizeof ent - 1);
            close(fd);
            if (n > 0) {
                ent[n] = '\0';
                snprintf(detail, dn, "device in use by holder: %.200s", ent);
                return 1;
            }
        }
    }
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
        snprintf(err, errn, "refusing to write: device is in use (%s)", detail);
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
