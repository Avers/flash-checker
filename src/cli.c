#include "flashcheck/cli.h"

#include <getopt.h>

#include "flashcheck/io.h"
#include "flashcheck/pool.h"
#include "flashcheck/util.h"

enum {
    OPT_MODE = 1000,
    OPT_DESTRUCTIVE,
    OPT_BLOCK,
    OPT_WINDOW,
    OPT_LIMIT,
    OPT_OFFSET,
    OPT_PASSES,
    OPT_PATTERN,
    OPT_BENCH,
    OPT_BOUNDARY_BUDGET,
    OPT_BOUNDARY_RES,
    OPT_JSON,
    OPT_CHECKPOINT,
    OPT_RESUME,
    OPT_DIRECT,
    OPT_NO_DIRECT,
    OPT_YES,
    OPT_DRY_RUN,
    OPT_VERBOSE,
    OPT_SELF_TEST,
    OPT_SELF_REPORTED,
    OPT_SELF_REAL,
    OPT_IO_BACKEND,
    OPT_DEPTH,
    OPT_HELP,
    OPT_VERSION
};

void cli_usage(FILE *f, const char *prog)
{
    fprintf(f,
            "usage: %s <device> [options]\n"
            "\n"
            "Detects USB flash devices that report more capacity than they really have.\n"
            "All tests write unique, reproducible data straight to the raw block device.\n"
            "\n"
            "modes:\n"
            "  -i, --identify          only print device information, never writes\n"
            "      --mode MODE         quick | standard | full   (default: standard)\n"
            "                         quick    cheap probe, never certifies capacity\n"
            "                         standard probe + retention + capacity search\n"
            "                         full     standard + full write/read verify\n"
            "\n"
            "safety:\n"
            "  -d, --destructive       allow writes (destroys all data on the device)\n"
            "      --dry-run           print the plan and exit without writing\n"
            "      --yes               do not ask for confirmation\n"
            "\n"
            "tuning:\n"
            "  -b, --block-size SIZE   region size, default 8MiB\n"
            "  -w, --window SIZE       sliding window for full mode, default 1GiB\n"
            "  -n, --limit SIZE        only test the first SIZE bytes\n"
            "      --offset SIZE       start testing at SIZE\n"
            "  -p, --passes N          retention passes, default 1\n"
            "      --pattern KIND      chacha20 | prng | zero, default chacha20\n"
            "      --depth N           parallel data-generation depth 0-16, default 4\n"
            "                          (0 or 1 generates data inline, no helper thread)\n"
            "      --bench-bytes SIZE  bytes used for the speed benchmark, default 256MiB\n"
            "      --boundary-budget SIZE   max bytes for the capacity search, default 8GiB\n"
            "      --boundary-resolution SIZE  bisection step, default 64MiB\n"
            "      --direct/--no-direct     force or forbid O_DIRECT\n"
            "      --io-backend KIND     sync | uring, default sync\n"
            "                            (uring needs a FLASHCHECK_IO_URING=1 build)\n"
            "\n"
            "output:\n"
            "      --json FILE         write a JSON report\n"
            "      --checkpoint FILE   write progress checkpoints\n"
            "      --resume             continue the full stage from a checkpoint\n"
            "  -v, --verbose           more detail\n"
            "  -h, --help              this text\n"
            "      --version           print version\n"
            "\n"
            "self test (no hardware, simulated devices):\n"
            "      --self-test POLICY  honest | alias | stale | unwritten | error | slow\n"
            "      --self-reported SIZE reported capacity of the simulated device\n"
            "      --self-real SIZE    real capacity of the simulated device\n"
            "\n"
            "exit codes: 0 PASS, 1 FAIL, 2 INCONCLUSIVE, 3 usage/safety, 4 I/O error\n",
            prog);
}

