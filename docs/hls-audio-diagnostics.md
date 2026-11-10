# Audio-only HLS delivery on the 68060

Local AAC-LC playing smoothly while YouTube Live stutters points toward
stream delivery, although different AAC rates/channels/bitrates still matter.
This change keeps the AAC decoder and existing PCM/request buffer sizes as
previously configured. It fixes these scheduler issues:

- Audio-only playback now calls `hls_fetch_poll()` about every 20 ms, including
  during pause. Completed compressed downloads are collected and the next
  queued prefetch starts while audio from the current segment plays.
- Collecting a reply no longer automatically starts another download inside
  a blocking demand wait. Previously `hls_fetch_wait_busy()` could see the
  worker become busy again immediately and wait for the entire remaining
  prefetch queue, even after the required segment was ready. Polls and hints
  still advance lookahead; demand callers consume their result first and
  then resume prefetch. An unhinted playlist request waits for at most the
  current speculative download before taking priority over queued hints.
  This shared fix also applies to normal video playback.
- A background live-playlist refresh is consumed only once the worker has a
  completed reply. Previously, opening a playable segment could immediately
  block again on an unfinished playlist hint, withholding that segment from
  the demuxer/decoder. The pending refresh is now deferred to a later open.
  At the real live edge, the existing serviced fetch/retry still applies, so
  a dropped hint cannot prevent progress. Synchronous overrides without a
  readiness probe retain their previous behavior.
- TLS session tickets are retained for up to four host/port pairs rather
  than only the last host contacted. Playlist and segment hosts can then
  alternate without discarding each other's ticket. Least-recently-saved
  entries are evicted; protocol-cap changes and shutdown release all entries.
  Resumption remains best-effort and the server can require a full handshake.

Audio output already has its own worker; adding `audio_service()` to the
network callback would not fix delivery. The core HLS source already refreshes
and retries live playlists in audio-only mode. The video scheduler additionally
has whole-player reconnect/resync logic; this patch does not port that logic
or add reconnects. Collect evidence before changing recovery policy.

## Test on the Amiga

First play the same live URL with the same audio/output/Fast-buffer settings
and **without diagnostics** to judge whether it sounds better. Then collect
about 1–2 minutes of the live stream and a local comparison:

```
mrplay "https://www.youtube.com/watch?v=YOUR_LIVE_ID" --no-video --audio-diag >NAS0:live-audio.txt
mrplay Work:local-aac.mp4 --no-video --audio-diag >NAS0:local-audio.txt
```

Keep any other options from your usual command. Press Esc to stop. The
recorder takes a snapshot about every five seconds, including during
serviced network/live-edge waits, plus demux-stop/final snapshots. It stores
these in RAM and **prints only after audio output has closed**. Expect the
NAS file to remain quiet during playback; wait for the player to finish
writing after Esc. Normal EOF saves the trace when the ended window closes.
Do not add `--time`, `--live-diag` or `--benchmark`.

`--audio-diag` requires `--no-video` and does not enable verbose worker/HLS
output. A bounded 128-entry ring retains roughly the latest ten minutes;
its header reports overwritten snapshots. This allocation is for diagnostic
records, with no change to PCM or compressed-media buffer sizes. If it
cannot be allocated, a startup message says recording is disabled.

The first hardware trace exposed a problem with the previous implementation:
it formatted and flushed each snapshot during playback. Its nominal
five-second reports arrived 6–10 seconds apart, with additional starvations
even between completed HLS waits. Slow console/NAS output was a likely
contributor and also inflated measured waits. The RAM recorder removes all
per-snapshot formatting and output from the playback and network-wait paths.
A clean trace is needed before assigning the remaining stalls to downloads,
AAC decoding or live-edge handling.

Fields are snapshots or cumulative since the audio-only loop opened:

| Field | Meaning |
| --- | --- |
| `at`, `phase` | Wall time in ms; playback/buffering, demux, pause, drain or live-edge wait |
| `pcm`, `fifo`, `hw` | Total buffered ms, software FIFO ms, playing-remaining + queued hardware ms |
| `starves`, `drops`, `active` | Backend hardware starvation count, dropped FIFO samples, active hardware requests |
| `hls` | Worker hit/miss counts (include initial opening/resolution); a hit can still require a wait |
| `ready`, `pending`, `busy`, `polls` | Queued lookahead state, in-flight flag, audio-loop poll count |
| `waits`, `wait`, `last`, `max`, `total` | Number of timed worker waits; current, last, worst and cumulative wait ms |
| `segment`, `playlist` | Cumulative main-task HLS segment-open and playlist-fetch wait ms |

