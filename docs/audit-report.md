# flashcheck — Project Audit & Recommendations

> **Audit date:** 2026-10-18
> **Scope:** Raw-device safety, interruption and resume behavior, I/O and reporting, build/test coverage, and project documentation.
> **Method:** Read-only source and documentation review, followed by the repository's clean release build and test suite. No physical block device was opened for testing.

## Executive Summary

The five P1 findings below and all three P2 findings have been addressed in the `ai/critical-audit-fixes` branch. The implementation now reports interrupted runs as `INCONCLUSIVE`, checks layered storage relationships, binds safety checks and locking to the opened device descriptor, stops unsubmitted writes on interruption, validates checkpoint device and run identity, writes JSON reports atomically with symlink protection and private permissions, builds and tests the optional io_uring backend in pull request CI, and uses the current repository slug and version in its binary installation example.

| Priority | Finding | Impact |
|---|---|---|
| P1 | Interrupted full runs can be reported as `PASS` — fixed in branch | A partial verification could appear to certify a device |
| P1 | Mounted-device checks miss some storage topology — fixed in branch | A disk backing an active filesystem could evade the write interlock |
| P1 | Interrupt handling can continue issuing writes — fixed in branch | Ctrl+C could allow queued destructive I/O to continue |
| P1 | Checkpoints are not bound to device or run settings — fixed in branch | Reusing a checkpoint could skip part of verification |
| P1 | Device checks are path-based and can race the writable open — fixed in branch | A path change could cause safety checks to inspect a different target |
| P2 | JSON report output follows symlinks and uses mode 0644 — fixed in branch | Privileged runs could overwrite a symlink target; reports could expose device identifiers |
| P2 | Optional io_uring backend is absent from normal PR CI — fixed in branch | Backend regressions may not be caught before release builds |
| P2 | README install example was outdated — fixed in branch | Readers could download an obsolete release from the wrong repository slug |

## Findings and Recommendations

### P1 — Interrupted verification can report `PASS`

**Evidence:** `run_verdict()` does not check `fc_interrupted` in `src/test/test.c:67-85`. `main()` computes and prints the verdict before its interrupt handling in `src/main.c:205-227`. `stage_full()` exits its loops on interruption and returns normally in `src/test/full.c:42-133`.

**Impact:** An interrupted full/adaptive run can print `RESULT: PASS` and write a JSON report with a passing verdict, even though the process later exits with status 130.

**Status:** Fixed in branch. Interrupted runs now produce `INCONCLUSIVE`; checkpoint completion requires full coverage without failure or interruption.

### P1 — Mounted-device interlock misses layered storage

**Evidence:** On Linux, `scan_holders()` checks only `/sys/block/<device>/holders` in `src/safety.c:136-165`; holders attached to a partition are under that partition's sysfs directory. On macOS, `scan_mounts_bsd()` compares mount source names in `src/safety.c:168-197` but does not resolve physical APFS stores to synthesized mounted containers. The roadmap already records this APFS gap in `docs/roadmap.md:49-53`.

**Impact:** A whole disk backing an active LVM/LUKS/RAID partition or mounted APFS container may not match the mounted device name. With `--yes`, the removable-device confirmation does not prevent writes to that in-use disk.

**Status:** Fixed in branch. Linux checks the opened device's sysfs partition and holder graph against mount and swap state. macOS checks APFS physical-store/container relationships and refuses destructive access when it cannot verify them.

### P1 — Device checks are path-based and can race the writable open

**Evidence:** `device_probe()` checks a path, `main()` calls `safety_check()` and only afterward opens the device for I/O in `src/main.c:112-157`. `safety_check()` ignores its `fd` argument in `src/safety.c:255-260`, and the caller passes `-1`.

**Impact:** A replaceable symlink or device-node path can refer to a different device between the safety check and the writable open. The mounted-state check is not bound to the descriptor that will receive writes.

**Status:** Fixed in branch. The final I/O descriptor is validated against the probed `st_rdev` and capacity before any test writes. Mounted-state checks and the exclusive lock use that descriptor, followed by another in-use check after locking.

### P1 — Ctrl+C does not stop all write paths promptly

