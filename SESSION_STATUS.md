# Session Status — io_uring Backend (Correctness, Tests, O_DIRECT) & Read-Path Parallelization

## Correction to the Original io_uring Report

Commit `3fb6c63` ("feat(io): add io_uring backend with raw syscalls") claimed
"`make` builds successfully" and "self-tests pass with both backends". Both were
wrong:

- The build failed under gcc 16 (`-Werror -Wincompatible-pointer-types` on
  `src/io/uring.c:146`), and the broken commit was already pushed to `origin/main`.
- `--self-test` opens `io_fake_open()` (`src/main.c:90`) and **never touches the
  uring backend**, so the backend had never been executed at all.

---

## Round 1 — Correctness Fix + Test Coverage (commit `4f3c88c`)

### Bugs Fixed in `src/io/uring.c`

| Bug | Was | Effect |
|-----|-----|--------|
| Wrong opcodes | `READV=7, WRITEV=9, FSYNC=10` | Kernel sees `POLL_REMOVE`/`SENDMSG`/`RECVMSG`; real values are `READ=22, WRITE=23, FSYNC=3` |
| `READV` with raw buffer | `addr`=buffer, `len`=bytes | Needs `IORING_OP_READ`/`WRITE` |
| Ring offsets read from `params+0/+4` | `sq_entries`/`cq_entries` used as offsets | mmap size ~64 B instead of ~2 KB → garbage / segfault |
| `sq_off`/`cq_off` base | `sq` at 0, `cq` at +64 | Real offsets are 40 and 80 |
| Stray call | `uring_mmap_rings(NULL, &params)` | NULL deref on every open |
| `munmap(ptr, 0)` | rings never released | EINVAL + leak |
| SQE struct size | 56 B | Kernel's is 64 B → wrong stride |
| `IORING_FEAT_SINGLE_MMAP` | ignored | CQ pointer wrong on modern kernels |
| head/tail access | plain loads/stores | Needs acquire/release ordering |
| Retry/`io_errors` accounting | none | Now mirrors `sync.c` (EINTR/EIO/ETIMEDOUT/EBUSY with backoff) |

Layouts are pinned with `_Static_assert` (SQE 64 B, CQE 16 B, params 120 B,
`sq_off`@40, `cq_off`@80).

`tests/test_uring.c` was added (registered in `tests/test_main.c`), and
`include/flashcheck/io.h` declares `io_uring_open`/`io_uring_probe`
unconditionally so default-build tests can reach them (they were always compiled).

---

## Round 2 — io_uring Quick Wins (this round)

### Files Changed

| File | Change |
|------|--------|
| `src/main.c` | `--io-backend=uring` in a build without `FLASHCHECK_IO_URING` now **warns** ("rebuild with FLASHCHECK_IO_URING=1") instead of silently running sync |
| `src/cli.c` | `--help` notes the build-flag requirement |
| `include/flashcheck/io.h` | `io_uring_open(path, writable, want_direct, capacity, err)` — same signature shape as `io_sync_open` |
| `src/io/uring.c` | `O_DIRECT` support: direct fd + buffered companion, per-transfer fd choice using sync's rules (512 B sector alignment + capacity bounds), `flush()` fsyncs both fds, `close()` releases both; `direct_requested`/`direct_active` report reality |
| `tests/test_uring.c` | Two suites: buffered mode and `want_direct=1` mode (aligned buffers), incl. unaligned-offset fallback and ring-reuse |

### Verification (all exit 0)

| Command | Result |
|---------|--------|
| `make test` | ✅ **193 checks, 0 failures** + CLI smoke (176 → 193 this round; 162 originally) |
| `make FLASHCHECK_IO_URING=1 test` | ✅ 193 checks, 0 failures |
| `make asan` | ✅ sanitizer-clean, 193 checks |
| `make asan FLASHCHECK_IO_URING=1` | ✅ sanitizer-clean, 193 checks |

