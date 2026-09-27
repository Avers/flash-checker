#include "flashcheck/checkpoint.h"
#include "flashcheck/io.h"
#include "flashcheck/pipeline.h"
#include "flashcheck/safety.h"
#include "flashcheck/stats.h"
#include "flashcheck/test.h"
#include "flashcheck/util.h"
#include "test_util.h"

#include <stdio.h>
#include <unistd.h>

static void test_io_fake_honest(void)
{
    io_ops *io;
    uint8_t buf[4096];
    int e = 0;

    T_BEGIN("fake honest device round trip");
    io = io_fake_open(1u << 20, 1u << 20, FAKE_HONEST, &e);
    CHECK(io != NULL);
    memset(buf, 0xab, sizeof buf);
    CHECK_EQ_U64(io->write(io, buf, 8192, sizeof buf), 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ_U64(io->read(io, buf, 8192, sizeof buf), 0);
    CHECK_EQ_U64(buf[0], 0xab);
    CHECK_EQ_U64(buf[sizeof buf - 1], 0xab);
    T_BEGIN("fake device rejects out of range");
    CHECK(io->read(io, buf, (1u << 20) - 16, 4096) != 0);
    CHECK(io->read(io, buf, (1ULL << 40), 4096) != 0);
    io->close(io);
}

static void test_io_fake_policies(void)
{
    io_ops *io;
    uint8_t buf[512];
    int e = 0;

    T_BEGIN("fake alias wraps past real capacity");
    io = io_fake_open(1u << 20, 1u << 16, FAKE_ALIAS, &e);
    memset(buf, 0x5a, sizeof buf);
    CHECK_EQ_U64(io->write(io, buf, 0, sizeof buf), 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ_U64(io->read(io, buf, 0x20000, sizeof buf), 0);
    CHECK_EQ_U64(buf[0], 0x5a);
    memset(buf, 0x99, sizeof buf);
    CHECK_EQ_U64(io->write(io, buf, 0x10000, sizeof buf), 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ_U64(io->read(io, buf, 0, sizeof buf), 0);
    CHECK_EQ_U64(buf[0], 0x99);
    io->close(io);

    T_BEGIN("fake stale discards writes past real capacity");
    io = io_fake_open(1u << 20, 1u << 16, FAKE_STALE, &e);
    memset(buf, 0x77, sizeof buf);
    CHECK_EQ_U64(io->write(io, buf, 1u << 16, sizeof buf), 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ_U64(io->read(io, buf, 1u << 16, sizeof buf), 0);
    CHECK_EQ_U64(buf[0], 0x00);
    io->close(io);

    T_BEGIN("fake error returns EIO past real capacity");
    io = io_fake_open(1u << 20, 1u << 16, FAKE_ERROR, &e);
    CHECK_EQ_U64(io->write(io, buf, 1u << 16, sizeof buf), -EIO);
    CHECK(io->stats->io_errors > 0);
    io->close(io);

    T_BEGIN("fake unwritten returns poison data");
    io = io_fake_open(1u << 20, 1u << 16, FAKE_UNWRITTEN, &e);
    CHECK_EQ_U64(io->read(io, buf, 0, sizeof buf), 0);
    CHECK_EQ_U64(buf[0], 0xa5);
    io->close(io);
}

typedef struct {
    uint64_t off;
    int failed;
} expect_t;

static void run_case(const char *name, int policy, uint64_t reported, uint64_t real, int want_fail,
                     int depth)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    int e = 0;
    uint64_t chunk = 1u << 20;
    uint64_t end = reported - reported % chunk;
    uint64_t off;
    int failed = 0;

    T_BEGIN(name);
    io = io_fake_open(reported, real, policy, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    CHECK_EQ_U64(pipeline_init(&p, io, chunk, PAT_CHACHA20, 0xfeedbeef, reported, depth, err,
                               sizeof err),
                 0);

    stage_stats_reset(&st);
    for (off = 0; off < end; off += chunk) {
        if (pipeline_write(&p, off, 1, &st) != 0)
            break;
    }
    CHECK_EQ_U64(io->flush(io), 0);
    for (off = 0; off < end; off += chunk) {
        if (pipeline_verify(&p, off, 1, &st) != 0)
            failed = 1;
    }
    CHECK_EQ_U64(failed ? 1 : 0, want_fail);
    if (want_fail)
        CHECK(st.has_first_fail);
    pipeline_free(&p);
    io->close(io);
    (void)sizeof(expect_t);
}

static void test_detection(void)
{
    printf("detection against simulated devices\n");
    run_case("honest device passes full verify", FAKE_HONEST, 16u << 20, 16u << 20, 0, 0);
    run_case("alias device fails full verify", FAKE_ALIAS, 16u << 20, 4u << 20, 1, 4);
    run_case("stale device fails full verify", FAKE_STALE, 16u << 20, 4u << 20, 1, 0);
    run_case("unwritten device fails verify", FAKE_UNWRITTEN, 16u << 20, 4u << 20, 1, 4);
}

static void test_alias_attribution(void)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    int e = 0;
    uint64_t chunk = 1u << 20;
    uint64_t src = 0;

    T_BEGIN("alias source is identified");
    io = io_fake_open(16u << 20, 4u << 20, FAKE_ALIAS, &e);
    pipeline_init(&p, io, chunk, PAT_CHACHA20, 99, 16u << 20, 0, err, sizeof err);
    stage_stats_reset(&st);
    pipeline_write(&p, src, 1, &st);
    io->flush(io);
    CHECK_EQ_U64(pipeline_verify(&p, 12u << 20, 1, &st), 1);
    CHECK(st.has_alias);
    CHECK(st.alias_confirmed);
    CHECK_EQ_U64(st.alias_src_off, src);
    CHECK_EQ_U64(st.first_fail_off, (12u << 20) + 32);
    pipeline_free(&p);
    io->close(io);
}

static void test_window_pass(void)
{
    io_ops *io;
    pipeline p;
    stage_stats st;
    char err[128];
    int e = 0;
    uint64_t chunk = 1u << 20;

    T_BEGIN("window pass writes then verifies");
    io = io_fake_open(16u << 20, 16u << 20, FAKE_HONEST, &e);
    pipeline_init(&p, io, chunk, PAT_CHACHA20, 5, 16u << 20, 4, err, sizeof err);
    stage_stats_reset(&st);
    CHECK_EQ_U64(pipeline_window_pass(&p, 0, 8u << 20, 2, 1, &st, 1), 0);
    CHECK_EQ_U64(st.chunks_written, 8);
    CHECK_EQ_U64(st.chunks_verified, 8);
    CHECK_EQ_U64(st.chunks_failed, 0);
    CHECK_EQ_U64(st.bytes_written, 8u << 20);
    T_BEGIN("window pass stops at first failure");
    io->close(io);
    io = io_fake_open(16u << 20, 5u << 20, FAKE_ALIAS, &e);
    pipeline_init(&p, io, chunk, PAT_CHACHA20, 5, 16u << 20, 4, err, sizeof err);
    stage_stats_reset(&st);
    CHECK(pipeline_window_pass(&p, 0, 16u << 20, 8, 1, &st, 1) > 0);
    CHECK(st.chunks_failed > 0);
    pipeline_free(&p);
    io->close(io);
}

static void test_speed_track(void)
{
    speed_track t;
    uint64_t bytes = 0;
    double sec = 0, avg = 0, mx = 0, mn = 0;

    T_BEGIN("speed tracker aggregates");
    speed_init(&t);
    for (int i = 0; i < 5; i++) {
        sleep_ms(2);
        speed_mark(&t, (uint64_t)i * 1024u * 1024u);
    }
    speed_summary(&t, &bytes, &sec, &avg, &mx, &mn);
    CHECK(bytes > 0);
    CHECK(sec > 0);
    CHECK(avg > 0);
    CHECK(t.n > 0);
    speed_free(&t);
}

static void test_checkpoint(void)
{
    checkpoint a, b;
    char path[] = "/tmp/flashcheck-test-cp-XXXXXX";
    int fd = mkstemp(path);

    T_BEGIN("checkpoint save and load");
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    close(fd);
    memset(&a, 0, sizeof a);
    snprintf(a.stage, sizeof a.stage, "full-verify");
    a.test_id = 12345;
    a.offset = 4096;
    a.bytes_written = 999;
    a.bytes_verified = 888;
    a.errors = 0;
    a.complete = 0;
    CHECK_EQ_U64(checkpoint_save(path, &a), 0);
    CHECK_EQ_U64(checkpoint_load(path, &b), 0);
    CHECK_STR(b.stage, "full-verify");
    CHECK_EQ_U64(b.test_id, 12345);
    CHECK_EQ_U64(b.offset, 4096);
    CHECK_EQ_U64(b.bytes_written, 999);
    CHECK_EQ_U64(b.bytes_verified, 888);
    CHECK_EQ_U64(b.complete, 0);
    unlink(path);
    T_BEGIN("missing checkpoint reports failure");
    CHECK(checkpoint_load("/tmp/flashcheck-does-not-exist-12345", &b) != 0);
}

static void test_safety_mounted(void)
{
    char detail[512];

    T_BEGIN("mount matcher covers linux device names");
    CHECK(safety_mount_matches("/dev/sda", "/dev/sda"));
    CHECK(safety_mount_matches("/dev/sda1", "/dev/sda"));
    CHECK(safety_mount_matches("/dev/sda10", "/dev/sda"));
    CHECK(safety_mount_matches("/dev/nvme0n1p1", "/dev/nvme0n1"));
    CHECK(safety_mount_matches("/dev/mmcblk0p2", "/dev/mmcblk0"));
    CHECK(!safety_mount_matches("/dev/sdb1", "/dev/sda"));
    CHECK(!safety_mount_matches("/dev/sda", "/dev/sda1"));
    CHECK(!safety_mount_matches("/dev/sda1x", "/dev/sda"));
    CHECK(!safety_mount_matches("overlay", "/dev/sda"));

    T_BEGIN("mount matcher covers macOS device names");
    CHECK(safety_mount_matches("/dev/disk8", "/dev/disk8"));
    CHECK(safety_mount_matches("/dev/disk8s1", "/dev/disk8"));
    CHECK(safety_mount_matches("/dev/disk8s1s2", "/dev/disk8"));
    CHECK(safety_mount_matches("/dev/disk8s1s1", "/dev/disk8s1"));
    CHECK(!safety_mount_matches("/dev/disk80", "/dev/disk8"));
    CHECK(!safety_mount_matches("/dev/disk8s10", "/dev/disk8s1"));
    CHECK(!safety_mount_matches("/dev/disk9s1", "/dev/disk8"));

    T_BEGIN("mount detection ignores absent devices");
    detail[0] = '\0';
    CHECK(safety_is_mounted("/dev/definitely-not-a-device-xyz", detail, sizeof detail) == 0);
    CHECK(detail[0] == '\0');

#ifdef __linux__
    T_BEGIN("mount detection finds /dev devices from /proc/mounts");
    {
        char line[1024], src[256], mnt[256];
        FILE *f = fopen("/proc/mounts", "r");

        if (f != NULL) {
            while (fgets(line, sizeof line, f) != NULL) {
                if (sscanf(line, "%255s %255s", src, mnt) != 2)
                    continue;
                if (strncmp(src, "/dev/", 5) != 0)
                    continue;
                CHECK(safety_is_mounted(src, detail, sizeof detail) == 1);
                break;
            }
            fclose(f);
        }
    }
#endif
}

static void test_verdict(void)
{
    config cfg;
    run_ctx ctx;

    memset(&cfg, 0, sizeof cfg);
    memset(&ctx, 0, sizeof ctx);
    config_defaults(&cfg);
    cfg.mode = MODE_IDENTIFY;
    ctx.cfg = &cfg;
    ctx.nst = 1;
    ctx.st[0].name = "identify";

    T_BEGIN("identify never reports a verdict");
    CHECK(run_verdict(&ctx) == VERDICT_INCONCLUSIVE);

    T_BEGIN("zero written bytes is never a pass");
    cfg.mode = MODE_STANDARD;
    CHECK(run_verdict(&ctx) == VERDICT_INCONCLUSIVE);

    T_BEGIN("verified write passes");
    ctx.tested_bytes = 1u << 20;
    ctx.has_capacity = 1;
    CHECK(run_verdict(&ctx) == VERDICT_PASS);

    T_BEGIN("mismatch fails");
    ctx.st[0].chunks_failed = 1;
    CHECK(run_verdict(&ctx) == VERDICT_FAIL);

    T_BEGIN("io errors are inconclusive");
    ctx.st[0].chunks_failed = 0;
    ctx.st[0].io_errors = 1;
    CHECK(run_verdict(&ctx) == VERDICT_INCONCLUSIVE);
}

static void test_bench(void)
{
    config cfg;
    run_ctx ctx;
    device_info dev;
    io_ops *io;
    char err[128];
    int e = 0;
    uint64_t chunk = 1u << 20;
    uint64_t reported = 16u << 20;
    int rc;

    memset(&cfg, 0, sizeof cfg);
    memset(&ctx, 0, sizeof ctx);
    memset(&dev, 0, sizeof dev);
    config_defaults(&cfg);
    cfg.chunk_size = chunk;
    cfg.bench_bytes = 4u << 20;
    dev.capacity = reported;
    ctx.cfg = &cfg;
    ctx.dev = &dev;

    T_BEGIN("bench measures write and read speed on an honest device");
    io = io_fake_open(reported, reported, FAKE_HONEST, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    ctx.io = io;
    CHECK_EQ_U64(pipeline_init(&ctx.pl, io, chunk, PAT_CHACHA20, 0xfeedbeef, reported, 2, err,
                               sizeof err),
                 0);
    CHECK_EQ_U64(bench_run(&ctx), 0);
    CHECK_EQ_U64(ctx.nst, 1);
    CHECK_EQ_U64(ctx.st[0].bytes_written, 4u << 20);
    CHECK_EQ_U64(ctx.st[0].bytes_verified, 4u << 20);
    CHECK_EQ_U64(ctx.st[0].chunks_failed, 0);
    CHECK(ctx.st[0].write_bps > 0);
    CHECK(ctx.st[0].read_bps > 0);
    CHECK_EQ_U64(ctx.tested_bytes, 4u << 20);
    pipeline_free(&ctx.pl);
    io->close(io);

    T_BEGIN("bench fails when the read-back does not match");
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.dev = &dev;
    cfg.bench_bytes = reported;
    io = io_fake_open(reported, reported / 4, FAKE_ALIAS, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    ctx.io = io;
    CHECK_EQ_U64(pipeline_init(&ctx.pl, io, chunk, PAT_CHACHA20, 0xfeedbeef, reported, 2, err,
                               sizeof err),
                 0);
    rc = bench_run(&ctx);
    CHECK(rc == 1);
    CHECK(ctx.st[0].chunks_failed > 0);
    pipeline_free(&ctx.pl);
    io->close(io);

    T_BEGIN("bench reports I/O errors from the device");
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.dev = &dev;
    io = io_fake_open(reported, reported / 4, FAKE_ERROR, &e);
    CHECK(io != NULL);
    if (io == NULL)
        return;
    ctx.io = io;
    CHECK_EQ_U64(pipeline_init(&ctx.pl, io, chunk, PAT_CHACHA20, 0xfeedbeef, reported, 2, err,
                               sizeof err),
                 0);
    CHECK(bench_run(&ctx) == -1);
    CHECK(ctx.st[0].io_errors > 0);
    pipeline_free(&ctx.pl);
    io->close(io);
}

void test_io_and_detection(void)
{
    test_io_fake_honest();
    test_io_fake_policies();
    test_detection();
    test_alias_attribution();
    test_window_pass();
    test_speed_track();
    test_checkpoint();
    test_safety_mounted();
    test_verdict();
    test_bench();
}
