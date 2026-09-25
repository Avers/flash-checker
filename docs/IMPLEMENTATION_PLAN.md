# Implementation Plan — `flashcheck`

Derived from `flash_scam_detection_design.md`. Target: C11, no external dependencies,
Linux first (macOS deferred to v0.4). Phases follow the design doc's v0.1 → v0.4 roadmap,
preceded by a research spike (design doc §33).

---

## 1. Scope

**In scope**
- Raw block-device testing (`/dev/sdX`), no filesystem involved.
- Deterministic, reproducible unique patterns; streaming generation and verification.
- Test modes: benchmark → sparse → retention → sliding-window/full destructive.
- Capacity-boundary estimation (exponential growth + bisection).
- PASS / FAIL / INCONCLUSIVE verdicts with measured evidence, never a bare "SCAM".
- Console + JSON reports, checkpoint/resume.
- A fake-device backend so the whole detection logic is testable in CI without hardware.

**Out of scope (v1)**
- NAND-level analysis, chip-off, electrical testing.
- Claiming physical NAND size — we only measure *reliable logical capacity*.
- Filesystem-level tests.
- Windows/macOS raw driver work (macOS raw I/O is in scope for v0.4, see §8).

---

## 2. Key technical decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | C11, `-std=c11 -Wall -Wextra -Werror`, Makefile only | Design doc is syscall-level C; zero-dep build on both Linux and macOS |
| D2 | Vendor minimal SHA-256 + ChaCha20 (`src/crypto/`) | Avoids OpenSSL on macOS (no libcrypto headers) and keeps the tool static-friendly; both are small and have published KATs |
| D3 | Every I/O chunk is **self-identifying**: `seed = SHA256(test_id ‖ pass ‖ chunk_index)` | Any chunk can be regenerated and verified at any time, at any offset, without storing data. Enables retention re-checks and resume |
| D4 | Each chunk begins with a 64-byte header (magic, version, test_id, pass, chunk_index, 64-bit tag) | Mismatches in the first 64 bytes localize corruption immediately; tags let us *name* the alias source instead of just saying "differs" (§21) |
| D5 | Tag table in RAM: 8 bytes per chunk (1 TB / 8 MiB = 128 Ki chunks = 1 MiB) | Turns "content mismatch" into "chunk 8123 aliased to chunk 41" — much stronger report |
| D6 | I/O is abstracted behind an `io_ops` vtable (`src/io/`) | sync backend first (§14), `io_uring` backend later with no changes above it; enables the fake device for tests |
| D7 | Verification is a streaming `memcmp` against regenerated data, not hashing | §23: avoids SHA-256 over the whole device. SHA-256 kept for the report only |
| D8 | Patterns are `chacha20` by default, with `prng` (xoshiro) and `zero` selectable | ChaCha20 resists compression/dedup and is ≥ 500 MB/s in plain C (above USB 2.0/3.0 flash throughput); `--pattern` allows measuring the generator's cost |
| D9 | Sliding window with in-window verify (write W, then read W back) | §9/§16: forces the FTL to hold many unique regions at once, finds errors earlier, bounds RAM |
| D10 | Verdict is computed from evidence classes, each with a confidence level | §28/§29: never overclaim; speed drop alone is never a failure signal (§25) |
| D11 | All writes are opt-in (`--destructive`) and gated by a safety interlock | Irreversible operation on a raw device |
| D12 | Exit codes: `0 PASS`, `1 FAIL`, `2 INCONCLUSIVE`, `3 usage/safety`, `4 I/O error` | Scriptable |

---

## 3. Repository layout

