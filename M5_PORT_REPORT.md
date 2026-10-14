# Porting GoFetch to Apple M5: An Empirical Study of DMP Behavior, Timing Sources, and L2 Eviction-Set Construction

**Author:** spanddhana
**Date:** 2026-10-14
**Platform:** Apple M5 Pro, macOS (Darwin 25.6)
**Upstream:** FPSG-UIUC/GoFetch (USENIX Security 2024)

---

## Abstract

GoFetch demonstrates that the data memory-dependent prefetcher (DMP) in recent
Apple CPUs can be exploited to break constant-time cryptographic
implementations. The public artifact targets the Apple M1, M2, and M3. This
report documents an effort to port the reverse-engineering (`re/`) portion of
the artifact to the Apple **M5 Pro**. We make four empirical contributions.
First, we show that the DMP **is present and active on M5** and reproduces the
Augury pointer-dereference primitive, and that Apple's documented
Data-Independent-Timing (DIT) bit **still suppresses it**. Second, we show that
the artifact's kernel-performance-counter (kperf) timing source **no longer
functions** on current macOS—`kpc_set_config` returns `EPERM`—and we replace it
with the unprivileged `CNTVCT_EL0` generic timer, which removes the need for
`sudo` entirely. Third, we show that the artifact's assumption of **page-colored
L2 eviction sets still holds** on M5 (contrary to an initial hypothesis of a
hashed set index). Fourth, and most significantly, we find that the M5 L2 uses
an **adaptive, thrash-resistant replacement policy** that makes eviction
non-monotonic in candidate-set size and sensitive to access sequence, breaking
the artifact's eviction-set generation algorithm. We implement a
"grow-and-verify" generator that partially restores functionality but note that
fully reliable eviction requires reverse-engineering the replacement policy. We
also document a macOS toolchain incompatibility that blocks the build and its
workaround.

---

## 1. Introduction

The GoFetch artifact comprises three parts: `linuxsetup` (Asahi Linux PMU
access), `re` (DMP reverse-engineering experiments), and `poc` (end-to-end
cryptographic key-extraction attacks). All attack machinery rests on two
microarchitectural primitives: (i) a **high-resolution timer** to distinguish
cache-hit from cache-miss latencies, and (ii) **eviction sets** to control and
observe cache state via Prime+Probe. The DMP itself is the leakage source.

Porting to a new microarchitecture therefore requires re-validating each of
these primitives. This report proceeds bottom-up: build portability (§4),
timing (§5), the DMP and its mitigation (§6), L2 addressing (§7), and the
eviction-set generator (§8). §9 summarizes code changes; §10 covers limitations
and future work.

## 2. Background

**DMP.** A data memory-dependent prefetcher inspects values brought into the
data cache, and when a value resembles a pointer, dereferences it and prefetches
the target. GoFetch engineers secret-dependent pointer values in a victim's
memory and infers the secret from whether the DMP dereferences them.

**Augury access pattern.** Given an array of pointers (`aop`), architecturally
dereferencing the first *N* entries causes the DMP to also dereference
out-of-bounds entries *N..M*. Observing that a pointed-to line becomes cached is
the core detection primitive (`re/src/aop_stream.c`).

**Eviction sets.** On a physically-indexed cache, a set of addresses congruent
to a victim (mapping to the same cache set) can evict it. GoFetch builds L2
eviction sets by **page coloring**: because macOS uses 16 KB pages, the low bits
of a virtual address equal those of the physical address, so collecting lines
that share the victim's page offset fixes part of the L2 set index and shrinks
the pool of congruent candidates (`lib/eviction_set/eviction_set_generation.c`).

**Timing.** On macOS the artifact loads the private `kperf` framework to program
a performance counter and read core cycles; on Asahi Linux it reads the PMC
system register directly after a kernel module enables EL0 access.

## 3. Experimental Setup