Environment: gcc 16.2, kernel `7.1.5+kali-amd64`, io_uring features `0x3ffff`.
The test log prints `[info] O_DIRECT active: yes`, so the direct path really runs.

**Still untested:** the new warning branch in `main.c` needs a block device to get
past `device_probe()` — cannot be exercised without a real `/dev/...` target
(AGENTS.md requires approval for that).

---

## Round 3 — Read-Path Parallelization (branch `ai/read-path-parallel`)

### What Was Built

Expected-data generation now overlaps blocking I/O via a generator pool with
one helper thread (Phase 3 plan item "read-path parallelization").

| File | Change |
|------|--------|
| `src/scheduler/pool.c` (new) | N-slot buffer pool (slots: FREE/FILLING/READY/BUSY), one worker thread, `gen_pool_prefetch/take/release`; inline-fill fallback when the worker is not ahead; `gen_ns` accumulated atomically. Depth 0/1 ⇒ no thread, single slot |
| `src/scheduler/pipeline.c` | `pipeline_write`/`pipeline_verify` take expected buffers from the pool; new `pipeline_prefetch()` and `pipeline_timing()`; `pipeline_init` gains a `depth` param (validated 0..16) |
| `src/test/{full,benchmark,retention,boundary}.c` | prefetch-next before each write/verify so `pattern_fill` runs while the previous `pread`/`pwrite` blocks |
| `src/cli.c`, `src/config.c` | `--depth N` (default 4), validated in `config_validate`, documented in `--help` |
| `src/report/console.c` | Summary prints `data generation:` / `device I/O:` split |
| `Makefile` | `-pthread` in `ALLCFLAGS` (covers compile + link; developer-approved) |
| `tests/test_pool.c` (new) | 16 checks: pool bytes == `pattern_fill` reference, inline fallback, depth 0, parallel ≡ sequential stats, alias detection with threads, out-of-order prefetch, timing > 0 |
| `tests/test_detection.c` | `run_case` takes a depth; honest/stale use 0, alias/unwritten use 4; window-pass cases use 4 |

Prefetch call sites: `pipeline_window_pass` (write + verify loops),
`pipeline_verify_offsets`, `stage_full`, `stage_benchmark`, `stage_retention`
fill, `stage_boundary` fill. Sparse/anchor loops are too short to benefit and
still use the inline path.

### Bug Found and Fixed During Development

`find_locked(g, SLOT_READY, 0, 0)` was used to mean "any READY slot", but the
function also filters `off == 0 && pass == 0`. Slot eviction therefore only
ever saw READY slots at offset 0 → worker pick failures → a
`pthread_cond_broadcast` livelock that starved the main thread and hung the
retention stage. Fixed with a separate `find_state_locked()` plus
wait-on-`cv_done` instead of spin (and `release` now broadcasts).

### Verification (all green)

| Command | Result |
|---------|--------|
| `make clean && make` | ✅ no warnings under `-Werror` |
| `make test` | ✅ **196 checks, 0 failures** (193 → 196) + CLI smoke |
| `CFLAGS="-fsanitize=address,undefined" make test` | ✅ sanitizer-clean |
| `CFLAGS="-fsanitize=thread" make test` | ✅ **no data races** |
| `--self-test=honest` 256 MiB, `--depth 0` vs default 4 | ✅ 5.4 s → 4.2 s wall (**~23 % faster**), CPU 113 %, both PASS |

### Known Issues Found Along the Way

