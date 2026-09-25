#include "test_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int g_test_failures;
int g_test_checks;
const char *g_test_current = "(none)";

void test_begin(const char *name)
{
    g_test_current = name;
    printf("  %-34s", name);
    fflush(stdout);
}

void test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    g_test_failures++;
    printf("\n    FAIL %s:%d [%s] ", file, line, g_test_current);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int test_summary(void)
{
    printf("\n%d checks, %d failures\n", g_test_checks, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
