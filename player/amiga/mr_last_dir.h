/*
 * MintVID - remembers the local-file browser's last-used drawer.
 *
 * Deliberately separate from mr_master_options.h's T: snapshot: that file
 * is a per-session controller->browser handoff (T: is typically RAM-based
 * and cleared on reboot), while this is a small user preference that should
 * survive a reboot, so it lives in ENVARC: (mirrored to ENV: at boot,
 * standard AmigaOS convention for small persistent tool settings). Same
 * crash-safe write-to-tmp-then-rename idiom as mr_master_options.h, using
 * only plain dos.library file I/O (Open/Read/Write/Close/Rename/DeleteFile)
 * already proven to build for AmigaOS in this codebase.
 */
#ifndef MR_LAST_DIR_H
#define MR_LAST_DIR_H

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>

#define MR_LAST_DIR_FILE "ENVARC:MintVID.lastdir"
#define MR_LAST_DIR_TMP  "ENVARC:MintVID.lastdir.tmp"

/* Checks whether `path` currently resolves - WITHOUT ever letting AmigaDOS
 * pop its own "Please insert volume X" system requester to find out.
 * SetProcWindow((APTR)-1) is documented, standard dos.library behaviour
 * (not a guess): it tells DOS to fail a Lock()/Open() on a recognised-but-
 * absent volume silently instead of prompting, for the duration this
 * process's pr_WindowPtr is set to that sentinel; the previous value is
 * captured first and restored immediately after, in case anything else
 * ever relies on it. This exists because a network share (the reported
 * case: "NAS1") is exactly the kind of volume DOS "recognises" (it has a
 * live DosList entry) but may currently be unreachable - Lock() on a path
 * naming it, done the ordinary way, is documented to trigger that same
 * system requester itself, not just ASL's own internal directory listing.
 * Real dos.library/exec.library calls this codebase already links
 * elsewhere - unverified against the real NDK on this dev host like
 * everything else in this file (see CLAUDE.md's "Validate against ffmpeg"
 * section). */
static inline int mr_last_dir_reachable(const char *path)
{
    struct Process *me;
    APTR old_window;
    BPTR lock;

    if (!path || !*path)
        return 0;
    me = (struct Process *)FindTask(NULL);
    old_window = me->pr_WindowPtr;
    SetProcWindow((APTR)-1);
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    SetProcWindow(old_window);
    if (!lock)
        return 0;
    UnLock(lock);
    return 1;
}

/* Reads the saved drawer path into out (out_size bytes, NUL-terminated,
 * trailing CR/LF stripped). Returns 1 with a non-empty out on success, 0
 * otherwise (no saved value, unreadable, empty, or no longer reachable -
 * see mr_last_dir_reachable()) - out is left untouched on failure so
 * callers can test the return value alone.
 *
 * A saved path that fails the reachability check is deliberately NOT
 * deleted from ENVARC: - a network share being intermittently offline is
 * normal and the remembered drawer is still worth keeping for when it is
 * back, so this just falls back to "no saved value" for the current call,
 * the same as if nothing had ever been saved (both call sites - browse()
 * in mrgui_gadtools.c, GETFILE_InitialDrawer/fr_Drawer seeding in
 * mrgui.c - already handle that case by opening the requester with no
 * initial drawer instead). */
static inline int mr_last_dir_load(char *out, size_t out_size)
{
    BPTR file;
    LONG got;
    size_t i;

    if (!out || out_size < 2)
        return 0;
    file = Open((CONST_STRPTR)MR_LAST_DIR_FILE, MODE_OLDFILE);
    if (!file)
        return 0;
    got = Read(file, out, (LONG)(out_size - 1));
    Close(file);
    if (got <= 0)
        return 0;
    out[got] = 0;
    for (i = (size_t)got; i > 0 &&
         (out[i - 1] == '\n' || out[i - 1] == '\r'); i--)
        out[i - 1] = 0;
    if (out[0] == 0)
        return 0;
    if (!mr_last_dir_reachable(out)) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

/* Saves drawer as the new last-used folder, only if it is non-empty and
 * actually differs from what is already saved - skips a disk write for the
 * common case of picking another file from the same drawer. */
static inline void mr_last_dir_save(const char *drawer)
{
    char current[256];
    BPTR file;
    LONG written, len;

    if (!drawer || !*drawer)
        return;
    if (mr_last_dir_load(current, sizeof(current)) && !strcmp(current, drawer))
        return;
    len = (LONG)strlen(drawer);
    file = Open((CONST_STRPTR)MR_LAST_DIR_TMP, MODE_NEWFILE);
    if (!file)
        return;
    written = Write(file, (APTR)drawer, len);
    Close(file);
    if (written != len) {
        DeleteFile((CONST_STRPTR)MR_LAST_DIR_TMP);
        return;
    }
    DeleteFile((CONST_STRPTR)MR_LAST_DIR_FILE);
    if (!Rename((CONST_STRPTR)MR_LAST_DIR_TMP, (CONST_STRPTR)MR_LAST_DIR_FILE))
        DeleteFile((CONST_STRPTR)MR_LAST_DIR_TMP);
}

/* Derives a drawer path from a full AmigaDOS file path (e.g.
 * "DH0:Videos/movie.avi" -> "DH0:Videos/", "DH0:movie.avi" -> "DH0:") by
 * keeping everything up to and including the last '/' or ':' separator,
 * whichever occurs later in the string. out is left empty for a bare
 * leafname with no path at all, or if out is too small to hold the result
 * safely. */
static inline void mr_last_dir_from_path(const char *path, char *out,
                                         size_t out_size)
{
    const char *slash, *colon, *sep;
    size_t len;

    if (out && out_size)
        out[0] = 0;
    if (!path || !*path || !out || out_size < 2)
        return;
    slash = strrchr(path, '/');
    colon = strrchr(path, ':');
    sep = slash;
    if (colon && (!sep || colon > sep))
        sep = colon;
    if (!sep)
        return;
    len = (size_t)(sep - path) + 1;
    if (len >= out_size)
        return;
    memcpy(out, path, len);
    out[len] = 0;
}

#endif /* MR_LAST_DIR_H */
