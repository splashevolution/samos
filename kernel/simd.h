/*
 * SAM OS — kernel/simd.h
 * =======================
 * Runtime SIMD capability detection and unified INT8 compute dispatch.
 *
 * Supports three paths, selected at boot based on CPUID:
 *
 *   Path A: AVX2   — 256-bit INT8, 32 elements per operation  (Haswell+)
 *   Path B: SSE4.2 — 128-bit INT8, 16 elements per operation  (Nehalem+)
 *   Path C: Scalar — pure C fallback, no SIMD required         (any x86-64)
 *
 * The VAIO VPCEB15FX (Core i5, pre-2011) takes Path B: SSE4.2.
 * A modern Core i5/i7 (4th gen+) takes Path A: AVX2.
 *
 * Key function:
 *   int32_t sam_int8_dot(const int8_t *a, const int8_t *b, int n)
 *   — computes dot product of two INT8 vectors of length n
 *   — n must be a multiple of 16 (SSE4.2) or 32 (AVX2)
 *   — dispatches to the best available path automatically
 *
 * This is the core of SAM OS AI inference:
 *   matrix multiply = repeated dot products across rows/columns
 *   quantized LLM token generation = INT8 matmul over weight matrices
 *
 * No libc. No external dependencies.
 */

#ifndef SAM_SIMD_H
#define SAM_SIMD_H

#include <stdint.h>

/* ── SIMD path identifiers ──────────────────────────────────────────────── */
#define SAM_SIMD_SCALAR  0
#define SAM_SIMD_SSE42   1
#define SAM_SIMD_AVX2    2

/* ── Global SIMD capability (set at boot by sam_simd_init()) ────────────── */
extern int sam_simd_level;

/* ── Forward declarations ───────────────────────────────────────────────── */
static inline int32_t sam_int8_dot_scalar(const int8_t *a, const int8_t *b, int n);
static inline int32_t sam_int8_dot_sse42 (const int8_t *a, const int8_t *b, int n);
#ifdef __AVX2__
static inline int32_t sam_int8_dot_avx2  (const int8_t *a, const int8_t *b, int n);
#endif

/* ============================================================
 * CPUID helpers (duplicated here so simd.h is self-contained)
 * ============================================================ */
typedef struct { uint32_t eax, ebx, ecx, edx; } sam_cpuid_t;

static inline sam_cpuid_t sam_cpuid(uint32_t leaf, uint32_t subleaf)
{
    sam_cpuid_t r;
    __asm__ volatile (
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf)
    );
    return r;
}

/* ============================================================
 * sam_simd_init — call once at kernel boot
 * Sets sam_simd_level to the best available path.
 * Returns the level selected.
 * ============================================================ */
/* sam_simd_level is DEFINED in main.c — only declared extern here.
 * Including simd.h from a second translation unit is safe. */
extern int sam_simd_level;

static inline int sam_simd_init(void)
{
    /* Check SSE4.2: CPUID leaf 1, ECX bit 20 */
    sam_cpuid_t c1 = sam_cpuid(1, 0);
    int has_sse42 = (c1.ecx >> 20) & 1;

    /* Check AVX2: CPUID leaf 7, EBX bit 5 */
    sam_cpuid_t c7 = sam_cpuid(7, 0);
    int has_avx2  = (c7.ebx >> 5) & 1;

    if (has_avx2)       sam_simd_level = SAM_SIMD_AVX2;
    else if (has_sse42) sam_simd_level = SAM_SIMD_SSE42;
    else                sam_simd_level = SAM_SIMD_SCALAR;

    return sam_simd_level;
}

static inline const char *sam_simd_name(void)
{
    switch (sam_simd_level) {
        case SAM_SIMD_AVX2:   return "AVX2 (256-bit)";
        case SAM_SIMD_SSE42:  return "SSE4.2 (128-bit)";
        default:              return "Scalar (no SIMD)";
    }
}

/* ============================================================
 * Path C: Scalar INT8 dot product (pure C, no intrinsics)
 * ============================================================ */
static inline int32_t sam_int8_dot_scalar(const int8_t *a, const int8_t *b, int n)
{
    int32_t acc = 0;
    for (int i = 0; i < n; i++)
        acc += (int32_t)a[i] * (int32_t)b[i];
    return acc;
}

/* ============================================================
 * Path B: SSE4.2 INT8 dot product (128-bit, 16 elements/iter)
 *
 * Uses PMADDUBSW + PMADDWD to compute 16 INT8 pairs per cycle:
 *   PMADDUBSW: uint8 * int8 → int16 (horizontal add pairs)
 *   PMADDWD:   int16 * 1   → int32 (widen and accumulate)
 *
 * Note: PMADDUBSW treats first operand as UNSIGNED. Since our
 * weights are signed INT8 [-128,127], we bias by +128 and
 * compensate in the final sum.
 * ============================================================ */
#ifdef __SSE4_2__
#include <immintrin.h>

