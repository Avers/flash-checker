#ifndef FLASHCHECK_CLI_H
#define FLASHCHECK_CLI_H

#include "flashcheck/common.h"
#include "flashcheck/config.h"

#define CLI_OK 0
#define CLI_EXIT 1
#define CLI_ERROR 2

typedef enum { CLI_ACTION_RUN = 0, CLI_ACTION_HELP, CLI_ACTION_VERSION } cli_action;

int cli_parse(int argc, char **argv, config *c, cli_action *action, char *err, size_t errn);
void cli_usage(FILE *f, const char *prog);

#endif
