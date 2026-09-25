#ifndef FLASHCHECK_TEST_UTIL_H
#define FLASHCHECK_TEST_UTIL_H

#include "flashcheck/common.h"

extern int g_test_failures;
extern int g_test_checks;
extern const char *g_test_current;

void test_begin(const char *name);
void test_fail(const char *file, int line, const char *fmt, ...);
int test_summary(void);

#define T_BEGIN(n) test_begin(n)
#define T_FAIL(...) test_fail(__FILE__, __LINE__, __VA_ARGS__)

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_test_checks++;                                                                           \
        if (!(cond))                                                                               \
            T_FAIL("CHECK(%s) failed", #cond);                                                     \
    } while (0)

#define CHECK_EQ_U64(a, b)                                                                         \
    do {                                                                                           \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);            \
        g_test_checks++;                                                                           \
        if (va_ != vb_)                                                                            \
            T_FAIL("%s == %s: %llu != %llu", #a, #b, va_, vb_);                                   \
    } while (0)

#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        g_test_checks++;                                                                           \
        if (strcmp((a), (b)) != 0)                                                                 \
            T_FAIL("%s != %s: \"%s\" vs \"%s\"", #a, #b, (a), (b));                                \
    } while (0)

#endif
