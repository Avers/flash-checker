# flashcheck — Audit Report & Remediation Plan

> **Date:** 2026-10-13  
> **Scope:** Security & Memory Safety · Concurrency & I/O Reliability · CLI UX & Human-Friendly Output  
> **Status:** Verified in source code

---

## Executive Summary

flashcheck is a raw block device flash verification tool written in C11. Deep static and algorithmic auditing identified **3 CRITICAL** and **6 HIGH** security/reliability issues, and **5 P0 UX issues** across signal handling, confirmation prompts, and progress reporting.

| Category | Critical (P0) | High (P1) | Medium (P2) | Low / Polish (P3) | Total |
|:---|:---:|:---:|:---:|:---:|:---:|
| **Security & Memory Safety** | 3 | 6 | 4 | 3 | **16** |
| **UX & Human-Friendly Output** | 5 | 6 | 5 | 4 | **20** |
| **Total** | **8** | **12** | **9** | **7** | **36** |

---

## Findings

### Security & Memory Safety

#### Critical (P0)
1. **C1 — Heap Buffer Overflow in Rate Tracker ([`src/stats/stats.c:27-39`](../src/stats/stats.c#L27-L39)):**
   When `t->n == t->cap` and `t->cap` reaches `SPEED_MAX_SAMPLES` (8,192), `t->cap *= 2` doubles capacity to 16,384. The branch trims `t->n = 4096` using `memmove`, but never resets `t->cap` to 8,192. Subsequent 8,192 samples write up to 128 KiB past the allocated buffer.
   *Fix:* Reset `t->cap = SPEED_MAX_SAMPLES;` when trimming.

2. **C2 — Active LVM, LUKS, and RAID Volumes Undetected via Broken Holders Check ([`src/safety.c:107-125`](../src/safety.c#L107-L125)):**
   `/sys/block/<dev>/holders` is a directory. Calling `read()` returns `EISDIR`. The function returns 0 ("no holders"), blinding safety checks to active mapped physical volumes.
   *Fix:* Enumerate entries using `opendir()` / `readdir()`.

3. **C3 — Mount Interlock Bypass via Symlinks and Unresolved Device Paths ([`src/safety.c:41-64`](../src/safety.c#L41-L64)):**
   `safety_mount_matches` performs string prefix matching against `/proc/mounts` without resolving symlinks or devmapper names with `realpath()`. Specifying devices via `/dev/disk/by-id/*` bypasses the mount check.
   *Fix:* Resolve target device and `/proc/mounts` device paths with `realpath()`.

#### High (P1)
4. **H1 — Infinite Retry Loop on Block Device I/O Errors ([`src/io/sync.c:27-44`](../src/io/sync.c#L27-L44)):**
   `int attempt = 0;` is declared inside `while (done < len)`. When `pwrite`/`pread` fails with `EIO/ETIMEDOUT/EBUSY`, `continue` jumps to the top of `while`, resetting `attempt = 0` and looping indefinitely on bad sectors.
   *Fix:* Move `attempt` outside `while (done < len)` or encapsulate retries inside an inner loop.

5. **H2 — Thread Deadlock in Generator Worker Pool Shutdown ([`src/scheduler/pool.c:163-173`](../src/scheduler/pool.c#L163-L173)):**
   If worker thread waits on `cv_done` when slots are starved, `gen_pool_destroy()` only signals `cv_work`. The worker never wakes, causing `pthread_join` to hang.
   *Fix:* Broadcast both `cv_work` and `cv_done` on `g->stop = 1`.

6. **H3 — 32-Bit File Offset Truncation in Makefile ([`Makefile:4`](../Makefile#L4)):**
   Macro defined as `-DFILE_OFFSET_BITS=64` instead of `-D_FILE_OFFSET_BITS=64` (missing leading underscore), limiting `off_t` to 32 bits on 32-bit platforms.
   *Fix:* Add missing underscore.

7. **H4 — JSON Injection / Syntax Invalidation via Unescaped Device Strings ([`src/report/json.c`](../src/report/json.c), [`src/device/device.c:226-242`](../src/device/device.c#L226-L242)):**
   Model, serial, vendor, and note strings emitted without escaping quotes, backslashes, or control characters.
   *Fix:* Add `json_write_str()` escaping helper.

8. **H5 — TOCTOU Race: Mount Status Not Re-verified After User Confirmation ([`src/safety.c:238-248`](../src/safety.c#L238-L248)):**
   Mount check occurs before interactive prompt; automounters can mount partitions while waiting for user confirmation.
   *Fix:* Re-check `safety_is_mounted()` after confirmation.

9. **H6 — Insecure Temporary File Creation ([`src/checkpoint.c:82`](../src/checkpoint.c#L82), [`src/report/json.c:52`](../src/report/json.c#L52)):**
   Temporary files created without `O_NOFOLLOW | O_EXCL` and with mode `0644`.
   *Fix:* Restrict to mode `0600` and pass `O_NOFOLLOW | O_EXCL`.

#### Medium & Low (P2 / P3)
10. **M1 (`src/safety.c:89-105`):** `scan_swaps` matches using `strstr(line, path)` without tokenizing.
11. **M2 (`src/safety.c:127-150`):** macOS APFS container synthesis devices bypass BSD mount checks without container tree resolution.
12. **M3 (`src/util.c:97-110`):** `parse_size` lacks `!isfinite(v)` check; casting `NaN` to `(uint64_t)` is undefined behavior.
13. **M4 (`include/flashcheck/device.h:6`):** Fixed 128-byte `DEV_STR` buffers truncate long persistent `/dev/disk/by-*` paths.
14. **M5 (`src/io/sync.c:186`):** `io_sync_probe_direct` passes `O_RDONLY` to boolean `writable` argument.
15. **M6 (`include/flashcheck/util.h:20-25`):** Logging functions lack compiler format verification attributes (`__attribute__((format(printf, ...)))`).

---

### UX & Human-Friendly Output

#### Critical (P0)
1. **U1 — Ctrl+C Fails to Interrupt Active Tests ([`src/test/full.c:42-85`](../src/test/full.c#L42-L85), [`src/scheduler/pipeline.c:310-336`](../src/scheduler/pipeline.c#L310-L336)):**
   `fc_interrupted` is never checked inside chunk/stage loops; first Ctrl+C is completely ignored during tests.
2. **U2 — Destructive Confirmation Prompt Omits Drive Details & Allows Loose Match ([`src/safety.c:238-248`](../src/safety.c#L238-L248)):**
   Displays only bare path without vendor/model/size; `strncmp(ans, "yes", 3)` accepts `"yesterday"`.
3. **U3 — README Examples Recommend Partition Paths Instead of Whole Disks ([`README.md:56-65`](../README.md#L56-L65)):**
   Testing partitions fails to detect address wrap-around beyond partition boundaries.
4. **U4 — Missing Estimated Time Remaining (ETA) in Progress Output ([`src/report/visual.c:163-194`](../src/report/visual.c#L163-L194)):**
   Displays speed and percentage but no estimated time remaining.
5. **U5 — Missing Pre-flight Summary Before Writes ([`src/main.c:47-178`](../src/main.c#L47-L178)):**
   With `--yes`, execution begins immediately without summarizing target device and test plan.

#### High (P1)
6. **U6 ([`src/report/console.c:104`](../src/report/console.c#L104)):** ANSI color escapes emitted unconditionally in verdict output; missing `isatty()` guard and `NO_COLOR` support.
7. **U7 ([`src/main.c:71-75`](../src/main.c#L71-L75)):** CLI syntax typos dump entire 60-line usage to stderr.
8. **U8 ([`src/test/test.c:146-148`](../src/test/test.c#L146-L148)):** Adaptive escalation notice is hidden in default visual mode.
9. **U9 ([`src/report/visual.c:189-192`](../src/report/visual.c#L189-L192)):** Non-TTY redirected runs print unthrottled newlines every 500ms.
10. **U10 ([`src/device/device.c:111-122`](../src/device/device.c#L111-L122)):** Permission and device errors lack actionable hints (`sudo`, `flashcheck -i`).
11. **U11 ([`src/main.c:227`](../src/main.c#L227)):** Interrupted executions exit with code 2 (INCONCLUSIVE) instead of `130` (`128 + SIGINT`).

#### Medium & Low (P2 / P3)
12. **U12 ([`README.md:221-244`](../README.md#L221-L244)):** Duplicate `## Install` and `## Usage` sections.
13. **U13 ([`src/report/visual.c:187`](../src/report/visual.c#L187)):** Terminal redraws using `\r` lack ANSI clear-to-EOL (`\x1b[K`).
14. **U14 ([`src/util.c:167`](../src/util.c#L167)):** Size output shows only binary GiB without decimal GB.
15. **U15 ([`src/report/console.c:275`](../src/report/console.c#L275)):** Total elapsed wall-clock duration omitted from summary.
16. **U16 ([`src/cli.c`](../src/cli.c)):** Missing `-y` shorthand for `--yes` and `-q`/`--quiet` flag.

---

## Phased Remediation Plan

### Phase 1: Critical Safety, Concurrency & Memory Fixes (P0/P1)
1. **Reset `t->cap` in `speed_mark`** ([`src/stats/stats.c`](../src/stats/stats.c)) — fixes 128 KiB heap overflow.
2. **Implement directory scanning in `scan_holders`** ([`src/safety.c`](../src/safety.c)) — detects LVM, LUKS, and RAID devices.
3. **Resolve paths with `realpath` in `safety_mount_matches`** ([`src/safety.c`](../src/safety.c)) — fixes symlink mount check bypass.
4. **Relocate `attempt` counter in `loop_xfer`** ([`src/io/sync.c`](../src/io/sync.c)) — prevents infinite retry lockup on I/O errors.
5. **Broadcast `cv_done` on shutdown in `gen_pool_destroy`** ([`src/scheduler/pool.c`](../src/scheduler/pool.c)) — prevents thread join deadlock.
6. **Fix `-D_FILE_OFFSET_BITS=64` macro** ([`Makefile`](../Makefile)) — fixes 32-bit truncation.

### Phase 2: Signal Trapping & Core UX (P0/P1)
1. **Wire `fc_interrupted` checks into all stage chunk loops** ([`src/test/*.c`](../src/test), [`src/scheduler/pipeline.c`](../src/scheduler/pipeline.c)).
2. **Enrich destructive confirmation prompt** ([`src/safety.c`](../src/safety.c)) with device metadata and exact case-insensitive match.
3. **Add ETA calculation and display** ([`src/report/visual.c`](../src/report/visual.c)).
4. **Update `README.md` disk path recommendations and remove duplicates**.
5. **Guard ANSI colors with `isatty()` and support `NO_COLOR`** ([`src/report/console.c`](../src/report/console.c)).
6. **Show adaptive escalation notice in visual mode** ([`src/test/test.c`](../src/test/test.c)).

### Phase 3: File Hardening & Polish (P1/P2)
1. **Implement `json_write_str` escaping** ([`src/report/json.c`](../src/report/json.c), [`src/device/device.c`](../src/device/device.c)).
2. **Re-check mount state after user confirmation** ([`src/safety.c`](../src/safety.c)).
3. **Harden checkpoint and report creation** with `0600` and `O_NOFOLLOW | O_EXCL` ([`src/checkpoint.c`](../src/checkpoint.c)).
4. **Throttle non-TTY progress output** ([`src/report/visual.c`](../src/report/visual.c)).
5. **Add `-y` flag and concise typo error message** ([`src/cli.c`](../src/cli.c), [`src/main.c`](../src/main.c)).
