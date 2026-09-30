# Windows Port Plan for flash-checker

## Decision: Hybrid Approach

Shared `core/` library (portable C11) + platform-specific `platform/linux/` and `platform/windows/` directories. CMake picks the correct platform subdirectory at configure time.

**Rationale:** ~60% of the code is portable (crypto, patterns, test logic, reporting, CLI, stats, checkpoint). ~40% is platform-specific (device I/O, safety, scheduler, I/O backends). Avoids code duplication while keeping platform differences cleanly separated.

---

## Proposed Directory Structure

```
flash-checker/
├── core/                    # Portable, zero #ifdef
│   ├── crypto/              # sha256, chacha20
│   ├── pattern/             # pattern generation/verification
│   ├── test/                # test stages, verdict logic
│   ├── report/              # console + JSON output
│   ├── stats/               # speed tracking
│   ├── checkpoint/          # save/load
│   └── cli/                 # argument parsing
├── platform/
│   ├── linux/               # device.c, safety.c, sync.c, uring.c, pool.c
│   └── windows/             # device.c, safety.c, sync.c, pool.c
├── include/flashcheck/      # Public headers (shared)
├── tests/                   # Shared test suite
├── CMakeLists.txt           # Picks platform/ subdir at configure time
└── Makefile                 # Thin wrapper
```

---

## POSIX → Windows API Mapping

| POSIX API | Windows Equivalent |
|-----------|-------------------|
| `pread`/`pwrite` | `ReadFile`/`WriteFile` with `OVERLAPPED` |
| `open(path, flags)` | `CreateFileW(path, ...)` |
| `O_DIRECT` | `FILE_FLAG_NO_BUFFERING` |
| `O_CLOEXEC` | `HANDLE_FLAG_INHERIT` (clear) |
| `fdatasync` | `FlushFileBuffers` |
| `flock` | `LockFileEx` |
| `geteuid` | `CheckTokenMembership(Administrators)` |
| `clock_gettime` | `QueryPerformanceCounter` |
| `sigaction` | `SetConsoleCtrlHandler` |
| `nanosleep` | `Sleep` / `WaitableTimer` |
| `mmap` | `CreateFileMapping` + `MapViewOfFile` |
| `syscall(io_uring)` | IOCP or skip (see Phase 4) |

---

## 10-Phase Porting Plan

### Phase 1: Build System
- Add `CMakeLists.txt` as primary build system (cross-platform)
- Keep Makefile as thin wrapper calling `cmake --build`
- Define `FLASHCHECK_WINDOWS` and `FLASHCHECK_LINUX` platform macros
- Link against `pthread-win32` or use native Windows threads
- Handle `.exe` suffix and Windows path separators

**Files:** `CMakeLists.txt` (new), `Makefile` (modify)

### Phase 2: Platform Abstraction Layer
- New files: `include/flashcheck/platform.h`, `src/platform/platform_win.c`, `src/platform/platform_unix.c`
- Wrap all OS-specific calls behind a common interface

### Phase 3: Device Access
- Capacity: `DeviceIoControl(fd, IOCTL_DISK_GET_LENGTH_INFO, ...)`
- Geometry: `DeviceIoControl(fd, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, ...)`
- Sector size: `IOCTL_DISK_GET_DRIVE_GEOMETRY` → `BytesPerSector`
- Device path: `\\.\PhysicalDriveN` instead of `/dev/sdX`
- Vendor/Model/Serial: `IOCTL_STORAGE_QUERY_PROPERTY` → `STORAGE_DEVICE_DESCRIPTOR`
- Removable: `STORAGE_DEVICE_DESCRIPTOR` → `RemovableMedia`
- USB VID/PID: `IOCTL_STORAGE_QUERY_PROPERTY` + registry lookup
- Mount detection: `GetVolumePathNamesForVolumeNameW`
- Swap detection: Registry `PagingFiles` key

**Files:** `src/device/device.c` (major refactor), `src/device/device_win.c` (new)

### Phase 4: I/O Backends

#### Sync Backend
- Replace `pread`/`pwrite` with `ReadFile`/`WriteFile` + `OVERLAPPED`
- Replace `O_DIRECT` with `FILE_FLAG_NO_BUFFERING`
- Replace `fdatasync` with `FlushFileBuffers`
- Keep dual-fd strategy (aligned + unaligned fallback)

#### io_uring Backend
- **Recommendation:** Skip on Windows — return `ENOTSUP`, fall back to sync backend
- Implement Windows IOCP backend for v2.0 if performance matters

#### Fake Backend
- No changes needed (pure computation)

**Files:** `src/io/sync.c` (major refactor), `src/io/uring.c` (guard with `#ifndef FLASHCHECK_WINDOWS`)

