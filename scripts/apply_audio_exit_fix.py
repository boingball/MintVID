#!/usr/bin/env python3
"""One-off, anchored patch for Amiga worker exit and ESC diagnostics.

Remove this script and its workflow from the finished PR.
"""
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one anchor, got {count}: {old[:80]!r}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "player/amiga/audio_paula.c",
    """    audio_worker_cleanup(a);
    a->worker_task = NULL;
    Signal(a->parent_task, 1UL << a->stopped_sig);
    return;
failed:
    audio_worker_cleanup(a);
    a->worker_task = NULL;
    a->ready_ok = 0;
    Signal(a->parent_task, 1UL << a->ready_sig);
}""",
    """    audio_worker_cleanup(a);
    /* The parent may free this context and unload mrplay as soon as it
     * receives stopped_sig. Prevent it from running until this NP_Entry
     * worker has actually returned and DOS removes the task. Match the
     * HLS worker's final Forbid()/ReplyMsg() handshake; no Permit here. */
    Forbid();
    a->worker_task = NULL;
    Signal(a->parent_task, 1UL << a->stopped_sig);
    return;
failed:
    audio_worker_cleanup(a);
    /* A failed startup also wakes a parent that immediately calls
     * audio_close() and frees this context: the same exit race applies. */
    Forbid();
    a->worker_task = NULL;
    a->ready_ok = 0;
    Signal(a->parent_task, 1UL << a->ready_sig);
}""",
)

replace_once(
    "player/amiga/mrplay.c",
    """static int mrplay_exit(int code)
{""",
    """/* --time-only, flushed breadcrumbs: the last completed stage identifies
 * which cleanup call a Guru occurred in, without guessing from a screen. */
static void shutdown_checkpoint(int enabled, const char *stage)
{
    if (!enabled) return;
    printf("shutdown: %s\\n", stage);
    Flush(Output());
}

static int mrplay_exit(int code)
{""",
)

replace_once(
    "player/amiga/mrplay.c",
    """    player_status(MR_PLAYER_STATE_ENDED, codec->name, "stream ended");
    { int qi; for (qi = 0; qi < VIDEO_QUEUE_CAP; qi++) free(vq[qi].rgb); }
    if (audio_dec) mr_audio_decoder_close(audio_dec);
    control_audio = NULL;
    if (audio) audio_close(audio);
    display_close(disp);
    mr_decoder_close(&dec);
    mr_demux_close(dx);
    free(buf);
    return mrplay_exit(0);
}""",
    """    shutdown_checkpoint(want_time, "begin");
    player_status(MR_PLAYER_STATE_ENDED, codec->name, "stream ended");
    shutdown_checkpoint(want_time, "free video queue");
    { int qi; for (qi = 0; qi < VIDEO_QUEUE_CAP; qi++) free(vq[qi].rgb); }
    shutdown_checkpoint(want_time, "close audio decoder");
    if (audio_dec) mr_audio_decoder_close(audio_dec);
    control_audio = NULL;
    shutdown_checkpoint(want_time, "close Paula worker");
    if (audio) audio_close(audio);
    shutdown_checkpoint(want_time, "close display/P96 overlay");
    display_close(disp);
    shutdown_checkpoint(want_time, "close video decoder");
    mr_decoder_close(&dec);
    shutdown_checkpoint(want_time, "close demux/HLS source");
    mr_demux_close(dx);
    free(buf);
    shutdown_checkpoint(want_time, "join HLS worker and close network");
    {
        int rc = mrplay_exit(0);
        shutdown_checkpoint(want_time, "complete");
        return rc;
    }
}""",
)

# Check source invariants: callback detachment already exists and must stay.
player = Path("player/amiga/mrplay.c").read_text()
audio = Path("player/amiga/audio_paula.c").read_text()
assert player.index("hls_fetch_set_service(NULL, NULL);") < player.index('shutdown_checkpoint(want_time, "begin")')
assert player.index('shutdown_checkpoint(want_time, "close display/P96 overlay")') < player.index('shutdown_checkpoint(want_time, "join HLS worker and close network")')
assert audio.count("    Forbid();\n    a->worker_task = NULL;\n    Signal(a->parent_task, 1UL << a->stopped_sig);") == 1
assert audio.count("    Forbid();\n    a->worker_task = NULL;\n    a->ready_ok = 0;") == 1
print("Audio worker exit handshake and ESC shutdown diagnostics: anchored checks passed")