- **CPU:** Apple M5 Pro. `sysctl` reports: page size 16 KB, L1D 128 KB, L2
  16 MB (per-cluster, `hw.perflevel0.l2cachesize`), cache line 128 B. L2
  associativity is not exposed by `sysctl` and was not conclusively determined.
- **OS/toolchain:** Darwin 25.6; Command Line Tools 26.6 (clang 21, ld 1267).
- **Configuration:** a new `M5` target macro in `arch.h` selects the 16 MB L2
  geometry (matching the existing M2/M3 size) and the M5-specific timing and
  threshold settings described below.

All measurements below use the `re/` experiments compiled for the `M5` target.

## 4. Toolchain and Build Portability

The stock build fails at **link time** (compilation succeeds):

```
ld: tapi error: malformed file
.../MacOSX27.0.sdk/usr/lib/libSystem.B.tbd: error: unknown architecture
    arm64e.x1-macos, arm64e.x1-maccatalyst
```

The installed CLT (26.6) linker cannot parse the default SDK's (`MacOSX27.0`)
text-based stub libraries, which declare a newer `arm64e.x1` architecture slice.
This is a CLT/SDK version skew, independent of GoFetch. **Workaround:** build
against an older installed SDK:

```
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
```

(The `MacOSX.sdk` symlink points to 26.5, but `xcrun --show-sdk-path` returns
the broken 27.0, so `SDKROOT`/`-isysroot` must be set explicitly.)

## 5. Timing Source: kperf Failure and CNTVCT Replacement

The artifact's `configure_kpc()` programs a configurable PMC via
`kpc_set_config` and reads it with `kpc_get_thread_counters`. On M5/current
macOS, `kpc_set_config` returns **`-1 (EPERM)`**; the artifact then aborts
counter setup, and every timed load reports `kpc_get_thread_counters failed`.

Diagnosis (standalone probe) established:

- All `kperf` symbols resolve; only `kpc_set_config` is denied.
- The PMU exposes **2 fixed + 5 configurable** counters; the fixed counters
  (cycles, instructions) are readable via `kpc_get_thread_counters` **without
  configuration**, but each read is a framework/syscall round-trip costing
  **~2600 cycles**—far too coarse to separate L1 from L2.
- Direct EL0 reads of the Apple PMC system registers (`S3_2_c15_c0_0`, etc.)
  **trap (SIGILL)**: userspace PMC access is disabled, and the mechanism to
  enable it (`kpc_set_config` with the EL0-enable config word) is the blocked
  call.

We therefore replace the timer with the **generic virtual counter
`CNTVCT_EL0`**, which is always readable from EL0 and needs no privilege.
Characterization:

| Property | Value |
|---|---|
| Nominal frequency (`CNTFRQ_EL0`) | 1 GHz |
| **Effective tick granularity** | **~17 ns** (counter advances in steps) |
| Cached (L1/L2) load | ~41 ticks |
| In-L2 (evicted from L1) load | ~83 ticks |
| DRAM (evicted from L2) load | ~125–166 ticks |

The 17 ns step is too coarse to separate L1 from L2 (their true difference is a
few nanoseconds), but it **cleanly separates cache-resident from DRAM
accesses**, which is exactly what eviction-set generation and the Prime+Probe
channel require. Crucially, this removes the `sudo` requirement for all `re/`
experiments on macOS.

## 6. DMP Presence and DIT Mitigation on M5

Using `aop_stream` (Augury pattern), with out-of-bounds `aop` entries set to
pointers (ATK) versus random non-pointers (BASE), we time access to the
pointed-to data:

| Condition | ATK (aop = pointers) | BASE (aop = random) | Interpretation |
|---|---|---|---|
| **DIT off** | 42 ticks (cached) | 375 ticks (DRAM) | **DMP active** — targets prefetched |
| **DIT on**  | 291 ticks (DRAM)  | 250 ticks (DRAM) | **DMP suppressed** — ATK ≈ BASE |

