#ifndef MR_MPEG1_SCHED_H
#define MR_MPEG1_SCHED_H

/*
 * Pacing policy for the MPEG-1 program-stream player (amiga/mrplay.c's
 * play_mpeg1()). That path pre-dates the generic streaming player and has no
 * queue of decoded frames: it decodes one frame, paces it against the Paula
 * audio clock, shows it, and repeats. The two decisions it makes per iteration
 * - "pull another MP2 frame?" and "drop this video frame?" - live here so they
 * are portable C that `make check` can exercise on the host, because nothing
 * under player/amiga/ can be compiled, let alone run, on the dev host.
 */

/* Milliseconds of decoded audio to keep queued ahead of the device.
 *
 * The top-up must be measured in milliseconds of audio, never in MP2 frames.
 * One MP2 frame is always 1152 sample frames, but that is 26 ms of a 44.1 kHz
 * stream decimated to 22.05 kHz for Paula and 52 ms of a stream that is
 * already 22.05 kHz and so is not decimated at all. A fixed
 * two-frames-per-video-frame top-up therefore queues 104 ms of audio for every
 * 40 ms of 25 fps video on a 22.05 kHz clip - 2.6x real time - which pushes
 * the whole track into the FIFO during the first third of the file and leaves
 * the rest of the clip with nothing queued at all. Past roughly six seconds of
 * such a clip it also overruns audio_paula.c's 4 s ring, and fifo_push() drops
 * what will not fit. Refilling to a cushion self-corrects in both directions:
 * it stops early when the device is well ahead, and keeps pulling when a slow
 * machine's iteration costs more than one MP2 frame of audio. */
#define MPEG1_AUDIO_CUSHION_MS 800UL

/* Upper bound on MP2 frames decoded in one video-frame iteration - loose while
 * priming, tight once Paula is playing.
 *
 * This is not just an event-handling guard: it is what keeps the refill from
 * shunting the picture out of sync. play_mpeg1() decodes MP2 with pl_mpeg's
 * own portable C Layer II decoder, not MintAMP's m68k-optimised one, and every
 * pull happens inline between showing one frame and the next. Time spent
 * pulling *after* the playback gate opens is time Paula spends playing while
 * video pts does not advance, so a burst there puts the audio clock
 * permanently ahead of the video timeline. The loop can only work that deficit
 * off by free-running - decoding flat out so pts gains on real time - and
 * while it does, the drop-run cap shows one frame in three.
 *
 * That is exactly what filling the cushion in two stages produced: priming to
 * 400 ms before the gate, then ramping to the full cushion afterwards, cost a
 * dozen post-gate pulls in one iteration. On an 060/50 that read as the player
 * locking up on frame 1, stepping to frame 3 and 6 with stuttery sound, and
 * only then running clean once the cushion was full and the pulls stopped.
 *
 * So prime the whole cushion before opening the gate, where nothing is playing
 * and the cost is paid once, invisibly, ahead of the first frame. After that
 * the top-up only replaces what Paula has drained - about one MP2 frame per
 * video frame - and MPEG1_AUDIO_MAX_PULLS keeps a recovery burst to ~200 ms of
 * 22.05 kHz audio, still refilling faster than it drains without opening a
 * deficit the picture has to pay for. */
#define MPEG1_AUDIO_PRIME_PULLS 20
#define MPEG1_AUDIO_MAX_PULLS 4

/* Longest run of consecutive video frames the pacer may drop.
 *
 * Dropping is worth doing - it skips the c2p/dither/blit cost of a frame that
 * is already late - but this path has no decoded-frame queue to skip forward
 * into, so a drop buys one frame's display time and nothing more. Left
 * uncapped on a machine that cannot decode the stream at its own frame rate,
 * the audio clock sits permanently more than one frame period ahead of video
 * pts, every frame after the first is dropped, and the picture freezes on
 * frame 1 for the whole clip while audio plays on. Capping the run guarantees
 * at least one frame in every MPEG1_MAX_DROP_RUN + 1 reaches the screen. */
#define MPEG1_MAX_DROP_RUN 2

/* Whether another MP2 frame should be decoded into the audio device now.
 * `buffered_ms` is audio_buffered_ms(): FIFO plus submitted-but-unplayed.
 * `started` is 0 only while priming, before the playback gate is opened, which
 * is the one time a long burst is free. `pulls` counts MP2 frames already
 * taken in this iteration. */
int mr_mpeg1_want_audio(unsigned long buffered_ms, int started, int pulls);

/* Whether the just-decoded video frame should be dropped instead of shown.
 * All arguments are milliseconds except `drop_run`, the number of frames
 * dropped consecutively since the last one actually displayed. */
int mr_mpeg1_drop_frame(unsigned long audio_elapsed_ms, unsigned long target_ms,
                        unsigned long period_ms, int drop_run);

/* Whether the next decoder call should discard any B pictures it encounters
 * before producing an I/P reference picture. This is the useful form of frame
 * dropping for the serial MPEG-1 path: it avoids the expensive reconstruction
 * instead of discarding an already-decoded result. */
int mr_mpeg1_skip_b_frames(unsigned long audio_elapsed_ms,
                           unsigned long next_target_ms,
                           unsigned long period_ms);

#endif
