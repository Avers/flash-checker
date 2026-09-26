# Session Status — io_uring Backend (Correctness Fix + Test Coverage)

## Correction to Previous Session's Report

Commit `3fb6c63` ("feat(io): add io_uring backend with raw syscalls") claimed
"`make` builds successfully" and "self-tests pass with both backends". Both were
wrong:

- The build failed under gcc 16 (`-Werror -Wincompatible-pointer-types` on
  `src/io/uring.c:146`), and the broken commit was already pushed to `origin/main`.
- `--self-test` opens `io_fake_open()` (`src/main.c:90`) and **never touches the
  uring backend**, so the backend had never been executed at all.

---

## Completed in This Session

### Files Changed

| File | Status | Description |
|------|--------|-------------|
| `src/io/uring.c` | Rewritten | Corrected opcodes, ring-offset parsing, mmap sizing, SQE layout, barriers, cleanup |
| `include/flashcheck/io.h` | Modified | `io_uring_open()`/`io_uring_probe()` declared unconditionally (they were always compiled) |
| `tests/test_uring.c` | **NEW** | Functional test against a temp file — real io_uring round trip |
| `tests/test_main.c` | Modified | Registers `test_uring()` |

### Bugs Fixed in `src/io/uring.c`

| Bug | Was | Effect |
|-----|-----|--------|
| Wrong opcodes | `READV=7, WRITEV=9, FSYNC=10` | Kernel sees `POLL_REMOVE`/`SENDMSG`/`RECVMSG`; real values are `READ=22, WRITE=23, FSYNC=3` |
| `READV` with raw buffer | `addr`=buffer, `len`=bytes | Needs `IORING_OP_READ`/`WRITE` (or a real iovec) |
| Ring offsets read from `params+0/+4` | `sq_entries`/`cq_entries` used as offsets | mmap size ~64 B instead of ~2 KB → garbage / segfault |
| `sq_off`/`cq_off` base | `sq` at 0, `cq` at +64 | Real offsets are 40 and 80 |
| Stray call | `uring_mmap_rings(NULL, &params)` | NULL deref on every open |
| `munmap(ptr, 0)` | rings never released | EINVAL + leak |
| SQE struct size | 56 B | Kernel's is 64 B → wrong stride |
| `IORING_FEAT_SINGLE_MMAP` | ignored | CQ pointer wrong on modern kernels |
| head/tail access | plain loads/stores | Needs acquire/release ordering |
| Retry/`io_errors` accounting | none | Now mirrors `sync.c` (EINTR/EIO/ETIMEDOUT/EBUSY with backoff) |
| `direct_requested/active` | reported `1` | Now honest `0` — this backend does not use `O_DIRECT` |

Layouts are now pinned with `_Static_assert` (SQE 64 B, CQE 16 B, params 120 B,
`sq_off`@40, `cq_off`@80).

### New Test Coverage

`tests/test_uring.c` runs in every `make test` (skips with `[skip]` if the kernel
lacks io_uring):

- backend identity + capacity stats
- write/read round trip and data verification
- visibility of data written through the page cache path
- read past EOF → error and `io_errors` increment
- 100 write/read iterations to exercise ring head/tail reuse
- `flush()` (fsync SQE)

### Verification

| Command | Result |
|---------|--------|
| `make` | ✅ builds clean (gcc 16, `-Werror`) |
| `make FLASHCHECK_IO_URING=1` | ✅ builds clean |
| `make test` (both variants) | ✅ **176 checks, 0 failures** + all CLI smoke tests (was 162) |
| `make asan` (both variants) | ✅ AddressSanitizer + UBSan clean |

Tested on kernel `7.1.5+kali-amd64`, `io_uring_setup` features `0x3ffff`.
No block device was used — all uring I/O ran against a temp file.

---

## Current Project Status

### Phase Completion
- **Phase 0 (Research)**: ✅ Complete (design.md, IMPLEMENTATION_PLAN.md)
- **Phase 1 (Skeleton + v0.1)**: ✅ Complete
- **Phase 2 (Patterns, Sparse, Retention, Boundary)**: ✅ Complete
- **Phase 3 (Performance)**: 🟡 io_uring backend now correct + tested; read-path parallelization pending
- **Phase 4 (Reporting/Resume/Orchestration)**: ✅ Mostly complete

### What's Next (Priority Order)

| Priority | Task | Phase | Effort |
|----------|------|-------|--------|
| 1 | Read-path parallelization (overlap generator/CPU with I/O) | P3 | Medium |
| 2 | Bench subcommand for generator vs device throughput | P3 | Low |
| 3 | macOS safety improvements (diskutil/IOKit mount detection) | P4 | Medium |
| 4 | Adaptive orchestration enhancements (auto-escalate) | P4 | Low |

### Known Follow-ups (io_uring)

1. `--io-backend=uring` is accepted by the CLI but **silently ignored** in builds
   without `FLASHCHECK_IO_URING=1` — should warn instead.
2. The uring backend never opens with `O_DIRECT` (unlike sync) — pass
   `cfg.want_direct` into `io_uring_open()` and use aligned buffers
   (pipeline buffers are already `FC_ALIGN`-aligned).
3. Single SQE/CQE, submit-and-wait per op — batching is the natural next step
   and a prerequisite for read-path parallelization.
4. Real-hardware comparison of `--io-backend=uring` vs `sync` still outstanding.

---

## Notes for Linux Testing

1. Build with `make FLASHCHECK_IO_URING=1`
2. Compare `--io-backend=uring` vs `--io-backend=sync` performance
3. io_uring requires kernel 5.1+ (`IORING_OP_READ`/`WRITE` need 5.6+)
4. Test with USB 2.0/3.0 flash drives to verify no regression
5. **Never trust `--self-test` as backend coverage — it always uses the fake device**

---

## Commit

```
fix(io): correct io_uring opcodes and ring offsets, add functional test

- Use IORING_OP_READ/WRITE/FSYNC (22/23/3); old defines were POLL_REMOVE,
  SENDMSG and RECVMSG
- Parse sq_off/cq_off at their real params offsets (40/80) and size the ring
  mmaps from them (was mmapping ~64 bytes)
- Handle IORING_FEAT_SINGLE_MMAP, fix 56B -> 64B SQE layout, remove NULL
  mmap call, munmap with real sizes, add acquire/release barriers
- Report O_DIRECT honestly as inactive for this backend
- New tests/test_uring.c exercises the backend on a temp file (176 checks,
  was 162); declare io_uring_open/io_uring_probe unconditionally so the
  default build's tests can reach them
- Verified: make, FLASHCHECK_IO_URING=1, make test, make asan (both variants)
```