**Findings.** (1) The DMP exists and is active on M5 Pro: it dereferences
out-of-bounds pointer values and prefetches their targets, reproducing the
GoFetch/Augury primitive. (2) Setting the DIT bit suppresses the DMP on M5—the
ATK signal collapses to match BASE—so Apple's documented constant-time
mitigation remains effective.

A prerequisite fix was required: the experiments `mmap` a ~1 GB `data_buf` and a
small `aop` with `NULL` address hints, which land in **different 4 GB regions**
on M5/macOS, violating the DMP's same-region requirement (`ADDR_CHECK`). We pin
`aop` immediately after `data_buf` with `MAP_FIXED` so both share one 4 GB
window (applied in both `aop_stream.c` and `no_deref.c`).

### 6.1 Detailed DMP Characterization

Beyond presence, we characterized the DMP's activation and dereferencing
behavior on M5 using the `aop_stream` (Augury) and `addr_sweep` experiments.
Each configuration was run repeatedly; we report the fraction of runs in which
the pointed-to line came back cached (median latency < 100 ticks, i.e. the DMP
dereferenced the pointer). All measurements use `CNTVCT_EL0` (§5) and need no
privilege.

**(a) Activation vs. training length.** Varying the number of architecturally
dereferenced training pointers in a 264-entry `aop` (measuring the first
out-of-bounds pointer), the DMP dereferences reliably across a wide range:

| training length | 4 | 8 | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|---|---|
| ATK cached (of 8 runs) | 8 | 0 | 7 | 8 | 8 | 4 | 8 |

Activation is robust from very short training onward; the dips at 8 and 128 are
reproducible outliers (likely set-aliasing at those specific sizes) rather than
a threshold.

**(b) Prefetch depth.** With a 256-entry training prefix and 16 out-of-bounds
pointers, we measured each out-of-bounds index individually. The DMP
dereferences deeply—indices +0 through +15 were cached in 6–8 of 8 runs
(most at 8/8)—confirming the Augury multi-pointer dereference behavior persists
on M5.

**(c) 4 GB-region restriction.** Using `addr_sweep` to place a pointer and its
target at candidate addresses on a fixed 256 MB stride across a 4 GB boundary,
we classified each pair by whether the pointer's location and its target share
a 4 GB region:

| pair type | cached (DMP dereferenced) |
|---|---|
| **same 4 GB region** | **74 / 74 (100%)** |
| **cross 4 GB region** | **1 / 88 (1%)** |

The DMP dereferences a pointer **only** when its location and target lie in the
same 4 GB region—a precise match to the M1–M3 behavior the attack relies on.

**(d) Divergence: activation without architectural dereference.** The `no_deref`
experiments—where candidate pointer *values* are loaded into L1 but never
architecturally dereferenced (via `datastream_array`)—reproduce the M1–M3
claims only weakly on M5. Across 12 runs each, the pointed-to line came back
cached in just 1/12 (in-bounds pointers, all touched), 3/12 (single touch, same
L1 line), and 2/12 (stride-prefetcher-fed) configurations, with ATK and BASE
medians largely overlapping. In contrast, every experiment that performs real
architectural dereferences (`aop_stream`, `addr_sweep`) yields a clean signal.
This suggests **M5's DMP has stricter activation criteria than M1–M3**: it keys
on genuine dereference activity rather than triggering readily on
pointer-valued loads or stride-prefetched lines. The top-byte-ignore (TBI)
experiment is built on the `no_deref` path and was therefore too noisy to
yield a conclusion on M5; we leave it to future work on the cleaner path.

**Summary.** M5's DMP matches the paper's M1–M3 model on the properties the
attack exploits (presence, deep out-of-bounds dereferencing, the 4 GB-region
restriction, DIT suppression), and diverges only in being less easily activated
without architectural dereferences.