```
flashcheck/
├── Makefile
├── README.md
├── docs/
│   └── design.md                     # the investigation document
├── include/flashcheck/
│   ├── common.h                      # sizes, logging, fatal()
│   ├── util.h                        # human sizes, monotonic clock, percentiles
│   └── config.h                      # parsed CLI state
├── src/
│   ├── main.c                        # arg parsing → orchestration → report
│   ├── cli.c / cli.h                 # option table, help text
│   ├── safety.c / safety.h           # root check, mount check, lock, confirm
│   ├── device/
│   │   ├── device.c / device.h       # open, capacity (BLKGETSIZE64), geometry
│   │   └── identify.c                # /sys/block/sdX + SCSI INQUIRY, USB VID/PID
│   ├── io/
│   │   ├── io.h                      # io_ops vtable + request struct
│   │   ├── sync.c                    # pread/pwrite (+ O_DIRECT, retries)
│   │   ├── uring.c                   # io_uring backend (#ifdef)
│   │   └── fake.c                    # simulated device w/ scam policies (tests)
│   ├── crypto/
│   │   ├── sha256.c / sha256.h
│   │   ├── chacha20.c / chacha20.h   # seekable keystream
│   │   └── prng.c                    # xoshiro256** for --pattern=prng
│   ├── pattern/
│   │   ├── pattern.c / pattern.h     # seed derivation, header, fill/verify
│   ├── test/
│   │   ├── test.h                    # result struct, verdict logic
│   │   ├── benchmark.c               # Stage 2
│   │   ├── sparse.c                  # Stage 3
│   │   ├── retention.c               # Stage 4 / §34
│   │   ├── full.c                    # Stage 5
│   │   └── boundary.c                # §19 capacity estimate
│   ├── scheduler/
│   │   ├── pipeline.c / pipeline.h   # buffer pool, depth N, write+verify
│   │   ├── window.c                  # sliding window policy
│   │   └── anchors.c                 # anchor record + periodic re-verify
│   ├── stats/
│   │   ├── speed.c                   # interval speed series, min/avg/max
│   │   └── errors.c                  # error classification, first-failure trace
│   ├── checkpoint.c / checkpoint.h   # atomic write (tmp+fsync+rename)
│   └── report/
│       ├── console.c
│       └── json.c
└── tests/
    ├── test_main.c                   # tiny test harness
    ├── kat_crypto.c                  # FIPS-180 / RFC 8439 known-answer tests
    ├── test_pattern.c
    ├── test_math.c                   # chunk math, partial tail, capacity clamp
    ├── test_safety.c
    ├── test_checkpoint.c
    └── test_detection.c              # end-to-end vs fake device policies
```

---

## 4. Build & verification

```sh
make                 # release build → build/flashcheck
make debug           # -O0 -g3, ASan+UBSan
make test            # unit + fake-device detection tests (no hardware, no root)
make asan            # full test suite under sanitizers
make lint            # clang-tidy if present, else -Werror build
```

CI (GitHub Actions, Linux): `make test`, `make asan`, plus a `--fake-scam` run of the
binary to assert the end-to-end verdict is `FAIL` with exit code 1.

---

## 5. Core interfaces (sketch)

```c
/* io.h */
typedef struct io_req {
    void *buf; uint64_t off; size_t len; int is_write; int buf_id;
} io_req;

typedef struct io_ops {
    int  (*submit)(void *ctx, io_req *r);          /* returns 0 ok, -1 err */
    int  (*wait)(void *ctx, io_req *r);            /* block for completion  */
    int  (*flush)(void *ctx);
    void (*destroy)(void *ctx);
} io_ops;

/* pattern.h */
typedef struct { uint64_t test_id; uint32_t pass; uint64_t chunk_index; } pattern_id;
void  pattern_seed(const pattern_id *id, uint8_t seed[32]);
void  pattern_fill(void *buf, size_t len, const pattern_id *id, uint64_t off_in_chunk);
int   pattern_check(const void *buf, const pattern_id *id, uint64_t off_in_chunk,
                    pattern_err *err);             /* first mismatching offset + byte */
uint64_t pattern_tag(const pattern_id *id);        /* 64-bit alias fingerprint */
```

`pattern_fill` must support a *seek*: given the chunk seed and an offset, produce the same
bytes as generating the whole chunk from the start. ChaCha20 supports this natively via the
64-bit block counter; `prng` backend is reset per 1 MiB sub-block with a derived key so
seeking works there too.

---

## 6. Phases

### Phase 0 — Research spike (design doc §33) — half a day, throwaway code under `tools/spike/`

Answer empirically, on a real device, and record answers in `docs/findings.md`; several
design decisions depend on them:

1. How Linux reports USB MSC size: `BLKGETSIZE64` vs sysfs `size` — agreement?
2. Max single transfer size accepted (`/sys/block/sdX/queue/max_sectors_kb`).
3. `O_DIRECT` on `/dev/sdX`: alignment requirements, EINVAL rates, throughput delta.
4. `fdatasync()` cost; SCSI `SYNCHRONIZE CACHE` (needs SG_IO) — is it needed to bypass cache?
5. Read-back of a chunk immediately after write: cached or on-media? (determines whether
   verification is meaningful at all)
6. `io_uring` availability/benefit on USB mass storage (does it beat sync?).
7. Unmount detection: reliable check without invoking `lsblk` (parse `/proc/self/mountinfo`).
8. Error behaviour past true capacity: silent alias, stale data, dropped writes, or EIO.
9. Whether two identical-devices exist for A/B testing (real vs known-scam).

Deliverable: `docs/findings.md` + updated defaults in `include/flashcheck/config.h`.
Kill criterion: if O_DIRECT/flush semantics are unclear, Phase 1 ships with
`--no-direct` default and a mandatory `fdatasync()` every N chunks.

### Phase 1 — Skeleton + v0.1 (sequential full test) — 2–3 days

