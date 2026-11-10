#!/usr/bin/env python3
"""Test actual audio-only polling and sparse reporting under slow output."""
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
ui = block('typedef struct audio_only_ui') + ' audio_only_ui;\n'
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "amiga/mr_audio.h"
#include "amiga/hls_fetch.h"
#include "core/mr_source.h"
static clock_t fake_clock;
static unsigned polls, snapshots, flushes, clock_reads;
static int active;
static clock_t test_clock(void) { clock_reads++; return fake_clock; }
#define clock test_clock
static int Output(void) { return 1; }
static void Flush(int output) { (void)output; flushes++; fake_clock += 10 * CLOCKS_PER_SEC; }
int hls_fetch_active(void) { return active; }
void hls_fetch_poll(void) { polls++; }
void audio_diagnostics(mr_audio *a, mr_audio_diagnostics *diag)
{ (void)a; memset(diag, 0, sizeof *diag); snapshots++; }
void hls_fetch_get_diagnostics(hls_fetch_diagnostic *diag) { memset(diag, 0, sizeof *diag); }
void mr_source_timing_get(mr_source_timing *diag) { memset(diag, 0, sizeof *diag); }
''' + ui + block('static void audio_only_report(') + '\n' + block('static void audio_only_poll_hls(') + r'''
int main(void)
{
    audio_only_ui ui = {0}; clock_t last_poll = 0;
    ui.audio = (mr_audio *)1; ui.phase = "demux";
    audio_only_report(&ui, NULL); assert(!clock_reads && !snapshots);
    ui.diagnostic = 1;
    fake_clock = 5 * CLOCKS_PER_SEC - 1;
    audio_only_report(&ui, NULL); assert(!snapshots);
    fake_clock++; audio_only_report(&ui, NULL);
    assert(snapshots == 1 && flushes == 1 && ui.last_report_clock == fake_clock);
    audio_only_report(&ui, NULL); assert(snapshots == 1); /* slow print cannot retrigger */
    fake_clock += 5 * CLOCKS_PER_SEC - 1;
    audio_only_report(&ui, NULL); assert(snapshots == 1);
    fake_clock++; audio_only_report(&ui, NULL); assert(snapshots == 2);
    audio_only_report(&ui, "quit"); assert(snapshots == 3); /* final snapshot always emitted */
    ui.audio = NULL; audio_only_report(&ui, "quit"); assert(snapshots == 3);
    clock_reads = 0; audio_only_poll_hls(&last_poll); assert(!polls && !clock_reads);
    active = 1; fake_clock = (CLOCKS_PER_SEC + 49UL) / 50UL;
    audio_only_poll_hls(&last_poll); assert(polls == 1);
    audio_only_poll_hls(&last_poll); assert(polls == 1);
    fake_clock += (CLOCKS_PER_SEC + 49UL) / 50UL;
    audio_only_poll_hls(&last_poll); assert(polls == 2);
    puts("Audio-only: 20ms polling, inactive fast path, 5s diagnostics, slow-output cadence, teardown passed");
    return 0;
}
'''
# Polling must remain before the pause path, and diagnostics must stay alive
# inside serviced network waits while being detached before audio teardown.
loop = s[s.index('static int play_audio_only('):s.index('int main(')]
assert loop.index('audio_only_poll_hls(&last_hls_poll)') < loop.index('if (paused) { Delay(2); continue; }')
assert loop.index('ui.audio = NULL') < loop.index('audio_close(audio)')
assert 'audio_only_report' in block('static int audio_only_io_service(')
with tempfile.TemporaryDirectory() as tmp:
    source, binary = Path(tmp) / 'check.c', Path(tmp) / 'check'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