1. ~~**`make asan` does not sanitize**~~ — **fixed in `8b99116` (PR #2)**: the
   recursive `$(MAKE) test` now receives `CFLAGS="$(CFLAGS)"` explicitly.
   Re-verified on macOS: `make asan` exits 0, test binary carries 1625
   asan/ubsan symbols and links `libclang_rt.asan_osx_dynamic.dylib`.

---

## Round 4 — Safety Realism, Stage Speed Stats, Adaptive Default (PRs #4–#7)

### What Landed

| PR | Commit | Change |
|----|--------|--------|
| #4 | `a089a25` | `safety.c`: real mount detection — Linux `/proc/mounts` (`safety_mount_matches`, suffix-aware for `s<n>`, `p<n>`, digits) + macOS `getfsstat(MNT_NOWAIT)`; swaps/holders kept; `diskutil unmountDisk` hint; tests + README |
| #5 | `557eb92` | `main.c`: `safety_check` now runs **before** the writable open — macOS `EBUSY` used to beat the refusal with a generic exit-4 `device busy`; `device_probe` failure on a mounted partition maps to the friendly `device is in use` message; `lock_fd` open failure reports the real errno instead of "another flashcheck instance" |
| #6 | `f82cea6` | sparse/retention/boundary track write/read speed (were reported as `0.00s (0.00 B)`); sparse re-inits the read track so verify rates exclude write time |
| #7 | `87add86` | **`adaptive` is the new default mode**: standard detector chain, stop at first failure with capacity estimate, escalate to `full-verify` only when no counter-evidence was found; `--mode quick` now actually stops after the sparse probe (it ran retention + capacity search before); README warns the default writes the entire reported capacity |

### Real-Hardware Validation (macOS, `/dev/disk8` 1.1 TB fake-size USB)

- Detection: `RESULT: FAIL, first failure at 4.00 GiB` vs claimed 1000.00 GiB (quick mode)
- Mounted refusal: exit 3 + `run: diskutil unmountDisk /dev/disk8`, **no bench stage ran** (verified on dry-run and real write path)
- Identify on a mounted partition: `device is in use (...); run: diskutil unmountDisk`, exit 3
- Unmounted: identify/dry-run proceed, exit 0; volume reformatted after the destructive test (approved)

### Verification (all green)

| Command | Result |
|---------|--------|
| `make clean && make` | ✅ `-Werror` clean |
| `./build/flashcheck-tests` | ✅ **262 checks, 0 failures** (235 → 262) |
| `make test` / `make asan` | ✅ CLI smoke green (self-test exits: honest 0, alias 1, stale 1, error 4) |
| mode matrix (self-test) | ✅ default=adaptive escalates + PASS; quick stops after sparse + INCONCLUSIVE (exit 2); standard stops at boundary + PASS |

---

## Round 5 — io_uring SQE/CQE Batching (branch `ai/uring-batching`, task #1)

### What Was Built

Batching now spans three layers: the io vtable, the uring backend, and the
pipeline.

| File | Change |
|------|--------|
| `include/flashcheck/io.h` | `io_seg {buf, off, len, res}` (`res`: 0 on success, negative errno on failure) and optional `readv`/`writev` in `io_ops`; NULL means "caller falls back to per-op" |
| `src/io/uring.c` | Wave-based batched submit/reap: up to `ring_entries` (32) SQEs in flight, `uring_submit_wait()` submits and waits in one `io_uring_enter(SUBMIT\|GETEVENTS)`, CQEs consumed from the ring without syscalls; user_data = segment index + 1; per-segment retry (`EINTR/EIO/ETIMEDOUT/EBUSY/EAGAIN`), partial-transfer advance, ring-level failures poison `u->err` and fail-fast afterwards; slices of 64 segs for `>ring_entries` batches; single-op `read`/`write`/`fsync` unified through the same path |
| `src/io/fake.c` | `readv`/`writev` as loops over the existing single-op functions — the whole existing test suite therefore exercises the new pipeline batching |
| `include/flashcheck/pipeline.h` | `PIPELINE_BATCH_MAX 16`, `pipeline_pend`/`pipeline_rd` queue/window state, `pipeline_flush()` |
| `src/scheduler/pipeline.c` | Write-behind queue (drains at queue-full, before reads, at flush, at free; only when `depth >= 2` and the backend has `writev`); read-ahead window (arena of `depth × chunk`, filled sequentially, invalidated on every write); stats counted at enqueue and un-counted per failed segment on drain; zero-length verify at `off == capacity` kept succeeding as before |
| `src/test/*.c`, `tests/*.c` | 7 stage/`pipeline_window_pass` + 3 test call sites moved from `io->flush(...)` to `pipeline_flush(&pl, &st)` — **contract: with batching active, stages must flush through `pipeline_flush`** (the sync backend has no vector ops, so its behaviour is unchanged) |
| `tests/test_uring.c` | Batch suite: 40-SQE write+read beyond ring size (multi-wave), mixed aligned/unaligned/zero-length segments, per-segment failure reporting; **pipeline-over-uring integration**: write-behind queue + flush + read-ahead verify against a real temp file, including a byte corrupted after the write that must be detected through the batched read |
| `tests/test_pool.c` | Write-queue progress before flush, read-ahead serves sequential verifies, a new write invalidates the window, failed batch un-counts bytes and reports `io_errors` |

### Bugs Found and Fixed During Development

| Symptom | Root cause | Fix |
|---------|-----------|-----|
| `--self-test=honest`: `Stage capacity-boundary: FAILED 1 of 4 regions differ, first failure: 0.00 B`, then `mode gating` test failures + a segfault | Verifying `off == capacity` produces `len == 0`; the new read-ahead fill returned `-EINVAL` for an empty window, so `pipeline_verify` reported an I/O error and `probe_ok` counted it as a mismatch with `first_fail_off = 0` | Treat `len == 0` as an already-verified chunk (matches the old `pread(…, 0)` behaviour) |
| `bench measures write and read speed`: `bytes_written == 2 MiB` instead of 4 MiB | Write-behind stats were only applied at drain time, and the stage report is built after the last drain without another `speed_mark` | Count `chunks_written`/`bytes_written` at enqueue and subtract per failed segment on drain — progress, speed tracks and reports are then exact at all times |
| LeakSanitizer: 4 MiB leak from `pipeline_init` in `test_window_pass` | The test re-initialised a pipeline without freeing the first one (the arena made it larger) | `pipeline_free(&p)` before the re-init |

### Verification (all green)

| Command | Result |
|---------|--------|
| `make clean && make` / `make FLASHCHECK_IO_URING=1` | ✅ no warnings under `-Werror` |
| `make test` | ✅ **549 checks, 0 failures** (294 → 549) + CLI smoke (honest 0, alias 1, stale 1, error 4) |
| `make FLASHCHECK_IO_URING=1 test` | ✅ 549 checks, 0 failures |
| ASAN+UBSAN+LSan (`CFLAGS="-O0 -g -fsanitize=address,undefined …"`) | ✅ clean, 549 checks, no leaks |
| TSan (`CFLAGS="-O0 -g -fsanitize=thread"`, test binary) | ✅ 549 checks, **0 data races** |
| `make lint` | ✅ (clang-tidy not installed → `-Werror` build) |

### Known Issues / Follow-ups from This Round

1. **`make asan` cannot compile on this toolchain**: `src/util.c:29: null format
   string [-Werror=format-overflow=]` at `-O1` — **reproduced on the unmodified
   baseline**, so it is pre-existing and out of scope here; sanitizer runs used
   `-O0` instead. Needs a separate fix (approve before touching `src/util.c`).
2. `--self-test` always opens `io_fake_open()` and **ignores `--io-backend`**, so
   backend throughput comparisons are not possible through it; uring coverage
   comes from `tests/test_uring.c` (roundtrip, batching, O_DIRECT, and the new
   pipeline integration). Real-hardware `uring` vs `sync` comparison remains
   outstanding (needs a block device, i.e. developer approval).
3. The read-ahead window is invalidated by `pipeline_write` only. Every current
   stage writes before it verifies, so no stale data can be served; if a future
   stage verifies ranges written by an earlier run (`--resume`), the window is
   empty at start and reads fresh.

---

## Round 6 — Real-Hardware Session: fake 1 TB stick, three bugs found

### Setup

Developer-granted access to `/dev/sdc`: generic "Mass Storage Device", serial
`121220160204`, USB 2.0 (480 Mb/s), **claims 1000.00 GiB**, no filesystem,
unmounted, not swapped. Root came from a temporary `/etc/sudoers.d/flashcheck`
listing only exact device-pinned command lines (removed at the end of the
session); every destructive run was approved before it ran.

Preflight: `--identify` (0 B written) → `--destructive --dry-run --yes`
(plan printed, 0 B written) → destructive runs.

### Detection result

| Run | Result |
|-----|--------|
| `--mode quick --io-backend sync` | **FAIL**, first failure **4.00 GiB**, 7/24 regions differ, "region at 4.00 GiB was never written" |
| `--mode quick --io-backend uring` | **FAIL**, byte-identical stage stats — same verdict, same offsets |

Real capacity ≈ **4 GiB** of a claimed 1000 GiB (0.4 %). This closes two
long-standing follow-ups: the `--io-backend=uring` CLI path on a real block
device, and the real-hardware `uring` vs `sync` comparison.

### Backend comparison (`--bench --bench-bytes 512MiB`, USB 2.0)

| Backend | O_DIRECT | Write | Read |
|---------|----------|-------|------|
| sync | yes | 9.15 MiB/s | 5.89 MiB/s |
| sync | buffered | 210.70 MiB/s | 8.39 MiB/s |
| uring | yes | 9.26 MiB/s | 5.60 MiB/s |
| uring | buffered | 263.51 MiB/s | 8.40 MiB/s |

Buffered writes are page-cache speed, not device speed; the O_DIRECT rows are
the device numbers. **uring matches sync within noise — no regression from the
SQE/CQE batching.**

### Bugs found and fixed

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | `--no-direct`: exit 4, 1 I/O error, **0 bytes written** (sync only, uring fine) | `src/io/sync.c:73,82` chose `s->fd_buf` whenever `can_direct()` was false, but `fd_buf == -1` when `want_direct == 0` → `pwrite(-1, …)` → EBADF | new `pick_fd()`: use the companion fd only when it exists — mirrors `uring.c:315` |
| 2 | Spurious `another flashcheck instance …` (exit 3) on any run started <2 s after the previous one | `(udev-worker)` takes a **shared** flock on the device inode while re-probing; `LOCK_EX\|LOCK_NB` failed instantly, with no retry and no errno in the message | `safety_lock_device()` retries for ~2 s (10 × 200 ms), preserves `errno`, and `main.c` now prints it |
| 3 | 32-byte leak per `io_sync_open` (LSan caught it the moment the sync backend had a test) | `sync_close()` freed `s` but never `s->ops.stats`; `fake.c:148` and `uring.c:563` both do | `free(s->ops.stats)` before `free(s)` |

New `tests/test_sync.c` (+13 checks): opens the sync backend with
`want_direct=0`, round trip, unaligned offset, flush, past-EOF read.
Reverting fix 1 makes it fail with `EBADF` (`-9`) — verified, so the regression
test really guards the bug.

### Verification (all green)

| Command | Result |
|---------|--------|
| `make clean && make FLASHCHECK_IO_URING=1` | ✅ no warnings under `-Werror` |
| `./build/flashcheck-tests` | ✅ **562 checks, 0 failures** (549 → 562) |
| `make test` | ✅ 562 checks + CLI smoke green |
| ASAN+UBSAN+LSan (`-O0`) | ✅ clean, 562 checks, no leaks |
| TSan (`-O0`) | ✅ 562 checks, **0 data races** |
| Hardware: 5 back-to-back `--dry-run` | ✅ 5/5 exit 0 (was 1/5 before fix 2) |
| Hardware: `--bench --no-direct` (sync) | ✅ exit 0, 0 I/O errors (was exit 4 before fix 1) |

### Follow-ups from this round

1. `--identify` prints `serial: 1.00` — that is the SCSI *revision* field; the
   real serial (`121220160204`, from INQUIRY/udev) is not shown.
   `src/device/device.c` is a protected file — needs approval.
2. With `want_direct=1` on a filesystem that rejects `O_DIRECT`, sync reports
   `direct_active = 1` although the fd actually used is buffered. Harmless for
   I/O, wrong in the JSON report.

---

## Current Project Status

### Phase Completion
- **Phase 0 (Research)**: ✅ Complete (design.md, IMPLEMENTATION_PLAN.md)
- **Phase 1 (Skeleton + v0.1)**: ✅ Complete
- **Phase 2 (Patterns, Sparse, Retention, Boundary)**: ✅ Complete
- **Phase 3 (Performance)**: 🟡 io_uring backend correct/tested/O_DIRECT-aware; read-path parallelization done (generator pool, `--depth`); bench subcommand done (PR #3); SQE/CQE batching landed (PR #9); **real-hardware validation done (Round 6) — uring == sync on a fake 1 TB stick, three bugs fixed**
- **Phase 4 (Reporting/Resume/Orchestration)**: ✅ Complete (adaptive orchestration landed in PR #7)

### What's Next (Priority Order)

| Priority | Task | Phase | Effort |
|----------|------|-------|--------|
| 1 | Batched release: `CHANGELOG.md` + `VERSION` bump | — | Low |
| 2 | Fix `make asan` on gcc 16 (`src/util.c:29` null format string at `-O1`) | — | Low |
| 3 | `--identify` should report the real serial, not the SCSI revision (`device.c`) | — | Low |

Landed this session: bench subcommand (PR #3), macOS/Linux mount detection
(PR #4), mount-refusal ordering (PR #5), stage speed stats (PR #6),
adaptive default (PR #7), io_uring SQE/CQE batching (PR #9), CI smoke fix
(PR #10), real-hardware session + three fixes (Round 6).

### Known Follow-ups (io_uring)

1. ~~Single SQE/CQE, submit-and-wait per op~~ — batching landed in Round 5 (PR #9).
2. ~~Real-hardware comparison of `--io-backend=uring` vs `sync`~~ — done in Round 6 (within noise of sync).
3. ~~Exercise the `--io-backend=uring` CLI path on a real block device~~ — done in Round 6 (identical verdict to sync).

---

## Notes for Linux Testing

1. Build with `make FLASHCHECK_IO_URING=1`
2. Compare `--io-backend=uring` vs `--io-backend=sync` performance ~~— done, Round 6~~
3. io_uring requires kernel 5.1+ (`IORING_OP_READ`/`WRITE` need 5.6+)
4. Test with USB 2.0/3.0 flash drives to verify no regression
5. **Never trust `--self-test` as backend coverage — it always uses the fake device**
6. `--direct/--no-direct` now applies to the uring backend as well

---

## Commits

```
feat(pipeline): parallelize read path with a generator pool and helper thread
```

```
4f3c88c fix(io): correct io_uring opcodes and ring offsets, add functional test
```

```
feat(io): honour --direct in the io_uring backend, warn when uring is unavailable

- io_uring_open() gains want_direct and opens an O_DIRECT fd plus a buffered
  companion, picking per transfer with the same sector/capacity rules as the
  sync backend; flush() fsyncs both fds
- direct_requested/direct_active now report what the backend actually does
- requesting --io-backend=uring in a build without FLASHCHECK_IO_URING logs a
  warning instead of silently falling back to sync; --help documents the flag
- tests cover buffered and O_DIRECT modes, including the unaligned fallback
- Verified: make, FLASHCHECK_IO_URING=1, make test and make asan in both
  variants; 193 checks, 0 failures (was 176)
```
