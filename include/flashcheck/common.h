#ifndef FLASHCHECK_COMMON_H
#define FLASHCHECK_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FC_VERSION
#define FC_VERSION VERSION_STR
#endif
#define FC_PROG "flashcheck"

#ifndef FC_ALIGN
#define FC_ALIGN 4096
#endif

#define FC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define FC_MAX(a, b) ((a) > (b) ? (a) : (b))

typedef enum {
    VERDICT_PASS = 0,
    VERDICT_FAIL = 1,
    VERDICT_INCONCLUSIVE = 2
} verdict;

const char *verdict_str(verdict v);

#define EXIT_OK 0
#define EXIT_FAIL 1
#define EXIT_INCONCLUSIVE 2
#define EXIT_USAGE 3
#define EXIT_IOERROR 4

#define COLOR_GREEN  "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_RED    "\x1b[31m"
#define COLOR_RESET  "\x1b[0m"
#define COLOR_BOLD   "\x1b[1m"

#endif
