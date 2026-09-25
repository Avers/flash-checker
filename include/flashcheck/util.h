#ifndef FLASHCHECK_UTIL_H
#define FLASHCHECK_UTIL_H

#include <signal.h>

#include "flashcheck/common.h"

typedef enum { LOG_QUIET = 0, LOG_NORMAL = 1, LOG_VERBOSE = 2 } log_level;

void log_set_level(log_level l);
log_level log_get_level(void);
void log_out(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_err(const char *fmt, ...);
void log_dbg(const char *fmt, ...);
void fatal(const char *fmt, ...);

int parse_size(const char *s, uint64_t *out);
int parse_u64(const char *s, uint64_t *out);
void fmt_size(char *buf, size_t n, uint64_t bytes);
void fmt_offset(char *buf, size_t n, uint64_t bytes);
void fmt_rate(char *buf, size_t n, double bytes_per_sec);
void fmt_time(char *buf, size_t n, double seconds);
double now_sec(void);
uint64_t now_ms(void);
void sleep_ms(unsigned ms);
size_t first_diff(const void *a, const void *b, size_t len);
int read_file(const char *path, char *buf, size_t n);
void *xalloc(size_t n);
void *xalloc_aligned(size_t align, size_t n);
uint64_t parse_udev_number(const char *s);
const char *errno_str(int err);
extern volatile sig_atomic_t fc_interrupted;
void fc_install_signal_handlers(void);
int is_power_of_two(uint64_t v);

#endif
