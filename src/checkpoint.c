#include "flashcheck/checkpoint.h"

#include <fcntl.h>
#include <unistd.h>

#include "flashcheck/util.h"

static void cp_parse(const char *txt, checkpoint *cp)
{
    char stage[CP_STAGE_MAX] = { 0 };
    unsigned long long id = 0, off = 0, bw = 0, bv = 0, er = 0;
    unsigned long long dev = 0, fingerprint = 0, cap = 0, start = 0, limit = 0, chunk = 0;
    int pattern = -1, mode = -1;
    double ws = 0, rs = 0;
    int complete = 0;
    const char *p = txt;

    while (p != NULL && *p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        char line[512];

        if (len > 0 && len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';
            if (sscanf(line, "stage=%31s", stage) == 1) {
            } else if (sscanf(line, "test_id=%llu", &id) == 1) {
            } else if (sscanf(line, "offset=%llu", &off) == 1) {
            } else if (sscanf(line, "bytes_written=%llu", &bw) == 1) {
            } else if (sscanf(line, "bytes_verified=%llu", &bv) == 1) {
            } else if (sscanf(line, "errors=%llu", &er) == 1) {
            } else if (sscanf(line, "device_id=%llu", &dev) == 1) {
            } else if (sscanf(line, "device_fingerprint=%llu", &fingerprint) == 1) {
            } else if (sscanf(line, "capacity=%llu", &cap) == 1) {
            } else if (sscanf(line, "tested_start=%llu", &start) == 1) {
            } else if (sscanf(line, "tested_limit=%llu", &limit) == 1) {
            } else if (sscanf(line, "chunk_size=%llu", &chunk) == 1) {
            } else if (sscanf(line, "pattern=%d", &pattern) == 1) {
            } else if (sscanf(line, "mode=%d", &mode) == 1) {
            } else if (sscanf(line, "write_sec=%lf", &ws) == 1) {
                cp->write_sec = ws;
            } else if (sscanf(line, "read_sec=%lf", &rs) == 1) {
                cp->read_sec = rs;
            } else if (sscanf(line, "complete=%d", &complete) == 1) {
            }
        }
        p = eol != NULL ? eol + 1 : NULL;
    }
    snprintf(cp->stage, sizeof cp->stage, "%s", stage);
    cp->test_id = id;
    cp->offset = off;
    cp->bytes_written = bw;
    cp->bytes_verified = bv;
    cp->errors = er;
    cp->device_id = dev;
    cp->device_fingerprint = fingerprint;
    cp->capacity = cap;
    cp->tested_start = start;
    cp->tested_limit = limit;
    cp->chunk_size = chunk;
    cp->pattern = pattern;
    cp->mode = mode;
    cp->write_sec = ws;
    cp->read_sec = rs;
    cp->complete = complete;
}

int checkpoint_matches(const checkpoint *cp, uint64_t device_id, uint64_t device_fingerprint,
                       uint64_t capacity,
                       uint64_t tested_start, uint64_t tested_limit, uint64_t chunk_size,
                       int pattern, int mode)
{
    uint64_t end;

    if (chunk_size == 0 || tested_start > capacity)
        return 0;
    end = tested_limit != 0 ? FC_MIN(tested_limit, capacity) : capacity;
    end = (end / chunk_size) * chunk_size;
    return cp->test_id != 0 && cp->device_id != 0 && device_fingerprint != 0 &&
           cp->device_id == device_id &&
           cp->device_fingerprint == device_fingerprint && cp->capacity == capacity &&
           cp->tested_start == tested_start && cp->tested_limit == tested_limit &&
           cp->chunk_size == chunk_size && cp->pattern == pattern && cp->mode == mode &&
           cp->offset >= tested_start && cp->offset <= end &&
           (cp->offset - tested_start) % chunk_size == 0 && (!cp->complete || cp->offset == end);
}

int checkpoint_load(const char *path, checkpoint *cp)
{
    char txt[4096];
    int fd;

    memset(cp, 0, sizeof *cp);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    {
        ssize_t n = read(fd, txt, sizeof txt - 1);
        close(fd);
        if (n <= 0)
            return -1;
        txt[n] = '\0';
    }
    cp_parse(txt, cp);
    return cp->stage[0] != '\0' ? 0 : -1;
}

int checkpoint_save(const char *path, const checkpoint *cp)
{
    char tmp[512];
    char txt[1024];
    int fd;
    int n;

    if (path == NULL)
        return 0;
    n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
n = snprintf(txt, sizeof txt,
             "# %s %s\nstage=%s\ntest_id=%llu\noffset=%llu\nbytes_written=%llu\n"
             "bytes_verified=%llu\nerrors=%llu\ndevice_id=%llu\ndevice_fingerprint=%llu\n"
             "capacity=%llu\n"
             "tested_start=%llu\ntested_limit=%llu\nchunk_size=%llu\npattern=%d\nmode=%d\n"
             "write_sec=%.3f\nread_sec=%.3f\ncomplete=%d\n",
             FC_PROG, FC_VERSION, cp->stage, (unsigned long long)cp->test_id,
             (unsigned long long)cp->offset, (unsigned long long)cp->bytes_written,
             (unsigned long long)cp->bytes_verified, (unsigned long long)cp->errors,
             (unsigned long long)cp->device_id, (unsigned long long)cp->device_fingerprint,
             (unsigned long long)cp->capacity,
             (unsigned long long)cp->tested_start, (unsigned long long)cp->tested_limit,
             (unsigned long long)cp->chunk_size, cp->pattern, cp->mode,
             cp->write_sec, cp->read_sec, cp->complete);
    if (n < 0 || (size_t)n >= sizeof txt || write(fd, txt, (size_t)n) != n) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