- [ ] `Makefile`, `common.h`, logging, human-readable size/time formatting, `--help`.
- [ ] `device.c`: open `O_RDWR|O_RDONLY`, `BLKGETSIZE64`, `BLKSSZGET`, `logical_block_size`
      from sysfs; refuse non-block devices and capacity 0.
- [ ] `identify.c`: model/serial/vendor from `/sys/block/sdX/device/*` + SCSI INQUIRY via
      `SG_IO`; USB VID/PID by walking up to the USB device node. Non-fatal on failure.
- [ ] `io/sync.c`: `pread`/`pwrite` with aligned buffers, bounded retries on `EIO`/`ETIMEDOUT`,
      `fdatasync` policy, per-stage timing.
- [ ] `crypto/`: SHA-256 and ChaCha20 + known-answer tests.
- [ ] `pattern.c` + streaming verify (§7, §22, §23).
- [ ] `test/full.c` (pass 1): sequential write → sequential read → compare, with early abort
      at first mismatch and a first-failure region report.
- [ ] `stats/speed.c`: bytes, elapsed, MB/s + MiB/s, interval series, min/avg/max.
- [ ] `safety.c`: root check, mount check, flock on the device, `--destructive` gate,
      interactive confirm.
- [ ] `report/console.c`: the §27 layout.
- [ ] `errors.c` + `test.h` verdict logic: distinguish `IO_ERROR` from `MISMATCH`.

Exit criteria: runs on a real stick end-to-end; unit + ASan suites green; a file-backed run
produces correct PASS; safety interlock refuses a mounted device.

### Phase 2 — v0.2 (patterns, sparse, retention, boundary) — 3–4 days

- [ ] `pipeline.c` with buffer pool depth N (default 8) — needed now for sparse/anchor
      phases to be usable; still sync backend.
- [ ] `test/sparse.c`: K anchors at exponentially spaced offsets (last one beyond plausible
      real size) with unique patterns, plus a post-write re-read of the earliest anchors
      (§8, §20 Stage 3).
- [ ] `test/retention.c` + `scheduler/anchors.c`: the §34 algorithm — anchors A/B/C, write
      far beyond them, re-verify; each pass re-anchors and extends the range. This is the
      primary detector; give it the most scrutiny and the best reporting.
- [ ] `scheduler/window.c`: region-wise write-then-verify sliding window (§9, §16).
- [ ] Tag table + alias attribution (§21, D5): on mismatch, search the tag table to name
      the source chunk; report "chunk X returned chunk Y's data".
- [ ] `test/boundary.c`: exponential growth 8→16→32→… until failure, then bisection to
      `--resolution` (§19). Explicitly report this as *reliable logical capacity*, with a
      note that it is not NAND size (§19, §29).
- [ ] `--passes n` with different `pass` values in the seed (§22).

Exit criteria: fake-scam policies `alias` and `stale` are detected by the sparse and
retention stages respectively, with a precise first-failure offset; an honest simulated
device of the same size yields PASS.

### Phase 3 — v0.3 (performance) — 3–4 days

- [ ] `io/uring.c` behind the vtable, `#ifdef FLASHCHECK_IO_URING`, graceful fallback;
      liburing-free (raw syscalls) or liburing if Phase 0 shows it helps.
- [ ] Buffer pool 4–16 with alignment for `O_DIRECT`; measure and pick defaults.
- [ ] Read-path parallelisation: overlap generator/CPU with I/O; 1 writer + 1 reader
      (§24) — no thread explosion.
- [ ] Generator throughput benchmark vs device throughput; if generation is >20% of wall
      time, consider generating on a helper thread (double-buffered).
- [ ] `--depth`, `--block-size` tuning flags; a `bench` subcommand to measure the
      configuration on a given device.
- [ ] Speed-drop anomaly recording (report, not a verdict input, per §25).

Exit criteria: measured speedup documented for a real stick; sync path remains the
fallback and produces byte-identical results.

### Phase 4 — v0.4 (reporting, resume, orchestration) — 3–4 days

- [ ] `report/json.c`: stable schema (device, stages, speeds, error classes, first-failure,
      reliable capacity, verdict, evidence, confidence).
- [ ] `checkpoint.c`: atomic `tmp`+`fsync`+`rename`; record offset, bytes written/verified,
      speeds, error count, tag-table digest. `--checkpoint FILE` / `--resume`.
- [ ] Adaptive stage selection (§20, §32): identify → benchmark → sparse → retention →
      stop on first failure (estimate capacity) or escalate to full on success. `--mode
      quick|standard|full` overrides.
- [ ] Verdict engine: aggregate per-stage evidence into PASS / FAIL / INCONCLUSIVE with
      a printed rationale; INCONCLUSIVE on I/O errors, unreadable sectors, throttling, or
      repeated retries (§28).
