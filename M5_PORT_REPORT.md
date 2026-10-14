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
the artifact's eviction-set generation algorithm. We reverse-engineer this
policy and identify it as an **RRIP/DRRIP-family** policy: we measure a resonant
eviction window (≈ 512–2048 colored lines), a thrash-resistant "dead zone" above
it, re-reference (recency) protection of the victim, and strong dependence on
physical page placement — each mapping onto a known RRIP mechanism. Guided by
this, we implement a "grow-and-verify" generator tuned to the resonant window
that restores partial functionality (≈ 4/6 runs), and we delimit precisely what
remains for a robust Prime+Probe channel (RRIP-state-aware access sequences and
L2 set-index recovery). We also document a macOS toolchain incompatibility that
blocks the build and its workaround.

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

## 8. L2 Replacement-Policy Reverse-Engineering and Eviction-Set Construction

The deepest obstacle to porting GoFetch to M5 is the L2 cache's **replacement
policy**. On M1–M3 the stock generator assumes a classic recency policy
(LRU/PLRU), under which a set of `L2_NWAYS` congruent lines, accessed once,
deterministically evicts a victim. On M5 this assumption fails completely, and
the cause is a far more sophisticated, *adaptive* replacement policy. This
section documents the reverse-engineering of that policy, the mechanism we
infer, and how it reshapes the eviction-set generator.

### 8.1 Initial symptom

Run unmodified, the stock `build_l2_eviction_set_superset` doubles the colored
candidate set (16, 32, …) until the victim's access latency crosses the
DRAM threshold. On M5 it does not cross until the candidate set reaches
**131072 lines — exactly 16 MB / 128 B, the entire L2** (resident set ~2.2 GB),
after which the group-testing reduction never converges and trips the built-in
20-second guard (`PANIC: takes >20 seconds reducing the eviction set`). The
generator effectively has to fill the whole cache to evict one line, and then
cannot reduce the result. This is the outward sign of an eviction curve that is
not monotonic in set size.

### 8.2 Methodology

We reverse-engineer the policy with small standalone probes (outside the GoFetch
tree) so that each variable can be controlled independently. Each probe:

- allocates a victim line and a pool of **page-colored** candidate lines (one
  per `mmap`'d 16 KB page, all at the victim's page offset — the same coloring
  the artifact uses, which §7 shows is effective on M5);
- links the candidates into a pointer-chase list and traverses them to apply
  cache pressure;
- times the victim with `CNTVCT_EL0` (§5), classifying a load as **evicted to
  DRAM** when its latency ≥ 100 ticks (cache ≈ 41, in-L2 ≈ 83, DRAM ≈ 125–166);
- repeats each measurement K times (typically 15–21) and reports the
  **DRAM-rate**: the fraction of trials in which the victim was evicted to DRAM.

The independent variables are the candidate-set size **N**, the number of
traversal passes **P**, and the victim address (page offset + physical
placement). DRAM-rate as a function of these variables is the eviction surface.

### 8.3 Eviction is non-monotonic in set size

Sweeping N for a single victim with a plain forward chase:

| N (colored lines) | 512 | 1024 | 2048 | 4096 | 8192 | 65536 | 131072 |
|---|---|---|---|---|---|---|---|
| DRAM-rate | ~1.0 | ~0.1 | ~0.7 | ~0.0 | ~0.0 | ~0.0 | ~0.9 |

Eviction succeeds at *small* N, fails across a wide *mid-size* band, and
succeeds again only when N approaches the *whole cache*. A policy in which more
cache pressure monotonically increases eviction (LRU/PLRU) cannot produce this
shape. A medium-sized working set is somehow **less** able to evict the victim
than a small one — the hallmark of thrash-resistant insertion.

### 8.4 The size × passes surface and re-traversal re-caching

Fixing a (page-aligned) victim and a working set of N = 512 and varying the
number of forward-chase passes P:

| passes P | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| DRAM-rate | 0.90 | 1.00 | 0.13 | 0.03 | 0.00 | 0.00 |

One or two passes evict reliably; **three or more passes return the victim to
the cache** (DRAM-rate collapses to ~0). Re-referencing the candidate set does
not deepen eviction — it *undoes* it. This immediately explains why the stock
`traverse_eviction_set`, which performs a dual-cursor forward+backward chase
repeated twice (≈ 4 passes over each line), fails to evict on M5 even with a
correctly sized set: it over-traverses into the re-caching regime.

### 8.5 Per-victim resonant windows

Eviction is strongly victim-dependent. Measuring DRAM-rate at P = 2 across a
fine N sweep for two victims (K = 21):

