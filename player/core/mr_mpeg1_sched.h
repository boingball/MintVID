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
#define MPEG1_AUDIO_STARTUP_MS 400UL
#define MPEG1_AUDIO_CUSHION_MS 1000UL

/* Upper bound on MP2 frames decoded in one video-frame iteration, so a long
 * refill cannot stall event handling. Sized to reach MPEG1_AUDIO_STARTUP_MS
 * from empty at 22.05 kHz (52 ms per MP2 frame) in a single iteration. */
#define MPEG1_AUDIO_MAX_PULLS 12

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
 * `started` is 0 only while priming, before the playback gate is opened.
 * `pulls` counts MP2 frames already taken in this iteration. */
int mr_mpeg1_want_audio(unsigned long buffered_ms, int started, int pulls);

/* Whether the just-decoded video frame should be dropped instead of shown.
 * All arguments are milliseconds except `drop_run`, the number of frames
 * dropped consecutively since the last one actually displayed. */
int mr_mpeg1_drop_frame(unsigned long audio_elapsed_ms, unsigned long target_ms,
                        unsigned long period_ms, int drop_run);

#endif
