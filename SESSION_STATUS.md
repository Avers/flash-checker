# Session Status — io_uring Backend (Correctness, Tests, O_DIRECT)

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

## Current Project Status

### Phase Completion
- **Phase 0 (Research)**: ✅ Complete (design.md, IMPLEMENTATION_PLAN.md)
- **Phase 1 (Skeleton + v0.1)**: ✅ Complete
- **Phase 2 (Patterns, Sparse, Retention, Boundary)**: ✅ Complete
- **Phase 3 (Performance)**: 🟡 io_uring backend correct, tested and O_DIRECT-aware; read-path parallelization pending
- **Phase 4 (Reporting/Resume/Orchestration)**: ✅ Mostly complete

### What's Next (Priority Order)

| Priority | Task | Phase | Effort |
|----------|------|-------|--------|
| 1 | Read-path parallelization (overlap generator/CPU with I/O) | P3 | Medium |
| 2 | Bench subcommand for generator vs device throughput | P3 | Low |
| 3 | macOS safety improvements (diskutil/IOKit mount detection) | P4 | Medium |
| 4 | Adaptive orchestration enhancements (auto-escalate) | P4 | Low |

### Known Follow-ups (io_uring)

1. Single SQE/CQE, submit-and-wait per op — batching is the natural next step and
   a prerequisite for read-path parallelization.
2. Real-hardware comparison of `--io-backend=uring` vs `sync` still outstanding.
3. Exercise the `--io-backend=uring` CLI path (and its warning) on a real block
   device — needs developer approval.

---

## Notes for Linux Testing

1. Build with `make FLASHCHECK_IO_URING=1`
2. Compare `--io-backend=uring` vs `--io-backend=sync` performance
3. io_uring requires kernel 5.1+ (`IORING_OP_READ`/`WRITE` need 5.6+)
4. Test with USB 2.0/3.0 flash drives to verify no regression
5. **Never trust `--self-test` as backend coverage — it always uses the fake device**
6. `--direct/--no-direct` now applies to the uring backend as well

---

## Commits

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
