#!/usr/bin/env python3
"""Run the player's actual periodic-report block against a slow-output clock.

Only dependencies (clock, audio service, and output) are stubbed. The report
simulates nine seconds of output and Flush another second: the old stale
window timestamp would immediately make the next report due. This also
checks that summary-only mode never enters that reporting block.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'amiga/mrplay.c').read_text()
start = source.index('if (want_time && !benchmark && now - stats.since_us >= STATS_INTERVAL_US)')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
block = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define STATS_INTERVAL_US 3000000ULL
static uint64_t fake_now;
static unsigned reports, flushes;
static struct { uint64_t since_us; } stats;
static struct { int enabled; uint64_t last_service_us, last_audio_gap_print_us; } trace;
static int audio = 1, dx, qcount;
static uint64_t monotonic_us(void) { return fake_now; }
static void trace_phase(void *p, const char *s) { (void)p; (void)s; }
static void service_audio_for_display(void *p) { (void)p; assert(!trace.enabled); }
static void report_stats(void *st, int a, int d, void *t, int q, uint64_t now)
{
    (void)st; (void)a; (void)d; (void)t; (void)q;
    reports++; fake_now += 9000000ULL;
    stats.since_us = now; /* report_stats resets its counters with entry time. */
}
static int Output(void) { return 1; }
static void Flush(int handle) { (void)handle; flushes++; fake_now += 1000000ULL; }
static void step(int want_time, int benchmark)
{
    uint64_t now = fake_now;
''' + block + r'''
}
int main(void)
{
    trace.enabled = 1;
    fake_now = 3000000ULL;
    step(1, 0);
    assert(reports == 1 && flushes == 1);
    assert(stats.since_us == 13000000ULL);
    assert(trace.enabled == 1 && trace.last_service_us == fake_now);
    assert(trace.last_audio_gap_print_us == fake_now);
    step(1, 0); assert(reports == 1); /* no immediate re-report */
    fake_now += 2999999ULL;
    step(1, 0); assert(reports == 1);
    fake_now++;
    step(1, 0); assert(reports == 2);
    fake_now += 10000000ULL;
    step(1, 1); assert(reports == 2); /* --benchmark, despite a due window */
    step(0, 0); assert(reports == 2); /* ordinary, untimed playback */
    puts("Reporting cadence: slow report/flush, quiet benchmark, trace restoration passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mintvid-report-test-') as directory:
    path = Path(directory)
    (path / 'report.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c99', '-Wall', '-Wextra', '-Werror', str(path / 'report.c'),
        '-o', str(path / 'report')], check=True)
    subprocess.run([str(path / 'report')], check=True)