### Phase 5: Safety & Signals
- Root check: `OpenProcessToken` + `CheckTokenMembership` for Administrators group
- Mount detection: `GetVolumePathNamesForVolumeNameW`
- Swap detection: Registry `PagingFiles` key
- File locking: `LockFileEx` with `LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY`
- Signal handling: `SetConsoleCtrlHandler` for Ctrl+C / Ctrl+Break
- Sleep: Replace `nanosleep` with `Sleep(ms)`

**Files:** `src/safety.c` (major refactor), `src/util.c` (signal handler changes)

### Phase 6: Scheduler (Generator Pool)
- **Option A (Recommended):** Use `pthreads-win32` library (minimal code changes)
- **Option B:** Replace with Windows threads (`CreateThread`, `CreateMutex`, `CreateEvent`)
- **Option C:** Use C11 `<threads.h>` (requires MinGW-w64 or MSVC 2022+)

**Files:** `src/scheduler/pool.c` (conditional compilation or rewrite)

### Phase 7: Crypto & Patterns
- No changes needed — pure computation, no OS calls
- `src/crypto/sha256.c`, `src/crypto/chacha20.c`, `src/pattern/pattern.c` are fully portable

### Phase 8: Tests
- Guard Linux-specific tests with `#ifdef __linux__`
- Add Windows-specific test cases (e.g., `CreateFile` on `\\.\PhysicalDrive0`)
- Adapt `test_sync.c` O_DIRECT rejection test for Windows
- Add CI workflow for Windows (GitHub Actions `windows-latest`)

**Files:** `tests/test_sync.c`, `tests/test_detection.c`, `.github/workflows/ci.yml`

### Phase 9: CLI & Reporting
- `getopt_long`: Use MinGW-w64 implementation or switch to cross-platform parser
- ANSI colors: Windows 10+ supports ANSI; for older Windows use `SetConsoleTextAttribute`
- JSON output: No changes needed

**Files:** `src/cli.c` (conditional compilation), `src/report/console.c` (Windows color support)

### Phase 10: CI/CD & Release
- Add Windows build job (MSVC or MinGW-w64)
- Add Windows release artifact (`.exe`)
- Update `Makefile` `install` target for Windows
- Update `README.md` with Windows build instructions

**Files:** `.github/workflows/ci.yml`, `.github/workflows/release.yml`, `README.md`

---

## Execution Order

```
Phase 1 (Build System)     ← Foundation, do first
    ↓
Phase 2 (Platform Layer)    ← Abstraction for everything below
    ↓
Phase 3 (Device Access)    ← Depends on Phase 2
Phase 4 (I/O Backends)     ← Depends on Phase 2
Phase 5 (Safety/Signals)   ← Depends on Phase 2
    ↓
Phase 6 (Scheduler)        ← Can parallel with 3-5
    ↓
Phase 7 (Crypto/Patterns)  ← No changes, verify only
    ↓
Phase 8 (Tests)            ← After all source changes
    ↓
Phase 9 (CLI/Reporting)    ← Polish
    ↓
Phase 10 (CI/CD)          ← Final integration
```

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| `FILE_FLAG_NO_BUFFERING` alignment differs from Linux O_DIRECT | Both require 512-byte alignment; test thoroughly |
| Windows device path (`\\.\PhysicalDriveN`) requires Admin | Document requirement; add UAC manifest |
| io_uring performance gap on Windows | Fall back to sync backend; document performance difference |
| `pthreads-win32` maintenance | Consider C11 `<threads.h>` with MinGW-w64 |
| MinGW vs MSVC differences | Standardize on MinGW-w64 for gcc compatibility |

---

## Open Questions

1. **Compiler preference:** MinGW-w64 (gcc) or MSVC? MinGW is easier for POSIX compatibility.
2. **Threading:** `pthreads-win32` or C11 `<threads.h>` or native Windows threads?
3. **io_uring:** Skip for v1.0 or implement IOCP backend?
4. **Device path:** Support `\\.\PhysicalDriveN` only, or also `\\.\CdRomN`, `\\.\TapeN`?
5. **Distribution:** Standalone `.exe` or installer (MSIX/NSIS)?

---

## Estimated Effort

| Phase | Lines Changed | Complexity |
|-------|--------------|------------|
| 1. Build System | ~150 | Low |
| 2. Platform Layer | ~400 | Medium |
| 3. Device Access | ~300 | High |
| 4. I/O Backends | ~350 | High |
| 5. Safety/Signals | ~250 | Medium |
| 6. Scheduler | ~100 | Low |
| 7. Crypto/Patterns | 0 | None |
| 8. Tests | ~200 | Medium |
| 9. CLI/Reporting | ~150 | Low |
| 10. CI/CD | ~100 | Low |
| **Total** | **~2000** | |
