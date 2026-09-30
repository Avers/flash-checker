<img src="badge.svg" width="72" alt="flashcheck logo">

# flashcheck

Detect USB flash devices that report more capacity than they really have.

[![Build](https://github.com/Avers/flash-checker/workflows/CI/badge.svg)](https://github.com/Avers/flash-checker/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

`flashcheck` writes unique, reproducible data directly to a raw block device and verifies it back. It detects controllers that map multiple logical addresses to the same physical storage (aliasing), discard old data when writing past real capacity (stale data), or simply never persist writes beyond their true limit.

## Features

- **Multi-stage testing**: benchmark → sparse probe → retention → full verify
- **Capacity boundary estimation**: exponential growth + bisection to find reliable capacity
- **Aliasing detection**: identifies when two LBA addresses return the same data
- **Color-coded results**: PASS (green), FAIL (red), INCONCLUSIVE (yellow)
- **Zero dependencies**: C11, Makefile-only build
- **Fake-device backend**: full test suite runs in CI without hardware
- **Checkpoint/resume**: interrupted tests can continue
- **JSON output**: machine-readable reports for automation

## Install

### Binary releases

Pre-built tarballs for Linux (x86_64, aarch64) and macOS (Intel, Apple
Silicon) are attached to every
[GitHub Release](https://github.com/Avers/flash-checker/releases) — no sources
and no build tools needed:

```sh
curl -LO https://github.com/Avers/flashcheck/releases/download/v0.2.0/flashcheck-0.2.0-linux-x86_64.tar.gz
shasum -a 256 -c flashcheck-0.2.0-linux-x86_64.tar.gz.sha256   # verify
tar xzf flashcheck-0.2.0-linux-x86_64.tar.gz
sudo ./flashcheck-0.2.0-linux-x86_64/install.sh               # → /usr/local/bin/flashcheck
flashcheck --version
```

Each tarball contains `flashcheck`, `LICENSE`, `README.md`, a portable
`install.sh` (`DESTDIR=/opt/bin sudo ./install.sh` to choose the location)
and a `.sha256` file. Linux builds need kernel 5.6+ for io_uring and fall
back to the sync backend automatically on older kernels.

### From source

```sh
make                     # build → build/flashcheck
sudo make install        # install to /usr/local/bin/flashcheck
```

## Usage

```sh
# Identify a device (no writes)
sudo flashcheck /dev/disk8s1 --identify

# Standard test (destructive, requires --yes to skip confirmation)
sudo flashcheck /dev/disk8s1 --destructive --yes

# Full test with custom block size
sudo flashcheck /dev/disk8s1 --destructive --yes --mode full --block-size 16MiB

# Dry run (see the plan, write nothing)
sudo flashcheck /dev/disk8s1 --destructive --dry-run --yes

# Self-test (no hardware needed)
flashcheck --self-test=honest --self-reported=256MiB --self-real=128MiB
```

## Modes

| Mode | Description |
|------|-------------|
| `identify` | Print device info only, never writes |
| `quick` | Benchmark + sparse probe only, never certifies capacity |
| `standard` | Probe + retention + capacity search, stops there |
| `adaptive` | Default. Standard stages, then full write/read verify only when no counter-evidence was found |
| `full` | Same stages as adaptive, explicit |

On a device that passes every probe, the default `adaptive` mode writes the **entire
reported capacity** — that full write/read round trip is what certifies it. Use
`--mode quick`, `--mode standard` or `--limit SIZE` to bound a run.

## Tuning

| Flag | Default | Description |
|------|---------|-------------|
| `--block-size SIZE` | `8MiB` | Region size used for write/verify chunks |
| `--window SIZE` | `1GiB` | Sliding write-then-verify window (`--mode full`) |
| `--passes N` | `1` | Retention passes |
| `--pattern KIND` | `chacha20` | Data pattern: `chacha20`, `prng` or `zero` |
| `--depth N` | `4` | Parallel data-generation depth (0-16); `0`/`1` generates inline, no helper thread |

Run `flashcheck --help` for the full list of tuning flags.

## Output

```
RESULT: FAIL
  7 regions differ, first at 4.00 GiB
```

| Verdict | Color | Meaning |
|---------|-------|---------|
| `PASS` | Green | Device returned every byte written over the tested range |
| `FAIL` | Red | Evidence of fake capacity (aliasing, stale data, unwritten regions) |
| `INCONCLUSIVE` | Yellow | Cannot certify — I/O errors, throttling, or need `--mode full` |

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | PASS |
| 1 | FAIL |
| 2 | INCONCLUSIVE |
| 3 | Usage/safety error |
| 4 | I/O error |
## Building from source

```sh
make                    # release build
make debug              # -O0 -g3 with AddressSanitizer + UBSanitizer
make test               # unit + fake-device detection tests
make asan               # full test suite under sanitizers
make lint               # clang-tidy if available, else -Werror build
make clean              # remove build artifacts
make version            # print current version
make bump part=patch    # bump version (patch|minor|major)
```

## macOS Setup

### Prerequisites

Xcode Command Line Tools must be installed:

```sh
xcode-select --install
```

This provides `clang` (the `cc` compiler), `make`, and standard headers.

### Build

```sh
make                     # build → build/flashcheck
./build/flashcheck --version   # verify
```

The Makefile detects the platform automatically. On macOS:
- Uses `clang` instead of `gcc`
- Uses `DKIOCGETBLOCKCOUNT`/`DKIOCGETBLOCKSIZE` instead of Linux `BLKGETSIZE64`
- `O_DIRECT` is disabled (not supported on macOS)

### Running on macOS

```sh
# Check what devices are available
diskutil list

# Identify a device (no writes needed)
sudo ./build/flashcheck /dev/disk8s1 --identify

# Unmount before destructive testing
sudo diskutil unmountDisk /dev/disk8s1

# Standard test (destructive)
sudo ./build/flashcheck /dev/disk8s1 --destructive --yes

# Dry run (see the plan, write nothing)
sudo ./build/flashcheck /dev/disk8s1 --destructive --dry-run --yes

# Self-test (no hardware needed)
./build/flashcheck --self-test=honest --self-reported=256MiB --self-real=128MiB --yes
```

### macOS Notes

| Topic | Detail |
|-------|--------|
| Device path | `/dev/diskN` (whole disk), `/dev/diskNs` (partition N) |
| Unmounting | `sudo diskutil unmountDisk /dev/diskN` |
| Root access | `sudo` required for raw block device access |
| `O_DIRECT` | Not supported on macOS; auto-disabled |
| Capacity detection | Uses macOS `DKIOCGETBLOCKCOUNT` ioctl |
| Mount detection | `getfsstat()`; refuses to write while the disk or any of its partitions is mounted |
| `diskutil` | Can be used to check device info before testing |

### Gatekeeper

macOS Gatekeeper may block the binary with *"cannot be opened because the developer cannot be verified"* or silently move it to Trash. Remove the quarantine attribute:

```sh
xattr -d com.apple.quarantine ./build/flashcheck
```

For installed releases:

```sh
xattr -d com.apple.quarantine /usr/local/bin/flashcheck
```

To check whether the attribute is present:

```sh
xattr ./build/flashcheck
```

### Troubleshooting

| Problem | Solution |
|---------|----------|
| `command not found: make` | Run `xcode-select --install` |
| `command not found: cc` | Run `xcode-select --install` |
| `Permission denied` on `/dev/disk*` | Use `sudo` |
| `device is mounted` | Run `sudo diskutil unmountDisk /dev/diskN` first |
| `cannot determine capacity` | Device may not be a block device; check `diskutil list` |
| Gatekeeper moves binary to Trash | Run `xattr -d com.apple.quarantine ./build/flashcheck` |

## Install

```sh
sudo make install        # install to /usr/local/bin/flashcheck
```

## Usage

```sh
# Identify a device (no writes)
sudo flashcheck /dev/disk8s1 --identify

# Standard test (destructive, requires --yes to skip confirmation)
sudo flashcheck /dev/disk8s1 --destructive --yes

# Full test with custom block size
sudo flashcheck /dev/disk8s1 --destructive --yes --mode full --block-size 16MiB

# Dry run (see the plan, write nothing)
sudo flashcheck /dev/disk8s1 --destructive --dry-run --yes

# Self-test (no hardware needed)
flashcheck --self-test=honest --self-reported=256MiB --self-real=128MiB
```

## Safety

- **Requires root** (or `CAP_SYS_RAWIO`) to access raw block devices
- **Refuses mounted devices** — unmount before testing
- **Requires `--destructive`** flag — all writes destroy data on the device
- **File lock** prevents concurrent instances
- `--dry-run` prints the exact plan and writes nothing

## Project Structure

```
├── Makefile
├── README.md
├── LICENSE
├── VERSION
├── CHANGELOG.md
├── CONTRIBUTING.md
├── docs/
│   └── design.md           # investigation & design document
├── include/flashcheck/     # public headers
│   ├── common.h
│   ├── config.h
│   ├── device.h
│   ├── io.h
│   ├── pattern.h
│   ├── test.h
│   ├── util.h
│   └── ...
├── src/                    # implementation
│   ├── main.c
│   ├── cli.c
│   ├── safety.c
│   ├── device/
│   ├── io/
│   ├── crypto/
│   ├── pattern/
│   ├── test/
│   ├── scheduler/
│   ├── stats/
│   └── report/
├── tests/                  # unit + integration tests
└── build/                  # build output (gitignored)
```

## How It Works

1. **Identify** — read device info (model, serial, capacity, block sizes)
2. **Benchmark** — measure write/read speeds
3. **Sparse probe** — write unique patterns at exponentially spaced offsets
4. **Retention** — write far beyond suspected capacity, then re-verify old regions
5. **Capacity search** — bisection to find the reliable capacity boundary
6. **Full verify** — write and verify the entire reported capacity (if `--mode full`)

Each chunk is self-identifying via `seed = SHA256(test_id ‖ pass ‖ chunk_index)`, enabling streaming verification without storing data.

## Design Principles

The core concept of flash-checker is to detect USB flash devices that report more capacity than they really have. The key design principles are:

- **Unique reproducible data**: Each write chunk uses a deterministic pattern seeded with `SHA256(test_id ‖ pass ‖ chunk_index)`, enabling streaming verification without storing all data in memory
- **Multi-stage testing**: Exponential growth + bisection to find the reliable capacity boundary, with sparse probe, retention, and full verify stages
- **Alias detection**: Identifies when two LBA addresses return the same data (controller mapping multiple logical addresses to the same physical storage)
- **Stale data detection**: Detects controllers that discard old data when writing past real capacity
- **Wear leveling resistance**: Tests must survive controller garbage wear leveling and garbage collection mechanisms
- **Sliding window**: A compromise between speed and reliability, forcing the controller to simultaneously support many unique data regions
- **Retention test**: The most important test — write unique data, then write new data far beyond, and verify old data still survives. This is the strongest way to detect fake capacity simulators.

Supported operating systems:

| OS | Status | Notes |
|----|--------|-------|
| **macOS** | Supported | Full support via `DKIOCGETBLOCKCOUNT`/`DKIOCGETBLOCKSIZE` ioctl. `O_DIRECT` disabled. Requires Xcode Command Line Tools. |
| **Linux** | Supported | Uses `BLKGETSIZE64`, `BLKSSZGET`, `BLKPBSZGET` ioctl. `O_DIRECT` optional. Tested with raw block devices (`/dev/sdX`). |
| **Other Unix** | Partial | POSIX-compliant systems should work with appropriate ioctl adaptation. |

## Contributing

All changes require developer approval. See [CONTRIBUTING.md](CONTRIBUTING.md) for rules.

## License

MIT — see [LICENSE](LICENSE).

## Versioning

This project follows [Semantic Versioning 2.0](https://semver.org/).

Format: `MAJOR.MINOR.PATCH` (e.g., `0.1.0`)

### Rules

| Bump | When |
|------|------|
| **PATCH** | Bug fixes, non-breaking changes |
| **MINOR** | New features, backwards-compatible additions |
| **MAJOR** | Breaking changes, removal of features |

### Files

| File | Purpose |
|------|---------|
| `VERSION` | Single source of truth for the current version |
| `CHANGELOG.md` | All notable changes |
| `include/flashcheck/common.h` | `FC_VERSION` macro (auto-generated from `VERSION`) |

### Commands

```sh
make version              # print current version
make bump patch           # bump PATCH version
make bump minor           # bump MINOR version
make bump major           # bump MAJOR version
```

> **Note:** `FC_VERSION` is injected at compile time via `-DVERSION_STR` from `VERSION`. No manual edits to `common.h` needed.