| N | victim off 0x0000 | victim off 0x1980 |
|---|---|---|
| 256 | 0.00 | 0.05 |
| 512 | **1.00** | **1.00** |
| 640 | **1.00** | **1.00** |
| 768 | **1.00** | **1.00** |
| 896 | **1.00** | 0.00 |
| 1024 | **1.00** | **0.86** |
| 1280 | 0.90 | **1.00** |
| 1536 | 0.95 | **1.00** |
| 2048 | **1.00** | 0.00 |
| 3072 | 0.10 | 0.00 |
| 4096 | 0.00 | 0.00 |
| 8192 | 0.24 | 0.05 |

Each victim has a **resonant window** of candidate-set sizes — roughly
**N ∈ [512, 2048]** — inside which eviction is reliable, bounded below by a
region where the victim is protected (too few congruent references) and above by
the collapse at ~3072 (the thrash-resistant "dead zone"). Within the window
there are reproducible victim-specific "holes" (e.g., off 0x1980 at N = 896 and
2048). A per-victim scan (not a single fixed size) is therefore required.

### 8.6 Physical-placement variance

The *same* victim page offset can be evictable in one run and not another. In an
earlier coarse sweep, offset 0x1980 showed DRAM-rate 0 at every tested N; in the
fine sweep above it is reliably evictable. The difference is the **physical
placement** of the colored candidate pages: each `mmap` receives different
physical frames, which changes which colored lines are *truly congruent* with
the victim's L2 set. Because the usable window is narrow and congruent-line
supply depends on placement, a given allocation may or may not contain enough
congruent lines at a usable size. This randomness — not fundamental
unevictability — is the dominant source of run-to-run variance.

### 8.7 Policy identification: RRIP / DRRIP

Every observation above maps cleanly onto a **Re-Reference Interval Prediction**
family policy (SRRIP/BRRIP/DRRIP, Jaleel et al., ISCA 2010), which modern CPUs
use in place of true LRU:

- In RRIP each line carries an *M*-bit re-reference prediction value (RRPV).
  Lines are **inserted** with a "distant" prediction (not MRU), **promoted** to
  "near" (RRPV = 0) on a hit, and the victim chosen on a miss is a line with the
  "distant-most" value (RRPV = max), incrementing all RRPVs if none is found.
- **Recency protection (§8.4, §8.9).** A just-referenced victim has RRPV = 0 and
  is evicted *last*; it takes many congruent misses to age its RRPV to max. A
  single pass over a correctly sized congruent set delivers roughly that many
  misses and evicts it; a *freshly primed* victim is in a known RRPV = 0 state,
  which is why priming matters.
- **Thrash resistance / dead zone (§8.3, §8.5).** BRRIP (used by DRRIP via
  set-dueling when a workload thrashes) inserts most lines at the distant-most
  RRPV, so a medium streaming working set **evicts itself** rather than
  displacing the resident victim. This is exactly the mid-size band where
  eviction fails, and why only a *small* set (fits the SRRIP-like regime) or
  *whole-cache* pressure evicts.
- **Re-traversal re-caching (§8.4).** Repeated passes turn the candidate lines
  into *hits*, promoting them to RRPV = 0; they stop being eviction pressure and
  instead occupy the "near" positions, so the victim is no longer forced out.

We therefore conclude M5's L2 uses an **adaptive, thrash-resistant RRIP/DRRIP-
family replacement policy**, not the LRU/PLRU the stock generator assumes. (We
do not claim the exact RRPV width or the precise set-dueling parameters; those
would require either documentation or a dedicated micro-benchmark that isolates a
single physical set.)

### 8.8 The M5 eviction-set generator

The RE findings directly reshape the generator. We replace
`find_L2_eviction_set_using_timer` (under the M5 timer macro) and
`traverse_eviction_set` as follows:

1. **Single-pass traversal.** The M5 `traverse_eviction_set` performs **one
   plain forward pointer-chase** instead of the stock dual-cursor
   forward+backward double round, keeping within the 1–2 pass regime that
   evicts (§8.4).
2. **Grow-and-verify over the resonant window.** Rather than grow to full cache
   and group-reduce to `L2_NWAYS` (both impossible here — §8.1, and minimal
   congruent sets sit below the window), the generator grows a page-colored set
   along a **fine ladder restricted to the window**
   (512, 640, 768, 896, 1024, 1152, 1280, 1536, 1792, 2048) and *directly
   measures* eviction at each step, returning the first size whose median victim
   latency reaches DRAM, confirmed by a second measurement.
3. **Retry on fresh allocation.** Because evictability depends on physical
   placement (§8.6), failure triggers up to `MAX_ATTEMPTS` (16) retries with
   newly allocated candidate pages.
4. **No `L2_NWAYS` reduction.** The result is a working but *non-minimal*
   eviction set (typically ~1100–1300 lines); minimal congruent reduction is
   infeasible under this policy without first recovering the set-index.

