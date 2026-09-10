/* Exact check for mr_u64_mul_u32/mr_u64_div_u16 (core/mr_muldiv64.h) against
 * the native `*`/`/` operators, plus the fused audio-clock shape those two
 * primitives exist to replace: samples * 1000000ULL / rate. */
#include <stdint.h>
#include <stdio.h>
#include "../core/mr_muldiv64.h"

static uint32_t rng = 24601;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }

static uint64_t next64(void)
{
    uint64_t hi = next(), lo = next();
    return (hi << 32) | lo;
}

int main(void)
{
    int fails = 0;

    /* mr_u64_mul_u32: edges + random, 64-bit x 32-bit truncating. */
    static const uint64_t a_edges[] = {
        0, 1, 0xffffffffULL, 0x100000000ULL, 0xffffffffffffffffULL,
        UINT64_C(238000000) * 1000000ULL /* a realistic 90-min-at-44.1kHz-ish magnitude */
    };
    static const uint32_t b_edges[] = {0, 1, 2, 1000000u, 0xffffffffu, 44100u, 8000u};
    for (unsigned i = 0; i < sizeof a_edges / sizeof a_edges[0]; ++i)
        for (unsigned j = 0; j < sizeof b_edges / sizeof b_edges[0]; ++j) {
            uint64_t expected = a_edges[i] * (uint64_t)b_edges[j];
            uint64_t actual = mr_u64_mul_u32(a_edges[i], b_edges[j]);
            if (expected != actual) {
                printf("MUL FAIL a=%llu b=%u expected=%llu actual=%llu\n",
                       (unsigned long long)a_edges[i], b_edges[j],
                       (unsigned long long)expected, (unsigned long long)actual);
                fails++;
            }
        }
    for (int t = 0; t < 200000; ++t) {
        uint64_t a = next64();
        uint32_t b = next();
        uint64_t expected = a * (uint64_t)b;
        uint64_t actual = mr_u64_mul_u32(a, b);
        if (expected != actual) {
            printf("MUL FAIL(rand) a=%llu b=%u expected=%llu actual=%llu\n",
                   (unsigned long long)a, b,
                   (unsigned long long)expected, (unsigned long long)actual);
            fails++;
        }
    }

    /* mr_u64_div_u16: divisor always < 65536 (real Paula output rates). */
    static const uint32_t divisors[] = {1, 2, 7, 255, 256, 8000, 22050,
                                         32000, 44100, 48000, 65535};
    static const uint64_t n_edges[] = {
        0, 1, 65535, 65536, 0xffffffffULL, 0x100000000ULL,
        0xffffffffffffffffULL, UINT64_C(238000000) * 1000000ULL
    };
    for (unsigned i = 0; i < sizeof n_edges / sizeof n_edges[0]; ++i)
        for (unsigned j = 0; j < sizeof divisors / sizeof divisors[0]; ++j) {
            uint64_t expected = n_edges[i] / divisors[j];
            uint64_t actual = mr_u64_div_u16(n_edges[i], divisors[j]);
            if (expected != actual) {
                printf("DIV FAIL n=%llu d=%u expected=%llu actual=%llu\n",
                       (unsigned long long)n_edges[i], divisors[j],
                       (unsigned long long)expected, (unsigned long long)actual);
                fails++;
            }
        }
    for (int t = 0; t < 200000; ++t) {
        uint64_t n = next64();
        uint32_t d = divisors[next() % (sizeof divisors / sizeof divisors[0])];
        uint64_t expected = n / d;
        uint64_t actual = mr_u64_div_u16(n, d);
        if (expected != actual) {
            printf("DIV FAIL(rand) n=%llu d=%u expected=%llu actual=%llu\n",
                   (unsigned long long)n, d,
                   (unsigned long long)expected, (unsigned long long)actual);
            fails++;
        }
    }

    /* The fused shape actually used in audio_paula.c: a growing sample
     * counter times 1000000 divided by a realistic output rate, checked
     * across a simulated multi-hour session so the counter genuinely needs
     * the full 64-bit range. */
    for (unsigned j = 0; j < sizeof divisors / sizeof divisors[0]; ++j) {
        uint64_t samples = 0;
        for (int frame = 0; frame < 500000; ++frame) {
            samples += 1000 + (next() % 4000); /* irregular buffer sizes */
            uint64_t expected = samples * 1000000ULL / divisors[j];
            uint64_t actual = mr_u64_div_u16(
                mr_u64_mul_u32(samples, 1000000u), divisors[j]);
            if (expected != actual) {
                printf("FUSED FAIL samples=%llu rate=%u expected=%llu actual=%llu\n",
                       (unsigned long long)samples, divisors[j],
                       (unsigned long long)expected, (unsigned long long)actual);
                fails++;
                break;
            }
        }
    }

    /* mr_u64_div_u24: divisor up to 2^24 (ReadEClock's tick frequency). */
    static const uint32_t freq_divisors[] = {1, 255, 256, 709379u, 715909u,
                                              1000000u, 0xffffffu};
    for (unsigned i = 0; i < sizeof n_edges / sizeof n_edges[0]; ++i)
        for (unsigned j = 0; j < sizeof freq_divisors / sizeof freq_divisors[0]; ++j) {
            uint64_t expected = n_edges[i] / freq_divisors[j];
            uint64_t actual = mr_u64_div_u24(n_edges[i], freq_divisors[j]);
            if (expected != actual) {
                printf("DIV24 FAIL n=%llu d=%u expected=%llu actual=%llu\n",
                       (unsigned long long)n_edges[i], freq_divisors[j],
                       (unsigned long long)expected, (unsigned long long)actual);
                fails++;
            }
        }
    for (int t = 0; t < 200000; ++t) {
        uint64_t n = next64();
        uint32_t d = freq_divisors[next() % (sizeof freq_divisors / sizeof freq_divisors[0])];
        uint64_t expected = n / d;
        uint64_t actual = mr_u64_div_u24(n, d);
        if (expected != actual) {
            printf("DIV24 FAIL(rand) n=%llu d=%u expected=%llu actual=%llu\n",
                   (unsigned long long)n, d,
                   (unsigned long long)expected, (unsigned long long)actual);
            fails++;
        }
    }
    /* The audio_now_us() shape: a growing EClock tick counter times 1e6
     * divided by the (session-invariant) tick frequency. */
    for (unsigned j = 0; j < sizeof freq_divisors / sizeof freq_divisors[0]; ++j) {
        uint64_t ticks = 0;
        for (int step = 0; step < 200000; ++step) {
            ticks += 1 + (next() % 4096); /* irregular tick deltas */
            uint64_t expected = ticks * 1000000ULL / freq_divisors[j];
            uint64_t actual = mr_u64_div_u24(
                mr_u64_mul_u32(ticks, 1000000u), freq_divisors[j]);
            if (expected != actual) {
                printf("FUSED24 FAIL ticks=%llu freq=%u expected=%llu actual=%llu\n",
                       (unsigned long long)ticks, freq_divisors[j],
                       (unsigned long long)expected, (unsigned long long)actual);
                fails++;
                break;
            }
        }
    }

    if (fails) {
        printf("mr_muldiv64: %d failures\n", fails);
        return 1;
    }
    puts("mr_muldiv64: exact edge/random/fused-audio-clock comparisons passed");
    return 0;
}
