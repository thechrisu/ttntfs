# Performance baseline — Linux core, 2026-11-27

Stream C (perf measurement) baseline. Measurement only: no write-path,
caching or flush behaviour was changed in this round.

## What was measured, and what was not

- **Measured here:** the NTFS core on Linux, through `ntfscli`
  (`tools/ntfscli/main.c`, `bench` + new `randread` subcommands) on
  file-backed NTFS images. This is the core's own cost: page cache,
  run lists, `ntfs_write`/`ntfs_read`, `ntfs_fsync`, create/stat/
  readdir/unlink. No FSKit, no Swift, no macOS kernel, no real disk.
- **Not measured here, and not claimable from these numbers:** anything
  on macOS. The phase-5 bar is Apple's FSKit exFAT on macOS; the
  macOS numbers that exist are the 2026-09-13/14 ones in
  `docs/progress/vfs.md`, quoted below for the gap table. Do not quote
  the Linux numbers below as macOS numbers.
- **Harness:** `tools/perf-bench.sh` (repeatable: fresh 2 GiB image per
  repeat, `bench --size 256 --files 1000 --randops 2000`, then a
  persistent-file cold/warm random-read pass and a host-side ceiling).
  Raw output: `~/workspace/ttntfs-work/tmp/perf-baseline-2026-11-27.txt`
  (not committed; the tables below are transcribed from it).

## Setup

- Host: Linux 7.0.0-44-generic x86_64, 4 CPUs, 7934 MiB RAM, image on
  `/dev/mapper/rv` (shared virtual disk). Fork head `a7a8567f`.
- Image: 2 GiB NTFS, `mkntfs -s 4096 -c 4096`, bench file 256 MiB.
- Contention warning, read first: this VM runs three coordinators at
  once (1.5 shared CPUs) plus an LLVM build and the journal line's
  pre-push during the runs. Absolute throughput swings by 10× across
  repeats on the same setup. The repeatable findings are the
  **ratios and splits inside a single run**, not the absolute MB/s.

## Results — Linux core (4 repeats + 1 quiet smoke run)

| op | repeat 1 | repeat 2 | repeat 3 | repeat 4 | smoke (64 MiB, quiet) |
|---|---|---|---|---|---|
| seq write 256 MiB | 62.5 MiB/s | 75.2 | 67.7 | 111.3 | 169.3 MiB/s |
| seq write: writes / fsync | 3.82 s / 0.27 s | 3.28 / 0.13 | 3.49 / 0.30 | 1.55 / 0.75 | 0.20 s / 0.18 s |
| seq read 256 MiB | 228.9 MiB/s | 183.0 | 1152.6 | 202.1 | 614.5 MiB/s |
| rand write 4 KiB x2000 | 16241 IOPS | 8959 | 20483 | 12039 | 173057 IOPS |
| rand write: writes / fsync | 123 ms / 265 ms | 223 / 190 | 98 / 184 | 166 / 64 | 1.2 ms / 95 ms |
| rand read 4 KiB x2000 (warm) | 19371 IOPS | 22830 | 258330 | 197538 | 543118 IOPS |
| create 1000 files + sync | 251 /s | 561 | 776 | 439 | 1007 /s |
| lookup+stat 1000 files | 10289 /s | 29353 | 39981 | 69460 | 197993 /s |
| readdir(+attr) 1002 entries | 88932 /s | 364155 | 80769 | 371833 | 837074 /s |
| unlink 1000 files + sync | 3326 /s | 2485 | 2760 | 3596 | 3806 /s |

Host ceiling (plain file, same 256 MiB, python3 `os.pread`/`pwrite`,
same loaded machine): rand write 19262 IOPS (writes 104 ms, fsync
181 ms); rand read warm 114267 IOPS. The python loop itself caps the
host read number (per-call overhead), so read it as a floor, not a
ceiling.

Cold random reads: **not measurable on this VM.** Fresh-process
(`randread`, empty in-process page cache) runs gave 154–198 IOPS while
the host was loaded, and 17k–30k IOPS when it was quieter; a raw
`pread` on the image file itself gave 1152 IOPS at the same moment,
and `/proc/sys/vm/drop_caches` is read-only in this container, so the
host page cache cannot be dropped. The cold number here is the shared
virtual disk under load, not the driver. Cold-read measurement needs a
machine with a droppable host cache and an unshared disk.

`ntfsck -n` on a bench image after the run: clean, exit 0.

