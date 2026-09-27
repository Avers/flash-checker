#include "test_util.h"

void test_crypto(void);
void test_pattern_and_math(void);
void test_io_and_detection(void);
void test_uring(void);
void test_pool(void);

int main(void)
{
    printf("%s %s test suite\n\n", FC_PROG, FC_VERSION);
    test_crypto();
    test_pattern_and_math();
    test_io_and_detection();
    test_uring();
    test_pool();
    return test_summary();
}