**Reliability.** Over repeated `evset_gen` runs the tuned generator finds a
working eviction set in roughly **4 of 6 runs**, versus the stock algorithm,
which never completes (it exhausts memory and times out). The remaining failures
are placement-unlucky allocations (§8.6).

### 8.9 Residual gap: access-sequence state dependence

A subtle but important limitation remains. The generator validates an eviction
set with a *prime-then-single-traverse* pattern (`evict_and_time` loads the
victim, then traverses once, then times it) and correctly measures DRAM
(~125 ticks). But `evset_gen`'s separate L2-miss diagnostic loop, which
*re-traverses without re-priming the victim each iteration*, measures the same
victim at **~83 ticks (still in L2)**. The eviction set is identical; only the
access sequence differs.

Under RRIP this is expected: eviction depends on the victim's **RRPV state** at
the moment the congruent pressure is applied. A freshly primed victim
(RRPV = 0 in L1) is driven out through L1 → L2 → DRAM by one traversal; a victim
left resident from a prior step sits at a different RRPV and the same single
traversal only displaces it from L1 to L2. In other words, on M5 an eviction set
is **not** an access-pattern-independent object the way it is under LRU — its
effect is a function of the shared RRIP state.

For GoFetch's purposes this is not fatal: the attack's *prime* step
deliberately accesses (and thus primes) the line it wants to evict, which is the
exact sequence the generator validates. But a *robust* Prime+Probe channel must
be written with the RRIP state in mind rather than assuming eviction is
stateless.

### 8.10 Open problems

Two concrete items remain before the `poc/` attacks can be ported:

1. **RRIP-aware Prime+Probe.** Characterize the exact number of congruent
   re-references needed to force a victim's RRPV to max from each starting
   state, and build the prime/probe sequences around it so eviction is
   deterministic regardless of the victim's prior cache state.
2. **Set-index recovery for minimal sets.** The current eviction sets are large
   (~1200 lines) because we cannot isolate the ~`L2_NWAYS` congruent lines.
   Recovering M5's physical L2 set-index function (e.g., via huge pages, or a
   timing-based congruence oracle that tolerates the adaptive policy) would
   allow true minimal eviction sets and sharper Prime+Probe monitoring.

Both are tractable follow-ups, but each is a research task in its own right;
together they are the gate to end-to-end key extraction on M5.

## 9. Summary of Code Changes

All changes are confined to `re/src/` and gated on a new `M5` configuration so
that M1/M2/M3 behavior is unchanged.

| File | Change |
|---|---|
| `lib/basics/arch.h` | Add `M5` target macro (16 MB L2, 128 B line, 16 KB page); add `USE_CNTVCT_TIMER` (set when `M5`); set CNTVCT-scale thresholds (`L1_HIT_MAX_LATENCY 60`, `L2_MISS_MIN_LATENCY 100`) between the measured L2 (83) and DRAM (125) clusters. |
| `lib/eviction_set/sys_utils.h` | Add a `CNTVCT_EL0`-based `kpc_time_load` for the M5 path (inline `mrs`, mirroring the Linux PMC path). |
| `lib/eviction_set/sys_utils.c` | `get_kpc_time()` reads `CNTVCT_EL0` on M5; `init_kpc()`/`configure_kpc()` become no-ops on M5 (no kperf, no `sudo`); retain a fixed-counter fallback for the non-CNTVCT path. |
| `lib/eviction_set/eviction_set.c` | M5 `traverse_eviction_set`: single forward pointer-chase instead of the dual-cursor forward+backward double round. |
| `lib/eviction_set/eviction_set_generation.c` | M5 `find_L2_eviction_set_using_timer`: grow-and-verify generator scanning a fine size ladder restricted to the resonant window (512–2048, §8.8), retrying up to 16× on fresh allocations; no `L2_NWAYS` reduction. Stock path retained for other targets. |
| `aop_stream.c` | Pin `aop` into `data_buf`'s 4 GB region via `MAP_FIXED` (fixes `ADDR_CHECK` on M5); add optional `argv[5]` to set the DIT bit for mitigation testing. |
| `no_deref.c` | Same `MAP_FIXED` `aop` allocation fix so the activation-criteria experiments run on M5 (§6.1d). |

## 10. Limitations and Future Work

- **Replacement policy.** The central open problem is the adaptive,
  thrash-resistant RRIP/DRRIP-family L2 replacement policy reverse-engineered in
  §8. The two concrete follow-ups (§8.10) are an RRIP-state-aware Prime+Probe
  sequence and recovery of the physical L2 set-index for minimal eviction sets;
  only then can the `poc/` attacks be ported.
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