## Gap to the bar, per operation (macOS numbers, 2026-09-13/14)

From `docs/progress/vfs.md` — the last macOS measurement, not
re-measured in this round:

| op (macOS, matched 2 GiB images on internal NVMe) | ours | Apple exFAT | ratio | bar (within 2×) |
|---|---|---|---|---|
| stream 512 MiB write | 1782 MB/s | 1551 | 1.15× | met |
| overwrite 256 MiB | 2192 MB/s | 2160 | 1.01× | met |
| append 64 KiB ×2048 | 1992 MB/s | 1834 | 1.09× | met |
| small file + fsync | 575 /s | 859 | 0.67× | met |
| small file, no fsync | 684 /s | 1059 | 0.65× | met |
| random 4 KiB write | 3929 IOPS | 9616 | **0.41×** | **missed** |
| metadata op (XPC round trip) | 0.3–0.6 ms | — | 10–20× kernel `stat` | structural (FSKit) |

The Linux numbers above do not move any row of this table: they are a
different machine, a different stack, and a contended one. The table
stands until a macOS re-measurement runs.

## Where the time goes (ranked, evidence only)

1. **The flush, not the writes — random-write cost.** In the quiet
   smoke run the random-write pass spent 1.2 ms writing and 95.1 ms
   in `fsync` (99% of the pass). Under load the writes slow to
   98–223 ms while the fsync stays 64–265 ms, so the split moves to
   30–65% fsync — but the fsync never stops being a large fraction,
   and the host ceiling shows the same shape (104 ms writes, 181 ms
   fsync for the same 2000 ops on a plain file). This is the same
   shape as the 2026-09-14 macOS finding: for a warmed 32 MiB file
   the writes were equal (~630k IOPS both sides) and the entire gap
   was the flush (70–75 ms ours vs 22 ms exFAT; 457 MB/s vs
   1450 MB/s on the flush). The Linux split corroborates it; it does
   not re-prove it on macOS.
2. **Host contention on this VM swamps absolute throughput.**
   Sequential read 183–1152 MiB/s and warm random read 19k–258k IOPS
   across identical repeats; cold reads swing 154 IOPS → 30k IOPS
   with nothing in the driver changing. Any absolute MB/s or IOPS
   quoted from this VM is a load reading, not a driver reading.
3. **Warm random reads are fast when resident.** 197k–258k IOPS
   in-process; `strace -c` on a 2000-op cold-core pass shows 2055
   `pread64` calls at 3 µs each. The core read path is not the
   bottleneck when the pages are resident.
4. **Metadata create/unlink are sync-bound; lookup/stat are not.**
   Create+sync 251–1007 files/s and unlink+sync 2485–3806 files/s
   (the sync dominates both), against lookup+stat 10k–198k files/s
   and readdir 80k–837k entries/s. The spread on the non-sync ops is
   load, not code — same run, same image, 10× apart.

## Proposed follow-ups (recorded, not implemented this round)

1. **Fresh-clone build bug (found this round).**
   `tools/merge-static-libs.sh` is mode 100644 in git, but the top
   CMake build invokes it directly, so a fresh clone fails at the
   `ntfscore_merged` step with `Permission denied` (Error 127) until
   someone runs `chmod +x` on disk. The main working clone only
   builds because the bit was set there by hand. Fix: record the
   executable bit in git (one-line mode change, no code). Not fixed
   here — measurement only; the local clone has the bit set by hand.
2. **Split the fsync cost on macOS (open since 2026-09-14).**
   Still unknown whether the excess per-request flush cost sits in
   the Swift/FSKit boundary or in the C write path. Needs an
   instrumented build timing inside `NTFSVolume.write` against the
   `ntfs_write` it wraps. That is the next measurement before any
   write-path change, and any change it motivates is gated by §1.3
   (crash-consistency evidence, not just a faster number).
3. **Re-measure on a quiet machine before drawing conclusions.**
   Absolute throughput from this VM is not usable. The repeatable
   vehicle is `tools/perf-bench.sh`; run it on a Mac (or a quiet
   Linux box with a droppable host cache) for numbers that mean
   something. Cold reads in particular cannot be measured here.
4. **Metadata-op latency on macOS is unmeasured by this round.**
   The 0.3–0.6 ms XPC figure is from 2026-09-14; the in-core
   lookup+stat rates above are not comparable to it. A macOS-side
   metadata benchmark (stat/create/unlink latency through a real
   mount) is still missing.
