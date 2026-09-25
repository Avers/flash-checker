#include "flashcheck/util.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static log_level g_level = LOG_NORMAL;

const char *verdict_str(verdict v)
{
    switch (v) {
    case VERDICT_PASS:
        return "PASS";
    case VERDICT_FAIL:
        return "FAIL";
    default:
        return "INCONCLUSIVE";
    }
}

void log_set_level(log_level l) { g_level = l; }
log_level log_get_level(void) { return g_level; }

static void vlog(FILE *f, const char *prefix, const char *fmt, va_list ap)
{
    fprintf(f, "%s", prefix);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}

void log_out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, "", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, FC_PROG ": warning: ", fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, FC_PROG ": error: ", fmt, ap);
    va_end(ap);
}

void log_dbg(const char *fmt, ...)
{
    if (g_level < LOG_VERBOSE)
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, "  .. ", fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, FC_PROG ": fatal: ", fmt, ap);
    va_end(ap);
    exit(EXIT_USAGE);
}

int is_power_of_two(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

int parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    double v;
    uint64_t mult = 1;

    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || v < 0)
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0') {
        switch (tolower((unsigned char)*end)) {
        case 'k':
            mult = 1ULL << 10;
            break;
        case 'm':
            mult = 1ULL << 20;
            break;
        case 'g':
            mult = 1ULL << 30;
            break;
        case 't':
            mult = 1ULL << 40;
            break;
        case 'p':
            mult = 1ULL << 50;
            break;
        case 'b':
            mult = 1;
            break;
        default:
            return -1;
        }
        end++;
        if (tolower((unsigned char)*end) == 'i')
            end++;
        if (tolower((unsigned char)*end) == 'b')
            end++;
        while (isspace((unsigned char)*end))
            end++;
        if (*end != '\0')
            return -1;
    }
    v = v * (double)mult;
    if (v < 0 || v > 1.8e19)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static void fmt_scaled(char *buf, size_t n, double bytes, const char *const *units, int nu)
{
    int i = 0;
    double v = bytes;

    if (bytes == 0) {
        snprintf(buf, n, "0.00 B");
        return;
    }
    while (v >= 1024.0 && i < nu - 1) {
        v /= 1024.0;
        i++;
    }
    snprintf(buf, n, "%.2f %s", v, units[i]);
}

void fmt_size(char *buf, size_t n, uint64_t bytes)
{
    static const char *const u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    fmt_scaled(buf, n, (double)bytes, u, 6);
}

void fmt_offset(char *buf, size_t n, uint64_t bytes)
{
    char tmp[64];
    fmt_size(tmp, sizeof tmp, bytes);
    snprintf(buf, n, "%s", tmp);
}

void fmt_rate(char *buf, size_t n, double bytes_per_sec)
{
    static const char *const u[] = { "B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s" };
    if (bytes_per_sec < 0)
        bytes_per_sec = 0;
    fmt_scaled(buf, n, bytes_per_sec, u, 5);
}

void fmt_time(char *buf, size_t n, double seconds)
{
    uint64_t total = seconds > 0 ? (uint64_t)(seconds + 0.5) : 0;
    uint64_t h = total / 3600;
    uint64_t m = (total % 3600) / 60;
    uint64_t s = total % 60;
    if (h > 0)
        snprintf(buf, n, "%lluh%02llum%02llus", (unsigned long long)h,
                 (unsigned long long)m, (unsigned long long)s);
    else if (m > 0)
        snprintf(buf, n, "%llum%02llus", (unsigned long long)m, (unsigned long long)s);
    else
        snprintf(buf, n, "%.2fs", seconds);
}

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

size_t first_diff(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    size_t i = 0;

    if (memcmp(a, b, len) == 0)
        return len;
    while (i + 8 <= len) {
        uint64_t u, v;
        memcpy(&u, x + i, 8);
        memcpy(&v, y + i, 8);
        if (u != v)
            break;
        i += 8;
    }
    while (i < len && x[i] == y[i])
        i++;
    return i;
}

int read_file(const char *path, char *buf, size_t n)
{
    int fd;
    ssize_t r;

    if (n == 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -1;
    buf[r] = '\0';
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r' || buf[r - 1] == ' '))
        buf[--r] = '\0';
    return 0;
}

void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (p == NULL)
        fatal("out of memory (%zu bytes)", n);
    return p;
}

void *xalloc_aligned(size_t align, size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, align, n ? n : align) != 0 || p == NULL)
        fatal("out of memory (%zu bytes, align %zu)", n, align);
    return p;
}

uint64_t parse_udev_number(const char *s)
{
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            break;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    return v;
}

const char *errno_str(int err)
{
    switch (err) {
    case EIO:
        return "I/O error";
    case ENOSPC:
        return "no space left (unexpected on a block device)";
    case EROFS:
        return "read-only device";
    case ETIMEDOUT:
        return "device timeout";
    case EINVAL:
        return "invalid argument";
    case EBUSY:
        return "device busy";
    case EPERM:
    case EACCES:
        return "permission denied";
    case EOVERFLOW:
        return "offset overflow (64-bit device size unsupported by this kernel)";
    default:
        return strerror(err);
    }
}

volatile sig_atomic_t fc_interrupted = 0;

static void on_signal(int sig)
{
    (void)sig;
    if (fc_interrupted)
        _exit(130);
    fc_interrupted = 1;
}

void fc_install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