**Evidence:** The standalone benchmark loops in `src/test/bench.c:39-63` do not check `fc_interrupted`. For batched backends, `pipeline_free()` calls `drain_writes()` in `src/scheduler/pipeline.c:43-50`; `main()` invokes cleanup after detecting interruption in `src/main.c:221-227`.

**Impact:** `--bench` can continue its full configured write/read run after Ctrl+C. An interrupted batched run can submit queued writes during cleanup, after the user has asked it to stop.

**Status:** Fixed in branch. Write submission and batch draining honor the interrupt flag; interrupted cleanup releases queued buffers without submitting them. Synchronous I/O already submitted is allowed to return before cleanup.

### P1 — Resume checkpoints are not bound to a device or test configuration

**Evidence:** The checkpoint contains a stage, test ID, offset, and counters (`include/flashcheck/checkpoint.h:8-18`). `main()` restores the ID and offset without validating the target device, capacity, tested range, chunk size, or pattern in `src/main.c:186-195`.

**Impact:** Reusing a checkpoint after changing devices or test settings can make `stage_full()` skip the earlier range. A run may then pass without fully verifying the current device's claimed range.

**Status:** Fixed in branch. Checkpoints now carry a SHA-256-derived device fingerprint, device ID and capacity, tested range, chunk size, pattern, and mode. Older, malformed, or mismatched checkpoints are rejected and the run starts from the beginning. Resume is disabled when the device does not provide a serial number for a stable fingerprint.

### P2 — JSON report creation followed symlinks and defaulted to world-readable

**Evidence:** `report_json()` now creates a same-directory temporary file with mode `0600`, flushes it, and publishes it with `rename()`. It checks the destination with `lstat()` and rejects symbolic links. `tests/test_report.c` verifies symlink rejection, target preservation, and private file permissions.

**Impact:** A privileged run can truncate a symlink target at the user-supplied report path. The resulting file can expose serial numbers and other device identifiers to local users.

**Status:** Fixed in branch. Report output uses a protected temporary file, rejects symlink destinations, publishes atomically, and restricts permissions to `0600`.

### P2 — io_uring was not covered by normal pull-request CI

**Evidence:** The Makefile enables io_uring only with `FLASHCHECK_IO_URING=1` (`Makefile:11-13`). The CI workflow now includes an Ubuntu job that builds and runs the test suite with io_uring enabled. Backend tests print a skip when the runner kernel does not permit io_uring.

**Impact:** Changes to the optional Linux backend may merge without compiling or exercising that path in routine pull-request checks.

**Status:** Fixed in branch. Pull request CI compiles and tests the io_uring backend on Ubuntu, with an explicit skip for unsupported runner kernels.

### P2 — README install example used an obsolete release location

**Evidence:** The README download example now uses `Avers/flash-checker`, the current `VERSION` value, and the asset naming pattern in `.github/workflows/release.yml`.

**Impact:** The old copy/paste download command could lead to an obsolete release location.

**Status:** Fixed in branch. The release URL uses the correct repository slug, and the example version matches the project `VERSION` file.

## Previously Reported Issues Rechecked

The following earlier findings appear fixed in the current source: speed tracker capacity trimming (`src/stats/stats.c:27-42`), directory enumeration of top-level holders (`src/safety.c:148-159`), mount-source path resolution (`src/safety.c:67-100`), bounded sync retries (`src/io/sync.c:20-55`), pool shutdown broadcasts (`src/scheduler/pool.c:165-175`), 64-bit file offsets (`Makefile:4`), JSON string escaping (`src/report/json.c:8-33`), exact confirmation matching and a second mount check after the prompt (`src/safety.c:285-324`), and interrupt exit status 130 (`src/main.c:221-227`).

The five P1 findings and all three P2 findings are addressed in the current branch.

## Verification

- `make clean && make && make test`: passed.
- `make debug`: passed.
- `make asan`: passed.
- Unit suite: 390 checks, 0 failures.
- CLI smoke checks: passed.
- No physical block device was accessed.
- The default test build does not enable io_uring. `FLASHCHECK_IO_URING=1` also compiled and passed tests; runtime io_uring tests were skipped because this kernel does not support io_uring.
- macOS-specific APFS behavior was not exercised in this Linux environment.
- No physical block device was accessed while implementing or verifying these changes.