- [ ] macOS support: `/dev/rdiskN` (skip sector cache), `diskutil`/IOKit for identify and
      unmount; same core. `safety.c` becomes pluggable per-OS.
- [ ] `--json`, `--log`, `--verbose`, `--dry-run`, `--limit`, `--offset`, `--yes`.
- [ ] README: usage, interpretation, FAQ ("can I trust PASS?").

Exit criteria: `flashcheck /dev/sdb --destructive --full` produces the §27 report plus JSON;
an interrupted run resumes without false failures.

---

## 7. Test strategy

| Level | What | How |
|---|---|---|
| KAT | SHA-256, ChaCha20 | Published vectors (FIPS-180-4, RFC 8439 §2.4.2) |
| Unit | seed derivation determinism, cross-chunk uniqueness, seek == full generate | property test over random ids |
| Unit | chunk math: partial tail chunk, offset alignment, clamp to capacity | table tests |
| Integration | detection logic | `io/fake.c` with policies: `honest`, `alias`, `stale`, `drop_writes`, `error`, `slow`, `throttle`; assert verdict, first-failure offset, exit code |
| Integration | safety | refuses mounted device, refuses without `--destructive`, second instance refused by flock |
| Integration | resume | kill mid-run, resume, result identical to uninterrupted run |
| Perf | generator vs device | `make bench` on CI + real sticks |
| Manual | real hardware | matrix: honest 32/64/128 GB USB 2.0/3.0, known scam drives, SD-card readers |

`io/fake.c` is the keystone: a simulated device with a *real* size and a *reported* size
larger than real lets CI assert the exact detection semantics, including the boundary
bisection, without owning any scam hardware.

---

## 8. Safety interlock (must ship before any write path)

- Require root (or `CAP_SYS_RAWIO`); clear message otherwise.
- Refuse if the device or any partition appears in `/proc/self/mountinfo`, or if it is the
  root/boot device, or if `/sys/block/sdX/removable` is `0` (warn, require `--force`).
- Refuse without `--destructive`; require interactive `y/N` unless `--yes`.
- `flock(LOCK_EX|LOCK_NB)` on the device fd to prevent concurrent instances; also lock the
  checkpoint file.
- `--dry-run` prints the exact plan (offset, size, passes) and writes nothing.
- `--limit`/`--offset` for partial testing; refuse a range that extends past device size
  with a clear error rather than clamping silently.
- No `SIGINT`-unsafe state: on interrupt, flush + write a checkpoint labelled *incomplete*.

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| False FAIL on genuine flash (ECC decay, thermal throttling, marginal cells) | Classify mismatch vs I/O error vs speed drop separately; INCONCLUSIVE on mixed signals; retest pass; require *correlated* evidence (same chunk wrong twice, or wrong data equal to another chunk's pattern) for FAIL |
| False PASS (cheap controller that happens to pass sparse test) | Retention + extended-range passes are mandatory before PASS on `--mode standard`; full test is the only certification |
| `O_DIRECT` unsupported/misbehaving on USB | Feature-probe at open; fall back to buffered + `fdatasync`; expose in report |
| Device hangs (`ETIMEDOUT` storm) | Bounded retries, overall stage deadline, clear abort with checkpoint |
| Generator becomes the bottleneck | Benchmark, helper thread, `prng` pattern option |
| Real capacity never fails (a very good scammer) | Report INCONCLUSIVE / "not proven" rather than PASS-by-absence |
| `io_uring` on USB gives nothing | Benchmark first (§14), keep sync default |
| Test corrupts the host system | Safety interlock (§8); refuse non-removable internal disks without `--force` |

---

## 10. Execution order (dependency graph)

```
Phase 0 spike
   └─> P1: build, device, sync io, crypto, pattern, full test, safety, console
         └─> P2: pipeline, sparse, retention, anchors, boundary, tag table
               ├─> P3: io_uring, buffer tuning, overlap
               └─> P4: JSON, checkpoint/resume, adaptive stages, macOS
```

Critical path is Phase 1 → Phase 2. Phase 3 is optional for correctness; Phase 4's adaptive
orchestration is what turns the tool from a full-destroyer into a fast detector, so if time
is short, cut Phase 3 depth first and keep Phase 4.

---

## 11. Definition of done for v1.0

- `make && make test` green, no warnings, ASan/TSan clean.
- Correctly labels honest 32 GB / 64 GB / 128 GB sticks as PASS (or INCONCLUSIVE with
  reasons) and known scam drives as FAIL with a reliable-capacity estimate and first-failure
  offset.
- Never claims PASS in `--mode quick` — quick mode's best possible verdict is
  "no evidence of fraud within X GiB written".
- Full test verifies 100% of the reported capacity with a streaming, reproducible pattern.
- JSON output is schema-stable and documented in the README.
- Resume after interruption is exact.
- No write occurs without `--destructive` passing the safety interlock.
