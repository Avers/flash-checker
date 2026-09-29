# Ideas for Optimal Detection of Scam Flash Drives

## 1. Goal

Develop a Unix (MacOSX / Linux) CLI utility for checking a USB flash drive that claims more memory capacity than is actually installed.

Main objectives:

- determine the actual available capacity;
- detect remapping of different LBAs to the same physical memory;
- detect controller behavior when the real capacity is exceeded;
- measure write and read speed;
- if possible, make the test significantly faster than fully filling the claimed capacity;
- support **destructive mode**, in which the contents of the flash drive are completely destroyed.

> Important: the regular file system is not part of the test. It is preferable to work directly with the block device (`/dev/sdX`).

---

## 2. Device Model

For Linux, the device looks approximately like this:

```text
Program
    |
    | read/write LBA
    v
/dev/sdX
    |
    v
USB Mass Storage
    |
    v
flash drive controller
    |
    | FTL
    | wear leveling
    | garbage collection
    | ECC
    v
NAND Flash
```

The program sees **logical blocks (LBA)**.

The controller independently decides which physical NAND page to write data to.

Therefore, the program should not attempt to directly determine the physical NAND address.

---

## 3. What a Fake Controller Does

Typical scheme of a cheap scam flash drive:

```text
Real NAND:
    32 GB

Controller reports:
    1 TB

Linux sees:
    1 TB of logical LBAs
```

The controller can:

1. operate normally within the real memory;
2. after the real memory is filled:
   - return old data;
   - reuse the same physical pages;
   - accept writes but not actually save them;
   - return errors;
   - degrade in speed;
   - use a combination of these mechanisms.

Therefore, a simple `lsblk` or reading the claimed size is not enough.

---

# 4. Main Test Idea

It is necessary to verify **the uniqueness of the contents of different logical regions after a large number of writes**.

Key principle:

> If two different LBAs return the same content after independent writes, even though we wrote different data there, there is a likelihood of aliasing / reuse of physical memory.

However, a simple two-address test is not sufficient.

For example:

```text
LBA A <- pattern A
LBA B <- pattern B

read A -> A
read B -> B
```

This does not yet prove that the entire capacity is genuine.

A test is needed that forces the controller to actually maintain a large amount of simultaneously existing unique data.

---

# 5. Two Testing Modes

## 5.1 Fast / heuristic

Goal — quickly detect obvious counterfeits.

Does not guarantee proof of genuine capacity.

Use:

- large blocks;
- several regions;
- unique patterns;
- repeated checks;
- random and strategically chosen LBAs.

Advantage:

- significantly fewer writes;
- quickly reveals most cheap counterfeits.

Disadvantage:

- the controller may randomly pass such a test.

---

## 5.2 Full destructive

Goal — verify the claimed capacity as reliably as possible.

Algorithm:

```text
for each large region:
    generate unique pattern
    write region
    verify region
```

Or a more efficient scheme:

```text
WRITE entire range
        |
        v
READ entire range
        |
        v
COMPARE
```

If necessary, perform multiple passes with different patterns.

This is the most reliable option, but it requires writing the entire claimed capacity.

---

# 6. Why You Shouldn't Just Write Zeros

Bad option:

```text
write zeros
```

Reasons:

- the controller may optimize repetitive data;
- NAND may have internal compression mechanisms;
- an identical pattern poorly detects aliasing;
- it is impossible to distinguish some types of data reuse.

**Unique data** is needed.

---

# 7. Patterns

Each large block must have a unique identifier.

For example:

```text
struct TestBlockHeader {
    uint64_t magic;
    uint64_t test_id;
    uint64_t lba;
    uint64_t sequence;
    uint64_t random_seed;
};
```

The rest of the block is filled with a deterministic PRNG.

For example:

```text
pattern = PRNG(seed = hash(test_id, lba))
```

Advantage:

- no need to store the entire written test array;
- data can be reproduced during verification;
- different LBAs receive different patterns;
- data can be verified in a streaming fashion.

---

# 8. Aliasing Check

A particularly useful test:

```text
LBA 0          <- pattern A
LBA 1 GB       <- pattern B
LBA 2 GB       <- pattern C
LBA 4 GB       <- pattern D
...
```