static inline int32_t sam_int8_dot_sse42(const int8_t *a, const int8_t *b, int n)
{
    __m128i acc = _mm_setzero_si128();
    __m128i ones = _mm_set1_epi16(1);
    __m128i bias_vec = _mm_set1_epi8((int8_t)128);

    int i = 0;
    for (; i + 16 <= n; i += 16) {
        /* Load 16 x int8 from each vector */
        __m128i va = _mm_loadu_si128((const __m128i *)(a + i));
        __m128i vb = _mm_loadu_si128((const __m128i *)(b + i));

        /* Bias 'a' to unsigned: va_u = va + 128 (treat as uint8) */
        __m128i va_u = _mm_add_epi8(va, bias_vec);

        /* PMADDUBSW: uint8 x int8 → int16 (saturating, adds pairs) */
        __m128i prod16 = _mm_maddubs_epi16(va_u, vb);

        /* PMADDWD: int16 x 1 → int32 (widens and sums pairs) */
        __m128i prod32 = _mm_madd_epi16(prod16, ones);

        acc = _mm_add_epi32(acc, prod32);
    }

    /* Horizontal sum of 4 int32 lanes */
    __m128i shuf = _mm_shuffle_epi32(acc, 0x4E);  /* swap 64-bit halves */
    __m128i sum2 = _mm_add_epi32(acc, shuf);
    __m128i shuf2 = _mm_shuffle_epi32(sum2, 0xB1); /* swap 32-bit pairs */
    __m128i sum1 = _mm_add_epi32(sum2, shuf2);
    int32_t result = _mm_cvtsi128_si32(sum1);

    /* Compensate for the +128 bias applied to the SIMD-processed elements only.
     * actual dot = biased_dot - 128 * sum(b[0..i-1])
     * The tail [i..n-1] was never biased, so only sum over [0..i-1]. */
    int32_t sum_b = 0;
    for (int j = 0; j < i; j++) sum_b += (int32_t)b[j];
    result -= 128 * sum_b;

    /* Handle remaining elements (tail) — computed without bias */
    result += sam_int8_dot_scalar(a + i, b + i, n - i);

    return result;
}
#else
/* SSE4.2 not available at compile time — fall back to scalar */
static inline int32_t sam_int8_dot_sse42(const int8_t *a, const int8_t *b, int n)
{
    return sam_int8_dot_scalar(a, b, n);
}
#endif /* __SSE4_2__ */

/* ============================================================
 * Path A: AVX2 INT8 dot product (256-bit, 32 elements/iter)
 * Same logic as SSE4.2 but 2x wider registers.
 * ============================================================ */
#ifdef __AVX2__
#include <immintrin.h>

static inline int32_t sam_int8_dot_avx2(const int8_t *a, const int8_t *b, int n)
{
    __m256i acc = _mm256_setzero_si256();
    __m256i ones = _mm256_set1_epi16(1);
    __m256i bias_vec = _mm256_set1_epi8((int8_t)128);

    int i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i va   = _mm256_loadu_si256((const __m256i *)(a + i));
        __m256i vb   = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i va_u = _mm256_add_epi8(va, bias_vec);
        __m256i p16  = _mm256_maddubs_epi16(va_u, vb);
        __m256i p32  = _mm256_madd_epi16(p16, ones);
        acc = _mm256_add_epi32(acc, p32);
    }

    /* Reduce 8 int32 lanes to scalar */
    __m128i lo   = _mm256_extracti128_si256(acc, 0);
    __m128i hi   = _mm256_extracti128_si256(acc, 1);
    __m128i sum4 = _mm_add_epi32(lo, hi);
    __m128i shuf = _mm_shuffle_epi32(sum4, 0x4E);
    __m128i sum2 = _mm_add_epi32(sum4, shuf);
    __m128i shuf2 = _mm_shuffle_epi32(sum2, 0xB1);
    __m128i sum1 = _mm_add_epi32(sum2, shuf2);
    int32_t result = _mm_cvtsi128_si32(sum1);

    /* Compensate for the +128 bias applied to the SIMD-processed elements only.
     * actual dot = biased_dot - 128 * sum(b[0..i-1])
     * The tail [i..n-1] was never biased, so only sum over [0..i-1]. */
    int32_t sum_b = 0;
    for (int j = 0; j < i; j++) sum_b += (int32_t)b[j];
    result -= 128 * sum_b;

    /* Tail — computed without bias */
    result += sam_int8_dot_scalar(a + i, b + i, n - i);

    return result;
}
#endif /* __AVX2__ */

/* ============================================================
 * sam_int8_dot — unified dispatch
 * Call this everywhere. It picks the best path automatically.
 * ============================================================ */
static inline int32_t sam_int8_dot(const int8_t *a, const int8_t *b, int n)
{
    switch (sam_simd_level) {
#ifdef __AVX2__
        case SAM_SIMD_AVX2:  return sam_int8_dot_avx2(a, b, n);
#endif
        case SAM_SIMD_SSE42: return sam_int8_dot_sse42(a, b, n);
        default:             return sam_int8_dot_scalar(a, b, n);
    }
}

/* ============================================================
 * sam_int8_matmul — tiled INT8 matrix multiply
 *
 * Computes C = A × B where:
 *   A is M×K (row-major, INT8)
 *   B is K×N (row-major, INT8) — NOTE: B must be pre-transposed
 *             so that B_T[j][k] = B[k][j], making each row of B_T
 *             a column of B. This lets us call sam_int8_dot() on
 *             contiguous memory for every output element.
 *   C is M×N (row-major, INT32 accumulator)
 *
 * Each output element:
 *   C[i][j] = dot(A[i], B_T[j], K)
 *           = sum over k of A[i*K+k] * B[j*K+k]
 *
 * K must be a multiple of 16 (SSE4.2) or 32 (AVX2).
 *
 * This is the exact operation performed by every transformer
 * layer: Q*K^T, attention*V, and FFN weight projections.
 * ============================================================ */
static inline void sam_int8_matmul(
    const int8_t  *A,    /* M×K input matrix  */
    const int8_t  *B_T,  /* N×K transposed B  */
    int32_t       *C,    /* M×N output (int32 accumulators) */
    int M, int K, int N)
{
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            C[i * N + j] = sam_int8_dot(
                A   + i * K,   /* row i of A */
                B_T + j * K,   /* row j of B_T = col j of B */
                K
            );
        }
    }
}

#endif /* SAM_SIMD_H */
