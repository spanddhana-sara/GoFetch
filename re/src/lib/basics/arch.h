#ifndef __ARCH_H__
#define __ARCH_H__

// Select the target microarchitecture by uncommenting ONE of the following.
// Leave all commented for M1.
// #define M2 0   // M2 and M3
#define M5 0   // M5 / M5 Pro (P-cluster)

#define KB 1024ULL
#define MB (1024*KB)

#define PAGE_SIZE               (16 * KB)

#define M1_L2_NWAYS     12
#define M2_L2_NWAYS     16
// M5 Pro P-cluster L2 size confirmed 16 MB via `sysctl hw.perflevel0.l2cachesize`.
// Associativity (NWAYS) is NOT reported by sysctl; 16 is a starting assumption
// inherited from M2/M3 (same 16 MB size) and MUST be validated empirically with
// evset_gen.out before trusting L2 eviction sets. TODO(M5): confirm L2_NWAYS.
#define M5_L2_NWAYS     16
#define M1_L2_SIZE      (12 * MB)
#define M2_L2_SIZE      (16 * MB)
#define M5_L2_SIZE      (16 * MB)

#define L1_NWAYS        8
#define L1_LINE_SIZE    64
#if defined(M5)
#define L2_NWAYS        M5_L2_NWAYS // M5 (TODO: verify associativity)
#define L2_SIZE         M5_L2_SIZE // M5
#elif defined(M2)
#define L2_NWAYS        M2_L2_NWAYS // M2 or M3
#define L2_SIZE         M2_L2_SIZE // M2 or M3
#else
#define L2_NWAYS        M1_L2_NWAYS // M1
#define L2_SIZE         M1_L2_SIZE // M1
#endif
#define L2_LINE_SIZE    128
#define L1_SIZE         (128 * KB)

// Timer selection. On M5 + current macOS, Apple blocks kpc_set_config (EPERM),
// so the programmable PMC cannot be configured and PMC registers are not
// EL0-readable -- the kperf fast timer is unavailable. Fall back to the
// always-userspace generic timer CNTVCT_EL0 (unprivileged, no kperf, ~17 ns
// effective step on M5: coarse but cleanly separates cache from DRAM, which is
// what eviction-set generation and the Prime+Probe channel rely on).
#if defined(M5) && defined(__APPLE__)
#define USE_CNTVCT_TIMER 1
#endif

#define MASK(x)     ((1 << x) - 1)
#define MSB_MASK    0x8000000000000000ULL

#define HPO_nbits   11
#define RPO_nbits   7
#define CLO_nbits   7

#define HPO(vaddr)  ( (vaddr >> (CLO_nbits + RPO_nbits)) & MASK(HPO_nbits) )
#define RPO(vaddr)  ( (vaddr >> CLO_nbits) & MASK(CLO_nbits) )
#define CLO(vaddr)  ( (vaddr) & MASK(CLO_nbits) )


#ifdef __linux__
#define L1_HIT_MAX_LATENCY  65
#define L2_MISS_MIN_LATENCY 200
#elif defined(M5)
// macOS on M5 uses CNTVCT_EL0 (see USE_CNTVCT_TIMER below), so these are in
// CNTVCT ticks (~17 ns effective step), NOT core cycles. Measured on M5 Pro:
// cache-resident load ~41 ticks, DRAM load ~125 ticks. L2_MISS_MIN_LATENCY is
// the "served from DRAM / successfully evicted" cutoff and is the one that
// matters for eviction-set generation; place it between the two clusters.
// Measured clusters (CNTVCT ticks): cached ~41, in-L2 ~83, DRAM ~125-166.
// L2_MISS_MIN_LATENCY must sit between the L2 plateau (83) and DRAM (125) so an
// L2 eviction (victim pushed to DRAM) is detected but an L1-only eviction
// (victim still in L2) is not.
#define L1_HIT_MAX_LATENCY  60
#define L2_MISS_MIN_LATENCY 100
#else
#define L1_HIT_MAX_LATENCY  224
#define L2_MISS_MIN_LATENCY 300
#endif

#endif