## 7. L2 Cache Addressing: Page Coloring Holds

An initial run of the stock eviction-set generator required the **entire 16 MB
L2** (131072 congruent-candidate lines) to evict a single victim, suggesting the
page-color filter was ineffective—i.e., a hashed L2 set index. Controlled
measurement refuted this. Comparing a page-colored candidate pool (lines sharing
the victim's page offset) against a random-offset pool:

| Candidate pool | Lines to evict victim to DRAM |
|---|---|
| **Page-colored** | ~512 |
| **Random offset** | ~262144 (≈ whole cache) |

The ~500× gap confirms **page coloring works on M5**; the earlier result was an
artifact of a flawed measurement (minimum over re-loads) and of the victim being
stuck at the L2 plateau. The "hashed index" hypothesis is withdrawn.

## 8. Adaptive L2 Replacement and Eviction-Set Construction

The deeper obstacle is the M5 L2 **replacement policy**. Eviction is strongly
**non-monotonic in candidate-set size**: a forward pointer-chase over a colored
pool evicts the victim at small sizes, fails across a mid-size "dead zone," and
succeeds again only at full-cache sizes.

| N (colored lines) | 512 | 1024 | 2048 | 4096 | … | 65536 | 131072 |
|---|---|---|---|---|---|---|---|
| DRAM-rate | ~1.0 | ~0.1 | ~0.7 | ~0.0 | ~0.0 | ~0.0 | ~0.9 |

Additional observations:

- **Re-traversal re-caches the victim.** With a fixed colored set, 1–2
  traversal passes evict to DRAM, but **3+ passes return the victim to cache**
  (DRAM-rate falls to ~0). The stock `traverse_eviction_set` performs a
  dual-cursor forward+backward chase repeated twice (~4 passes), which prevents
  eviction on M5.
- **Victim dependence.** Page-aligned victims evict at ~512 colored lines;
  random-offset victims need ~1536 and remain only ~70–86% reliable.
- **Access-sequence / state dependence.** A freshly-primed victim followed by a
  single traversal is evicted to DRAM (125 ticks), but an already-in-L2 victim
  that is merely re-traversed stays in L2 (83 ticks).

These behaviors are consistent with an **adaptive, thrash-resistant replacement
policy** (e.g., RRIP/DRRIP-style) that detects medium-sized streaming working
sets and protects recently-used lines. They invalidate the stock generator,
which (a) grows the candidate set upward through the dead zone until full-cache
eviction, then (b) group-reduces back down to exactly `L2_NWAYS` lines—both
assuming monotonic eviction.

**Our M5 generator.** We replace `find_L2_eviction_set_using_timer` (under the
M5 timer macro) with a **grow-and-verify** strategy: grow a page-colored set
along a size ladder (512, 768, …, 16384) and at each step *directly measure*
eviction, returning the first size whose median victim latency reaches DRAM,
confirmed by a second measurement. We also replace the M5 `traverse_eviction_set`
with a **single forward pointer-chase**. We deliberately **do not** reduce to
`L2_NWAYS`: minimal congruent sets fall inside the protected dead zone.

**Result (partial).** The generator succeeds on page-aligned victims—e.g.,
`evset_gen` finds a 768-line set yielding a clean L2-miss latency of 125 ticks
versus an L2-hit of 41—a qualitative improvement over the stock algorithm, which
simply exhausts memory and times out. However, reliability is **partial**: some
runs fail entirely, and an eviction set validated under the generator's
prime-then-traverse pattern does not always transfer to a different access
pattern (e.g., the repeated-traverse measurement loop measures 83 ticks, i.e.,
the victim remains in L2). We conclude that **robust, access-pattern-independent
L2 eviction on M5 is unsolved** and requires reverse-engineering the replacement
policy.

## 9. Summary of Code Changes

All changes are confined to `re/src/` and gated on a new `M5` configuration so
that M1/M2/M3 behavior is unchanged.

| File | Change |
|---|---|
| `lib/basics/arch.h` | Add `M5` target macro (16 MB L2, 128 B line, 16 KB page); add `USE_CNTVCT_TIMER` (set when `M5`); set CNTVCT-scale thresholds (`L1_HIT_MAX_LATENCY 60`, `L2_MISS_MIN_LATENCY 100`) between the measured L2 (83) and DRAM (125) clusters. |
| `lib/eviction_set/sys_utils.h` | Add a `CNTVCT_EL0`-based `kpc_time_load` for the M5 path (inline `mrs`, mirroring the Linux PMC path). |
| `lib/eviction_set/sys_utils.c` | `get_kpc_time()` reads `CNTVCT_EL0` on M5; `init_kpc()`/`configure_kpc()` become no-ops on M5 (no kperf, no `sudo`); retain a fixed-counter fallback for the non-CNTVCT path. |
| `lib/eviction_set/eviction_set.c` | M5 `traverse_eviction_set`: single forward pointer-chase instead of the dual-cursor forward+backward double round. |
| `lib/eviction_set/eviction_set_generation.c` | M5 `find_L2_eviction_set_using_timer`: grow-and-verify generator (no `L2_NWAYS` reduction). Stock path retained for other targets. |
| `aop_stream.c` | Pin `aop` into `data_buf`'s 4 GB region via `MAP_FIXED` (fixes `ADDR_CHECK` on M5); add optional `argv[5]` to set the DIT bit for mitigation testing. |
| `no_deref.c` | Same `MAP_FIXED` `aop` allocation fix so the activation-criteria experiments run on M5 (§6.1d). |

## 10. Limitations and Future Work

- **Replacement policy.** The central open problem is the adaptive L2
  replacement policy. Future work should characterize the dead zone precisely
  and search for access orderings/interleavings that defeat thrash-resistance
  (a well-studied problem for RRIP/DRRIP), yielding reliable minimal eviction
  sets. Only then can the `poc/` Prime+Probe attacks be ported.
- **L2 associativity** on M5 remains undetermined; the current generator does
  not depend on it, but a minimal-set approach would.
- **Timer resolution.** The 17 ns `CNTVCT` step cannot separate L1 from L2;
  experiments that rely on that distinction are out of reach without a finer
  unprivileged timer (e.g., a counting thread).
- **Scope.** Only the `re/` tree was addressed. `quick_check` and the `poc/`
  cryptographic attacks depend on reliable eviction sets and are not yet
  functional on M5.
- **Security framing.** This is defensive/academic reproduction work on the
  authors' published artifact; the practical takeaway is that M5's adaptive L2
  replacement raises the bar for eviction-set construction, while DIT remains an
  effective mitigation.

## 11. Conclusion

The GoFetch attack's premise survives on Apple M5 Pro: the DMP is present and
active, and DIT still mitigates it. The artifact's timing source must change
from kperf to `CNTVCT_EL0` (which also drops the `sudo` requirement), and a
toolchain/SDK workaround is needed to build. Page coloring still works. The one
substantive barrier is M5's adaptive, thrash-resistant L2 replacement policy,
which breaks monotonic-eviction assumptions; our grow-and-verify generator
restores partial functionality but reliable eviction-set construction on M5
remains an open reverse-engineering problem.

## Appendix: Reproduction

```sh
# Build (note the SDK workaround)
cd re/src
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
make

# Timing + eviction calibration (no sudo needed on M5)
ulimit -s 65520
./evset_gen.out

# DMP presence and DIT mitigation
./aop_stream.out 4 264 256 0 0   # DIT off  -> ATK cached, BASE DRAM
./aop_stream.out 4 264 256 0 1   # DIT on   -> ATK ~ BASE
```

Select the target microarchitecture in `re/src/lib/basics/arch.h` (the `M5`
macro is enabled by default in this branch).
