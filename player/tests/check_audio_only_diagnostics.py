#!/usr/bin/env python3
"""Test production RAM snapshots: no output while feeding PCM, bounded history."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'amiga/mrplay.c').read_text()
def block(marker):
    start = s.index(marker)
    brace = s.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
ui = '#define AUDIO_DIAG_SNAPSHOTS 128\n' + block('typedef struct audio_only_snapshot') + ' audio_only_snapshot;\n' + block('typedef struct audio_only_ui') + ' audio_only_ui;\n'
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "amiga/mr_audio.h"
#include "amiga/hls_fetch.h"
#include "core/mr_source.h"
#include "core/mr_http.h"
static clock_t fake_clock;
static unsigned polls, snapshots, flushes, clock_reads, writes;
static int playing = 1;
static int active;
static clock_t test_clock(void) { clock_reads++; return fake_clock; }
#define clock test_clock
static int Output(void) { return 1; }
static void Flush(int output) { (void)output; assert(!playing); flushes++; fake_clock += 10 * CLOCKS_PER_SEC; }
static int test_printf(const char *format, ...) {
    (void)format; assert(!playing); writes++; fake_clock += 3 * CLOCKS_PER_SEC; return 0;
}
#define printf test_printf
int hls_fetch_active(void) { return active; }
void hls_fetch_poll(void) { polls++; }
void audio_diagnostics(mr_audio *a, mr_audio_diagnostics *diag)
{ (void)a; memset(diag, 0, sizeof *diag); snapshots++; }
void hls_fetch_get_diagnostics(hls_fetch_diagnostic *diag) { memset(diag, 0, sizeof *diag); }
void mr_source_timing_get(mr_source_timing *diag) { memset(diag, 0, sizeof *diag); }
void mr_http_timing_get(mr_http_timing *diag) { memset(diag, 0, sizeof *diag); }
''' + ui + block('static void audio_only_report(') + '\n' + block('static void audio_only_dump(') + '\n' + block('static void audio_only_poll_hls(') + r'''
int main(void)
{
    audio_only_ui ui = {0}; clock_t last_poll = 0;
    audio_only_snapshot records[AUDIO_DIAG_SNAPSHOTS]; unsigned i;
    ui.audio = (mr_audio *)1; ui.phase = "demux";
    audio_only_report(&ui, NULL); assert(!clock_reads && !snapshots);
    ui.diagnostic = 1;
    audio_only_report(&ui, "quit"); assert(!snapshots); /* allocation failure */
    ui.snapshots = records;
    fake_clock = 5 * CLOCKS_PER_SEC - 1;
    audio_only_report(&ui, NULL); assert(!snapshots);
    fake_clock++; audio_only_report(&ui, NULL);
    assert(snapshots == 1 && !flushes && !writes && ui.last_report_clock == fake_clock);
    assert(ui.snapshot_count == 1 && records[0].at_ms == 5000);
    audio_only_report(&ui, NULL); assert(snapshots == 1);
    fake_clock += 5 * CLOCKS_PER_SEC - 1;
    audio_only_report(&ui, NULL); assert(snapshots == 1);
    fake_clock++; ui.phase = "live-edge"; audio_only_report(&ui, NULL);
    assert(snapshots == 2 && records[1].at_ms == 10000);
    assert(!strcmp(records[0].phase, "demux") && !strcmp(records[1].phase, "live-edge"));
    audio_only_report(&ui, "quit"); assert(snapshots == 3);
    assert(!strcmp(records[2].tag, "quit"));
    ui.audio = NULL; audio_only_report(&ui, "quit"); assert(snapshots == 3);
    /* Simulate a run longer than capacity: keep newest records chronologically. */
    ui.audio = (mr_audio *)1;
    ui.first_snapshot = ui.snapshot_count = ui.overwritten_snapshots = 0;
    for (i = 0; i < AUDIO_DIAG_SNAPSHOTS + 5; i++) {
        fake_clock = (clock_t)(i * 5UL * CLOCKS_PER_SEC);
        audio_only_report(&ui, "tick");
    }
    assert(ui.snapshot_count == AUDIO_DIAG_SNAPSHOTS && ui.overwritten_snapshots == 5);
    assert(records[ui.first_snapshot].at_ms == 25000);
    audio_only_report(&ui, "quit");
    assert(ui.overwritten_snapshots == 6 && records[ui.first_snapshot].at_ms == 30000);
    for (i = 0; i < AUDIO_DIAG_SNAPSHOTS - 1; i++)
        assert(records[(ui.first_snapshot + i) % AUDIO_DIAG_SNAPSHOTS].at_ms == (i + 6) * 5000UL);
    assert(!strcmp(records[(ui.first_snapshot + AUDIO_DIAG_SNAPSHOTS - 1) % AUDIO_DIAG_SNAPSHOTS].tag, "quit"));
    assert(!writes && !flushes); /* all snapshots, including terminal, are output-free */
    ui.audio = NULL; ui.diagnostic = 0; playing = 0;
    ui.demux_stopped = 1; ui.demux_status = MR_ERR;
    strcpy(ui.last_source_error, "simulated last source error");
    audio_only_dump(&ui);
    assert(writes == 2 * AUDIO_DIAG_SNAPSHOTS + 2 && flushes == 1);
    assert(ui.snapshot_count == AUDIO_DIAG_SNAPSHOTS); /* slow dump cannot recapture */
    ui.snapshots = NULL; audio_only_dump(&ui);
    assert(writes == 2 * AUDIO_DIAG_SNAPSHOTS + 2 && flushes == 1);
    clock_reads = 0; audio_only_poll_hls(&last_poll); assert(!polls && !clock_reads);
    active = 1; fake_clock = (CLOCKS_PER_SEC + 49UL) / 50UL;
    audio_only_poll_hls(&last_poll); assert(polls == 1);
    audio_only_poll_hls(&last_poll); assert(polls == 1);
    fake_clock += (CLOCKS_PER_SEC + 49UL) / 50UL;
    audio_only_poll_hls(&last_poll); assert(polls == 2);
    puts("Audio-only: 20ms polling, zero live writes, 5s RAM snapshots, bounded ring, post-stop dump passed");
    return 0;
}
'''
# Polling must remain before the pause path, and diagnostics must stay alive
# inside serviced network waits while being detached before audio teardown.
loop = s[s.index('static int play_audio_only('):s.index('int main(')]
assert loop.index('audio_only_poll_hls(&last_hls_poll)') < loop.index('if (paused) { Delay(2); continue; }')
assert loop.index('ui.audio = NULL') < loop.index('audio_close(audio)') < loop.index('audio_only_dump(&ui)')
assert 'audio_only_report' in block('static int audio_only_io_service(')
with tempfile.TemporaryDirectory() as tmp:
    source, binary = Path(tmp) / 'check.c', Path(tmp) / 'check'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