After writing:

```text
read LBA 0
read LBA 1 GB
read LBA 2 GB
...
```

If the controller starts returning:

```text
A
A
A
A
```

or the data starts matching an earlier region — this is a strong indicator of a problem.

But the test must be multi-pass.

---

# 9. Sliding Window

A good compromise between speed and reliability is the **sliding window**.

For example:

```text
window = 1 GB

WRITE:
    region 0
    region 1
    ...
    region N

READ:
    region 0
    region 1
    ...
    region N
```

Then the window moves:

```text
[0 ........ 1 GB]
       [1 GB ........ 2 GB]
              [2 GB ........ 3 GB]
```

This forces the FTL to simultaneously maintain many unique data sets.

---

# 10. Sequential Full Test

For maximum speed, sequential operation is preferable:

```text
WRITE
0 -> end

READ
0 -> end
```

rather than:

```text
random write
random read
```

Reason:

- USB works better with large sequential transfers;
- less overhead;
- higher throughput;
- easier to measure speed.

---

# 11. Block Size

Do not work with individual 512-byte sectors.

Proposed range for experiments:

```text
1 MiB
4 MiB
8 MiB
16 MiB
32 MiB
```

Initial implementation:

```text
block = 4 MiB or 8 MiB
```

Then select the optimal value experimentally.

---

# 12. Linux API

Main level:

```c
open()
pread()
pwrite()
close()
```

For the device:

```text
/dev/sdX
```

Useful flags:

```text
O_RDWR
O_DIRECT
```

`O_DIRECT` should be made optional: some USB Mass Storage devices and configurations may behave worse with it.

---

# 13. Caching

It is important to eliminate the situation where the test actually works with Linux RAM rather than the flash drive.

Need to consider:

```text
O_DIRECT
```

and/or:

```text
fsync()
fdatasync()
```

After writing, it is critical to ensure that data is sent below the file/block cache.

For a destructive raw-device test, it is preferable to build a pipeline around direct I/O and explicitly control flush/barriers.

---

# 14. io_uring

For subsequent optimization, a backend can be added:

```text
sync I/O
    |
    +-- pread/pwrite

async I/O
    |
    +-- io_uring
```

But the first implementation is better done with regular `pread/pwrite`.

Reason:

- easier to debug;
- easier to prove correctness;
- the USB flash drive itself usually becomes the bottleneck.

After that, it can be measured whether `io_uring` provides a real benefit.

---

# 15. Pipeline

For maximum speed, writing should not wait for the completion of each individual block.

Desired architecture:

```text
Generator
    |
    v
Buffer pool
    |
    v
Writer
    |
    v
USB
```

For reading:

```text
USB
 |
 v
Reader
 |
 v
Verifier
```

Number of buffers:

```text
N = 4..16
```

is selected experimentally.

---

# 16. Verification During Writing

Writing and verification can be combined.

For example:

```text
write block 0
write block 1
write block 2
...

after filling the window:

read block 0
read block 1
...
```

Result:

```text
       WRITE WINDOW
<-------------------->

              READ
              <---->
```

This reduces RAM consumption and allows detecting errors earlier.

---

# 17. Important Test After Exceeding Real Capacity

If, for example, the following is suspected:

```text
real = 32 GB
reported = 1 TB
```

one cannot simply stop after the first 32 GB.

It is necessary to continue:

```text
0 GB
8 GB
16 GB
24 GB
32 GB
40 GB
48 GB
...
```

and verify that previously written data remains unchanged.

Key point:

> A fake controller may accept new writes but destroy previously written data.

Therefore, the test must verify **old data after writing new data**.

---

# 18. The Most Interesting Algorithm — Retention / Overwrite Test

Example:

```text
A <- unique A
B <- unique B
C <- unique C
...
```

After filling a certain region:

```text
write new data far beyond its boundary
```

Then:

```text
read A
read B
read C
...
```

If old data has changed:

```text
A_expected != A_actual
```

we obtain proof of data loss.

This is especially important for controllers that simulate a large capacity.

---

# 19. Finding the Real Capacity Boundary

The degradation point can be searched for automatically.

Start:

