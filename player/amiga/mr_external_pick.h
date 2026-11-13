/*
 * mr_external_pick.h - the IPTV/YouTube browsers' choice of player.
 *
 * With a PiStorm display (MR_DISPLAY_PISTORM*) and PROGDIR:mrberry present,
 * a stream goes to mrberry, which plays http(s), HLS and YouTube URLs on
 * the VideoCore with MintVID's network code. Otherwise the caller starts
 * mrplay with the arguments it already built (for PiStorm that is mrplay
 * --p96, the old behaviour).
 */
#ifndef MR_EXTERNAL_PICK_H
#define MR_EXTERNAL_PICK_H

#include <string.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "../core/mr_play_options.h"

/* Returns mrberry's seglist and rewrites args for it, or 0 with args
 * untouched. The new arguments are built aside first, so a failure never
 * leaves the caller's mrplay command line half overwritten. */
static BPTR mr_load_external_player(const mr_play_options *o, char *args,
                                    size_t cap, const char *url,
                                    const char *ua, const char *referer)
{
    static char ext[3200];
    BPTR seg;
    if (!o || !args || !mr_display_uses_external_player(o->display)) return 0;
    if (!mr_build_external_stream_arguments(ext, sizeof ext, o, url, ua, referer) ||
        strlen(ext) >= cap)
        return 0;
    seg = LoadSeg((CONST_STRPTR)"PROGDIR:mrberry");
    if (!seg) return 0;
    strcpy(args, ext);
    return seg;
}

#endif