static int set_mode(const char *s, config *c)
{
    if (strcmp(s, "quick") == 0)
        c->mode = MODE_QUICK;
    else if (strcmp(s, "standard") == 0)
        c->mode = MODE_STANDARD;
    else if (strcmp(s, "full") == 0)
        c->mode = MODE_FULL;
    else if (strcmp(s, "identify") == 0)
        c->mode = MODE_IDENTIFY;
    else
        return -1;
    return 0;
}

static int parse_mode_arg(const char *arg, config *c)
{
    const char *s = arg;
    if (strncmp(s, "--mode=", 7) == 0)
        s += 7;
    return set_mode(s, c);
}

static int set_size_opt(const char *arg, const char *name, uint64_t *dst, char *err, size_t errn)
{
    if (parse_size(arg, dst) != 0) {
        snprintf(err, errn, "invalid size for %s: '%s'", name, arg);
        return -1;
    }
    return 0;
}

int cli_parse(int argc, char **argv, config *c, cli_action *action, char *err,
              size_t errn)
{
    static const struct option opts[] = {
        { "identify", no_argument, NULL, 'i' },
        { "mode", required_argument, NULL, OPT_MODE },
        { "destructive", no_argument, NULL, 'd' },
        { "block-size", required_argument, NULL, 'b' },
        { "window", required_argument, NULL, 'w' },
        { "limit", required_argument, NULL, 'n' },
        { "offset", required_argument, NULL, OPT_OFFSET },
        { "passes", required_argument, NULL, 'p' },
        { "pattern", required_argument, NULL, OPT_PATTERN },
        { "depth", required_argument, NULL, OPT_DEPTH },
        { "bench-bytes", required_argument, NULL, OPT_BENCH },
        { "boundary-budget", required_argument, NULL, OPT_BOUNDARY_BUDGET },
        { "boundary-resolution", required_argument, NULL, OPT_BOUNDARY_RES },
        { "json", required_argument, NULL, OPT_JSON },
        { "checkpoint", required_argument, NULL, OPT_CHECKPOINT },
        { "resume", no_argument, NULL, OPT_RESUME },
        { "io-backend", required_argument, NULL, OPT_IO_BACKEND },
        { "direct", no_argument, NULL, OPT_DIRECT },
        { "no-direct", no_argument, NULL, OPT_NO_DIRECT },
        { "yes", no_argument, NULL, OPT_YES },
        { "dry-run", no_argument, NULL, OPT_DRY_RUN },
        { "verbose", no_argument, NULL, 'v' },
        { "self-test", required_argument, NULL, OPT_SELF_TEST },
        { "self-reported", required_argument, NULL, OPT_SELF_REPORTED },
        { "self-real", required_argument, NULL, OPT_SELF_REAL },
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, OPT_VERSION },
        { NULL, 0, NULL, 0 }
    };
    int ch;

    *action = CLI_ACTION_RUN;
    optind = 1;
    while ((ch = getopt_long(argc, argv, "idb:w:n:p:vh", opts, NULL)) != -1) {
        switch (ch) {
        case 'i':
            c->mode = MODE_IDENTIFY;
            break;
        case OPT_MODE:
            if (parse_mode_arg(optarg, c) != 0) {
                snprintf(err, errn, "unknown mode: '%s'", optarg);
                return CLI_ERROR;
            }
            break;
        case 'd':
            c->destructive = 1;
            break;
        case 'b':
            if (set_size_opt(optarg, "--block-size", &c->chunk_size, err, errn) != 0)
                return CLI_ERROR;
            break;
        case 'w':
            if (set_size_opt(optarg, "--window", &c->window_size, err, errn) != 0)
                return CLI_ERROR;
            break;
        case 'n':
            if (set_size_opt(optarg, "--limit", &c->limit, err, errn) != 0)
                return CLI_ERROR;
            break;
        case OPT_OFFSET:
            if (set_size_opt(optarg, "--offset", &c->offset, err, errn) != 0)
                return CLI_ERROR;
            break;
        case 'p':
            if (parse_u64(optarg, &c->passes) != 0 || c->passes == 0) {
                snprintf(err, errn, "invalid --passes: '%s'", optarg);
                return CLI_ERROR;
            }
            break;
        case OPT_PATTERN:
            if (pattern_kind_parse(optarg, &c->pattern) != 0) {
                snprintf(err, errn, "unknown pattern: '%s' (chacha20|prng|zero)", optarg);
                return CLI_ERROR;
            }
            break;
        case OPT_DEPTH: {
            uint64_t v;
            if (parse_u64(optarg, &v) != 0 || v > GEN_POOL_MAX_DEPTH) {
                snprintf(err, errn, "invalid --depth: '%s' (0..%d)", optarg,
                         GEN_POOL_MAX_DEPTH);
                return CLI_ERROR;
            }
            c->depth = (int)v;
            break;
        }
        case OPT_BENCH:
            if (set_size_opt(optarg, "--bench-bytes", &c->bench_bytes, err, errn) != 0)
                return CLI_ERROR;
            break;
        case OPT_BOUNDARY_BUDGET:
            if (set_size_opt(optarg, "--boundary-budget", &c->boundary_budget, err, errn) != 0)
                return CLI_ERROR;
            break;
        case OPT_BOUNDARY_RES:
            if (set_size_opt(optarg, "--boundary-resolution", &c->boundary_resolution, err,
                             errn) != 0)
                return CLI_ERROR;
            break;
        case OPT_JSON:
            c->json_path = optarg;
            break;
        case OPT_CHECKPOINT:
            c->checkpoint_path = optarg;
            break;
        case OPT_RESUME:
            c->resume = 1;
            break;
        case OPT_IO_BACKEND:
            if (strcmp(optarg, "sync") == 0)
                c->io_backend = IO_BACKEND_SYNC;
            else if (strcmp(optarg, "uring") == 0)
                c->io_backend = IO_BACKEND_URING;
            else {
                snprintf(err, errn, "unknown io backend: '%s' (sync|uring)", optarg);
                return CLI_ERROR;
            }
            break;
        case OPT_DIRECT:
            c->want_direct = 1;
            break;
        case OPT_NO_DIRECT:
            c->want_direct = 0;
            break;
        case OPT_YES:
            c->assume_yes = 1;
            break;
        case OPT_DRY_RUN:
            c->dry_run = 1;
            break;
        case 'v':
            c->verbose++;
            break;
        case OPT_SELF_TEST:
            if (fake_policy_parse(optarg, &c->self_test) != 0) {
                snprintf(err, errn,
                         "unknown policy: '%s' (honest|alias|stale|unwritten|error|slow)",
                         optarg);
                return CLI_ERROR;
            }
            break;
        case OPT_SELF_REPORTED:
            if (set_size_opt(optarg, "--self-reported", &c->st_reported, err, errn) != 0)
                return CLI_ERROR;
            break;
        case OPT_SELF_REAL:
            if (set_size_opt(optarg, "--self-real", &c->st_real, err, errn) != 0)
                return CLI_ERROR;
            c->st_real_set = 1;
            break;
        case 'h':
            *action = CLI_ACTION_HELP;
            return CLI_EXIT;
        case OPT_VERSION:
            *action = CLI_ACTION_VERSION;
            return CLI_EXIT;
        default:
            snprintf(err, errn, "unknown option");
            return CLI_ERROR;
        }
    }
    if (optind < argc && c->device == NULL) {
        c->device = argv[optind];
        optind++;
    }
    if (optind < argc) {
        snprintf(err, errn, "unexpected extra argument: '%s'", argv[optind]);
        return CLI_ERROR;
    }
    if (c->device == NULL && c->self_test < 0) {
        snprintf(err, errn, "no device given (try --help)");
        return CLI_ERROR;
    }
    if (c->self_test >= 0 && c->device != NULL) {
        snprintf(err, errn, "--self-test does not take a device path");
        return CLI_ERROR;
    }
    if (c->self_test >= 0 && !c->destructive)
        c->destructive = 1;
    return CLI_OK;
}
