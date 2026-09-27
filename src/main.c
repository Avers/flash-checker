#include "flashcheck/checkpoint.h"
#include "flashcheck/cli.h"
#include "flashcheck/config.h"
#include "flashcheck/device.h"
#include "flashcheck/io.h"
#include "flashcheck/pipeline.h"
#include "flashcheck/report.h"
#include "flashcheck/safety.h"
#include "flashcheck/test.h"
#include "flashcheck/util.h"

#include <fcntl.h>
#include <unistd.h>

static uint64_t random_test_id(void)
{
    uint64_t id = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        ssize_t n = read(fd, &id, sizeof id);
        close(fd);
        if (n == (ssize_t)sizeof id && id != 0)
            return id;
    }
    id = (uint64_t)now_ms() * 6364136223846793005ULL;
    return id == 0 ? 1 : id;
}

static void fake_device(device_info *d, const config *c)
{
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "simulated:%s", fake_policy_name(c->self_test));
    snprintf(d->name, sizeof d->name, "fake");
    d->capacity = c->st_reported;
    d->logical_sector = 512;
    d->physical_sector = 4096;
    d->removable = 1;
    d->direct_supported = 0;
    snprintf(d->model, sizeof d->model, "simulated %s device", fake_policy_name(c->self_test));
    snprintf(d->serial, sizeof d->serial, "selftest-%llu",
             (unsigned long long)c->st_real);
    snprintf(d->transport, sizeof d->transport, "simulation");
}

int main(int argc, char **argv)
{
    config cfg;
    char err[512];
    cli_action action = CLI_ACTION_RUN;
    device_info dev;
    io_ops *io = NULL;
    run_ctx ctx;
    checkpoint cp;
    int rc;
    int lock_fd = -1;
    verdict v;

    memset(&cp, 0, sizeof cp);
    config_defaults(&cfg);
    rc = cli_parse(argc, argv, &cfg, &action, err, sizeof err);
    if (rc == CLI_EXIT) {
        if (action == CLI_ACTION_HELP) {
            cli_usage(stdout, FC_PROG);
            return EXIT_OK;
        }
        printf("%s %s\n", FC_PROG, FC_VERSION);
        return EXIT_OK;
    }
    if (rc != CLI_OK) {
        log_err("%s", err);
        cli_usage(stderr, FC_PROG);
        return EXIT_USAGE;
    }
    if (cfg.verbose > 0)
        log_set_level(LOG_VERBOSE);
    if (config_validate(&cfg, err, sizeof err) != 0) {
        log_err("%s", err);
        return EXIT_USAGE;
    }
    if (cfg.self_test < 0 && cfg.dry_run && !cfg.destructive) {
        log_err("--dry-run only makes sense together with --destructive");
        return EXIT_USAGE;
    }

    fc_install_signal_handlers();

    if (cfg.self_test >= 0) {
        fake_device(&dev, &cfg);
        io = io_fake_open(cfg.st_reported, cfg.st_real, cfg.self_test, &rc);
        if (io == NULL) {
            log_err("cannot create simulated device: %s", errno_str(rc));
            return EXIT_USAGE;
        }
        log_warn("self test: simulated device, reported %llu bytes, real %llu bytes, policy %s",
                 (unsigned long long)cfg.st_reported, (unsigned long long)cfg.st_real,
                 fake_policy_name(cfg.self_test));
    } else {
        if (device_probe(cfg.device, &dev, err, sizeof err) != 0) {
            log_err("%s", err);
            return EXIT_USAGE;
        }
        if (cfg.want_direct < 0)
            cfg.want_direct = dev.direct_supported;

        if (cfg.io_backend == IO_BACKEND_URING) {
#ifdef FLASHCHECK_IO_URING
            io = io_uring_open(dev.path, cfg.mode != MODE_IDENTIFY, cfg.want_direct, dev.capacity,
                               &rc);
            if (io == NULL) {
                log_warn("io_uring not available, falling back to sync: %s", errno_str(rc));
                io = io_sync_open(dev.path, cfg.mode != MODE_IDENTIFY, cfg.want_direct, dev.capacity,
                                  &rc);
            }
#else
            log_warn("--io-backend=uring requested but this build has no io_uring support, "
                     "using sync (rebuild with FLASHCHECK_IO_URING=1)");
            io = io_sync_open(dev.path, cfg.mode != MODE_IDENTIFY, cfg.want_direct, dev.capacity,
                              &rc);
#endif
        } else {
            io = io_sync_open(dev.path, cfg.mode != MODE_IDENTIFY, cfg.want_direct, dev.capacity,
                              &rc);
        }
        if (io == NULL) {
            log_err("cannot open %s: %s", dev.path, errno_str(rc));
            return EXIT_IOERROR;
        }
        lock_fd = open(dev.path, O_RDONLY | O_CLOEXEC);
        if (safety_lock_device(lock_fd) != 0) {
            log_err("another %s instance seems to be running on %s", FC_PROG, dev.path);
            return EXIT_USAGE;
        }
        if (cfg.mode != MODE_IDENTIFY) {
            if (safety_check(&cfg, &dev, lock_fd, err, sizeof err) != 0) {
                log_err("%s", err);
                return EXIT_USAGE;
            }
        }
        if (cfg.dry_run) {
            safety_plan(&cfg, &dev);
            log_out("");
            log_out("dry run: nothing was written.");
            return EXIT_OK;
        }
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.dev = &dev;
    ctx.io = io;
    ctx.test_id = random_test_id();
    ctx.checkpoint_path = cfg.checkpoint_path;
    if (cfg.resume && cfg.checkpoint_path != NULL) {
        if (checkpoint_load(cfg.checkpoint_path, &cp) == 0) {
            ctx.test_id = cp.test_id != 0 ? cp.test_id : ctx.test_id;
            ctx.resume_off = cp.offset;
            if (strcmp(cp.stage, "full-verify") == 0 && cp.complete)
                ctx.resume_off = 0;
        } else {
            log_warn("no usable checkpoint at %s, starting from the beginning",
                     cfg.checkpoint_path);
        }
    }

    if (pipeline_init(&ctx.pl, io, cfg.chunk_size, cfg.pattern, ctx.test_id, dev.capacity,
                      cfg.depth, err, sizeof err) != 0) {
        log_err("%s", err);
        return EXIT_USAGE;
    }
    ctx.pipeline_ready = 1;

    rc = run_execute(&ctx);
    v = run_verdict(&ctx);

    report_console(&ctx, v);
    if (cfg.json_path != NULL)
        report_json(&ctx, v, cfg.json_path);

    if (fc_interrupted) {
        log_warn("interrupted by signal: progress checkpoint written if --checkpoint was used");
        pipeline_free(&ctx.pl);
        io->close(io);
        if (lock_fd >= 0)
            close(lock_fd);
        return EXIT_INCONCLUSIVE;
    }
    if (rc < 0) {
        pipeline_free(&ctx.pl);
        io->close(io);
        if (lock_fd >= 0)
            close(lock_fd);
        return EXIT_IOERROR;
    }
    pipeline_free(&ctx.pl);
    io->close(io);
    if (lock_fd >= 0)
        close(lock_fd);
    return v == VERDICT_PASS ? EXIT_OK
                             : (v == VERDICT_FAIL ? EXIT_FAIL : EXIT_INCONCLUSIVE);
}
