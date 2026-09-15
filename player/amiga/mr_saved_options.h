/*
 * MintVID - remembers the main controller's last-used playback settings
 * (display mode, C2P, H.264 speed, audio options, scale, video policy).
 *
 * Deliberately separate from mr_master_options.h's T: snapshot (a per-session
 * controller->browser handoff, cleared on reboot) and from mr_last_dir.h's
 * ENVARC:MintVID.lastdir (a single path string): this is the small,
 * structured preference set the controller's own gadgets represent, meant to
 * survive both a reboot and, unlike either of those, being read back across
 * a MintVID upgrade whose mr_play_options layout may have grown or changed
 * field order. A raw binary struct dump can't tell "old but compatible" from
 * "stale and dangerous to trust" on its own, so a magic value plus the
 * exact struct size the writer was built with are stored alongside it -
 * any mismatch on load is treated as "no saved settings" rather than risking
 * a partially-overlaid struct, the same fail-safe spirit as
 * mr_last_dir_load()'s "treat as no saved value" fallback for an
 * unreachable drawer. Same crash-safe write-to-tmp-then-rename idiom as
 * mr_last_dir.h/mr_master_options.h, using only plain dos.library file I/O.
 */
#ifndef MR_SAVED_OPTIONS_H
#define MR_SAVED_OPTIONS_H

#include "../core/mr_play_options.h"
#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <string.h>

#define MR_SAVED_OPTIONS_FILE "ENVARC:MintVID.settings"
#define MR_SAVED_OPTIONS_TMP  "ENVARC:MintVID.settings.tmp"
/* 'MVSO' (MintVID Saved Options) - just a sanity tag, not a format version;
 * the struct-size check alongside it is what actually protects against a
 * mismatched mr_play_options layout. */
#define MR_SAVED_OPTIONS_MAGIC 0x4D56534FUL

typedef struct {
    ULONG magic;
    ULONG size;
    mr_play_options options;
} mr_saved_options_blob;

/* Loads the last-saved controller options into *out. Returns 1 on success,
 * 0 otherwise (no saved file, short/corrupt read, or a magic/size mismatch
 * from a build whose mr_play_options layout differs) - callers should fall
 * back to their own hardware-detected defaults in every failure case, the
 * same way a fresh install would. */
static inline int mr_saved_options_load(mr_play_options *out)
{
    BPTR file;
    mr_saved_options_blob blob;
    LONG got;

    if (!out)
        return 0;
    file = Open((CONST_STRPTR)MR_SAVED_OPTIONS_FILE, MODE_OLDFILE);
    if (!file)
        return 0;
    got = Read(file, &blob, (LONG)sizeof(blob));
    Close(file);
    if (got != (LONG)sizeof(blob))
        return 0;
    if (blob.magic != MR_SAVED_OPTIONS_MAGIC ||
        blob.size != (ULONG)sizeof(mr_play_options))
        return 0;
    *out = blob.options;
    return 1;
}

/* Saves options as the new last-used controller settings - called on every
 * change, so this is meant to be cheap; skips the write entirely when
 * nothing actually changed from what's already on disk. */
static inline void mr_saved_options_save(const mr_play_options *options)
{
    mr_saved_options_blob blob;
    mr_play_options current;
    BPTR file;
    LONG written;

    if (!options)
        return;
    if (mr_saved_options_load(&current) &&
        memcmp(&current, options, sizeof(current)) == 0)
        return;
    blob.magic = MR_SAVED_OPTIONS_MAGIC;
    blob.size = (ULONG)sizeof(mr_play_options);
    blob.options = *options;
    file = Open((CONST_STRPTR)MR_SAVED_OPTIONS_TMP, MODE_NEWFILE);
    if (!file)
        return;
    written = Write(file, &blob, (LONG)sizeof(blob));
    Close(file);
    if (written != (LONG)sizeof(blob)) {
        DeleteFile((CONST_STRPTR)MR_SAVED_OPTIONS_TMP);
        return;
    }
    DeleteFile((CONST_STRPTR)MR_SAVED_OPTIONS_FILE);
    if (!Rename((CONST_STRPTR)MR_SAVED_OPTIONS_TMP,
                (CONST_STRPTR)MR_SAVED_OPTIONS_FILE))
        DeleteFile((CONST_STRPTR)MR_SAVED_OPTIONS_TMP);
}

#endif /* MR_SAVED_OPTIONS_H */