```text
8 GB
16 GB
32 GB
64 GB
128 GB
...
```

After an error is detected, perform a binary search.

For example:

```text
32 GB  -> OK
64 GB  -> FAIL

48 GB  -> FAIL
40 GB  -> OK
44 GB  -> OK
46 GB  -> FAIL
...
```

We obtain an approximate boundary.

But:

> This boundary is not necessarily equal to the physical NAND size. It is the actually observed reliable logical capacity of the device.

---

# 20. Adaptive Test

It is useful to implement several stages.

### Stage 1 — Identify

Obtain:

```text
USB VID
USB PID
serial
manufacturer
model
reported capacity
logical block size
physical block size
```

Sources:

```text
/sys/block/sdX/
/sys/class/block/
/dev/sdX
udev
SCSI inquiry
```

---

### Stage 2 — Speed Benchmark

Measure:

```text
sequential write
sequential read
```

For example:

```text
write: 85 MB/s
read: 120 MB/s
```

---

### Stage 3 — Sparse Probe

Check a large number of LBAs with unique patterns.

---

### Stage 4 — Retention

Verify old data after writing new regions.

---

### Stage 5 — Full Test

If necessary, pass through the entire claimed capacity.

---

# 21. Don't Rely Only on Block Equality

Detection:

```text
block A == block B
```

by itself is not always proof of counterfeiting.

For example:

- the data may have coincidentally matched;
- the pattern may have been identical;
- the device may correctly return certain data;
- the controller may use deduplication/compression.

Therefore, patterns must be cryptographically or statistically distinguishable.

---

# 22. A Good Pattern

Practical option:

```text
seed = SHA-256(
    test_id ||
    LBA ||
    pass_number
)
```

Then:

```text
ChaCha20 / AES-CTR / a good PRNG
```

generates a data stream.

Advantage:

- practically eliminates random coincidences;
- data is reproducible;
- no need to store the original data.

---

# 23. Hashing

Additionally, one can compute:

```text
SHA-256(block)
```

But it is not necessary to compute SHA-256 of every block if comparison is done directly against a deterministic pattern.

For speed, it is better:

```text
generate expected data
compare(buffer, expected)
```

Hashing can be reserved for:

- the log;
- diagnostics;
- the final report.

---

# 24. Multithreading

It is not advisable to immediately create many write threads.

For a USB flash drive:

```text
1 writer
1 reader
```

is usually a good starting point.

Too many threads can:

- increase overhead;
- worsen sequentiality;
- trigger internal garbage collection;
- reduce speed.

Parallelism is better implemented as a pipeline with multiple outstanding I/Os rather than dozens of threads.

---

# 25. Speed Measurement

For each stage, measure:

```text
bytes
elapsed time
MB/s
MiB/s
```

Also record:

```text
min
max
average
```

and preferably speed over time intervals.

A particularly interesting graph:

```text
time
 |\
 | \
 |  \
 |   \________
 +----------------> written GB
```

A sharp drop in speed after a certain volume may be a diagnostic sign.

But a speed drop by itself **does not prove** that the flash drive is counterfeit: genuine devices also have SLC cache, garbage collection, and thermal throttling.

---

# 26. Checkpoints

During a large test, save:

```text
checkpoint:
    offset
    bytes_written
    bytes_verified
    write_speed
    read_speed
    errors
```

This will allow:

- resuming the test;
- analyzing the location of the first error;
- building graphs.

---

# 27. CLI Result Format

For example:

```text
flashcheck /dev/sdb --destructive --full

Device:
  /dev/sdb
  USB: 1234:5678
  Reported capacity: 1.00 TB
  Logical block size: 512 B

Write test:
  0.00 TB -> 1.00 TB
  Average: 83.4 MiB/s

Verification:
  PASS: 0.00 -> 24.00 GiB
  FAIL: 24.00 GiB

First corrupted region:
  24.37 GiB

Reliable capacity:
  approximately 24 GiB

RESULT:
  DATA INTEGRITY FAILURE
```

---

# 28. Result Levels

Don't output only:

```text
SCAM / NOT SCAM
```

It is better to output factual results:

```text
PASS
FAIL
INCONCLUSIVE
```

