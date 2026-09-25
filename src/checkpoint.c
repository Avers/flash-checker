#include "flashcheck/checkpoint.h"

#include <fcntl.h>
#include <unistd.h>

#include "flashcheck/util.h"

static void cp_parse(const char *txt, checkpoint *cp)
{
    char stage[CP_STAGE_MAX] = { 0 };
    unsigned long long id = 0, off = 0, bw = 0, bv = 0, er = 0;
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
    cp->complete = complete;
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
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    n = snprintf(txt, sizeof txt,
                 "# %s %s\nstage=%s\ntest_id=%llu\noffset=%llu\nbytes_written=%llu\n"
                 "bytes_verified=%llu\nerrors=%llu\ncomplete=%d\n",
                 FC_PROG, FC_VERSION, cp->stage, (unsigned long long)cp->test_id,
                 (unsigned long long)cp->offset, (unsigned long long)cp->bytes_written,
                 (unsigned long long)cp->bytes_verified, (unsigned long long)cp->errors,
                 cp->complete);
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
