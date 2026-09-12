/*
 * MintVID - player -> IPTV controller status channel (Amiga only).
 *
 * The IPTV browser (iptv_reaction.c) launches the stream in a *separate*
 * mrplay process, so until now the controller only knew "a player is running"
 * (the public control port exists) or "it is gone". It could not tell whether a
 * stream actually decoded, and when a stream refused to play the reason only
 * reached the player's own shell, never the IPTV status line. That made it
 * impossible to see, from the browser, *which* streams fail and *why*.
 *
 * This header adds a tiny shared status block that mrplay publishes and the
 * controller reads, so the IPTV status line can show either the codec that is
 * on screen ("Playing (H.264): 1280x720, MPEG-TS") or the reason a stream did
 * not play ("Not supported: H.265/HEVC video (fourcc 'HVC1') has no decoder").
 *
 * Transport: AmigaOS is a single flat address space with no memory protection,
 * so once the controller has found mrplay's public control port by name it can
 * read fields written by the player task directly. The block is therefore
 * embedded in the very MsgPort mrplay already publishes (MR_IPTV_PLAYER_PORT),
 * with the MsgPort as the first member so FindPort()'s result is also the
 * address of the wrapper. A magic word validates the block before it is
 * trusted, and every update bumps `seq` so the reader only refreshes the GUI
 * when something changed. All access happens under Forbid()/Permit(); classic
 * AmigaOS is cooperatively scheduled on a single CPU, so a copy taken under
 * Forbid() cannot be torn by the other task.
 */
#ifndef MR_PLAYER_STATUS_H
#define MR_PLAYER_STATUS_H

#include <exec/ports.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <string.h>

#include "../iptv/mr_iptv.h" /* MR_IPTV_PLAYER_PORT */

#define MR_PLAYER_STATUS_MAGIC 0x4D525653UL /* 'MRVS' */
#define MR_PLAYER_STATUS_FILE "T:MintVID.player-status"
#define MR_PLAYER_STATUS_TMP  "T:MintVID.player-status.tmp"
#define MR_PLAYER_STATUS_TEXT_MAX 208
#define MR_PLAYER_CODEC_MAX 48

#define MR_PLAYER_COMMAND_PAUSE      (1UL << 0)
#define MR_PLAYER_COMMAND_FAST       (1UL << 1)
#define MR_PLAYER_COMMAND_FULLSCREEN (1UL << 2)
#define MR_PLAYER_COMMAND_VOLUME_DOWN (1UL << 3)
#define MR_PLAYER_COMMAND_VOLUME_UP   (1UL << 4)
#define MR_PLAYER_COMMAND_VOLUME_SET  (1UL << 5)

enum {
  MR_PLAYER_STATE_STARTING = 0, /* process up, nothing opened yet            */
  MR_PLAYER_STATE_OPENING,      /* fetching/parsing the container / URL       */
  MR_PLAYER_STATE_PLAYING,      /* decoding: codec[] and text[] are live      */
  MR_PLAYER_STATE_UNSUPPORTED,  /* refused: text[] says why (codec etc.)      */
  MR_PLAYER_STATE_ERROR,        /* could not open/connect: text[] says why    */
  MR_PLAYER_STATE_ENDED         /* stream finished normally                   */
};

typedef struct {
  volatile ULONG magic; /* MR_PLAYER_STATUS_MAGIC once initialised */
  volatile ULONG seq;   /* bumped after every field update         */
  volatile LONG state;  /* MR_PLAYER_STATE_*                       */
  char codec[MR_PLAYER_CODEC_MAX];       /* video codec name, "" if unknown  */
  char text[MR_PLAYER_STATUS_TEXT_MAX];  /* human-readable line for the GUI  */
} mr_player_status;

/* A terminal status must outlive mrplay's public port: otherwise a controller
 * which polls just after process exit loses the reason and remains stuck on
 * "Connecting...".  mrplay atomically replaces this tiny T: snapshot whenever
 * state changes; the trailing magic rejects partial/stale writes. */
typedef struct {
  mr_player_status status;
  ULONG trailer_magic;
} mr_player_status_snapshot;

static inline int
mr_player_status_snapshot_valid(const mr_player_status_snapshot *snapshot) {
  return snapshot &&
         snapshot->status.magic == MR_PLAYER_STATUS_MAGIC &&
         snapshot->trailer_magic == MR_PLAYER_STATUS_MAGIC;
}

/* MsgPort first so a FindPort() result is also the wrapper address. */
typedef struct {
  struct MsgPort port;
  volatile ULONG commands;
  volatile ULONG volume; /* Paula volume, 0..64, for absolute GUI updates */
  mr_player_status status;
} mr_player_control_port;

static inline int mr_player_control_send(ULONG command) {
  mr_player_control_port *block;
  int sent = 0;
  Forbid();
  block = (mr_player_control_port *)FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
  if (block && block->status.magic == MR_PLAYER_STATUS_MAGIC) {
    block->commands |= command;
    Signal(block->port.mp_SigTask, SIGBREAKF_CTRL_E);
    sent = 1;
  }
  Permit();
  return sent;
}

/* Copy a NUL-terminated string into a fixed field, always terminating. */
static inline void mr_player_status_copy(char *dst, size_t size,
                                         const char *src) {
  size_t i = 0;
  if (!size)
    return;
  if (src)
    for (; i + 1 < size && src[i]; i++)
      dst[i] = src[i];
  dst[i] = 0;
}

/* Writer (mrplay). Publishes a new state; codec/text may be NULL. Safe to call
 * with block == NULL (control port allocation failed) so callers need no
 * guards of their own. */
static inline void mr_player_status_set(mr_player_control_port *block,
                                        LONG state, const char *codec,
                                        const char *text) {
  if (!block)
    return;
  Forbid();
  block->status.state = state;
  mr_player_status_copy(block->status.codec, sizeof(block->status.codec),
                        codec ? codec : "");
  mr_player_status_copy(block->status.text, sizeof(block->status.text),
                        text ? text : "");
  block->status.seq++;
  block->status.magic = MR_PLAYER_STATUS_MAGIC;
  Permit();
}

static inline int mr_player_control_set_volume(ULONG volume) {
  mr_player_control_port *block;
  int sent = 0;
  if (volume > 64)
    volume = 64;
  Forbid();
  block = (mr_player_control_port *)FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
  if (block && block->status.magic == MR_PLAYER_STATUS_MAGIC) {
    block->volume = volume;
    block->commands |= MR_PLAYER_COMMAND_VOLUME_SET;
    Signal(block->port.mp_SigTask, SIGBREAKF_CTRL_E);
    sent = 1;
  }
  Permit();
  return sent;
}

/* Reader (IPTV controller). Copies the currently published status into `out`.
 * Returns 1 if a valid player status exists, 0 if no player is running. */
static inline int mr_player_status_read(mr_player_status *out) {
  mr_player_control_port *block;
  int have = 0;
  Forbid();
  block = (mr_player_control_port *)FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
  if (block && block->status.magic == MR_PLAYER_STATUS_MAGIC) {
    *out = block->status;
    have = 1;
  }
  Permit();
  if (have) {
    out->codec[sizeof(out->codec) - 1] = 0;
    out->text[sizeof(out->text) - 1] = 0;
  }
  return have;
}

#endif /* MR_PLAYER_STATUS_H */
