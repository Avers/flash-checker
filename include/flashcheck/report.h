#ifndef FLASHCHECK_REPORT_H
#define FLASHCHECK_REPORT_H

#include "flashcheck/common.h"
#include "flashcheck/test.h"

void report_console(const run_ctx *c, verdict v);
void report_bench(const run_ctx *c);
int report_json(const run_ctx *c, verdict v, const char *path);
const char *report_evidence(const run_ctx *c, char *buf, size_t n);

#endif
