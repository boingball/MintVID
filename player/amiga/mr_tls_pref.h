/* HTTPS TLS version preference, shared by the MintVID menus, mrplay and the
 * IPTV/YouTube browsers. Stored as its own small text file, like
 * mr_p96_format.h, so it applies to a Shell-launched mrplay too and does not
 * change the binary mr_play_options snapshot. A missing or unreadable file
 * means TLS 1.2: its session resumption skips the key exchange that a TLS 1.3
 * resumption still does, which cost ~0.9 s of 68060 time per HLS segment.
 * mrplay and the browsers read it when they start. */
#ifndef MR_TLS_PREF_H
#define MR_TLS_PREF_H

#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <string.h>

#define MR_TLS_PREF_FILE "ENVARC:MintVID.tls"
#define MR_TLS_PREF_TMP  "ENVARC:MintVID.tls.tmp"

/* Nonzero: allow TLS 1.3. Zero (the default): cap at TLS 1.2. */
static inline int mr_tls_pref_load_allow13(void)
{
    char text[3];
    BPTR file = Open((CONST_STRPTR)MR_TLS_PREF_FILE, MODE_OLDFILE);
    LONG got;
    if (!file) return 0;
    got = Read(file, text, (LONG)sizeof text);
    Close(file);
    return got == (LONG)sizeof text && memcmp(text, "1.3", 3) == 0;
}

static inline int mr_tls_pref_save_allow13(int allow13)
{
    BPTR file = Open((CONST_STRPTR)MR_TLS_PREF_TMP, MODE_NEWFILE);
    const char *text = allow13 ? "1.3\n" : "1.2\n";
    LONG written;
    if (!file) return 0;
    written = Write(file, (APTR)text, 4);
    Close(file);
    if (written != 4) {
        DeleteFile((CONST_STRPTR)MR_TLS_PREF_TMP);
        return 0;
    }
    DeleteFile((CONST_STRPTR)MR_TLS_PREF_FILE);
    if (!Rename((CONST_STRPTR)MR_TLS_PREF_TMP,
                (CONST_STRPTR)MR_TLS_PREF_FILE)) {
        DeleteFile((CONST_STRPTR)MR_TLS_PREF_TMP);
        return 0;
    }
    return 1;
}

/* The mr_http_set_tls_max() value for the saved preference. A macro so this
 * header stays usable where core/mr_http.c is not linked (the controller
 * GUIs); callers that expand it include core/mr_http.h. */
#define MR_TLS_PREF_HTTP_MAX() \
    (mr_tls_pref_load_allow13() ? MR_HTTP_TLS_AUTO : MR_HTTP_TLS_12)

#endif /* MR_TLS_PREF_H */
