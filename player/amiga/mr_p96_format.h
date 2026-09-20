/* P96 PIP chroma layout preference, shared by MintVID's menu and mrplay.
 * A separate text preference avoids changing the binary mr_play_options
 * snapshot format and applies to Shell-launched mrplay as well as both GUIs.
 * Changes take effect when the next P96 PIP display is opened. */
#ifndef MR_P96_FORMAT_H
#define MR_P96_FORMAT_H

#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <string.h>

#define MR_P96_FORMAT_FILE "ENVARC:MintVID.p96format"
#define MR_P96_FORMAT_TMP  "ENVARC:MintVID.p96format.tmp"

/* Zero is the existing, WinUAE-tested Y0,V0,Y1,U0 order (YVYU).
 * Nonzero selects Y0,U0,Y1,V0 (YUYV) for the Voodoo tester. */
static int mr_p96_format_load(void)
{
    char text[4];
    BPTR file = Open((CONST_STRPTR)MR_P96_FORMAT_FILE, MODE_OLDFILE);
    LONG got;
    if (!file) return 0;
    got = Read(file, text, (LONG)sizeof text);
    Close(file);
    return got == (LONG)sizeof text && memcmp(text, "YUYV", 4) == 0;
}

static int mr_p96_format_save(int yuyv)
{
    BPTR file = Open((CONST_STRPTR)MR_P96_FORMAT_TMP, MODE_NEWFILE);
    const char *text = yuyv ? "YUYV\n" : "YVYU\n";
    LONG written;
    if (!file) return 0;
    written = Write(file, (APTR)text, 5);
    Close(file);
    if (written != 5) {
        DeleteFile((CONST_STRPTR)MR_P96_FORMAT_TMP);
        return 0;
    }
    /* Like mr_saved_options_save(): stage the new preference before rename. */
    DeleteFile((CONST_STRPTR)MR_P96_FORMAT_FILE);
    if (!Rename((CONST_STRPTR)MR_P96_FORMAT_TMP,
                (CONST_STRPTR)MR_P96_FORMAT_FILE)) {
        DeleteFile((CONST_STRPTR)MR_P96_FORMAT_TMP);
        return 0;
    }
    return 1;
}

#endif /* MR_P96_FORMAT_H */
