#!/usr/bin/env python3
"""Exercise the production Amiga HLS queue with simulated Exec message delivery.

No network or Amiga runtime required. Only task/message/time primitives are
replaced; queue, reply ownership, demand waits and diagnostic code are real.
"""
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = (root / 'amiga/hls_fetch.c').read_text()
def function(name):
    matches = list(re.finditer(r'^(?:static )?(?:void|int|hls_fetch_slot \*)\s*' + name + r'\(', s, re.M))
    start = next(m.start() for m in matches if s.index('{', m.start()) < s.index(';', m.start()))
    brace = s.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end] + '\n'

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/mr_http.h"
#include "amiga/hls_fetch.h"
struct Message { int unused; };
struct MsgPort { int unused; };
static clock_t fake_clock;
static clock_t test_clock(void) { return fake_clock; }
#define clock test_clock
#define mr_free free
static int in_flight, ticks_left, completions, sends, abort_wait;
static char completed[32][64], error[192];
static struct Message *GetMsg(struct MsgPort *port);
static void PutMsg(struct MsgPort *port, struct Message *msg);
static void Delay(int ticks);
void mr_source_set_error(const char *text) { snprintf(error, sizeof error, "%s", text); }
''' + s[s.index('#define HLS_FETCH_STACK'):s.index('/* ---- worker task')] + r'''
static void PutMsg(struct MsgPort *port, struct Message *msg)
{
    (void)port; assert(msg == &g_req.msg); assert(!in_flight);
    in_flight = 1; ticks_left = 2; sends++;
}
static struct Message *GetMsg(struct MsgPort *port)
{
    (void)port;
    if (!in_flight || ticks_left) return NULL;
    in_flight = 0;
    assert(strlen(g_req.url) < 64);
    strcpy(completed[completions++], g_req.url);
    g_req.ok = strcmp(g_req.url, "bad") != 0 &&
        !(strcmp(g_req.url, "oversized") == 0 && g_req.max_size < HLS_FETCH_HINT_MAX);
    g_req.buf = g_req.ok ? (unsigned char *)strdup(g_req.url) : NULL;
    g_req.len = g_req.ok ? strlen(g_req.url) + 1 : 0;
    strcpy(g_req.error, "simulated failure");
    return &g_req.msg;
}
static void Delay(int ticks)
{
    assert(ticks == 1); fake_clock += CLOCKS_PER_SEC / 50;
    if (in_flight && ticks_left) ticks_left--;
}
static int service(void *unused)
{
    hls_fetch_diagnostic diag;
    (void)unused;
    hls_fetch_get_diagnostics(&diag);
    assert(diag.busy && diag.pending <= HLS_FETCH_LOOKAHEAD_DEPTH);
    return abort_wait;
}
static void hls_fetch_send(const char *, const mr_http_options *, const char *, size_t);
'''
for name in ['hls_fetch_free_ready', 'hls_fetch_free_slots', 'hls_fetch_pump',
             'hls_fetch_reclaim', 'hls_fetch_take_ready', 'hls_fetch_wait_busy',
             'hls_fetch_send', 'hls_fetch_find_slot', 'hls_fetch_override',
             'hls_fetch_hint', 'hls_fetch_poll', 'hls_fetch_ready', 'hls_fetch_set_diagnostics',
             'hls_fetch_get_diagnostics']:
    harness += function(name)
harness += r'''
static void reset(void)
{
    g_stopping = 1;
    if (in_flight) { ticks_left = 0; hls_fetch_reclaim(); }
    hls_fetch_free_ready(); hls_fetch_free_slots();
    g_busy = g_stopping = g_verbose = g_prefetch_blocked = 0;
    g_prefetch_budget = g_generation = g_next_seq = 0;
    g_hits = g_misses = g_worst_wait_ms = 0;
    memset(&g_diag, 0, sizeof g_diag);
    g_active = 1; g_reply_port = (struct MsgPort *)1;
    g_service = service; g_diagnostics = 1;
    in_flight = ticks_left = completions = sends = abort_wait = 0;
    fake_clock = 0;
}
static void demand(const char *url)
{
    unsigned char *out = NULL; size_t len = 0;
    assert(hls_fetch_override(url, NULL, NULL, &out, &len, 0));
    assert(len == strlen(url) + 1 && !strcmp((char *)out, url)); free(out);
}
int main(void)
{
    unsigned char *out; size_t len; hls_fetch_diagnostic diag;
    reset();
    hls_fetch_hint("a", NULL); hls_fetch_hint("b", NULL); hls_fetch_hint("c", NULL);
    assert(!hls_fetch_ready("a") && !hls_fetch_ready("untracked") && !fake_clock);
    demand("a");
    assert(completions == 1 && !strcmp(completed[0], "a"));
    assert(g_busy && !strcmp(g_req.url, "b")); /* next fetch runs AFTER demand returns */
    hls_fetch_get_diagnostics(&diag);
    assert(diag.waits == 1 && diag.last_wait_ms == 40 && diag.pending == 1);
    demand("b"); assert(completions == 2);
    reset();
    hls_fetch_hint("a", NULL); hls_fetch_hint("b", NULL); hls_fetch_hint("c", NULL);
    hls_fetch_hint("d", NULL);
    demand("c"); assert(completions == 3 && !strcmp(g_req.url, "d"));
    reset();
    hls_fetch_hint("a", NULL); hls_fetch_hint("b", NULL); hls_fetch_hint("c", NULL);
    demand("playlist"); /* on-demand work must precede queued speculative work */
    assert(completions == 2 && !strcmp(completed[0], "a") && !strcmp(completed[1], "playlist"));
    assert(!strcmp(g_req.url, "b"));
    reset();
    hls_fetch_hint("a", NULL); hls_fetch_hint("b", NULL);
    Delay(1); Delay(1); hls_fetch_poll();
    assert(completions == 1 && g_busy && !strcmp(g_req.url, "b"));
    assert(hls_fetch_ready("a") && !hls_fetch_ready("b"));
    demand("a"); assert(completions == 1); /* READY hit never waits on b */
    hls_fetch_get_diagnostics(&diag); assert(diag.polls == 1);
    reset();
    hls_fetch_hint("bad", NULL); hls_fetch_hint("b", NULL);
    assert(!hls_fetch_override("bad", NULL, NULL, &out, &len, 0));
    assert(!out && !len && completions == 1 && !strcmp(error, "simulated failure"));
    reset();
    { mr_http_options opts = {0}; opts.source_buffer_bytes = 1;
      hls_fetch_hint("oversized", &opts); }
    demand("oversized"); assert(completions == 2 && g_misses == 1);
    reset();
    hls_fetch_hint("old", NULL); g_generation++; /* cancel's stale reply contract */
    demand("new"); assert(completions == 2 && !g_have_ready);
    reset();
    hls_fetch_hint("a", NULL); abort_wait = 1;
    assert(!hls_fetch_override("a", NULL, NULL, &out, &len, 0));
    assert(g_busy && !g_waiting && !out && !len); /* abort keeps request owned */
    abort_wait = 0; demand("a"); assert(completions == 1);
    reset();
    hls_fetch_set_diagnostics(0); demand("direct");
    hls_fetch_get_diagnostics(&diag); assert(!diag.waits && !diag.total_wait_ms);
    reset();
    g_stopping = 1; hls_fetch_hint("a", NULL); hls_fetch_poll();
    assert(!g_busy && sends == 0 && !hls_fetch_ready("a")); /* shutdown never promotes lookahead */
    reset(); g_active = 0;
    puts("HLS scheduling: demand latency, priority, polling, errors, budget, cancellation, quiet timing passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source, binary = Path(tmp) / 'check.c', Path(tmp) / 'check'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu99', '-Wall', '-Wextra', '-Wno-unused-variable',
                    '-I', str(root), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