and separately:

```text
reported capacity
verified capacity
first corruption offset
read/write speed
I/O errors
data mismatches
```

This will prevent erroneous conclusions.

---

# 29. Important Limitation

The program does not directly test the NAND chip.

It tests:

> the way the device controller provides the claimed logical address space and stores data.

Therefore, situations are possible where:

```text
NAND = 64 GB
reported = 64 GB
```

and the test passes.

And:

```text
NAND = 64 GB
reported = 1 TB
```

and the test detects corruption after a certain volume.

---

# 30. Proposed Program Architecture

```text
flashcheck
│
├── device
│   ├── open
│   ├── identify
│   ├── capacity
│   └── geometry
│
├── io
│   ├── sync
│   └── io_uring
│
├── pattern
│   ├── generator
│   └── verifier
│
├── test
│   ├── benchmark
│   ├── sparse
│   ├── retention
│   └── full
│
├── scheduler
│   ├── sequential
│   └── sliding_window
│
├── statistics
│   ├── speed
│   ├── errors
│   └── capacity
│
└── report
    ├── console
    └── JSON
```

---

# 31. Implementation Priority

### Version 0.1

First, make the simplest possible verifiable implementation:

```text
open /dev/sdX
↓
get capacity
↓
sequential write
↓
sequential read
↓
compare
↓
speed + errors
```

### Version 0.2

Add:

```text
unique deterministic patterns
retention test
sparse test
capacity boundary search
```

### Version 0.3

Add:

```text
sliding window
buffer pool
asynchronous I/O
io_uring
```

### Version 0.4

Add:

```text
JSON output
resume/checkpoints
detailed statistics
automatic test selection
```

---

# 32. Main Optimization Principle

Do not attempt to immediately verify the entire 1 TB.

First, determine:

```text
"Can the device correctly store several large independent regions?"
```

If not — the counterfeit is detected quickly.

If yes:

```text
expand the range
↓
verify retention
↓
localize the first error
↓
only if necessary, perform a full destructive test
```

This way, one can obtain:

```text
fast test
        +
deep test
        +
full test
```

in a single program.

---

# 33. Investigate Separately Before Implementation

Before writing an optimized backend, it is advisable to verify:

1. How Linux reports the size of USB Mass Storage.
2. How `BLKGETSIZE64` and related ioctls work.
3. The behavior of `O_DIRECT` on USB flash.
4. The impact of `fsync/fdatasync`.
5. SCSI `SYNCHRONIZE CACHE`.
6. The maximum I/O size for a specific device.
7. The behavior of `io_uring` with `/dev/sdX`.
8. USB Bulk-Only Transport and/or UAS.
9. How to eliminate the influence of the file system — work only with the raw block device.
10. How to correctly determine that the device is unmounted before a destructive test.

---

# 34. Key Idea of the Entire System

The most important check should conceptually look like this:

```text
1. Write unique data to region A.
2. Write unique data to region B.
3. Write unique data to region C.
4. Go far beyond the assumed real capacity.
5. Return to A/B/C.
6. Verify them.
7. Repeat with range expansion.
```

It is the verification of **preservation of old data after writing new data** that is one of the most powerful ways to detect a controller that simulates a capacity larger than the physical NAND.

---

## Final Strategy

```text
              ┌──────────────┐
              │ Device info  │
              └──────┬───────┘
                     ↓
              ┌──────────────┐
              │ Speed test   │
              └──────┬───────┘
                     ↓
              ┌──────────────┐
              │ Sparse test  │
              └──────┬───────┘
                     ↓
              ┌──────────────┐
              │ Retention    │
              └──────┬───────┘
                     ↓
           ┌─────────────────────┐
           │ Failure detected?   │
           └──────┬────────┬─────┘
                  │ YES    │ NO
                  ↓        ↓
           estimate      extend
           capacity      test
                  │        │
                  └───┬────┘
                      ↓
              ┌──────────────┐
              │ Full test    │
              │ if required  │
              └──────────────┘
```

**Main optimization goal:** minimize the amount of physical writing necessary to detect a mismatch, but at the same time not rely on a single weak test. For final certification of the claimed capacity, a full destructive write/read test is still required.