Worker wait timing starts at audio-only setup; hit/miss counts start earlier.
Cancelled waits are included. Segment/playlist counters update when calls
return, so an in-progress stall appears first in `wait`/`phase`. Segment-open
waits are not the background download durations and should not be added to
worker wait totals. Core live-edge sleep is visible as `phase=live-edge` but
is not a worker download wait. With no HLS worker, queue/wait fields stay zero.
Each snapshot also has an `audio-net` line, saved in RAM and printed after
stop. These are process-wide cumulative HTTP counters (including resolver
and opening requests): `fetches`, `fetch` and `max` are completed fetch count,
total duration and longest duration; `connects`, `dns`, `tcp`, `tls`, `header`
and `body` split the connection/request/body cost in ms. `resumed` is resumed
TLS handshakes / attempted TLS handshakes. The worker may be partway through
updating these diagnostic counters; compare changes across several snapshots,
not a single instantaneous subtraction. They are wall times, including CPU
contention, and must not be added to the overlapping HLS wait totals.

The demux-stop line prints its status and last source diagnostic, which may
be stale at ordinary EOF; it does not by itself establish a network error.

A rising starvation count with low PCM and a long HLS wait supports delivery
starvation. Growing drops point toward overfilling instead. Zero starvation
with audible glitches needs further investigation; it does not clear the AAC
path or hardware timing. Compare equivalent source rate/channel settings.

## Verification

`make -C player check-hls-worker-lifecycle` runs production queue code with
simulated message delivery: demanded segment returns before later lookahead,
queued demand ordering, playlist priority, READY hits, polling, failed
segments, byte-budget fallback, stale generations, abort ownership, quiet
counters and shutdown promotion suppression. It also checks the production
audio-only polling and RAM recorder against simulated slow output, asserting
zero diagnostic writes while playing, bounded chronological retention and a
post-stop dump. `mr_hls_override_check` covers deferred unfinished refreshes,
consuming ready refreshes and mandatory fetches at the live edge.
These checks do not measure real 68060 timing or prove live playback quality.

## Clean hardware trace motivating the readiness check

The RAM trace recorded 77 hardware starvations in 243.428 seconds, zero PCM
FIFO drops and 113.076 seconds of worker waits. Segment-open waits totalled
81.628 seconds and playlist-fetch waits 31.104 seconds; these overlap worker
waits. The longest worker wait was 3.922 seconds. Several snapshots have zero
PCM and zero active requests during demux waits, including a 3.291-second
in-progress wait at 35 seconds. Reports now arrive about five seconds apart.
This establishes delivery underruns in that run, but does not identify the
network/CPU phase responsible for every stall or prove the readiness fix
eliminates them. The HTTP phase counters are intended to narrow that down.

## Remaining load with video disabled

`--no-video` drops video packets after downloading and demuxing the selected
rendition. It does not request a separate audio-only YouTube stream. As a
separate comparison, `--hls-low` asks for the lowest-bandwidth supported HLS
variant, reducing unwanted video traffic/TLS processing where the server
provides a cheaper variant. Its audio characteristics may differ. Keep other
options constant and label that run separately from the same-rendition test.

## Low-rendition trace and TLS setup

The subsequent low-rendition run recorded nine starvations in 140.772 seconds,
with zero FIFO drops and 28.514 seconds of main-task worker waits. Starvation
counts rose on roughly a 15-second cycle. HTTP counters, including resolver
and startup requests, recorded 98.488 seconds of TLS handshake time versus
18.172 seconds reading response bodies. Only 22 of 45 handshakes resumed.
These are wall times, including CPU contention, rather than a CPU profile.
The new counters point at setup cost even after reducing media traffic.

The old single-ticket cache could forget the segment host's resumable session
when a playlist on another host completed. The per-origin cache addresses
that avoidable cache miss; it does not remove TCP connects, guarantee server
resumption, or prove every remaining glitch is TLS-related. Compare the same
low-rendition command and the changes in `tls` / `resumed` on hardware.

`make -C player check-http-tls-cache` exercises production cache code for
host/port isolation, replacement, bounded eviction, reference ownership,
non-resumable sessions, protocol-cap changes and repeated cleanup.
`make -C player check-https` additionally alternates real local HTTPS origins
under automatic TLS and TLS 1.2: each origin has one initial full handshake,
then resumes. The previous single-ticket implementation fails this test.
