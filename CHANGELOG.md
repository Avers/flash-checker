# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-09-30

### Added
- Easy default output for general users: a single live `[██░░]` progress
  line for the whole session with a `[i/N]` step counter (bench-write,
  bench-read, sparse, retention, boundary, full-verify; quick=3,
  standard=5, adaptive/full=6 steps), plus a short result
  (`Real size ~= ...`,
  64-cell block map (░ untested, █ ok, # fail, ? io-error)

### Changed
- `-v/--verbose` now selects the full 0.2.0-style detail (per-stage
  headers, rates, summary); without it only the progress line and the
  short result are shown
- Full-verify progress now reports the real write rate instead of `0.00 B/s`

## [0.2.0] - 2026-09-28

### Added
- `--bench` subcommand with `--bench-bytes` to measure device write/read speed (PR #3)
- `--depth N` generator pool: expected-data generation now overlaps blocking I/O
  through a helper thread, default 4 (PR #2)
- io_uring SQE/CQE batching: optional `readv`/`writev` in the io vtable,
  wave-based submit/reap with per-segment retry, write-behind queue and
  read-ahead window in the pipeline, `pipeline_flush()` (PR #9)
- Write/read speed reporting for the sparse, retention and boundary stages (PR #6)
- Real mount detection: `/proc/mounts`, swaps and holders on Linux,
  `getfsstat` on macOS, with suffix-aware partition matching (PR #4)
- `tests/test_sync.c` — the sync backend is now covered directly; the suite
  runs 562 checks (test binary only: `./build/flashcheck-tests`)

### Changed
- `adaptive` is the default mode: standard detector chain, escalating to
  `full-verify` only when no counter-evidence was found (PR #7)
- `--mode quick` stops after the sparse probe (it used to also run retention
  and the capacity search) (PR #7)
- README warns that the default mode writes the entire reported capacity (PR #7)
- Stages flush through `pipeline_flush()` so batched backends commit at the
  right points (PR #9)

### Fixed
- io_uring backend could not run at all as shipped in 0.1.1: wrong opcodes and
  ring offsets, SQE/CQE sizes, `IORING_FEAT_SINGLE_MMAP`, head/tail ordering
  (`4f3c88c`)
- `--direct` is honoured by the uring backend, and asking for `--io-backend=uring`
  in a build without it warns instead of silently falling back (`e143198`)
- `--identify` (and any stage that writes nothing) never reports PASS (PR #1)
- `make asan` now really sanitizes the test run (PR #2)
- A mounted device is reported before the device is opened, instead of a
  generic `device busy` (PR #5)
- Sparse/retention/boundary reported `0.00s (0.00 B)` rates (PR #6)
- Sync backend wrote nothing with `--no-direct` (or when the `O_DIRECT` probe
  failed): transfers picked a non-existent companion fd and failed with EBADF (PR #11)
- Spurious `another flashcheck instance seems to be running` while udev holds a
  shared lock on the device; the lock now retries for ~2 s and the message
  carries the real errno (PR #11)
- 32-byte leak per `io_sync_open` in the sync backend (PR #11)
- CI smoke expectations are `bash -e` safe and the `honest` self-test uses
  matching capacities — CI has been green on every run since (PR #10)

## [0.1.1] - 2026-09-26

### Fixed
- Prevent division by zero when `--chunk-size` is 0 (`src/config.c`)

### Added
- Design Principles section in README summarizing core concepts
- OS support table (macOS via DKIOC, Linux via BLKIOCTL, other Unix partial)
- Checkpoint speed fields: `write_sec` and `read_sec` for progress reporting

### Changed
- Version bumped to 0.1.1

## [0.1.0] - 2025-09-26

### Added
- Initial release
- Multi-stage testing: benchmark, sparse probe, retention, full verify
- macOS support (DKIOCGETBLOCKCOUNT/BKIOCGETBLOCKSIZE)
- Color-coded PASS/FAIL/INCONCLUSIVE output
- Fake-device backend for CI testing
- Checkpoint/resume support
- JSON output
- Unit + integration test suite with fake device policies
- Makefile build system (release, debug, asan, lint targets)