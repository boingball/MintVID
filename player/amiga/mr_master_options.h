/*
 * MintVID controller -> browser playback-option snapshot (Amiga only).
 *
 * This deliberately uses a tiny T: file rather than a public MsgPort/shared
 * memory block.  mrgui writes only when one of its controls changes; ytgui and
 * iptvgui read once immediately before launching mrplay.  Consequently this
 * code cannot affect scheduling while video or audio is playing.
 */
#ifndef MR_MASTER_OPTIONS_H
#define MR_MASTER_OPTIONS_H

#include "../core/mr_play_options.h"

#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>

#define MR_MASTER_OPTIONS_MAGIC 0x4D524D4FUL /* 'MRMO' */
#define MR_MASTER_OPTIONS_FILE "T:MintVID.master-options"
#define MR_MASTER_OPTIONS_TMP  "T:MintVID.master-options.tmp"

typedef struct {
    ULONG magic;
    mr_play_options options;
    ULONG trailer_magic;
} mr_master_options_snapshot;

/* An owner token keeps the existing call sites simple.  It is process-local;
 * no port, task, signal or shared-memory object is created. */
typedef struct {
    int active;
} mr_master_options_port;

static inline mr_master_options_port *mr_master_options_open(void)
{
    static mr_master_options_port owner;
    owner.active = 1;
    return &owner;
}

static inline void mr_master_options_close(mr_master_options_port **owner)
{
    if (!owner || !*owner)
        return;
    DeleteFile((CONST_STRPTR)MR_MASTER_OPTIONS_TMP);
    DeleteFile((CONST_STRPTR)MR_MASTER_OPTIONS_FILE);
    (*owner)->active = 0;
    *owner = NULL;
}

static inline void mr_master_options_publish(mr_master_options_port *owner,
                                             const mr_play_options *options)
{
    mr_master_options_snapshot snapshot;
    BPTR file;
    LONG written;

    if (!owner || !owner->active || !options)
        return;
    snapshot.magic = MR_MASTER_OPTIONS_MAGIC;
    snapshot.options = *options;
    snapshot.trailer_magic = MR_MASTER_OPTIONS_MAGIC;

    file = Open((CONST_STRPTR)MR_MASTER_OPTIONS_TMP, MODE_NEWFILE);
    if (!file)
        return;
    written = Write(file, &snapshot, (LONG)sizeof(snapshot));
    Close(file);
    if (written != (LONG)sizeof(snapshot)) {
        DeleteFile((CONST_STRPTR)MR_MASTER_OPTIONS_TMP);
        return;
    }
    DeleteFile((CONST_STRPTR)MR_MASTER_OPTIONS_FILE);
    if (!Rename((CONST_STRPTR)MR_MASTER_OPTIONS_TMP,
                (CONST_STRPTR)MR_MASTER_OPTIONS_FILE))
        DeleteFile((CONST_STRPTR)MR_MASTER_OPTIONS_TMP);
}

static inline int mr_master_options_read(mr_play_options *options)
{
    mr_master_options_snapshot snapshot;
    BPTR file;
    LONG got;

    if (!options)
        return 0;
    file = Open((CONST_STRPTR)MR_MASTER_OPTIONS_FILE, MODE_OLDFILE);
    if (!file)
        return 0;
    got = Read(file, &snapshot, (LONG)sizeof(snapshot));
    Close(file);
    if (got != (LONG)sizeof(snapshot) ||
        snapshot.magic != MR_MASTER_OPTIONS_MAGIC ||
        snapshot.trailer_magic != MR_MASTER_OPTIONS_MAGIC)
        return 0;
    *options = snapshot.options;
    return 1;
}

/* Only mrgui-owned controls are merged.  Browser-local HLS quality and live
 * policy survive unchanged. */
static inline int mr_master_options_apply(mr_play_options *options)
{
    mr_play_options master;

    if (!options || !mr_master_options_read(&master))
        return 0;
    options->display = master.display;
    options->c2p = master.c2p;
    options->laced = master.laced;
    options->scale_2x = master.scale_2x;
    options->copper_vdouble = master.copper_vdouble;
    options->h264_performance = master.h264_performance;
    options->audio_rate = master.audio_rate;
    options->fast_buffer = master.fast_buffer;
    options->no_audio = master.no_audio;
    options->mono_audio = master.mono_audio;
    return 1;
}

#endif /* MR_MASTER_OPTIONS_H */
