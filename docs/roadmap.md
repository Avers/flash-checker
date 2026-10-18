# flashcheck — Future Roadmap & Feature Plans

> **Document Version:** 1.0  
> **Status:** Proposed  
> **Last Updated:** 2026-10-13  

---

## 1. Functional Features

### 1.1 Non-Destructive In-Place Verification (`--safe-roundtrip`)
* **Objective:** Enable testing of drives containing existing user data without reformatting or data loss.
* **Mechanism:**
  * Segmented rolling buffer: read original block into memory (or temporary scratch storage on disk).
  * Write pseudorandom verification pattern to the block.
  * Flush and read back to verify integrity.
  * Restore the original data block back to the drive and flush.
* **Safety:** Automatic write barriers, crash-safe transaction logging to ensure interrupted tests never corrupt original data.

### 1.2 Fake-Drive Capacity Carving (`--carve-real-size`)
* **Objective:** Make counterfeit drives (e.g. 128 GiB fake drives with only 32 GiB real flash) safe and usable.
* **Mechanism:**
  * Once the true capacity threshold is determined via `capacity-boundary` stage, write a fresh partition table (MBR or GPT).
  * Clamp total usable space strictly to verified safe capacity (minus safety headroom).
  * Provide an option to initialize an exFAT or FAT32 filesystem within the clamped boundary.

### 1.3 SMART & NAND Health Telemetry
* **Objective:** Detect worn, degraded, or failing flash memory cells beyond capacity spoofing.
* **Telemetry Vectors:**
  * **Linux:** SCSI ATA PASS-THROUGH and NVMe `ioctl` interfaces.
  * **macOS:** IOKit storage driver properties (`IOBlockStorageDevice`).
* **Metrics:**
  * Wear leveling count / percentage life remaining.
  * Reallocated sector / bad block count.
  * Controller temperature monitoring before and after stress tests.
  * Pre-test and post-test health comparison diff.

---

## 2. Core Engine & Safety Hardening

### 2.1 Checkpoint Auto-Resume (`--auto-resume`)
* **Objective:** Seamless recovery from accidental disconnects, power loss, or user interruptions.
* **Mechanism:**
  * Checkpoints stored with device hardware fingerprints (`serial`, `usb_vidpid`, capacity).
  * When invoked without flags on a device that has a recent checkpoint, detect it automatically and prompt:
    `"Found interrupted test from <timestamp> at 45.2% (offset 28.5 GiB). Resume? [Y/n]"`.

### 2.2 macOS APFS Container Tree Resolution
* **Objective:** Close security gap M2 by resolving physical partitions to synthesized APFS containers.
* **Mechanism:**
  * Inspect APFS container metadata via `diskutil info -plist` or IOKit framework.
  * Ensure physical partitions (e.g., `/dev/disk0s2`) belonging to mounted APFS containers (e.g., synthesized `/dev/disk3` holding `/System/Volumes/Data`) cannot be accidentally targeted.

### 2.3 Access Latency Profiling & Heatmap
* **Objective:** Identify decaying or marginal flash cells.
* **Mechanism:**
  * Track per-chunk I/O response time latency during write and read phases.
  * Flag high-latency blocks (> 500 ms) as suspicious / deteriorating even if bit comparison passed.

---

## 3. User Experience, Reporting & TUI

### 3.1 2D Terminal Block Map (TUI Mode)
* **Objective:** Comprehensive block visualization similar to Victoria / MHDD / badblocks.
* **Mechanism:**
  * Full-terminal curses/terminfo-free ANSI grid rendering.
  * Color-coded latency and status cells:
    * `[.]` fast access (< 50 ms)
    * `[-]` medium access (< 250 ms)
    * `[!]` slow / degrading access (> 500 ms)
    * `[#]` corrupted / alias mismatch
    * `[?]` hardware I/O error

### 3.2 Self-Contained HTML / SVG Dispute Reports (`--report-html`)
* **Objective:** Produce visual evidence reports for marketplace counterfeit refund disputes (AliExpress, Amazon, eBay).
* **Contents:**
  * Device hardware metadata, verified honest size vs. claimed capacity.
  * Throughput speed graphs over time.
  * Visual capacity corruption map with timestamped cryptographic hashes.
  * Embedded vector charts with zero external JavaScript or CSS dependencies.
