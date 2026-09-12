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
#include <exec/types.h>
#include <proto/dos.h>
#include <string.h>

#define MR_LAST_DIR_FILE "ENVARC:MintVID.lastdir"
#define MR_LAST_DIR_TMP  "ENVARC:MintVID.lastdir.tmp"

/* Reads the saved drawer path into out (out_size bytes, NUL-terminated,
 * trailing CR/LF stripped). Returns 1 with a non-empty out on success, 0
 * otherwise (no saved value, unreadable, or empty) - out is left untouched
 * on failure so callers can test the return value alone. */
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
    return out[0] != 0;
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
