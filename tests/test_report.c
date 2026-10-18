#include "flashcheck/config.h"
#include "flashcheck/report.h"
#include "flashcheck/test.h"
#include "test_util.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

void test_report(void)
{
    char dir[] = "/tmp/flashcheck-report-XXXXXX";
    char report[PATH_MAX], target[PATH_MAX], linkpath[PATH_MAX];
    config cfg;
    device_info dev = {0};
    io_stats stats = {0};
    io_ops io = {0};
    run_ctx ctx = {0};
    struct stat st;
    char content[16] = {0};
    int fd;

    T_BEGIN("JSON report is atomically created with private permissions");
    if (mkdtemp(dir) == NULL) {
        CHECK(0);
        return;
    }
    CHECK(1);
    snprintf(report, sizeof report, "%s/report.json", dir);
    snprintf(target, sizeof target, "%s/target", dir);
    snprintf(linkpath, sizeof linkpath, "%s/link.json", dir);
    config_defaults(&cfg);
    cfg.mode = MODE_IDENTIFY;
    dev.capacity = 4096;
    io.name = "test";
    io.stats = &stats;
    ctx.cfg = &cfg;
    ctx.dev = &dev;
    ctx.io = &io;

    CHECK_EQ_U64(report_json(&ctx, VERDICT_INCONCLUSIVE, report), 0);
    fd = lstat(report, &st);
    CHECK(fd == 0);
    if (fd == 0)
        CHECK_EQ_U64(st.st_mode & 0777, 0600);
    CHECK(unlink(report) == 0);

    T_BEGIN("JSON report refuses symlinks without changing their targets");
    fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    if (fd < 0)
        goto cleanup;
    CHECK(write(fd, "keep", 4) == 4);
    CHECK(close(fd) == 0);
    CHECK(symlink(target, linkpath) == 0);
    CHECK(report_json(&ctx, VERDICT_INCONCLUSIVE, linkpath) != 0);
    fd = lstat(linkpath, &st);
    CHECK(fd == 0);
    if (fd == 0)
        CHECK(S_ISLNK(st.st_mode));
    fd = open(target, O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(read(fd, content, sizeof content) == 4);
        CHECK(close(fd) == 0);
        CHECK(strcmp(content, "keep") == 0);
    }

cleanup:
    unlink(report);
    unlink(linkpath);
    unlink(target);
    rmdir(dir);
}
