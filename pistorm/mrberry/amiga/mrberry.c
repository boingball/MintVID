/* MintVID demux -> berryvp.library hardware A/V bridge.
 * Stage 4F HUNKSAFE: the hardware-confirmed Stage-3D H.264 path is unchanged. AAC-LC packets are
 * demuxed by MintVID, wrapped as ADTS where required, decoded by BerryFAAD03
 * on ARM CPU1 and sent as PCM16 to MMAL audio_render/HDMI. */
#include "../core/mr_demux.h"
#include "../core/mr_mkv_tracks.h"
#include "mr_berryvp.h"
#include "mrb_paula.h"
#include "mrb_sub.h"
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MR_DEBUG_LOGS
#define MR_DEBUG_LOGS 0
#endif

#if MR_DEBUG_LOGS
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

extern ULONG bvp_exec_setsignal(ULONG newSignals, ULONG signalSet);
extern void mrb_source_set_read_kb(unsigned kb);   /* core/mr_source.c, W77 */
extern int mr_berryvp_force_isp;                    /* mr_berryvp.c, ISP82 */
extern unsigned long mr_berryvp_isp_out;            /* mr_berryvp.c, N84 */
extern void *mr_berryvp_osd_req;                    /* mr_berryvp.c, O1 */

/* ---- O1: OSD feasibility test ------------------------------------------
 * OSDTEST (or OSDTEST=MIX) builds one RGBA picture, 512x96: transparent
 * background, a half transparent dark box, "OSD TEST" in opaque white, an
 * opaque red and a half transparent green square.  berryvp.library shows it
 * on a second video_render layer above the film (cinema only), centred near
 * the bottom.  It shows whether a layer over the film works, whether the
 * per-pixel alpha is used, and whether the film stays smooth. */
/* O3: OSDTEST=COUNT uses a 1280x128 picture (subtitle size) and sends a
 * new one every second with a counter - the worst case for subtitles. */
static int o1_w = 512, o1_h = 96;
static uint32_t o1_block[8];
/* O2: key O shows/hides the OSD while the film plays (cinema).
 * OSDTEST=MOVE hides it by moving it below the screen instead of alpha 0. */
static int o2_key_osd;
static int o2_osd_shown = 1;
static int o3_count_mode;
extern ULONG ac309i_read_sys_us(void);
/* S1: SRT subtitles (mrb_sub.c) on the OSD layer, cinema only. */
static int s1_on;                     /* .srt loaded, OSD request set */
static int s1_user_off;               /* key O: subtitles off */
static int s1_nosub;                  /* NOSUB */
static const char *s1_name_arg;       /* SUB=file */
static volatile unsigned long *s1_blk;/* library clock block (BVP_CTRL_OSDCLOCK) */
static int s1_have_resume;
static uint32_t s1_resume_sys;
static long s1_offset_ms;             /* '+'/'-' (HDMI sound): + = later */
static unsigned long s1_shows, s1_hides;
/* v96.83: key S in full screen = next subtitle file (Off, 1, 2, ...) */
static int s3_key;
static int s3_cur;                    /* 0 = Off, n = file n of s2_* */
static const char *s3_film;
static uint32_t s3_banner_until;
static int s3_banner_on;
static void s1_after_pause(void)
{
    s1_resume_sys = (uint32_t)ac309i_read_sys_us();
    s1_have_resume = 1;
}
static unsigned long o3_counter;
static ULONG o3_last_us;
static uint8_t *o1_pixels;
static const uint8_t o1_font[16][7] = {         /* 5x7, bit 4 = left */
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},       /* O */
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},       /* S */
    {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},       /* D */
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},       /* T */
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},       /* E */
    {0,0,0,0,0,0,0},                            /* space */
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},       /* 0 */
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},       /* 1 */
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},       /* 2 */
    {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},       /* 3 */
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},       /* 4 */
    {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},       /* 5 */
    {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},       /* 6 */
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},       /* 7 */
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},       /* 8 */
    {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}        /* 9 */
};
static void o1_px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *p;
    if (x < 0 || y < 0 || x >= o1_w || y >= o1_h) return;
    p = o1_pixels + ((size_t)y * (size_t)o1_w + (size_t)x) * 4u;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
static void o1_rect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++) o1_px(x, y, r, g, b, a);
}
static int o1_glyph(char c)
{
    switch (c) {
    case 'O': return 0; case 'S': return 1; case 'D': return 2;
    case 'T': return 3; case 'E': return 4;
    default:
        if (c >= '0' && c <= '9') return 6 + (c - '0');
        return 5;
    }
}
/* Whole picture: transparent, dark box, text centred, red + green square. */
static void o1_draw(const char *text, int scale)
{
    int i, row, col, x0, y0, n = (int)strlen(text), adv = 6 * scale;
    memset(o1_pixels, 0, (size_t)o1_w * (size_t)o1_h * 4u);
    o1_rect(8, 8, o1_w - 16, o1_h - 16, 16, 16, 48, 160);      /* dark box, alpha 160 */
    o1_rect(20, o1_h / 2 - 16, 32, 32, 255, 0, 0, 255);        /* red, opaque */
    o1_rect(o1_w - 52, o1_h / 2 - 16, 32, 32, 0, 255, 0, 128); /* green, half */
    x0 = (o1_w - n * adv + scale) / 2;
    y0 = (o1_h - 7 * scale) / 2;
    for (i = 0; i < n; i++) {
        int g = o1_glyph(text[i]);
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (o1_font[g][row] & (0x10 >> col))
                    o1_rect(x0 + i * adv + col * scale, y0 + row * scale,
                            scale, scale, 255, 255, 255, 255);
    }
}
static int o1_build(int mix, int move, int count)
{
    if (count) { o1_w = 1280; o1_h = 128; o3_count_mode = 1; }
    o1_pixels = (uint8_t *)malloc((size_t)o1_w * (size_t)o1_h * 4u);
    if (!o1_pixels) return 0;
    if (count) o1_draw("OSD 0000", 12);
    else o1_draw("OSD TEST", 8);
    o1_block[0] = 0x4F534432UL;                 /* 'OSD2' (O2) */
    o1_block[1] = (uint32_t)o1_w;
    o1_block[2] = (uint32_t)o1_h;
    o1_block[3] = mix ? 1u : 0u;
    o1_block[4] = (uint32_t)(unsigned long)o1_pixels;
    o1_block[5] = 0xFFFFFFFFUL;                 /* centred */
    o1_block[6] = 0xFFFFFFFFUL;                 /* near the bottom */
    o1_block[7] = move ? 1u : 0u;               /* O2: hide by moving away */
    mr_berryvp_osd_req = o1_block;
    return 1;
}
extern ULONG ac309i_read_sys_us(void);

/* v97.1: enough stack also from the Shell.  MintVID-pi starts mrberry with
 * 320000 bytes; from the Shell mrberry only got the Shell's stack (often
 * 4096 bytes) and crashed on MKV/MP4 (Coachman, EAB 07.11.2026).  v97.0 used
 * libnix's stack swap (__stack); that crashed when mrberry ended (Guru
 * 8000 0004 / 8700000E, Michael 08.11.2026), so it is gone again.  Now mrberry
 * does what "Stack 320000" + "mrberry ..." does: when its own stack is too
 * small it loads itself once more and runs that copy with dos.library's
 * RunCommand() and the full stack, then ends with the copy's return code. */
#define V971_STACK      320000UL
#define V971_STACK_MIN  300000UL
static int v971_relaunch(long *rc)
{
    struct Task *me = FindTask(NULL);
    ULONG have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    char prog[108], path[160];
    STRPTR args;
    BPTR seg;
    LONG r;
    if (have >= V971_STACK_MIN || !Cli()) return 0;
    if (!GetProgramName((STRPTR)prog, sizeof prog)) return 0;
    strcpy(path, "PROGDIR:");
    strncat(path, (const char *)FilePart((STRPTR)prog), sizeof path - 9);
    seg = LoadSeg((STRPTR)path);
    if (!seg) return 0;
    args = GetArgStr();
    if (!args) args = (STRPTR)"\n";
    r = RunCommand(seg, V971_STACK, args, (LONG)strlen((const char *)args));
    UnLoadSeg(seg);
    if (r == -1) return 0;      /* could not start: go on with the small stack */
    *rc = r;
    return 1;
}

/* D4: keep the ~4 KiB BerryVP bridge context out of the Amiga CLI stack.
 * A normal Shell stack can be only 4096 bytes; the previous automatic
 * mr_berryvp made main reserve 4168 bytes before any useful work. */
static mr_berryvp g_bvp;


/* MintVID PiStorm04: keep compressed startup audio host-owned until the first
 * real H.264 access unit has successfully crossed BerryVP. Earlier standalone
 * VPr/VPv second-start paths always had video/VCHIQ traffic before the first
 * PCM BULK; N79 library mode can stall on a persistent second start when PCM
 * is the first real BULK. This queue changes only startup ordering. */
typedef struct p04_audio_node {
    struct p04_audio_node *next;
    uint8_t *data;
    uint32_t len;
    int has_pts;
    uint64_t pts_us;
} p04_audio_node;

static p04_audio_node *p04_ahead, *p04_atail;
static unsigned long p04_aqueued;
static unsigned long p04_abytes;

static int p04_queue_audio(const mr_packet *pkt)
{
    p04_audio_node *n;
    if (!pkt || !pkt->data || !pkt->len) return 0;
    n = (p04_audio_node *)malloc(sizeof *n);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    n->data = (uint8_t *)malloc(pkt->len);
    if (!n->data) { free(n); return 0; }
    memcpy(n->data, pkt->data, pkt->len);
    n->len = pkt->len;
    n->has_pts = pkt->has_pts;
    n->pts_us = pkt->pts_us;
    if (p04_atail) p04_atail->next = n; else p04_ahead = n;
    p04_atail = n;
    p04_aqueued++;
    p04_abytes += pkt->len;
    return 1;
}

static void p04_free_audio_queue(void)
{
    while (p04_ahead) {
        p04_audio_node *n = p04_ahead;
        p04_ahead = n->next;
        free(n->data);
        free(n);
    }
    p04_atail = NULL;
    p04_aqueued = 0;
    p04_abytes = 0;
}

static mr_status p04_flush_audio_queue(mr_berryvp *bvp,
                                       unsigned long *audio_sent)
{
    while (p04_ahead) {
        p04_audio_node *n = p04_ahead;
        mr_packet p;
        mr_status st;
        memset(&p, 0, sizeof p);
        p.is_video = 0;
        p.has_pts = n->has_pts;
        p.pts_us = n->pts_us;
        p.data = n->data;
        p.len = n->len;
        st = mr_berryvp_send_audio_packet(bvp, &p);
        if (st != MR_OK) return st;
        if (audio_sent) (*audio_sent)++;
        p04_ahead = n->next;
        if (!p04_ahead) p04_atail = NULL;
        p04_abytes -= n->len;
        free(n->data);
        free(n);
        p04_aqueued--;
    }
    return MR_OK;
}

/* MintVID PiStorm05: N79Q's historical ESC handler belongs to the standalone
 * Intuition window. Library mode deliberately skips that window, so mrberry
 * owns a tiny activated Intuition input window while HVS fullscreen is active.
 * The window itself is hidden under the VideoCore layer; only RAWKEY/vanilla
 * input is used. */
static struct Window *p05_input_window;

static int p05_input_open(void)
{
    p05_input_window = OpenWindowTags(NULL,
        WA_Left, 0,
        WA_Top, 0,
        WA_Width, 1,
        WA_Height, 1,
        WA_Borderless, TRUE,
        WA_Activate, TRUE,
        WA_RMBTrap, TRUE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY,
        TAG_DONE);
    return p05_input_window != NULL;
}

static FILE *n76_hostlog;
static void n76_hlog(const char *s);

/* P96W38: the library's P96 video window takes the keyboard focus when it
 * opens or is clicked.  The library then reports Esc / 0 / close gadget in
 * its sync block (libbase+120, 'P96S', long 18 = byte offset 72). */
static int v9682_full;                 /* v96.82: F in the video window */
/* v96.85: Tab / Shift-Tab = next / previous chapter (MKV), without
 * chapters +/- 10 minutes.  mrberry stops with the target spot in the .t
 * and status ENDED "Jump"; MintVID-pi starts the film again there. */
static int s4_dir;                     /* request: +1 / -1 */
static int s4_check(void);
static int s4_jump;                    /* stopping for a jump */
static unsigned long s4_win_fwd, s4_win_back;
/* v96.87: M = mute on/off (window: count at P96S long 23, +92) */
static int m1_key;
static int m1_muted;
static unsigned long m1_win;
static int m1_win_seen;
static int p58_switch;
static int p96w38_window_stop(void)
{
    unsigned long v;
    if (!g_bvp.lib) return 0;
    v = *(volatile unsigned long *)((volatile unsigned char *)g_bvp.lib + 120);
    if (!v || ((volatile unsigned long *)v)[0] != 0x50393653UL) return 0;   /* 'P96S' */
    /* v96.82: F (long 20, +80) = stop here and let MintVID-pi go on in
     * full screen at this spot (same as its Fullscreen button, Ctrl-F) */
    if (mr_berryvp_stream_flags && ((volatile unsigned long *)v)[20] != 0) {
        if (!v9682_full) { v9682_full = 1; p58_switch = 1; n76_hlog("V9682_WINDOW_KEY_F"); }
        return 1;
    }
    if (mr_berryvp_stream_flags) {             /* v96.85: Tab in the window */
        unsigned long f = ((volatile unsigned long *)v)[21];
        unsigned long b = ((volatile unsigned long *)v)[22];
        if (f != s4_win_fwd) { s4_win_fwd = f; s4_dir = 1; }
        if (b != s4_win_back) { s4_win_back = b; s4_dir = -1; }
        {
            unsigned long mm = ((volatile unsigned long *)v)[23];
            if (!m1_win_seen) { m1_win_seen = 1; m1_win = mm; }
            if (mm != m1_win) { m1_win = mm; m1_key = 1; }
        }
    }
    return ((volatile unsigned long *)v)[18] != 0;
}

/* P96W44: libnix read()/write() call __chkabort(), which ends the program
 * on Ctrl-C without closing the BerryVP stream (window stays, Pi stuck).
 * mrberry checks Ctrl-C itself and always stops through CloseStream. */
void __chkabort(void)
{
}

/* P96W45: pause.  Space in mrberry's 1x1 input window (cinema) is noted
 * here by p05_input_poll_stop. */
static int p45_key_space;

/* P96W70: Paula sound offset = fixed base (-500 ms) + fine value per film.
 * The fine value comes from AV= (Shell), else from the film's .t (P:ms),
 * else 0; '+'/'-' change it by 50 ms while the film plays; it is written
 * to the .t as P:ms. */
static long p70_key_delta;
static long p70_fine_ms;
static int p70_fine_changed;
static int p05_input_poll_stop(void)
{
    struct IntuiMessage *msg;
    int stop = 0;

    if (p96w38_window_stop()) {
        static int p96w38_logged;
        if (!p96w38_logged) { p96w38_logged = 1; n76_hlog("P96W38_WINDOW_STOP"); }
        return 1;
    }
    if (s4_dir && s4_check()) return 1;        /* v96.85: Tab in the window */
    if (!p05_input_window || !p05_input_window->UserPort)
        return 0;

    while ((msg = (struct IntuiMessage *)GetMsg(p05_input_window->UserPort)) != NULL) {
        ULONG cls = msg->Class;
        UWORD code = msg->Code;
        UWORD qual = msg->Qualifier;
        ReplyMsg((struct Message *)msg);

        if (cls == IDCMP_RAWKEY) {
            /* Amiga raw key 0x45 = ESC. Ignore key-up (bit 7 set). */
            if ((code & 0x80u) == 0 && ((code & 0x7fu) == 0x45u))
                stop = 1;
            /* v96.86: raw 0x42 = Tab; Shift-Tab arrives only as RAWKEY
             * (the keymap gives no character for it) */
            if (code == 0x42u && !(qual & IEQUALIFIER_REPEAT))
                s4_dir = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ? -1 : 1;
            /* P96W45: raw 0x40 = Space (key down, no auto-repeat) */
            if (code == 0x40u && !(qual & IEQUALIFIER_REPEAT))
                p45_key_space = 1;
        } else if (cls == IDCMP_VANILLAKEY) {
            if (code == 0x1bu || code == '0')
                stop = 1;
            if (code == ' ' && !(qual & IEQUALIFIER_REPEAT))
                p45_key_space = 1;
            /* P96W70: Paula fine offset, 50 ms per press */
            if (code == '+') p70_key_delta += 50;
            if (code == '-') p70_key_delta -= 50;
            /* O2: O = OSD on/off */
            if ((code == 'o' || code == 'O') && !(qual & IEQUALIFIER_REPEAT))
                o2_key_osd = 1;
            /* v96.85: Tab / Shift-Tab = chapter / 10 min jump */
            if (code == 0x09 && !(qual & IEQUALIFIER_REPEAT))
                s4_dir = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ? -1 : 1;
            /* v96.83: S = next subtitle file */
            if ((code == 's' || code == 'S') && !(qual & IEQUALIFIER_REPEAT))
                s3_key = 1;
            /* v96.87: M = mute on/off */
            if ((code == 'm' || code == 'M') && !(qual & IEQUALIFIER_REPEAT))
                m1_key = 1;
        }
    }
    if (!stop && s4_dir && s4_check()) stop = 1;   /* v96.85: Tab */
    return stop;
}

static void p05_input_close(void)
{
    if (p05_input_window) {
        CloseWindow(p05_input_window);
        p05_input_window = NULL;
    }
}

/* MintVID PiStorm06: publish the same atomic terminal status snapshot used by
 * mrplay. MintVID launches mrberry asynchronously, so this T: record is the
 * reliable hand-off for Opening/Playing/Stopped/Finished after mrberry exits. */
#define P06_STATUS_MAGIC 0x4D525653UL
#define P06_STATUS_FILE  "T:MintVID.player-status"
#define P06_STATUS_TMP   "T:MintVID.player-status.tmp"
#define P06_CODEC_MAX 48
#define P06_TEXT_MAX 208
enum {
    P06_STATE_STARTING = 0,
    P06_STATE_OPENING,
    P06_STATE_PLAYING,
    P06_STATE_UNSUPPORTED,
    P06_STATE_ERROR,
    P06_STATE_ENDED
};
typedef struct {
    volatile ULONG magic;
    volatile ULONG seq;
    volatile LONG state;
    char codec[P06_CODEC_MAX];
    char text[P06_TEXT_MAX];
} p06_player_status;
typedef struct {
    p06_player_status status;
    ULONG trailer_magic;
} p06_player_status_snapshot;
static ULONG p06_status_seq;

/* P96W77: the live control port MintVID's own GUI looks for while a player
 * runs (docs/EXTERNAL_PLAYER.md in Darren's MintVID, layout of
 * mr_player_control_port in player/amiga/mr_player_status.h): MsgPort first,
 * then the command bits (Ctrl-E), the volume and the live status. */
#define P77_PORT_NAME "MintVID.player"
#define P77_CMD_PAUSE      (1UL << 0)
#define P77_CMD_FAST       (1UL << 1)
#define P77_CMD_FULLSCREEN (1UL << 2)
typedef struct {
    struct MsgPort port;
    volatile ULONG commands;
    volatile ULONG volume;
    p06_player_status status;
} p77_control_port;
static p77_control_port *p77_port;

static void p77_port_close(void)
{
    if (p77_port) {
        Forbid();
        RemPort(&p77_port->port);
        Permit();
        FreeMem(p77_port, sizeof *p77_port);
        p77_port = NULL;
    }
}

static void p77_port_open(void)
{
    p77_control_port *b;
    Forbid();
    if (FindPort((CONST_STRPTR)P77_PORT_NAME)) { Permit(); return; }
    Permit();
    b = (p77_control_port *)AllocMem(sizeof *b, MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) return;
    b->port.mp_Node.ln_Type = NT_MSGPORT;
    b->port.mp_Node.ln_Name = (char *)P77_PORT_NAME;
    b->port.mp_Flags = PA_IGNORE;
    b->port.mp_SigTask = FindTask(NULL);
    b->port.mp_MsgList.lh_Head = (struct Node *)&b->port.mp_MsgList.lh_Tail;
    b->port.mp_MsgList.lh_Tail = NULL;
    b->port.mp_MsgList.lh_TailPred = (struct Node *)&b->port.mp_MsgList.lh_Head;
    b->volume = 64;
    b->status.state = 0;
    b->status.magic = 0x4D525653UL;
    AddPort(&b->port);
    p77_port = b;
    atexit(p77_port_close);
}

/* Ctrl-E: take and clear the command bits. */
static ULONG p77_take_commands(void)
{
    ULONG c = 0;
    if (!p77_port) return 0;
    Forbid();
    c = p77_port->commands;
    p77_port->commands = 0;
    Permit();
    return c;
}

static void p06_status_copy(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    if (!size) return;
    if (src)
        for (; i + 1 < size && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

static void p06_status_write(LONG state, const char *codec, const char *text)
{
    p06_player_status_snapshot snapshot;
    BPTR file;
    LONG written;

    memset(&snapshot, 0, sizeof snapshot);
    snapshot.status.magic = P06_STATUS_MAGIC;
    snapshot.status.seq = ++p06_status_seq;
    snapshot.status.state = state;
    p06_status_copy(snapshot.status.codec, sizeof snapshot.status.codec,
                    codec ? codec : "");
    p06_status_copy(snapshot.status.text, sizeof snapshot.status.text,
                    text ? text : "");
    snapshot.trailer_magic = P06_STATUS_MAGIC;

    if (p77_port) {                       /* P96W77: live status */
        Forbid();
        p77_port->status.state = state;
        p06_status_copy(p77_port->status.codec, sizeof p77_port->status.codec,
                        codec ? codec : "");
        p06_status_copy(p77_port->status.text, sizeof p77_port->status.text,
                        text ? text : "");
        p77_port->status.seq = snapshot.status.seq;   /* same counter as T: */
        p77_port->status.magic = P06_STATUS_MAGIC;
        Permit();
    }

    file = Open((CONST_STRPTR)P06_STATUS_TMP, MODE_NEWFILE);
    if (!file) return;
    written = Write(file, &snapshot, (LONG)sizeof snapshot);
    Close(file);
    if (written == (LONG)sizeof snapshot) {
        DeleteFile((CONST_STRPTR)P06_STATUS_FILE);
        if (!Rename((CONST_STRPTR)P06_STATUS_TMP,
                    (CONST_STRPTR)P06_STATUS_FILE))
            DeleteFile((CONST_STRPTR)P06_STATUS_TMP);
    } else {
        DeleteFile((CONST_STRPTR)P06_STATUS_TMP);
    }
}

static void n76_hlog(const char *s)
{
    if (n76_hostlog) { fputs(s, n76_hostlog); fputc('\n', n76_hostlog); fflush(n76_hostlog); }
}

typedef struct n73_video_node {
    struct n73_video_node *next;
    uint8_t *data;
    uint32_t len;
    int has_pts;
    uint64_t pts_us;
    int is_annexb;
    int nonref;   /* P96W3: every VCL NAL has nal_ref_idc == 0 */
} n73_video_node;

/* P96W3: 1 when this access unit is safe to drop (contains slices, and no
 * slice is used as a reference).  Unknown/odd packets count as reference. */
static int p96w_au_is_nonref(const uint8_t *d, uint32_t n, int annexb,
                             unsigned ls)
{
    uint32_t i = 0;
    int vcl = 0;
    if (!d || !n) return 0;
    if (annexb) {
        while (i + 3 < n) {
            uint32_t sc;
            if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) sc = 3;
            else if (i + 4 < n && d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) sc = 4;
            else { i++; continue; }
            i += sc;
            if (i >= n) break;
            {
                unsigned t = d[i] & 31u;
                if (t >= 1 && t <= 5) {
                    vcl = 1;
                    if (t == 5 || (d[i] & 0x60u)) return 0;
                }
            }
        }
        return vcl;
    }
    if (ls < 1 || ls > 4) return 0;
    while (i + ls < n) {
        uint32_t len = 0, k;
        for (k = 0; k < ls; k++) len = (len << 8) | d[i + k];
        i += ls;
        if (!len || len > n - i) return 0;
        {
            unsigned t = d[i] & 31u;
            if (t >= 1 && t <= 5) {
                vcl = 1;
                if (t == 5 || (d[i] & 0x60u)) return 0;
            }
        }
        i += len;
    }
    return vcl;
}
static unsigned long p96w_dropped;
/* P96W5: keep audio ahead of the window video.
 * prefill: before the first AU is sent, read until ~0.8 s of audio is known.
 * audio-first: a due AU waits while the audio read position is less than
 * 0.4 s ahead of the playback clock, so the demux reads more audio first. */
static int p96w_have_vpts, p96w_have_apts, p96w_prefill_done, p96w_drain;
static uint64_t p96w_first_vpts, p96w_last_apts;
static unsigned long p96w_audio_first_holds;
#define P96W_PREFILL_US   800000ULL
#define P96W_AUDIO_LEAD_US 400000ULL
#define P96W_HOLD_MAX_Q   60u   /* P96W27: +0.45 s of frames for the AV delay */
#define P96W_READ_AHEAD_Q 72u   /* P96W27: was 48; stop demux only above this */
#define P96W_DROP_LATE_US 83000u   /* drop only when ~2 frames late */
/* P96W26: the Pi plays sound ~0.45 s after it is handed over; the cinema
 * scheduler knows that, the host-paced window does not, so window frames are
 * ~0.4-0.5 s early (phone-measured with SyncTest2 at W:25/50/75).
 * P96W30: delaying the host pacer is NOT a reliable fix - the effective shift
 * was 1x (SyncTest2 W:50), 2x (SyncTest2 W:75, plus a hang) and ~2.7x
 * (trailerDE W:25: 0.45 s -> 1.2 s), and Michael found trailerDE W:50 better
 * without it.  Default 0; AV:<ms> stays as an experiment switch.  A real fix
 * must delay presentation in the library, not the feed. */
#define P96W_AV_DELAY_DEFAULT_US 0
/* P96W37: AV:<ms> is handed to berryvp.library (signed, + = picture later);
 * the library holds finished frames until the Pi audio clock is there.
 * mrberry's own feed timing is never moved any more (W32-W36 did, and that
 * made the old VideoCore stall much more frequent). */
static long p96w_av_delay_us = P96W_AV_DELAY_DEFAULT_US;

/* P96W32: window A/V sync on the Pi's audio clock.
 * The library publishes at libbase+120 a small block (see berryvp.library.s,
 * p96w_sync): the Pi audio-clock media time with the 1-MHz system time it was
 * sampled at, and the PTS + system time of the frame last drawn into the
 * window.  We compare both on the same system timer and move our own frame
 * grid (pace->base_us) until the shown frame matches the sound.  One large
 * step at the start, then small corrections.  AV:<ms> = picture this much
 * later than the sound clock; NOSYNC = old behaviour (host time only). */
static volatile unsigned long *p96s;
static int p96s_off;            /* NOSYNC: no clock queries, no shift */
static int p96s_measure = 1;    /* P96W37: mrberry only measures; the library holds frames */
static unsigned long p96s_last_seq, p96s_samples, p96s_used, p96s_corrections;
static long p96s_first_err, p96s_last_err, p96s_min_err, p96s_max_err, p96s_shift;
static long p96s_init_err[3];
static long p96s_hist[3];
static unsigned p96s_hist_n;
static int p96s_init_n, p96s_init_done;
static uint32_t p96s_settle_until;
#define P96S_LOG_MAX 48
static long p96s_log_t[P96S_LOG_MAX], p96s_log_e[P96S_LOG_MAX];
static unsigned p96s_log_n;

static n73_video_node *n73_vhead, *n73_vtail;
static unsigned long n73_vqueued;

static int n73_queue_video(const mr_packet *pkt)
{
    n73_video_node *n;
    if (!pkt || !pkt->data || !pkt->len) return 0;
    n = (n73_video_node *)malloc(sizeof *n);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    n->data = (uint8_t *)malloc(pkt->len);
    if (!n->data) { free(n); return 0; }
    memcpy(n->data, pkt->data, pkt->len);
    n->len = pkt->len;
    n->has_pts = pkt->has_pts;
    n->pts_us = pkt->pts_us;
    n->is_annexb = pkt->is_annexb;
    if (mr_berryvp_stream_flags && !p96w_have_vpts && pkt->has_pts) {
        p96w_first_vpts = pkt->pts_us;
        p96w_have_vpts = 1;
    }
    n->nonref = mr_berryvp_stream_flags ?
        p96w_au_is_nonref(pkt->data, pkt->len, pkt->is_annexb,
                          g_bvp.avc.nal_length_size) : 0;
    if (n73_vtail) n73_vtail->next = n; else n73_vhead = n;
    n73_vtail = n;
    n73_vqueued++;
    return 1;
}

static void n73_free_video_queue(void)
{
    while (n73_vhead) {
        n73_video_node *n = n73_vhead;
        n73_vhead = n->next;
        free(n->data);
        free(n);
    }
    n73_vtail = NULL;
    n73_vqueued = 0;
}

static mr_status n73_flush_video_queue(mr_berryvp *bvp,
                                       unsigned long *sent,
                                       unsigned long *skipped)
{
    while (n73_vhead) {
        n73_video_node *n = n73_vhead;
        mr_packet p;
        mr_status st;
        memset(&p, 0, sizeof p);
        p.is_video = 1;
        p.has_pts = n->has_pts;
        p.pts_us = n->pts_us;
        p.data = n->data;
        p.len = n->len;
        p.is_annexb = n->is_annexb;
        st = mr_berryvp_send_packet(bvp, &p);
        if (st == MR_EAGAIN) { if (skipped) (*skipped)++; }
        else if (st != MR_OK) return st;
        else if (sent) (*sent)++;
        n73_vhead = n->next;
        if (!n73_vhead) n73_vtail = NULL;
        free(n->data);
        free(n);
        n73_vqueued--;
    }
    return MR_OK;
}


/* AC309I: port of the hardware-confirmed VPv149 MP4 host-time feeder pacer.
 *
 * Presentation still belongs to BerryVP/video_scheduler and every original
 * MP4 stts/ctts PTS is passed through unchanged.  This clock has a different
 * job: INPUT BACKPRESSURE.  A fresh MOV/MP4 H.264 access unit may enter the
 * decoder inside a bounded 250-ms window before its absolute decode-order
 * frame deadline on the Pi 1-MHz system timer.  First AU is immediate; later deadlines stay on
 * an absolute rational grid derived from container rate/scale, so late work
 * never moves the clock.  This is the VPv102/VPv149 principle which removed
 * the historical animel.mp4 freeze.
 *
 * Unlike a blocking per-frame wait, not-due video stays in the host-owned AU
 * queue while the demux loop continues to deliver intervening audio packets.
 * Thus Audio/VCHIQ service remains alive while Video waits for its deadline.
 */
#define AC309I_FEED_LEAD_US 250000UL

typedef struct ac309i_pacer {
    int enabled;
    int started;
    uint32_t base_us;
    uint32_t target_us;
    uint32_t step_whole_us;
    uint32_t step_rem;
    uint32_t step_den;
    uint32_t accum;
    uint32_t lead_us;
    unsigned long sent_aus;
    unsigned long late_count;
    uint32_t max_late_us;
    unsigned long queue_peak;
} ac309i_pacer;

static uint32_t ac309i_next_interval(ac309i_pacer *p)
{
    uint32_t d = p->step_whole_us;
    if (p->step_rem && p->step_den) {
        uint32_t a = p->accum + p->step_rem;
        if (a >= p->step_den) { a -= p->step_den; d++; }
        p->accum = a;
    }
    return d ? d : 1u;
}

static void ac309i_pacer_init(ac309i_pacer *p, const mr_video_info *vi, int enable)
{
    uint64_t num;
    memset(p, 0, sizeof *p);
    if (!enable || !vi || !vi->rate || !vi->scale) return;
    num = 1000000ULL * (uint64_t)vi->scale;
    p->step_whole_us = (uint32_t)(num / (uint64_t)vi->rate);
    p->step_rem      = (uint32_t)(num % (uint64_t)vi->rate);
    p->step_den      = vi->rate;
    p->lead_us       = AC309I_FEED_LEAD_US;
    if (!p->step_whole_us && !p->step_rem) return;
    p->enabled = 1;
}

/* P96W58: Ctrl-F = stop for the MintVID-pi "Fullscreen" button.  Same as a
 * stop (Ctrl-C), but the .t position is written without the 30-s recap, so
 * the cinema restart continues right where the window film was. */
static int p58_stop_sig(ULONG sig)
{
    if (sig & SIGBREAKF_CTRL_F) p58_switch = 1;
    return (sig & (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F)) != 0;
}

/* ---- P96W45: pause (Space / MintVID-pi "||" = Ctrl-D) ------------------
 * Like VPr8's SPACE-PAUSE: nothing new is read or sent while paused; what is
 * already at the Pi plays out.  In the window the library is kept alive with
 * BVP_Control(IDLE) (callbacks, clock replies, window keys).  On continue
 * every wall-clock anchor moves forward by the pause, so no frames are
 * "caught up".  Esc / 0 / close / Ctrl-C end the film also while paused. */
#define P45_CTRL_IDLE 5
extern LONG bvp_call_control(struct Library *base, ULONG cmd);
extern LONG bvp_call_control2(struct Library *base, ULONG cmd, ULONG arg);
#define P47_CTRL_TIMESHIFT 6
#define P68_CTRL_CLOCKSTART 7
static unsigned long p45_win_space_seen;
static uint32_t p45_last_toggle_us;
static int p45_have_toggle;
static unsigned long p45_pauses;
static uint32_t p45_paused_total_us;
static char p45_status_text[128];
static int p63_noaudio;           /* P96W63: NOAUDIO = play without sound */
/* P96W67: PAULA = sound through the Amiga (Darren Banfi's mrb_paula module),
 * picture on the Pi with its own free-running clock (CLK81), as NOAUDIO. */
static int p67_paula, p67_low, p67_mono;
static mrb_paula *p67_pa;
/* P96W69/W73: Paula base offset.  W69: built-in -500 ms; W73: Michael set
 * -350 ms as the built-in value, and MintVID-pi's menu can change it
 * (ENVARC:MintVID-pi.paula, text "-350"; same reader as
 * player/amiga/mr_paula_pref.h).  The per-film fine value is added. */
static long p73_base_ms = -350L;
#define P69_PAULA_DEFAULT_AV_US (p73_base_ms * 1000L)
static void p73_base_load(void)
{
    char text[16];
    BPTR f = Open((CONST_STRPTR)"ENVARC:MintVID-pi.paula", MODE_OLDFILE);
    LONG got;
    long v;
    if (!f) return;
    got = Read(f, text, (LONG)sizeof text - 1);
    Close(f);
    if (got <= 0) return;
    text[got] = 0;
    v = strtol(text, NULL, 10);
    if (v >= -1000L && v <= 0L) p73_base_ms = v;
}
static int p69_av_set;
static int p70_used;               /* Paula was really playing (for .t) */
static int p67_anchored;
/* P96W78: pause in the Paula fullscreen = stop at the exact spot.  The free
 * Pi clock (CLK81) cannot be paused safely, so a pause key (Space, Ctrl-D,
 * MintVID.player PAUSE) ends mrberry like Ctrl-F: .t without the 30-s recap,
 * status ENDED "Paused".  MintVID-pi then shows ">" and Play starts again
 * at that spot.  The spot is the Pi clock = picture on screen:
 * film time = (now - clock start) + T= base. */
static int p50_t_set;               /* defined with the T= code below */
static uint64_t p50_base_us;
static uint64_t p54_last_vpts_raw;
static int p54_have_raw;
static int p78_pause_stop;
static uint32_t p78_clock_sys;      /* Pi clock start (same timer as Paula) */
static uint64_t p78_stop_us;
static int p78_have_stop;
/* P96W79: STILL = still picture for the Paula pause.  MintVID-pi starts
 * "mrberry <film> W=50 T=<spot> STILL" after a Paula pause: window, no sound,
 * no clock queries (NOSYNC).  As soon as the first picture is in the window
 * mrberry stops reading and waits like the window pause (W49).  It never
 * writes the .t or the timeline position.  Space (in the window) = continue:
 * status ENDED "Resume" -> MintVID-pi starts the Paula fullscreen again.
 * Esc / close / Ctrl-C / Ctrl-F = just end. */
static int p79_still, p79_resume;
static uint32_t p79_t0;
static int p79_have_t0;
#define P67_FREE (p63_noaudio || p67_paula)   /* no HDMI audio, Pi clock alone */
static void p67_cleanup(void)
{
    if (p67_pa) { mrb_paula_close(p67_pa); p67_pa = NULL; }
}
static void p70_apply_keys(void);
static unsigned long p66_lead_ms = 1000; /* P96W66: NOAUDIO feed lead (LEAD=ms) */
static int p45_no_idle;          /* P96W46: PIDLE0 = no library calls while paused */

static int p45_pause_requested(void)
{
    int req = 0;
    uint32_t now;

    if (p45_key_space) { p45_key_space = 0; req = 1; }
    if (g_bvp.lib) {
        unsigned long v = *(volatile unsigned long *)((volatile unsigned char *)g_bvp.lib + 120);
        if (v && ((volatile unsigned long *)v)[0] == 0x50393653UL) {   /* 'P96S' */
            unsigned long c = ((volatile unsigned long *)v)[19];       /* +76 */
            if (c != p45_win_space_seen) { p45_win_space_seen = c; req = 1; }
        }
    }
    if (bvp_exec_setsignal(0, SIGBREAKF_CTRL_D) & SIGBREAKF_CTRL_D) req = 1;
    /* P96W77: Ctrl-E from MintVID's GUI: MR_PLAYER_COMMAND_PAUSE toggles
     * pause.  FAST and FULLSCREEN are not supported yet (ignored). */
    if (bvp_exec_setsignal(0, SIGBREAKF_CTRL_E) & SIGBREAKF_CTRL_E)
        if (p77_take_commands() & P77_CMD_PAUSE) req = 1;
    if (!req) return 0;
    if (P67_FREE) {
        /* P96W67: the free Pi clock cannot pause.  P96W78: with Paula the
         * pause key stops at the exact spot (see p78_*). NOAUDIO: ignored. */
        if (p67_paula && p67_anchored && !p78_pause_stop) {
            uint32_t now = (uint32_t)ac309i_read_sys_us();
            uint64_t at = (uint64_t)(uint32_t)(now - p78_clock_sys) +
                          (p50_t_set ? p50_base_us : 0);
            if (p54_have_raw && at > p54_last_vpts_raw) at = p54_last_vpts_raw;
            p78_stop_us = at;
            p78_have_stop = 1;
            p78_pause_stop = 1;
            if (n76_hostlog) {
                fprintf(n76_hostlog, "P78_PAUSE_STOP film_ms=%lu read_ms=%lu\n",
                        (unsigned long)(at / 1000ULL),
                        (unsigned long)(p54_last_vpts_raw / 1000ULL));
                fflush(n76_hostlog);
            }
        }
        return 0;
    }
    /* one key press may arrive as RAWKEY and VANILLAKEY: merge (250 ms) */
    now = (uint32_t)ac309i_read_sys_us();
    if (p45_have_toggle && (uint32_t)(now - p45_last_toggle_us) < 250000u) return 0;
    p45_last_toggle_us = now;
    p45_have_toggle = 1;
    return 1;
}

/* Returns 1 when the user stopped the film during the pause. */
static int p45_pause_loop(ac309i_pacer *pace, uint32_t *eac3_wall)
{
    uint32_t t0 = (uint32_t)ac309i_read_sys_us(), dt;
    int stop = 0;
    char line[64];

    n76_hlog("P45_PAUSE_ON");
    p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", "Paused");
    for (;;) {
        Delay(2);
        if (g_bvp.lib && !p45_no_idle) bvp_call_control(g_bvp.lib, P45_CTRL_IDLE);
        if (p05_input_poll_stop() ||
            p58_stop_sig(bvp_exec_setsignal(0, 0))) { stop = 1; break; }
        if (p45_pause_requested()) break;
    }
    dt = (uint32_t)ac309i_read_sys_us() - t0;
    pace->base_us += dt;
    /* P96W47: the library's own window pacer must move with ours */
    if (g_bvp.lib) bvp_call_control2(g_bvp.lib, P47_CTRL_TIMESHIFT, dt);
    if (eac3_wall) *eac3_wall += dt;
    p45_pauses++;
    p45_paused_total_us += dt;
    snprintf(line, sizeof line, "P45_PAUSE_OFF ms=%lu stop=%d",
             (unsigned long)(dt / 1000u), stop);
    n76_hlog(line);
    if (!stop)
        p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", p45_status_text);
    s1_after_pause();                 /* S1: old clock samples are stale */
    return stop;
}

/* P96W79: has the library drawn a picture in the window yet?  (P96S +20 =
 * shown counter, counted after every window blit.) */
static int p79_shown(void)
{
    unsigned long v;
    volatile unsigned long *s;
    if (!g_bvp.lib) return 0;
    v = *(volatile unsigned long *)((volatile unsigned char *)g_bvp.lib + 120);
    if (!v || ((volatile unsigned long *)v)[0] != 0x50393653UL) return 0;   /* 'P96S' */
    s = (volatile unsigned long *)v;
    s[16] = 1;                                   /* no clock queries */
    return s[5] != 0;
}

/* P96W79: hold the still.  Returns 1 = continue (Space), 0 = end. */
static int p79_still_loop(void)
{
    int resume = 0;
    p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", "Paused");
    if (n76_hostlog) {
        fprintf(n76_hostlog, "P79_STILL_HOLD shown=%d\n", p79_shown());
        fflush(n76_hostlog);
    }
    for (;;) {
        Delay(2);
        if (g_bvp.lib) bvp_call_control(g_bvp.lib, P45_CTRL_IDLE);
        if (p05_input_poll_stop() ||
            p58_stop_sig(bvp_exec_setsignal(0, 0))) break;
        if (p45_pause_requested()) { resume = 1; break; }
    }
    n76_hlog(resume ? "P79_STILL_RESUME" : "P79_STILL_END");
    return resume;
}

/* ---- P96W46: soft pause in the window ----------------------------------
 * W45 stopped the picture at once, but the sound already sent to the Pi
 * (0.4-0.8 s ahead) kept playing.  After "continue" the sound clock was then
 * ahead of the picture (seen: 0.1-0.5 s), and the Pi bulk hang came much more
 * often (87-MB film 4 of 5 runs) - like W32-W36, where the picture also ran
 * behind the sound.  Now, as at the film start:
 *   1. pause key: no more audio is sent (it is kept in the P04 queue); the
 *      picture keeps running until it reaches the last sound already sent
 *      (max 3 s) - picture and sound stop at the same film position;
 *   2. pause loop (as W45);
 *   3. continue: first one picture, then the kept audio (like P04 at start).
 * Cinema: unchanged (immediate pause). */
static int p45_phase;            /* 0 play, 1 draining to the pause point */
static int p45_hold_audio;       /* audio packets go to the P04 queue */
static int p45_flush_pending;
static unsigned long p45_sent_mark;
static uint64_t p45_audio_end;
static uint32_t p45_drain_start;

/* 0 = go on, 1 = user stop, 2 = audio send error */
static int p45_service(mr_berryvp *bvp, ac309i_pacer *pace, uint32_t *eac3_wall,
                       unsigned long sent, unsigned long *audio_sent)
{
    int req = p45_pause_requested();
    uint32_t now = (uint32_t)ac309i_read_sys_us();

    if (p78_pause_stop) return 1;   /* P96W78: Paula pause = stop here */
    if (req) {
        if (p45_phase == 1) {
            /* second key press while draining: continue at once */
            p45_phase = 0;
            p45_flush_pending = 1;
            p45_sent_mark = sent ? sent - 1 : 0;
            n76_hlog("P46_DRAIN_CANCEL");
            p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", p45_status_text);
        } else if (mr_berryvp_stream_flags && pace->started &&
                   p96w_have_apts && p96w_have_vpts && !p45_flush_pending) {
            p45_phase = 1;
            p45_hold_audio = 1;
            p45_audio_end = p96w_last_apts;
            p45_drain_start = now;
            n76_hlog("P46_DRAIN_START");
            p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", "Paused");
        } else {
            if (p45_pause_loop(pace, eac3_wall)) return 1;
        }
    }
    if (p45_phase == 1) {
        uint64_t now_media = p96w_first_vpts + (uint64_t)(uint32_t)(now - pace->base_us);
        uint32_t waited = now - p45_drain_start;
        if (now_media >= p45_audio_end || waited >= 3000000u) {
            char line[80];
            snprintf(line, sizeof line, "P46_DRAIN_DONE ms=%lu", (unsigned long)(waited / 1000u));
            n76_hlog(line);
            p45_phase = 0;
            if (p45_pause_loop(pace, eac3_wall)) return 1;
            p45_flush_pending = 1;
            p45_sent_mark = sent;
        }
    }
    if (p45_flush_pending && sent > p45_sent_mark) {
        mr_status st;
        char line[80];
        snprintf(line, sizeof line, "P46_AUDIO_RESUME queued=%lu", p04_aqueued);
        n76_hlog(line);
        p45_flush_pending = 0;
        p45_hold_audio = 0;
        st = p04_flush_audio_queue(bvp, audio_sent);
        if (st != MR_OK) return 2;
    }
    return 0;
}

static int ac309i_video_due(const ac309i_pacer *p, uint32_t *elapsed_out)
{
    uint32_t elapsed, feed_deadline;
    if (!p->enabled || !p->started) {
        if (elapsed_out) *elapsed_out = 0;
        return 1;
    }
    elapsed = (uint32_t)(ac309i_read_sys_us() - p->base_us);
    if (elapsed_out) *elapsed_out = elapsed;
    /* AC309I: bounded decoder headroom. Presentation target/PTS is NOT moved.
     * Only decoder admission opens LEAD_US before the absolute frame target. */
    feed_deadline = (p->target_us > p->lead_us) ?
        (p->target_us - p->lead_us) : 0u;
    return elapsed >= feed_deadline;
}

static void ac309i_commit_video_deadline(ac309i_pacer *p, FILE *log)
{
    uint32_t now, elapsed, late = 0;
    if (!p->enabled) return;
    now = (uint32_t)ac309i_read_sys_us();
    if (!p->started) {
        p->base_us = now;
        p->started = 1;
        p->target_us = ac309i_next_interval(p);
        p->sent_aus = 1;
        if (log) {
            if (mr_berryvp_stream_flags)
                fprintf(log, "P96W37_AV_US=%ld SYNC=%s\n", p96w_av_delay_us,
                        p96s_off ? "off" : "library-hold");
            fprintf(log,
                    "AC309I_PACE_ANCHOR base=$%08lx next=%lu lead=%lu step=%lu rem=%lu/%lu\n",
                    (unsigned long)p->base_us, (unsigned long)p->target_us,
                    (unsigned long)p->lead_us, (unsigned long)p->step_whole_us,
                    (unsigned long)p->step_rem, (unsigned long)p->step_den);
            fflush(log);
        }
        return;
    }
    elapsed = (uint32_t)(now - p->base_us);
    {
        uint32_t feed_deadline = (p->target_us > p->lead_us) ?
            (p->target_us - p->lead_us) : 0u;
        if (elapsed > feed_deadline) {
            late = elapsed - feed_deadline;
            p->late_count++;
            if (late > p->max_late_us) p->max_late_us = late;
        }
    }
    p->target_us += ac309i_next_interval(p);
    p->sent_aus++;
    if (log && (mr_berryvp_stream_flags ? (p->sent_aus & 1023u) == 0u :
                ((p->sent_aus & 255u) == 0u || (elapsed >= 45000000u && (p->sent_aus & 31u) == 0u)))) {
        fprintf(log,
                "AC309I_PACE sent=%lu elapsed=%lu next=%lu lead=%lu queue=%lu feedlate=%lu maxfeedlate=%lu latecnt=%lu\n",
                p->sent_aus, (unsigned long)elapsed, (unsigned long)p->target_us,
                (unsigned long)p->lead_us, n73_vqueued, (unsigned long)late,
                (unsigned long)p->max_late_us, p->late_count);
        fflush(log);
    }
}

static long p96s_median3(long a, long b, long c)
{
    if (a > b) { long t = a; a = b; b = t; }
    if (b > c) { long t = b; b = c; c = t; }
    if (a > b) { long t = a; a = b; b = t; }
    return b;
}

static void p96w_sync_update(ac309i_pacer *pace)
{
    volatile unsigned long *s;
    uint64_t clk, disp;
    int32_t dt;
    int64_t e64;
    long e, corr;
    uint32_t now, elapsed;
    if (!mr_berryvp_stream_flags) return;
    if (!p96s) {
        unsigned long v;
        if (!g_bvp.lib) return;
        v = *(volatile unsigned long *)((volatile unsigned char *)g_bvp.lib + 120);
        if (!v || ((volatile unsigned long *)v)[0] != 0x50393653UL) return;   /* 'P96S' */
        p96s = (volatile unsigned long *)v;
        if (p96s_off) p96s[16] = 1;               /* P96W36: library must not ask */
        p96s[17] = (unsigned long)p96w_av_delay_us; /* P96W37: AV offset for the hold */
    }
    if (p96s_off || !pace->enabled || !pace->started) return;
    s = p96s;
    if (s[1] == p96s_last_seq) return;          /* no new clock sample */
    p96s_last_seq = s[1];
    if (!s[5]) return;                          /* nothing shown yet */
    clk  = ((uint64_t)s[2] << 32) | (uint64_t)s[3];
    disp = ((uint64_t)s[6] << 32) | (uint64_t)s[7];
    dt = (int32_t)(uint32_t)(s[8] - s[4]);      /* display time - clock time */
    if (dt > 1000000 || dt < -1000000) return;  /* samples too far apart */
    /* >0: the shown frame is ahead of the sound -> show later. */
    e64 = (int64_t)disp - ((int64_t)clk + dt) + (int64_t)p96w_av_delay_us;
    if (e64 > 5000000 || e64 < -5000000) return;
    e = (long)e64;
    p96s_samples++;
    now = (uint32_t)ac309i_read_sys_us();
    elapsed = now - pace->base_us;
    if (p96s_log_n < P96S_LOG_MAX && (p96s_samples <= 30 || (p96s_samples % 25) == 0)) {
        p96s_log_t[p96s_log_n] = (long)(elapsed / 1000u);
        p96s_log_e[p96s_log_n] = e / 1000;
        p96s_log_n++;
    }
    if (p96s_samples == 1) { p96s_first_err = e; p96s_min_err = e; p96s_max_err = e; }
    p96s_last_err = e;
    if (e < p96s_min_err) p96s_min_err = e;
    if (e > p96s_max_err) p96s_max_err = e;
    if (p96s_measure) return;                   /* P96W36: only measure */
    if ((int32_t)(now - p96s_settle_until) < 0) return;   /* pipeline still shows old frames */
    p96s_used++;
    if (!p96s_init_done) {
        p96s_init_err[p96s_init_n++] = e;
        if (p96s_init_n < 3) return;
        p96s_init_done = 1;
        corr = p96s_median3(p96s_init_err[0], p96s_init_err[1], p96s_init_err[2]);
        if (corr < 20000 && corr > -20000) return;
        if (corr > 1000000) corr = 1000000;       /* more than 1 s is not plausible */
        if (corr < -1000000) corr = -1000000;
    } else {
        /* P96W33: median of the last three values ignores single outliers,
         * and at most 8 ms per value (~40 ms/s) keeps corrections invisible. */
        long f;
        p96s_hist[p96s_hist_n % 3u] = e;
        p96s_hist_n++;
        if (p96s_hist_n < 3) return;
        f = p96s_median3(p96s_hist[0], p96s_hist[1], p96s_hist[2]);
        if (f < 15000 && f > -15000) return;      /* close enough */
        corr = f / 4;
        {
            /* P96W34: after 25 queries the library asks only once per second,
             * so allow a bit more per value (still <= 20 ms per second). */
            long lim = (s[9] >= 25u) ? 20000 : 8000;
            if (corr > lim) corr = lim;
            if (corr < -lim) corr = -lim;
        }
    }
    /* never move the grid start into the future */
    if (corr > 0 && (uint32_t)corr + 100000u > elapsed) {
        if (elapsed <= 100000u) return;
        corr = (long)(elapsed - 100000u);
    }
    pace->base_us += (uint32_t)corr;
    p96s_shift += corr;
    p96s_corrections++;
    if (corr > 50000 || corr < -50000)
        p96s_settle_until = now + 1000000u;     /* let the queued frames pass */
}

static mr_status ac309i_service_mov_video(mr_berryvp *bvp, ac309i_pacer *pace,
                                          unsigned long *sent,
                                          unsigned long *skipped)
{
    while (n73_vhead) {
        n73_video_node *n;
        mr_packet p;
        mr_status st;
        uint32_t elapsed;
        p96w_sync_update(pace);
        if (mr_berryvp_stream_flags && !p96w_drain && n73_vqueued < P96W_HOLD_MAX_Q) {
            if (!p96w_prefill_done) {
                if (!(p96w_have_vpts && p96w_have_apts &&
                      p96w_last_apts >= p96w_first_vpts + P96W_PREFILL_US)) break;
                p96w_prefill_done = 1;
                if (n76_hostlog) {
                    fprintf(n76_hostlog, "P96W5_PREFILL_DONE queue=%lu\n", n73_vqueued);
                    fflush(n76_hostlog);
                }
            } else if (pace->started && p96w_have_apts && p96w_have_vpts) {
                /* P96W27: audio must stay ahead of REAL time as in W25, not of
                 * the delayed picture (W26 shrank the audio cushion by the AV
                 * delay -> dropouts and silence at W:75). */
                uint64_t now_media = p96w_first_vpts +
                    (uint64_t)(uint32_t)(ac309i_read_sys_us() - pace->base_us);
                if (p96w_last_apts < now_media + P96W_AUDIO_LEAD_US) {
                    p96w_audio_first_holds++;
                    break;
                }
            }
        }
        if (!ac309i_video_due(pace, &elapsed)) break;
        n = n73_vhead;
        /* P96W3: in the P96 window the shared VCHIQ transport cannot always
         * keep real time with 1080p.  When this AU is already more than two
         * frame periods late and nothing references it, drop it: no decode,
         * no ISP->host transfer, no blit.  The deadline grid still advances,
         * so video catches up and audio stays in sync. */
        if (mr_berryvp_stream_flags && n->nonref && pace->started) {
            uint32_t fd = (pace->target_us > pace->lead_us) ?
                (pace->target_us - pace->lead_us) : 0u;
            /* P96W6: like VPberry, show late frames and let them catch up;
             * drop only when really far behind. */
            if (elapsed > fd + P96W_DROP_LATE_US) {
                ac309i_commit_video_deadline(pace, NULL);
                p96w_dropped++;
                n73_vhead = n->next;
                if (!n73_vhead) n73_vtail = NULL;
                free(n->data);
                free(n);
                n73_vqueued--;
                continue;
            }
        }
        memset(&p, 0, sizeof p);
        p.is_video = 1;
        p.has_pts = n->has_pts;
        p.pts_us = n->pts_us;
        p.data = n->data;
        p.len = n->len;
        p.is_annexb = n->is_annexb;
        /* VPv149 semantics: claim/advance the absolute input deadline before
         * entering SendVideo.  The first-frame anchor therefore excludes
         * decoder/VCHIQ submission latency, just like v04_pace_new_frame. */
        ac309i_commit_video_deadline(pace, n76_hostlog);
        st = mr_berryvp_send_packet(bvp, &p);
        if (st == MR_EAGAIN) { if (skipped) (*skipped)++; }
        else if (st != MR_OK) return st;
        else if (sent) (*sent)++;
        if (p67_pa && !p67_anchored && st == MR_OK) {
            /* P96W67: the Pi clock (CLK81) started inside this first
             * SendVideo at media time 0; Paula follows the same Pi timer. */
            uint32_t t = (uint32_t)ac309i_read_sys_us();
            /* P96W68: the library says when the Pi clock really started
             * (BVP_CTRL_CLOCKSTART); W67 used the end of the first send. */
            uint32_t cs = g_bvp.lib ? (uint32_t)bvp_call_control(g_bvp.lib, P68_CTRL_CLOCKSTART) : 0;
            if (cs && (uint32_t)(t - cs) > 5000000u) cs = 0;   /* old library: error code */
            p67_anchored = 1;
            p78_clock_sys = cs ? cs : t;           /* P96W78: pause spot */
            mrb_paula_anchor(p67_pa, cs ? cs : t, 0);
            if (n76_hostlog) { fprintf(n76_hostlog, "P67_ANCHOR sys=%lu clockstart=%lu gap_ms=%ld first_pts_ms=%lu\n",
                               (unsigned long)t, (unsigned long)cs,
                               cs ? (long)((int32_t)(t - cs) / 1000) : 0L,
                               p.has_pts ? (unsigned long)(p.pts_us / 1000ULL) : 0UL); fflush(n76_hostlog); }
        }
        if (p67_pa) mrb_paula_service(p67_pa);
        n73_vhead = n->next;
        if (!n73_vhead) n73_vtail = NULL;
        free(n->data);
        free(n);
        n73_vqueued--;
    }
    return MR_OK;
}

/* ---- P96W50: T: start time ------------------------------------------------
 * mrberry <film> [W=50] T=H:MM  (T=H:MM:SS too; ':' works instead of '=',
 * so VPr8's .t lines T:HH:MM:SS / W:50 can be passed as they are).
 * The demux jumps to the last H.264 IDR at or before T (MKV and MP4) before
 * the Pi is touched.  From there every PTS is moved down by the IDR's PTS,
 * so berryvp.library and the Pi see a stream that starts at 0, exactly like
 * a normal film start.  Audio/video packets before the IDR's PTS are
 * dropped. */
static int p50_t_set;
static uint64_t p50_t_us, p50_seek_us, p50_base_us;
static int p50_have_base;
static unsigned long p50_drop_audio, p50_drop_video;

static int p50_parse_time(const char *s, uint64_t *us)
{
    unsigned long part[3];
    int n = 0;
    uint64_t sec;
    for (;;) {
        unsigned long v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10UL + (unsigned long)(*s - '0');
            if (++digits > 6) return 0;
            s++;
        }
        if (!digits || n >= 3) return 0;
        part[n++] = v;
        if (*s == ':') { s++; continue; }
        if (*s) return 0;
        break;
    }
    /* P96W52: hours and minutes, seconds optional; a single number is
     * refused (T=1 could mean seconds, minutes or hours). */
    if (n == 1) return 0;
    if (part[1] > 59) return 0;
    sec = (uint64_t)part[0] * 3600U + (uint64_t)part[1] * 60U;
    if (n == 3) {
        if (part[2] > 59) return 0;
        sec += part[2];
    }
    *us = sec * 1000000ULL;
    return 1;
}

static void p50_fmt(char *dst, size_t size, uint64_t us)
{
    unsigned long s = (unsigned long)(us / 1000000ULL);
    snprintf(dst, size, "%02lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
}

static uint64_t p54_last_vpts_raw;
static int p54_have_raw;

/* P96W62: live position for MintVID-pi's timeline.  Film time of the last
 * video packet read (no Pi clock query - see W26-W36), written as whole
 * seconds to T:MintVID-pi.pos every P62_STEP_US of film time.  RAM only;
 * the .t next to the film is still written once, at stop. */
#define P62_POS_FILE "T:MintVID-pi.pos"
#define P62_POS_TMP  "T:MintVID-pi.pos.tmp"
#define P62_STEP_US  10000000ULL
static uint64_t p62_next_us;
static int p62_have;
static void p62_pos_clear(void)
{
    DeleteFile((CONST_STRPTR)P62_POS_FILE);
    DeleteFile((CONST_STRPTR)P62_POS_TMP);
    p62_have = 0;
}
static void p62_pos_write(uint64_t film_us)
{
    char line[24];
    BPTR f;
    LONG len, w;
    if (p70_used)                    /* P96W72: + live Paula fine offset */
        snprintf(line, sizeof line, "%lu\nP:%ld\n",
                 (unsigned long)(film_us / 1000000ULL), p70_fine_ms);
    else
        snprintf(line, sizeof line, "%lu\n", (unsigned long)(film_us / 1000000ULL));
    len = (LONG)strlen(line);
    f = Open((CONST_STRPTR)P62_POS_TMP, MODE_NEWFILE);
    if (!f) return;
    w = Write(f, line, len);
    Close(f);
    if (w == len) {
        DeleteFile((CONST_STRPTR)P62_POS_FILE);
        if (!Rename((CONST_STRPTR)P62_POS_TMP, (CONST_STRPTR)P62_POS_FILE))
            DeleteFile((CONST_STRPTR)P62_POS_TMP);
    } else {
        DeleteFile((CONST_STRPTR)P62_POS_TMP);
    }
}
/* P96W65 test: NOAUDIO heartbeat from the main loop (N76_HOST.TXT, 1/s). */
static void p65_hb(unsigned long sent, unsigned long queued)
{
    static ULONG t0, last;
    ULONG now;
    if (!P67_FREE || !n76_hostlog) return;
    now = ac309i_read_sys_us();
    if (!t0) { t0 = now; last = now; return; }
    if (now - last < 1000000UL) return;
    last = now;
    fprintf(n76_hostlog, "P65_HB wall_ms=%lu sent=%lu read_pts_ms=%lu queued=%lu\n",
            (unsigned long)((now - t0) / 1000UL), sent,
            (unsigned long)(p54_last_vpts_raw / 1000ULL), queued);
    fflush(n76_hostlog);
}
/* S1: film time on screen in ms (0 = not known yet).
 * HDMI sound: the Pi audio clock (sampled by the library about once a
 * second) plus the Pi timer since the sample.  Paula / no sound: the free
 * Pi clock (CLK81) from its start, as P96W78. */
static int s1_now_ms(uint32_t *ms)
{
    uint32_t now = (uint32_t)ac309i_read_sys_us();
    uint64_t film;
    if (P67_FREE) {
        uint32_t cs = (uint32_t)bvp_call_control(g_bvp.lib, P68_CTRL_CLOCKSTART);
        if (!cs || (uint32_t)(now - cs) > 0x7fffffffu) return 0;
        film = (uint64_t)(uint32_t)(now - cs);
    } else {
        uint32_t sample;
        if (!s1_blk) {
            unsigned long v = (unsigned long)bvp_call_control2(g_bvp.lib, 13, 1);
            if (v < 0x1000UL) return 0;           /* OSD not ready yet / old library */
            s1_blk = (volatile unsigned long *)v;
        }
        if (!s1_blk[0]) return 0;                 /* no clock answer yet */
        sample = (uint32_t)s1_blk[3];
        if (s1_have_resume && (int32_t)(sample - s1_resume_sys) < 0) return 0;
        film = (((uint64_t)s1_blk[1]) << 32) | (uint64_t)s1_blk[2];
        film += (uint64_t)(uint32_t)(now - sample);
    }
    if (p50_t_set) film += p50_base_us;
    film /= 1000u;
    if (s1_offset_ms > 0 && film < (uint64_t)s1_offset_ms) film = 0;
    else film -= (uint64_t)(int64_t)s1_offset_ms;
    *ms = (uint32_t)film;
    return 1;
}
/* v96.85: film time on screen in us (best guess), for Tab */
static int s4_seek_ok;
static uint64_t s4_len_us, s4_target_us;
static uint64_t s4_now_us(void)
{
    uint64_t base = p50_t_set ? p50_base_us : 0;
    uint32_t now = (uint32_t)ac309i_read_sys_us();
    if (mr_berryvp_stream_flags && g_bvp.lib) {          /* window: frame shown */
        unsigned long v = *(volatile unsigned long *)((volatile unsigned char *)g_bvp.lib + 120);
        if (v && ((volatile unsigned long *)v)[0] == 0x50393653UL && ((volatile unsigned long *)v)[5])
            return ((((uint64_t)((volatile unsigned long *)v)[6]) << 32) |
                    ((volatile unsigned long *)v)[7]) + base;
    }
    if (P67_FREE && g_bvp.lib) {                         /* Paula / no sound */
        uint32_t cs = (uint32_t)bvp_call_control(g_bvp.lib, P68_CTRL_CLOCKSTART);
        if (cs && (uint32_t)(now - cs) < 0x7fffffffu) return (uint64_t)(uint32_t)(now - cs) + base;
    }
    if (s1_blk && s1_blk[0])                             /* HDMI: Pi audio clock */
        return ((((uint64_t)s1_blk[1]) << 32) | s1_blk[2]) +
               (uint64_t)(uint32_t)(now - (uint32_t)s1_blk[3]) + base;
    if (p54_have_raw)                                    /* last frame read, ~1.5 s ahead */
        return p54_last_vpts_raw > 1500000u ? p54_last_vpts_raw - 1500000u : 0;
    return base;
}
static int s4_from_gui(void)
{
    struct Task *me = FindTask(NULL);
    return me && me->tc_Node.ln_Name && !strcmp(me->tc_Node.ln_Name, "MintVID-pi player");
}
/* 1 = stop now for the jump (target in s4_target_us) */
static int s4_check(void)
{
    int dir = s4_dir;
    uint64_t now, target;
    unsigned i;
    s4_dir = 0;
    if (s4_jump) return 1;
    if (!s4_from_gui() || !s4_seek_ok || p79_still) {
        n76_hlog("S4_TAB_IGNORED (Shell start or no seek)");
        return 0;
    }
    now = s4_now_us();
    if (mr_mkv_chapter_count) {
        if (dir > 0) {
            target = (uint64_t)-1;
            for (i = 0; i < mr_mkv_chapter_count; i++)
                if (mr_mkv_chapters_us[i] > now + 2000000u) { target = mr_mkv_chapters_us[i]; break; }
        } else {
            target = 0;                                  /* 3 s into a chapter: its start */
            for (i = 0; i < mr_mkv_chapter_count; i++)
                if (mr_mkv_chapters_us[i] + 3000000u < now) target = mr_mkv_chapters_us[i];
        }
        if (target == (uint64_t)-1) target = now + 600000000u;   /* after the last */
    } else {
        target = dir > 0 ? now + 600000000u : (now > 600000000u ? now - 600000000u : 0);
    }
    if (s4_len_us && target + 5000000u >= s4_len_us) {
        n76_hlog("S4_TAB_IGNORED (past the end)");
        return 0;
    }
    s4_target_us = target;
    s4_jump = 1;
    p58_switch = 1;                                      /* stop like Ctrl-F, no recap */
    if (n76_hostlog) {
        fprintf(n76_hostlog, "S4_TAB dir=%d now_ms=%lu target_ms=%lu chapters=%u\n", dir,
                (unsigned long)(now / 1000u), (unsigned long)(target / 1000u), mr_mkv_chapter_count);
        fflush(n76_hostlog);
    }
    return 1;
}

static void s1_tick(void)
{
    uint32_t ms;
    int r;
    if (!s1_on || s1_user_off || !g_bvp.lib) return;
    if (s3_banner_on) {                       /* v96.83: name of the new file */
        if ((int32_t)((uint32_t)ac309i_read_sys_us() - s3_banner_until) < 0) return;
        s3_banner_on = 0;
    }
    if (!s1_now_ms(&ms)) return;
    r = mrb_sub_tick(ms);
    if (r == MRB_SUB_SHOW) {
        LONG rc = bvp_call_control2(g_bvp.lib, 12, (unsigned long)mrb_sub_pixels());
        if (rc != 0) {
            s1_on = 0;
            if (n76_hostlog) { fprintf(n76_hostlog, "S1_PIC_FAIL rc=%ld\n", (long)rc); fflush(n76_hostlog); }
            return;
        }
        (void)bvp_call_control2(g_bvp.lib, 11, 1);
        s1_shows++;
        if (n76_hostlog && s1_shows <= 5) {
            fprintf(n76_hostlog, "S1_SHOW cue=%d film_ms=%lu\n", mrb_sub_shown(), (unsigned long)ms);
            fflush(n76_hostlog);
        }
    } else if (r == MRB_SUB_HIDE) {
        (void)bvp_call_control2(g_bvp.lib, 11, 0);
        s1_hides++;
    }
}
/* S2 file list (defined with s2_scan below) */
static char s2_label[MRB_SUB_FILES][64];
static char s2_path[MRB_SUB_FILES][512];
static unsigned s2_n;
static const char *s2_used_label;
static uint64_t s2_mkv[MRB_SUB_FILES];
static unsigned s2_activate(const char *film, int k, char *used, size_t used_size);
/* v96.83: S = next subtitle file of the film: Off -> 1 -> 2 ... -> Off.
 * Its name is shown for 2 s; the choice goes into the .t (S:). */
static void s3_next_file(void)
{
    char t[96];
    int next;
    LONG rc;
    if (!s1_on || !s3_film || !s2_n || !g_bvp.lib) return;
    next = (s3_cur + 1) % (int)(s2_n + 1);
    if (next) {
        if (!s2_activate(s3_film, next, NULL, 0))
            mrb_sub_clear();     /* empty / unreadable: nothing to draw */
        snprintf(t, sizeof t, "Subtitles: %s", s2_label[next - 1]);
        s2_used_label = s2_label[next - 1];
    } else {
        (void)s2_activate(s3_film, 0, NULL, 0);
        strcpy(t, "Subtitles: Off");
        s2_used_label = "OFF";
    }
    s3_cur = next;
    s1_user_off = 0;
    mrb_sub_banner(t);
    rc = bvp_call_control2(g_bvp.lib, 12, (unsigned long)mrb_sub_pixels());
    if (rc == 0) {
        (void)bvp_call_control2(g_bvp.lib, 11, 1);
        s3_banner_until = (uint32_t)ac309i_read_sys_us() + 2000000u;
        s3_banner_on = 1;
    }
    if (n76_hostlog) {
        fprintf(n76_hostlog, "S3_KEY_S file=%d of=%u cues=%u rc=%ld\n",
                next, s2_n, mrb_sub_count(), (long)rc);
        fflush(n76_hostlog);
    }
}

/* v96.87: M = mute.  HDMI: the library sends zeros instead of the sound
 * (the Pi audio clock keeps running, picture timing stays the same).
 * Paula: volume 0 / 64.  With subtitles (OSD) a short banner is shown. */
static void m1_toggle(void)
{
    LONG rc = -1;
    m1_muted = !m1_muted;
    if (p67_pa) mrb_paula_set_mute(p67_pa, m1_muted);
    else if (g_bvp.lib) rc = bvp_call_control2(g_bvp.lib, 14, m1_muted ? 1UL : 0UL);
    if (s1_on && !s1_user_off && g_bvp.lib) {
        mrb_sub_banner(m1_muted ? "Mute" : "Sound on");
        if (bvp_call_control2(g_bvp.lib, 12, (unsigned long)mrb_sub_pixels()) == 0) {
            (void)bvp_call_control2(g_bvp.lib, 11, 1);
            s3_banner_until = (uint32_t)ac309i_read_sys_us() + 2000000u;
            s3_banner_on = 1;
        }
    }
    if (n76_hostlog) {
        fprintf(n76_hostlog, "M1_KEY_M mute=%d paula=%d rc=%ld\n", m1_muted, p67_pa != NULL, (long)rc);
        fflush(n76_hostlog);
    }
}

static void p70_apply_keys(void)
{
    long d;
    if (s3_key) { s3_key = 0; s3_next_file(); }
    if (m1_key) { m1_key = 0; m1_toggle(); }
    s1_tick();
    if (o2_key_osd && s1_on) {
        /* S1: key O = subtitles off / on */
        o2_key_osd = 0;
        s1_user_off = !s1_user_off;
        if (g_bvp.lib) (void)bvp_call_control2(g_bvp.lib, 11, 0);
        mrb_sub_reset();                  /* on again: draw the current cue */
        if (n76_hostlog) { fprintf(n76_hostlog, "S1_KEY_O off=%d\n", s1_user_off); fflush(n76_hostlog); }
    }
    if (o2_key_osd) {
        o2_key_osd = 0;
        if (g_bvp.lib && mr_berryvp_osd_req) {
            LONG rc;
            o2_osd_shown = !o2_osd_shown;
            rc = bvp_call_control2(g_bvp.lib, 11, o2_osd_shown ? 1UL : 0UL);
            if (n76_hostlog) {
                fprintf(n76_hostlog, "O2_OSD_KEY show=%d rc=%ld\n", o2_osd_shown, (long)rc);
                fflush(n76_hostlog);
            }
        }
    }
    /* O3: OSDTEST=COUNT - a new picture every second */
    if (o3_count_mode && g_bvp.lib && mr_berryvp_osd_req) {
        ULONG now = ac309i_read_sys_us();
        if (!o3_last_us) o3_last_us = now;
        else if ((ULONG)(now - o3_last_us) >= 1000000UL) {
            char t[16];
            LONG rc;
            o3_last_us = now;
            o3_counter++;
            snprintf(t, sizeof t, "OSD %04lu", o3_counter % 10000UL);
            o1_draw(t, 12);
            rc = bvp_call_control2(g_bvp.lib, 12, (unsigned long)o1_pixels);
            if (n76_hostlog && (o3_counter <= 3 || rc != 0)) {
                fprintf(n76_hostlog, "O3_PIC n=%lu rc=%ld\n", o3_counter, (long)rc);
                fflush(n76_hostlog);
            }
            if (rc != 0) o3_count_mode = 0;
        }
    }
    d = p70_key_delta;
    if (!d) return;
    p70_key_delta = 0;
    if (!p67_pa) {
        /* S1: HDMI sound - '+'/'-' move the subtitles by 100 ms */
        if (s1_on) {
            s1_offset_ms += d * 2;
            if (n76_hostlog) { fprintf(n76_hostlog, "S1_OFFSET_MS=%ld\n", s1_offset_ms); fflush(n76_hostlog); }
        }
        return;
    }
    p70_fine_ms += d;
    if (p70_fine_ms > 3000) p70_fine_ms = 3000;
    if (p70_fine_ms < -3000) p70_fine_ms = -3000;
    p70_fine_changed = 1;
    mrb_paula_set_offset(p67_pa, (int32_t)(P69_PAULA_DEFAULT_AV_US + p70_fine_ms * 1000L));
    if (n76_hostlog) { fprintf(n76_hostlog, "P70_FINE_MS=%ld\n", p70_fine_ms); fflush(n76_hostlog); }
    p62_pos_write(p54_last_vpts_raw);   /* P96W72: MintVID-pi shows it at once */
}
static void p62_pos_update(uint64_t film_us)
{
    if (p79_still) return;          /* P96W79: the still leaves the timeline alone */
    if (p62_have && film_us < p62_next_us) return;
    p62_pos_write(film_us);
    p62_next_us = film_us - film_us % P62_STEP_US + P62_STEP_US;
    p62_have = 1;
}
static mr_status p50_next_packet(mr_demux *d, mr_packet *pkt)
{
    for (;;) {
        mr_status st = mr_demux_next_packet(d, pkt);
        if (st == MR_OK && pkt->len && pkt->is_video && pkt->has_pts &&
            (!p54_have_raw || pkt->pts_us > p54_last_vpts_raw)) {
            p54_last_vpts_raw = pkt->pts_us;      /* P96W54: film time */
            p54_have_raw = 1;
            p62_pos_update(p54_last_vpts_raw);
        }
        if (st != MR_OK || !p50_t_set || !pkt->len) return st;
        if (!p50_have_base) {
            if (!pkt->is_video) { p50_drop_audio++; continue; }
            p50_base_us = pkt->has_pts ? pkt->pts_us : p50_seek_us;
            p50_have_base = 1;
            if (n76_hostlog) {
                fprintf(n76_hostlog, "P50_BASE_MS=%lu\n", (unsigned long)(p50_base_us / 1000U));
                fflush(n76_hostlog);
            }
        }
        if (pkt->has_pts) {
            if (pkt->pts_us < p50_base_us) {
                if (pkt->is_video) p50_drop_video++; else p50_drop_audio++;
                continue;
            }
            pkt->pts_us -= p50_base_us;
        }
        return st;
    }
}

/* ---- P96W54: .t resume file (as VPr8) -------------------------------------
 * Stop in the middle (Esc, close gadget, Ctrl-C, MintVID-pi []) writes
 * <film>.t next to the film (extension replaced, Supergirl.mkv ->
 * Supergirl.t):
 *     T:HH:MM:SS      position 30 s before the stop point
 *     W:50 | W:C      window size or cinema
 *     A:HDMI | A:NONE sound output (P96W55; A:PAULA reserved for later)
 * P96W55: the file stays after the natural end of the film (Michael: a hint
 * that the film / episode was already watched).  "mrberry <film> R" (or
 * RESUME) reads it; T=/W= given on the command line win over the file. */
#define P54_RECAP_US (30ULL * 1000000ULL)

static void p54_name(char *dst, size_t size, const char *film)
{
    size_t n = strlen(film), i, dot = (size_t)-1;
    if (n + 3 > size) n = size - 3;
    memcpy(dst, film, n);
    dst[n] = 0;
    for (i = 0; i < n; i++) {
        if (dst[i] == ':' || dst[i] == '/') dot = (size_t)-1;
        else if (dst[i] == '.') dot = i;
    }
    if (dot != (size_t)-1) dst[dot] = 0;
    strcat(dst, ".t");
}

/* 1 = file found; *t_us / *window (-1 = no W line, 0 = cinema) filled */
static char p55_audio_read[16];        /* A: value of the last .t read */

static long p70_t_fine_ms;
static int p70_t_have_fine;

/* MKA1: audio track choice (MKV).  The track list lives in mr_mkv.c. */
#define MKA_TRACK_MAX MR_MKV_ATRACK_MAX
static int mka_cli_track;              /* AUDIO= given on the command line */
static unsigned mka_t_track;           /* AT: line of the last .t read */

/* S2: SRT files of the film (<base>.srt, <base>.<label>.srt), sorted by
 * label.  SUB=n picks file n (1-based), SUB=OFF none; the .t keeps the
 * choice as S:<label> / S:OFF.  Without either: start rule
 * (mrb_sub_pick_default: forced file in the sound's language, else the
 * first forced file, else none). */
static char s2_label[MRB_SUB_FILES][64];
static char s2_path[MRB_SUB_FILES][512];
static unsigned s2_n;
static char s2_t_label[64];          /* S: of the last .t read */
static int s2_t_have;
static int s2_cli_n = -1;            /* SUB=n (0 = OFF), -1 = not given */
static const char *s2_used_label;    /* for the .t: NULL = do not write */

static unsigned s2_scan(const char *film)
{
    char dir[512], base[256];
    const char *fp = (const char *)FilePart((CONST_STRPTR)film);
    const char *pp = (const char *)PathPart((CONST_STRPTR)film);
    size_t dl = (size_t)(pp - film), i;
    char *dot;
    BPTR lock;
    struct FileInfoBlock *fib;
    s2_n = 0;
    if (dl >= sizeof dir) return 0;
    memcpy(dir, film, dl);
    dir[dl] = 0;
    strncpy(base, fp, sizeof base - 1);
    base[sizeof base - 1] = 0;
    dot = strrchr(base, '.');
    if (dot) *dot = 0;
    lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    if (!lock) return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib) && s2_n < MRB_SUB_FILES) {
            char label[64];
            if (fib->fib_DirEntryType >= 0) continue;          /* directory */
            if (!mrb_sub_file_label(base, (const char *)fib->fib_FileName, label, sizeof label))
                continue;
            strcpy(s2_label[s2_n], label);
            strcpy(s2_path[s2_n], dir);
            AddPart((STRPTR)s2_path[s2_n], fib->fib_FileName, sizeof s2_path[s2_n]);
            s2_n++;
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    for (i = 0; i < MRB_SUB_FILES; i++) s2_mkv[i] = 0;
    for (i = 1; i < s2_n; i++) {                 /* sort by label (stable numbers) */
        size_t j = i;
        while (j > 0 && strcmp(s2_label[j - 1], s2_label[j]) > 0) {
            char tl[64], tp[512];
            strcpy(tl, s2_label[j]); strcpy(tp, s2_path[j]);
            strcpy(s2_label[j], s2_label[j - 1]); strcpy(s2_path[j], s2_path[j - 1]);
            strcpy(s2_label[j - 1], tl); strcpy(s2_path[j - 1], tp);
            j--;
        }
    }
    {
        /* v96.84: text subtitle tracks inside the MKV come first,
         * label "<lang>[ <name>][ forced] (MKV)" */
        unsigned m = 0, t;
        for (t = 0; t < mr_mkv_strack_count; t++) if (mr_mkv_stracks[t].kind) m++;
        if (m > MRB_SUB_FILES) m = MRB_SUB_FILES;
        if (m) {
            unsigned keep = s2_n + m > MRB_SUB_FILES ? MRB_SUB_FILES - m : s2_n, w = 0;
            for (i = keep; i-- > 0;) {
                strcpy(s2_label[i + m], s2_label[i]);
                strcpy(s2_path[i + m], s2_path[i]);
                s2_mkv[i + m] = 0;
            }
            for (t = 0; t < mr_mkv_strack_count && w < m; t++) {
                const mr_mkv_strack *st = &mr_mkv_stracks[t];
                char *l = s2_label[w];
                if (!st->kind) continue;
                snprintf(l, 64, "%s", st->lang[0] ? st->lang : "und");
                if (st->name[0]) { strncat(l, " ", 63 - strlen(l)); strncat(l, st->name, 63 - strlen(l)); }
                if (st->is_forced && !mrb_sub_is_forced(l)) strncat(l, " forced", 63 - strlen(l));
                strncat(l, " (MKV)", 63 - strlen(l));
                s2_path[w][0] = 0;
                s2_mkv[w] = st->number;
                w++;
            }
            s2_n = keep + m;
        }
    }
    return s2_n;
}

/* v96.84: a subtitle block of the chosen MKV track (mr_mkv.c) */
static void s2_mkv_block(uint64_t pts_us, uint64_t dur_us, const uint8_t *data,
                         size_t len, int kind)
{
    (void)mrb_sub_add((uint32_t)(pts_us / 1000u), (uint32_t)((pts_us + dur_us) / 1000u),
                      data, len, kind == MR_MKV_SUB_ASS);
}

/* Show entry k (1-based, 0 = none): an SRT file or an MKV track.
 * Returns the number of cues (an MKV track: 1, its cues come while the
 * film is read). */
static unsigned s2_activate(const char *film, int k, char *used, size_t used_size)
{
    mr_mkv_sub_track = 0;
    if (k < 1 || (unsigned)k > s2_n) { mrb_sub_clear(); return 0; }
    if (s2_mkv[k - 1]) {
        mrb_sub_clear();
        mr_mkv_sub_cb = s2_mkv_block;
        mr_mkv_sub_track = s2_mkv[k - 1];
        if (used && used_size) { strncpy(used, s2_label[k - 1], used_size - 1); used[used_size - 1] = 0; }
        return 1;
    }
    return mrb_sub_load(film, s2_path[k - 1], used, used_size);
}

static void p54_name(char *dst, size_t size, const char *film);
/* S2: only the S: line of the film's .t (the .t is read fully only with
 * RESUME). */
static void s2_read_t(const char *film)
{
    char name[512], line[96];
    FILE *f;
    p54_name(name, sizeof name, film);
    f = fopen(name, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if ((line[0] == 'S' || line[0] == 's') && (line[1] == ':' || line[1] == '=')) {
            strncpy(s2_t_label, line + 2, sizeof s2_t_label - 1);
            s2_t_label[sizeof s2_t_label - 1] = 0;
            s2_t_have = 1;
        }
    }
    fclose(f);
}

static const char *s2_audio_lang(void)
{
    if (mr_mkv_atrack_count && mr_mkv_audio_chosen >= 1 &&
        mr_mkv_audio_chosen <= mr_mkv_atrack_count)
        return mr_mkv_atracks[mr_mkv_audio_chosen - 1].lang;
    return "";
}

/* 1-based file to show, 0 = none.  *why: "cli", "t" or "auto". */
static int s2_choose(const char **why)
{
    unsigned i;
    if (s2_cli_n >= 0) {
        *why = "cli";
        return (s2_cli_n >= 1 && (unsigned)s2_cli_n <= s2_n) ? s2_cli_n : 0;
    }
    if (s2_t_have) {
        *why = "t";
        if (!strcmp(s2_t_label, "OFF")) return 0;
        for (i = 0; i < s2_n; i++) if (!strcmp(s2_label[i], s2_t_label)) return (int)i + 1;
    }
    *why = "auto";
    return mrb_sub_pick_default((const char (*)[64])s2_label, s2_n, s2_audio_lang());
}

static int p54_read(const char *film, uint64_t *t_us, int *have_t, long *window)
{
    char name[512], line[64];
    p55_audio_read[0] = 0;
    p70_t_have_fine = 0;
    mka_t_track = 0;
    s2_t_have = 0;
    FILE *f;
    p54_name(name, sizeof name, film);
    f = fopen(name, "r");
    if (!f) return 0;
    *have_t = 0; *window = -1;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if ((line[0] == 'T' || line[0] == 't') && (line[1] == ':' || line[1] == '=')) {
            if (!strcmp(line + 2, "0") || !strcmp(line + 2, "00:00:00")) { *t_us = 0; *have_t = 1; }
            else if (p50_parse_time(line + 2, t_us)) *have_t = 1;
        } else if ((line[0] == 'W' || line[0] == 'w') && (line[1] == ':' || line[1] == '=')) {
            if (line[2] == 'C' || line[2] == 'c') *window = 0;
            else {
                long w = strtol(line + 2, NULL, 10);
                if (w == 10 || w == 25 || w == 50 || w == 75) *window = w;
            }
        } else if ((line[0] == 'P' || line[0] == 'p') && (line[1] == ':' || line[1] == '=')) {
            p70_t_fine_ms = strtol(line + 2, NULL, 10);   /* P96W70 */
            p70_t_have_fine = 1;
        } else if ((line[0] == 'A' || line[0] == 'a') && (line[1] == 'T' || line[1] == 't') &&
                   (line[2] == ':' || line[2] == '=')) {
            unsigned long n = strtoul(line + 3, NULL, 10);   /* MKA1 */
            if (n >= 1 && n <= MKA_TRACK_MAX) mka_t_track = (unsigned)n;
        } else if ((line[0] == 'S' || line[0] == 's') && (line[1] == ':' || line[1] == '=')) {
            strncpy(s2_t_label, line + 2, sizeof s2_t_label - 1);   /* S2 */
            s2_t_label[sizeof s2_t_label - 1] = 0;
            s2_t_have = 1;
        } else if ((line[0] == 'A' || line[0] == 'a') && (line[1] == ':' || line[1] == '=')) {
            strncpy(p55_audio_read, line + 2, sizeof p55_audio_read - 1);
            p55_audio_read[sizeof p55_audio_read - 1] = 0;
        }
    }
    fclose(f);
    return 1;
}

static void p54_write(const char *film, uint64_t film_us, const char *audio,
                      uint64_t recap_us)
{
    char name[512], t[16];
    FILE *f;
    uint64_t at = film_us > recap_us ? film_us - recap_us : 0;
    p54_name(name, sizeof name, film);
    p50_fmt(t, sizeof t, at);
    f = fopen(name, "w");
    if (!f) {
        printf("Warning: cannot write resume file %s.\n", name);
        return;
    }
    fprintf(f, "T:%s\n", t);
    if (mr_berryvp_stream_flags) fprintf(f, "W:%lu\n", (unsigned long)mr_berryvp_stream_flags);
    else fprintf(f, "W:C\n");
    fprintf(f, "A:%s\n", audio);
    if (!strcmp(audio, "PAULA")) fprintf(f, "P:%ld\n", p70_fine_ms);   /* P96W70 */
    if (mr_mkv_atrack_count > 1 && mr_mkv_audio_chosen)
        fprintf(f, "AT:%u\n", mr_mkv_audio_chosen);                     /* MKA1 */
    if (s2_used_label) fprintf(f, "S:%s\n", s2_used_label);           /* S2 */
    else if (s2_t_have) fprintf(f, "S:%s\n", s2_t_label);             /* keep it */
    fclose(f);
    printf("Resume position saved: %s (T=%s).\n", name, t);
    if (n76_hostlog) {
        fprintf(n76_hostlog, "P54_T_WRITTEN stop_ms=%lu t=%s\n",
                (unsigned long)(film_us / 1000U), t);
        fflush(n76_hostlog);
    }
}


/* ---- P96W56: INFO mode ------------------------------------------------------
 * mrberry <film> INFO  - no Pi, no window, no status file, no logs.
 * Prints KEY=VALUE lines for MintVID-pi (timeline length, status line,
 * resume position) and returns 0 when mrberry can play the film, 5 when it
 * cannot (REASON=...), 10 when the file cannot be opened. */
extern uint64_t mr_demux_duration_us(const mr_demux *d);

/* MKA1: can this sound go over HDMI (CPU1 modules)?  *why = reason if not. */
static int mka_hdmi_ok(const mr_audio_info *ai, const char **why)
{
    *why = NULL;
    if (!ai || !ai->valid) { *why = "no sound"; return 0; }
    if (ai->format_tag == MR_AUDIO_FORMAT_AAC) {
        unsigned ch = ai->config_len >= 2 ? ((ai->config[1] >> 3) & 15u) : ai->channels;
        if (ai->config_len && (ai->config[0] >> 3) != 2) { *why = "only AAC-LC sound is supported"; return 0; }
        if (ch != 2 && ch != 5 && ch != 6) { *why = "AAC sound must be stereo or 5.1"; return 0; }
        return 1;
    }
    if (ai->format_tag == MR_AUDIO_FORMAT_AC3) return 1;
    if (ai->format_tag == MR_AUDIO_FORMAT_EAC3) {
        if (ai->channels != 2) { *why = "E-AC3 multichannel is not supported in real time"; return 0; }
        return 1;
    }
    *why = "this sound format plays only through Paula";
    return 0;
}

/* MKA1: Paula player (Darren's mrb_paula): AAC, MP3, MP2, AC3, PCM. */
static int mka_paula_ok(const mr_audio_info *ai)
{
    return ai && ai->valid &&
           (ai->format_tag == MR_AUDIO_FORMAT_AAC || ai->format_tag == MR_AUDIO_FORMAT_MP3 ||
            ai->format_tag == MR_AUDIO_FORMAT_MP2 || ai->format_tag == MR_AUDIO_FORMAT_AC3 ||
            ai->format_tag == MR_AUDIO_FORMAT_PCM);
}

/* MKA1c: for HDMI sound only tracks the CPU1 modules can play are chosen
 * (an MP3 track is skipped like Opus; with PAULA it stays usable). */
static int mka_usable_hdmi(const mr_audio_info *ai)
{
    const char *why;
    return mka_hdmi_ok(ai, &why);
}

static const char *mka_codec_short(const mr_mkv_atrack *t)
{
    if (t->supported) {
        switch (t->ai.format_tag) {
        case MR_AUDIO_FORMAT_AAC:  return "AAC";
        case MR_AUDIO_FORMAT_AC3:  return "AC3";
        case MR_AUDIO_FORMAT_EAC3: return "E-AC3";
        case MR_AUDIO_FORMAT_MP3:  return "MP3";
        case MR_AUDIO_FORMAT_MP2:  return "MP2";
        case MR_AUDIO_FORMAT_PCM:  return "PCM";
        default: break;
        }
    }
    return t->codec[0] == 'A' && t->codec[1] == '_' ? t->codec + 2 : t->codec;
}

/* MKA1: one line per audio track for MintVID-pi:
 *   TRACK<n>=<lang>|<codec>|<channels>|<rate>|<hdmi 0/1>|<paula 0/1>|<default 0/1>|<name>
 * (name last - it may contain anything but '|' and newlines). */
static void mka_info_tracks(void)
{
    unsigned i;
    printf("AUDIO_TRACKS=%u\n", mr_mkv_atrack_count);
    if (mr_mkv_audio_chosen) printf("AUDIO_TRACK=%u\n", mr_mkv_audio_chosen);
    for (i = 0; i < mr_mkv_atrack_count; i++) {
        const mr_mkv_atrack *t = &mr_mkv_atracks[i];
        const char *why;
        int hdmi = t->supported && mka_hdmi_ok(&t->ai, &why);
        int paula = t->supported && mka_paula_ok(&t->ai);
        printf("TRACK%u=%s|%s|%u|%lu|%d|%d|%d|%s\n", i + 1, t->lang, mka_codec_short(t),
               (unsigned)t->ai.channels, (unsigned long)t->ai.sample_rate,
               hdmi, paula, t->is_default, t->name);
    }
}

static int p56_info(const char *film)
{
    mr_demux *d;
    const mr_video_info *vi;
    const mr_audio_info *ai;
    char text[96];
    const char *reason = NULL, *sound = "NONE";
    uint64_t dur;
    uint64_t r_t = 0;
    int r_have = 0, r_have_t = 0;
    long r_w = -1;

    printf("MRBERRY_INFO=1\nFILE=%s\n", film);
    {
        /* P96W78a: stack this INFO run got (check for MintVID-pi's call). */
        struct Task *me = FindTask(NULL);
        printf("STACK=%lu\n", (unsigned long)((ULONG)me->tc_SPUpper -
                                              (ULONG)me->tc_SPLower));
    }
    /* MKA1: the .t first, so the saved audio track is the one reported */
    r_have = p54_read(film, &r_t, &r_have_t, &r_w);
    if (r_have && mka_t_track && !mka_cli_track) mr_mkv_audio_want = mka_t_track;
    d = mr_demux_open_file(film);
    if (!d) {
        printf("PLAYABLE=0\nREASON=cannot open file (%s)\n", mr_demux_last_open_error());
        return 10;
    }
    vi = mr_demux_video(d);
    ai = mr_demux_audio(d);
    printf("CONTAINER=%s\n", mr_demux_container_name(d));
    mr_demux_describe_video_codec(d, text, sizeof text);
    printf("VIDEO=%s\n", text);
    if (vi && vi->valid) {
        printf("SIZE=%dx%d\n", vi->width, vi->height);
        if (vi->rate && vi->scale) {
            uint64_t milli = (uint64_t)vi->rate * 1000U / vi->scale;
            printf("FPS=%lu.%03lu\n", (unsigned long)(milli / 1000U),
                   (unsigned long)(milli % 1000U));
        }
    }
    if (!vi || !vi->valid || vi->fourcc != MR_FOURCC('a','v','c','1'))
        reason = "H.264/AVC video is required";
    dur = mr_demux_duration_us(d);
    if (dur) {
        char len[16];
        p50_fmt(len, sizeof len, dur);
        printf("LENGTH=%s\nLENGTH_MS=%lu\n", len, (unsigned long)(dur / 1000U));
    } else {
        printf("LENGTH=unknown\n");
    }
    mr_demux_describe_audio_codec(d, text, sizeof text);
    printf("AUDIO=%s\n", (ai && ai->valid) ? text : "none");
    if (ai && ai->valid) {
        const char *why;
        printf("AUDIO_RATE=%lu\nAUDIO_CH=%u\n",
               (unsigned long)ai->sample_rate, (unsigned)ai->channels);
        if (mka_hdmi_ok(ai, &why)) sound = "HDMI";
        else if (why) printf("AUDIO_WHY=%s\n", why);
        /* v96.87: the sound is no longer a REASON here - MintVID-pi decides
         * with AUDIO_HAS / AUDIO_PAULA / SOUND for the chosen display */
    }
    printf("SOUND=%s\n", sound);
    {
        /* v96.87: AUDIO_HAS = the film has any audio track (also one we
         * cannot play), AUDIO_NAME = short codec name for the hint */
        char nm[48];
        int has = (ai && ai->valid) || mr_mkv_atrack_count > 0;
        nm[0] = 0;
        if (ai && ai->valid) {
            char *q;
            strncpy(nm, text, sizeof nm - 1); nm[sizeof nm - 1] = 0;
            q = strstr(nm, " (fourcc");
            if (q) *q = 0;
        } else if (mr_mkv_atrack_count > 0) {
            unsigned i;
            for (i = 0; i < mr_mkv_atrack_count && i < 4; i++) {
                size_t l = strlen(nm);
                snprintf(nm + l, sizeof nm - l, "%s%s", i ? ", " : "",
                         mka_codec_short(&mr_mkv_atracks[i]));
            }
        }
        printf("AUDIO_HAS=%d\nAUDIO_PAULA=%d\nAUDIO_NAME=%s\n", has,
               mka_paula_ok(ai) ? 1 : 0, nm);
    }
    printf("SEEK=%d\n", mr_demux_can_seek(d) ? 1 : 0);
    mka_info_tracks();                          /* MKA1 */
    {
        /* S2: subtitle files: SUBS=n, SUB<i>=label|forced, SUB_CHOSEN=k */
        const char *why;
        unsigned i;
        int k;
        s2_scan(film);
        k = s2_choose(&why);
        printf("SUBS=%u\n", s2_n);
        for (i = 0; i < s2_n; i++)
            printf("SUB%u=%s|%d\n", i + 1, s2_label[i], mrb_sub_is_forced(s2_label[i]));
        if (s2_n) printf("SUB_CHOSEN=%d\nSUB_WHY=%s\n", k, why);
    }
    if (r_have) {
        char t[16];
        p50_fmt(t, sizeof t, r_have_t ? r_t : 0);
        printf("RESUME_T=%s\n", t);
        if (r_w == 0) printf("RESUME_W=C\n");
        else if (r_w > 0) printf("RESUME_W=%ld\n", r_w);
        if (p55_audio_read[0]) printf("RESUME_A=%s\n", p55_audio_read);
        if (p70_t_have_fine) printf("RESUME_P=%ld\n", p70_t_fine_ms);   /* P96W72 */
        if (mka_t_track) printf("RESUME_AT=%u\n", mka_t_track);         /* MKA1 */
    }
    mr_demux_close(d);
    printf("PLAYABLE=%d\n", reason ? 0 : 1);
    if (reason) printf("REASON=%s\n", reason);
    return reason ? 5 : 0;
}

int main(int argc, char **argv)
{
    {
        long v971_rc;                          /* v97.1 */
        if (v971_relaunch(&v971_rc)) return (int)v971_rc;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if ((argc == 3 || argc == 4) &&
        (!strcmp(argv[2], "INFO") || !strcmp(argv[2], "info") ||
         !strcmp(argv[2], "Info"))) {
        /* MKA1: "mrberry <film> INFO AUDIO=n" = info for that audio track */
        if (argc == 4) {
            unsigned long n = 0;
            if (!strncmp(argv[3], "AUDIO=", 6) || !strncmp(argv[3], "AUDIO:", 6))
                n = strtoul(argv[3] + 6, NULL, 10);
            if (n < 1 || n > MKA_TRACK_MAX) {
                printf("Error: use mrberry <film> INFO [AUDIO=n]\n");
                return 5;
            }
            mr_mkv_audio_want = (unsigned)n;
            mka_cli_track = 1;
        }
        return p56_info(argv[1]);
    }
#if MR_DEBUG_LOGS
    n76_hostlog = fopen("N76_HOST.TXT", "ab");
#endif
    n76_hlog("N76_START");
    DBG_PRINTF("[N76] mrberry HOST-DEMUX trace on N75 scheduler base\n");
    DBG_PRINTF("[H4F] BerryVP context in BSS: %lu bytes @ %p\n", (unsigned long)sizeof g_bvp, (void *)&g_bvp);
    mr_demux *d;
    const mr_video_info *vi;
    const mr_audio_info *ai;
    mr_packet pkt;
    mr_berryvp *bvp = &g_bvp;
    unsigned long seen = 0, sent = 0, skipped = 0;
    unsigned long audio_seen = 0, audio_sent = 0;
    /* P32: FAST06 E-AC3 natural-EOF drain clock, merged into the
     * hardware-confirmed P06 ESC/status host. */
    uint64_t eac3_first_pts_us = 0, eac3_last_pts_us = 0;
    uint32_t eac3_first_wall_us = 0;
    int eac3_pts_clock_valid = 0;
    int ac309i_is_mov = 0;
    ac309i_pacer ac309i_pace;
    int audio_enabled = 0;
    int n73_prime_ready = 0;
    int p04_first_video_sent = 0;
    int aborted = 0;
    int p54_demux_eof = 0, p54_can_seek = 0, p54_have_stop = 0;
    uint64_t p54_stop_us = 0;
    mr_status st;

    {
        /* P96W50: options in any order after the film:
         *   W=10|25|50|75 (or the bare number, first one) = P96 window size
         *   T=H:MM or T=H:MM:SS                           = start time
         *   AV=<ms> (or a second bare number), NOSYNC, PIDLE0 = test switches
         * P96W52: '=' and ':' are both accepted after W, T and AV. */
        int i, have_window = 0, bad = (argc < 2), resume = 0, cli_t = 0;
        for (i = 2; i < argc && !bad; i++) {
            const char *a = argv[i];
            if (!a[0]) { bad = 1; break; }
            if ((a[0] == 'T' || a[0] == 't') && (a[1] == ':' || a[1] == '=')) {
                if (!p50_parse_time(a + 2, &p50_t_us)) {
                    printf("Error: start time must be T=hours:minutes, e.g. T=0:01 (1 minute) or T=1:30 (1 h 30 min); T=1:30:15 with seconds.\n");
                    return 5;
                }
                p50_t_set = p50_t_us != 0;
                cli_t = 1;
            } else if (!strcmp(a, "R") || !strcmp(a, "r") ||
                       !strcmp(a, "RESUME") || !strcmp(a, "resume") || !strcmp(a, "Resume")) {
                resume = 1;                       /* P96W54: continue from <film>.t */
            } else if ((a[0] == 'W' || a[0] == 'w') && (a[1] == ':' || a[1] == '=') &&
                       (a[2] == 'C' || a[2] == 'c') && !a[3]) {
                /* MintVID: W=C = full screen (cinema), even with R and a
                 * W: line in the .t.  The GUI uses "R W=C" to take a film
                 * from the window to full screen at the same spot. */
                mr_berryvp_stream_flags = 0;
                have_window = 1;
            } else if (((a[0] == 'W' || a[0] == 'w') && (a[1] == ':' || a[1] == '=')) ||
                       (!have_window && a[0] >= '0' && a[0] <= '9')) {
                /* Optional window size: play in a P96 window.  Without it
                 * mrberry stays in the cinema/HVS mode. */
                const char *w = (a[1] == ':' || a[1] == '=') ? a + 2 : a;
                unsigned long pct = strtoul(w, NULL, 10);
                if (pct == 10 || pct == 25 || pct == 50 || pct == 75) {
                    mr_berryvp_stream_flags = pct;
                    have_window = 1;
                } else {
                    printf("Error: window size must be W=10, W=25, W=50 or W=75.\n");
                    return 5;
                }
            } else if ((a[0] == 'L' || a[0] == 'l') && (a[1] == 'E' || a[1] == 'e') &&
                       (a[2] == 'A' || a[2] == 'a') && (a[3] == 'D' || a[3] == 'd') &&
                       (a[4] == '=' || a[4] == ':')) {
                p66_lead_ms = strtoul(a + 5, NULL, 10);   /* P96W66 test */
                if (p66_lead_ms < 100 || p66_lead_ms > 3000) {
                    printf("Error: LEAD= must be 100..3000 (milliseconds).\n");
                    return 5;
                }
            } else if (!strcmp(a, "PAULA") || !strcmp(a, "paula") || !strcmp(a, "Paula")) {
                p67_paula = 1;                    /* P96W67: sound through Paula */
            } else if (!strcmp(a, "STILL") || !strcmp(a, "still") || !strcmp(a, "Still")) {
                p79_still = 1;                    /* P96W79: still picture (Paula pause) */
                p96s_off = 1;                     /* no clock queries, draw at once */
            } else if (!strcmp(a, "NOSUB") || !strcmp(a, "nosub")) {
                s1_nosub = 1;                     /* S1: no subtitles */
            } else if (!strncmp(a, "SUB=", 4) || !strncmp(a, "sub=", 4)) {
                /* S2: SUB=n (file n of the film), SUB=OFF, or SUB=<file> */
                if (!strcmp(a + 4, "OFF") || !strcmp(a + 4, "off")) s2_cli_n = 0;
                else if (a[4] >= '1' && a[4] <= '9' && strtoul(a + 4, NULL, 10) <= MRB_SUB_FILES &&
                         a[4 + strspn(a + 4, "0123456789")] == 0)
                    s2_cli_n = (int)strtoul(a + 4, NULL, 10);
                else s1_name_arg = a + 4;         /* S1: subtitle file */
            } else if (!strncmp(a, "OSDTEST", 7) || !strncmp(a, "osdtest", 7)) {
                /* O1/O2 test: OSD picture on a second layer (cinema).
                 * OSDTEST, OSDTEST=MIX, OSDTEST=MOVE, OSDTEST=MIXMOVE */
                int mix = strstr(a, "MIX") || strstr(a, "mix");
                int move = strstr(a, "MOVE") || strstr(a, "move");
                int count = strstr(a, "COUNT") || strstr(a, "count");
                if (!o1_build(mix, move, count)) {
                    printf("Error: no memory for the OSD test picture.\n");
                    return 10;
                }
            } else if (!strncmp(a, "ISPOUT=", 7) || !strncmp(a, "ispout=", 7)) {
                /* N84 test: ISPOUT=OFF (no N83 upscale) or ISPOUT=WxH */
                unsigned long w = 0, h = 0;
                char *x = NULL;
                if (!strcmp(a + 7, "OFF") || !strcmp(a + 7, "off")) {
                    mr_berryvp_isp_out = 0xFFFFFFFFUL;
                } else {
                    w = strtoul(a + 7, &x, 10);
                    if (x && (*x == 'x' || *x == 'X')) h = strtoul(x + 1, NULL, 10);
                    if (w < 128 || w > 1920 || h < 64 || h > 1088 || (w & 15) || (h & 1)) {
                        printf("Error: ISPOUT=OFF or ISPOUT=WxH (W 128..1920, multiple of 16; H 64..1088, even).\n");
                        return 5;
                    }
                    mr_berryvp_isp_out = (w << 16) | h;
                }
            } else if (!strcmp(a, "UPSCALE") || !strcmp(a, "upscale")) {
                mr_berryvp_isp_out = 0;          /* MintVID: N83 back on */
            } else if (!strcmp(a, "ISP") || !strcmp(a, "isp")) {
                mr_berryvp_force_isp = 1;        /* ISP82 test: picture via ISP */
            } else if (!strcmp(a, "PLOW") || !strcmp(a, "plow")) {
                p67_low = 1;                      /* P96W67: Paula lower rate */
            } else if (!strcmp(a, "PMONO") || !strcmp(a, "pmono")) {
                p67_mono = 1;                     /* P96W67: Paula mono */
            } else if (!strcmp(a, "NOAUDIO") || !strcmp(a, "noaudio") ||
                       !strcmp(a, "NoAudio")) {
                p63_noaudio = 1;                  /* P96W63 test: cinema, no sound */
            } else if ((a[0] == 'N' || a[0] == 'n') && (a[1] == 'O' || a[1] == 'o')) {
                p96s_off = 1;                     /* NOSYNC: no clock, draw at once */
            } else if ((a[0] == 'P' || a[0] == 'p') && (a[1] == 'I' || a[1] == 'i')) {
                p45_no_idle = 1;                  /* P96W46 test: PIDLE0 */
            } else if ((a[0] == 'R' || a[0] == 'r') && (a[1] == 'E' || a[1] == 'e') &&
                       (a[2] == 'A' || a[2] == 'a') && (a[3] == 'D' || a[3] == 'd') &&
                       (a[4] == '=' || a[4] == ':')) {
                /* P96W77: READ=KB, size of one file read (Darren's --read) */
                unsigned long kb = strtoul(a + 5, NULL, 10);
                if (kb < 64 || kb > 1024) {
                    printf("Error: READ= must be 64..1024 (KB).\n");
                    return 5;
                }
                mrb_source_set_read_kb((unsigned)kb);
            } else if (a[0] == '-' && a[1] == '-') {
                /* P96W77: the option names MintVID (Darren's GUI) passes,
                 * docs/EXTERNAL_PLAYER.md.  Unknown --options are an error
                 * (before W77 they were silently read as AV=0). */
                if (!strcmp(a, "--paula")) p67_paula = 1;
                else if (!strcmp(a, "--upscale")) mr_berryvp_isp_out = 0;
                else if (!strcmp(a, "--audio-rate=low")) p67_low = 1;
                else if (!strncmp(a, "--audio-rate=", 13)) { /* normal: default */ }
                else if (!strcmp(a, "--audio-mono")) p67_mono = 1;
                else if (!strcmp(a, "--no-audio")) p63_noaudio = 1;
                else if (!strncmp(a, "--audio-track=", 14)) {   /* MKA1 */
                    unsigned long n = strtoul(a + 14, NULL, 10);
                    if (n < 1 || n > 16) {
                        printf("Error: --audio-track must be 1..16.\n");
                        return 5;
                    }
                    mr_mkv_audio_want = (unsigned)n;
                    mka_cli_track = 1;
                }
                else if (!strncmp(a, "--start=", 8)) {
                    if (!p50_parse_time(a + 8, &p50_t_us)) {
                        printf("Error: --start must be H:MM or H:MM:SS.\n");
                        return 5;
                    }
                    p50_t_set = p50_t_us != 0;
                    cli_t = 1;
                } else if (!strncmp(a, "--read=", 7)) {
                    unsigned long kb = strtoul(a + 7, NULL, 10);
                    if (kb < 64 || kb > 1024) {
                        printf("Error: --read must be 64..1024 (KB).\n");
                        return 5;
                    }
                    mrb_source_set_read_kb((unsigned)kb);
                } else {
                    printf("Error: unknown option %s\n", a);
                    bad = 1;
                }
            } else if (!strncmp(a, "AUDIO=", 6) || !strncmp(a, "AUDIO:", 6) ||
                       !strncmp(a, "audio=", 6)) {
                /* MKA1: audio track, 1-based (MKV only) */
                unsigned long n = strtoul(a + 6, NULL, 10);
                if (n < 1 || n > 16) {
                    printf("Error: AUDIO= must be 1..16 (audio track number).\n");
                    return 5;
                }
                mr_mkv_audio_want = (unsigned)n;
                mka_cli_track = 1;
            } else if (((a[0] == 'A' || a[0] == 'a') && (a[1] == 'V' || a[1] == 'v') &&
                        (a[2] == ':' || a[2] == '=')) ||
                       (a[0] == '-' && a[1] >= '0' && a[1] <= '9') ||
                       (a[0] >= '0' && a[0] <= '9')) {
                /* P96W26: AV:<ms> = how much later window frames are shown */
                int av = (a[0] == 'A' || a[0] == 'a');
                long ms = strtol(av ? a + 3 : a, NULL, 10);
                if (ms > 2000 || ms < -2000) {
                    printf("Error: AV= must be -2000..2000 (milliseconds).\n");
                    return 5;
                }
                p96w_av_delay_us = ms * 1000L;
                p69_av_set = 1;
            } else {
                bad = 1;
            }
        }
        if (bad) {
            printf("mrberry - H.264 hardware A/V player\n"
                   "Usage: mrberry <movie.mkv|movie.mp4|movie.ts> [W=10|25|50|75] [T=h:mm] [R] [PAULA [PLOW] [PMONO]] [NOAUDIO] [READ=KB] [AV=ms|NOSYNC]\n"
                   "  T=0:01 = start at 1 minute, T=1:30 = 1 h 30 min (T=1:30:15 with seconds)\n"
                   "  R = continue where the film was stopped (<film>.t); W=C = full screen\n"
                   "  UPSCALE = scale films below 1280x720 up to 1920x1080 on the Pi\n"
                   "    (needs more GPU memory than gpu_mem=64)\n"
                   "  PAULA = sound through the Amiga (PLOW lower rate, PMONO mono)\n"
                   "  READ=64..1024 = KB per file read (default 64, bigger for network shares)\n"
                   "  AUDIO=n = audio track n (MKV; see INFO for the list)\n"
                   "  Also understood (MintVID): --paula --audio-rate=low --audio-mono\n"
                   "    --no-audio --start=H:MM:SS --read=KB\n"
                   "  mrberry <movie> INFO = show length, video and sound, no playback\n");
            return 5;
        }
        if (resume) {
            uint64_t rt = 0;
            int have_t = 0;
            long rw = -1;
            if (p54_read(argv[1], &rt, &have_t, &rw)) {
                if (!cli_t && have_t) { p50_t_us = rt; p50_t_set = rt != 0; }
                if (!have_window && rw >= 0) mr_berryvp_stream_flags = (ULONG)rw;
                if (!mka_cli_track && mka_t_track)
                    mr_mkv_audio_want = mka_t_track;   /* MKA1: same track as before */
                printf("Resume from saved position (%s).\n",
                       mr_berryvp_stream_flags ? "window" : "cinema");
                n76_hlog("P54_RESUME_FILE_READ");
                if (p55_audio_read[0] && (p55_audio_read[0] == 'P' || p55_audio_read[0] == 'p') &&
                    !p63_noaudio && !mr_berryvp_stream_flags)
                    p67_paula = 1;                /* P96W70: continue with Paula */
                if (n76_hostlog) {
                    fprintf(n76_hostlog, "P55_T_AUDIO=%s\n",
                            p55_audio_read[0] ? p55_audio_read : "(none)");
                    fflush(n76_hostlog);
                }
            } else {
                printf("No saved position for this film - starting from the beginning.\n");
                n76_hlog("P54_RESUME_NO_FILE");
            }
        }
    }
    p77_port_open();                      /* P96W77: MintVID.player */
    p62_pos_clear();
    p06_status_write(P06_STATE_OPENING, "H.264 / VideoCore",
                     "Opening PiStorm VideoCore playback...");
    /* MKA1c: HDMI sound -> only tracks the Pi modules can play */
    if (!p67_paula && !p63_noaudio && !p79_still)
        mr_mkv_audio_usable = mka_usable_hdmi;
    DBG_PRINTF("[D01] before mr_demux_open_file\n");
    d = mr_demux_open_file(argv[1]);
    DBG_PRINTF("[D02] after mr_demux_open_file ptr=%p\n", (void *)d);
    if (!d) {
        {
            BPTR v971_lock = Lock((STRPTR)argv[1], ACCESS_READ);   /* v97.1 */
            if (v971_lock) {
                UnLock(v971_lock);
                printf("Error: cannot open media: %s\n", mr_demux_last_open_error());
            } else {
                printf("Error: file not found: %s\n", argv[1]);
            }
        }
        p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                         "Could not open media file");
        return 10;
    }
    s4_seek_ok = mr_demux_can_seek(d);             /* v96.85: Tab */
    s4_len_us = mr_demux_duration_us(d);
    if (n76_hostlog && mr_mkv_chapter_count) {
        fprintf(n76_hostlog, "S4_CHAPTERS=%u\n", mr_mkv_chapter_count);
        fflush(n76_hostlog);
    }
    DBG_PRINTF("[D03] before mr_demux_video\n");
    vi = mr_demux_video(d);
    DBG_PRINTF("[D04] after mr_demux_video ptr=%p\n", (void *)vi);
    if (!vi || !vi->valid || vi->fourcc != MR_FOURCC('a','v','c','1')) {
        char codec[80];
        mr_demux_describe_video_codec(d, codec, sizeof codec);
        printf("Error: H.264/AVC video is required (found: %s).\n", codec);
        p06_status_write(P06_STATE_UNSUPPORTED, codec,
                         "H.264/AVC video is required");
        mr_demux_close(d);
        return 10;
    }
    DBG_PRINTF("MintVID demux: %s, H.264 %dx%d, config=%lu bytes\n",
           mr_demux_container_name(d), vi->width, vi->height,
           (unsigned long)vi->config_len);
    ac309i_is_mov = !strcmp(mr_demux_container_name(d), "MOV");
    ai = mr_demux_audio(d);
    if (mr_mkv_atrack_count) {                            /* MKA1 */
        if (mr_mkv_audio_want_bad)
            printf("Warning: audio track %u is missing or cannot be played%s - using track %u.\n",
                   mr_mkv_audio_want, mr_mkv_audio_usable ? " over HDMI" : "",
                   mr_mkv_audio_chosen);
        if (mr_mkv_atrack_count > 1 && mr_mkv_audio_chosen)
            printf("Audio track %u of %u (%s).\n", mr_mkv_audio_chosen, mr_mkv_atrack_count,
                   mr_mkv_atracks[mr_mkv_audio_chosen - 1].lang);
        if (n76_hostlog) {
            fprintf(n76_hostlog, "MKA1_AUDIO_TRACK want=%u chosen=%u of=%u bad=%d\n",
                    mr_mkv_audio_want, mr_mkv_audio_chosen, mr_mkv_atrack_count,
                    mr_mkv_audio_want_bad);
            fflush(n76_hostlog);
        }
    }
    if (p63_noaudio) {
        if (mr_berryvp_stream_flags) {
            printf("Error: NOAUDIO is a cinema test - leave out W=.\n");
            mr_demux_close(d);
            return 5;
        }
        printf("NOAUDIO: playing without sound (clock test CLK81).\n");
        n76_hlog("P63_NOAUDIO");
        ai = NULL;
    }
    if (p79_still) {
        if (!mr_berryvp_stream_flags || p67_paula || p63_noaudio) {
            printf("Error: STILL needs a window (W=50) and no PAULA/NOAUDIO.\n");
            mr_demux_close(d);
            return 5;
        }
        n76_hlog("P79_STILL");
        ai = NULL;                        /* picture only, no sound at all */
    }
    if (p67_paula && !p63_noaudio) {
        char why[80];
        if (mr_berryvp_stream_flags) {
            printf("Error: PAULA is cinema only - leave out W=.\n");
            mr_demux_close(d);
            return 5;
        }
        why[0] = 0;
        atexit(p67_cleanup);
        p73_base_load();
        /* P96W70: fine value: AV= (Shell) > .t P: > 0 */
        if (p69_av_set) p70_fine_ms = (long)(p96w_av_delay_us / 1000L);
        else {
            uint64_t rt = 0; int ht = 0; long rw = -1;
            p70_t_have_fine = 0;
            if (p54_read(argv[1], &rt, &ht, &rw) && p70_t_have_fine)
                p70_fine_ms = p70_t_fine_ms;
        }
        if (p70_fine_ms > 3000) p70_fine_ms = 3000;
        if (p70_fine_ms < -3000) p70_fine_ms = -3000;
        p67_pa = (ai && ai->valid) ?
            mrb_paula_open(ai, p67_low, p67_mono,
                           (int32_t)(P69_PAULA_DEFAULT_AV_US + p70_fine_ms * 1000L),
                           why, sizeof why) : NULL;
        if (p67_pa) {
            p70_used = 1;
            printf("PAULA: sound offset %ld ms (base %ld + fine %ld; +/- keys change it).\n",
                   (long)(P69_PAULA_DEFAULT_AV_US / 1000L) + p70_fine_ms,
                   (long)(P69_PAULA_DEFAULT_AV_US / 1000L), p70_fine_ms);
        }
        if (p67_pa) {
            printf("PAULA: %s (sound through the Amiga).\n", mrb_paula_describe(p67_pa));
            if (n76_hostlog) { fprintf(n76_hostlog, "P67_PAULA_OPEN %s offset_ms=%ld\n",
                               mrb_paula_describe(p67_pa),
                               (long)(P69_PAULA_DEFAULT_AV_US / 1000L) + p70_fine_ms); fflush(n76_hostlog); }
        } else {
            printf("PAULA: no sound (%s) - picture only.\n",
                   (ai && ai->valid) ? why : "no audio track");
            n76_hlog("P67_PAULA_FAILED");
        }
    }
    if (ai && ai->valid) {
        char acodec[80];
        mr_demux_describe_audio_codec(d, acodec, sizeof acodec);
        DBG_PRINTF("MintVID audio: %s, %lu Hz, %u ch, config=%u bytes\n",
               acodec, (unsigned long)ai->sample_rate, (unsigned)ai->channels,
               (unsigned)ai->config_len);
    } else {
        DBG_PRINTF("MintVID audio: no usable audio track\n");
    }
    DBG_PRINTF("[N75] Video PTS enabled; natural A/V interleave, scheduler owns presentation time.\n");
    fflush(stdout);

    /* AC308N3 realtime policy: do not even start the BerryVP graph for
     * multichannel E-AC3.  The current CPU1 scalar path decodes it correctly
     * but measured far below realtime; stereo E-AC3 remains supported. */
    if (ai && ai->valid && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && ai->channels != 2) {
        if (ai->channels == 6)
            printf("Error: E-AC3 5.1 audio is not supported for real-time playback. Stereo E-AC3 only.\n");
        else
            printf("Error: E-AC3 multichannel audio (%u channels) is not supported for real-time playback. Stereo E-AC3 only.\n",
                   (unsigned)ai->channels);
        p06_status_write(P06_STATE_UNSUPPORTED, "E-AC3",
                         "Multichannel E-AC3 is not supported in real time");
        mr_demux_close(d);
        return 20;
    }

    if (p50_t_set) {
        char want[16], got[16];
        p50_fmt(want, sizeof want, p50_t_us);
        if (!mr_demux_can_seek(d)) {
            printf("Error: T= start time works only with MKV and MP4 files.\n");
            p06_status_write(P06_STATE_UNSUPPORTED, "H.264 / VideoCore",
                             "T= start time needs an MKV or MP4 file");
            mr_demux_close(d);
            return 10;
        }
        {
            /* P96W53: T= must lie inside the film */
            extern uint64_t mr_demux_duration_us(const mr_demux *d);
            uint64_t dur = mr_demux_duration_us(d);
            if (n76_hostlog) {
                fprintf(n76_hostlog, "P53_DURATION_MS=%lu\n", (unsigned long)(dur / 1000U));
                fflush(n76_hostlog);
            }
            if (dur && p50_t_us >= dur) {
                char len[16], text[96];
                p50_fmt(len, sizeof len, dur);
                printf("Error: T=%s is behind the end of the film (length %s).\n", want, len);
                snprintf(text, sizeof text, "Start time is behind the end (film length %s)", len);
                p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore", text);
                mr_demux_close(d);
                return 10;
            }
        }
        p06_status_write(P06_STATE_OPENING, "H.264 / VideoCore",
                         "Searching start position...");
        st = mr_demux_seek(d, p50_t_us, &p50_seek_us);
        if (st != MR_OK) {
            printf("Error: cannot find a start picture for T=%s.\n", want);
            p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                             "Start time not found in the file");
            mr_demux_close(d);
            return 10;
        }
        p50_fmt(got, sizeof got, p50_seek_us);
        printf("Start at %s (T=%s, nearest key frame before).\n", got, want);
        if (n76_hostlog) {
            fprintf(n76_hostlog, "P50_SEEK T_MS=%lu SEEK_MS=%lu\n",
                    (unsigned long)(p50_t_us / 1000U), (unsigned long)(p50_seek_us / 1000U));
            fflush(n76_hostlog);
        }
    }

    /* S1: <film>.srt next to the film -> subtitles on the OSD layer
     * (cinema only; not with OSDTEST, STILL or NOSUB). */
    if (!s1_nosub && !mr_berryvp_osd_req && !mr_berryvp_stream_flags && !p79_still) {
        static char s1_used[512];
        unsigned n = 0;
        if (s1_name_arg) {
            n = mrb_sub_load(argv[1], s1_name_arg, s1_used, sizeof s1_used);
        } else {
            /* S2: choose among <film>.srt / <film>.<label>.srt */
            const char *why = "auto";
            int k;
            s2_scan(argv[1]);
            if (s2_n && s2_cli_n < 0 && !s2_t_have) s2_read_t(argv[1]);
            k = s2_n ? s2_choose(&why) : 0;
            if (s2_n && strcmp(why, "auto"))   /* chosen by the user: keep it */
                s2_used_label = k ? s2_label[k - 1] : "OFF";
            if (k) n = s2_activate(argv[1], k, s1_used, sizeof s1_used);
            /* v96.83: with SRT files the OSD layer is made also for Off,
             * so key S can switch subtitles on while the film plays */
            if (s2_n && mrb_sub_buffers()) {
                s3_film = argv[1];
                s3_cur = n ? k : 0;
                if (!n) { n = 1; strcpy(s1_used, "(off - key S)"); }
            }
            if (n76_hostlog) {
                fprintf(n76_hostlog, "S2_SUBS files=%u chosen=%d why=%s lang=%s\n",
                        s2_n, k, why, s2_audio_lang());
                fflush(n76_hostlog);
            }
        }
        if (n) {
            o1_block[0] = 0x4F534432UL;                 /* 'OSD2' */
            o1_block[1] = MRB_SUB_W;
            o1_block[2] = MRB_SUB_H;
            o1_block[3] = 0;
            o1_block[4] = (uint32_t)(unsigned long)mrb_sub_pixels();   /* transparent */
            o1_block[5] = 0xFFFFFFFFUL;
            o1_block[6] = 0xFFFFFFFFUL;
            o1_block[7] = 0;
            mr_berryvp_osd_req = o1_block;
            s1_on = 1;
            printf("Subtitles: %s (%u).\n", s1_used, mrb_sub_count());
        }
        if (n76_hostlog) {
            fprintf(n76_hostlog, "S1_SRT cues=%u file=%s\n", n, s1_used);
            fflush(n76_hostlog);
        }
    }
    DBG_PRINTF("[D05] before mr_berryvp_open\n");
    st = mr_berryvp_open(bvp, vi);
    DBG_PRINTF("[D06] after mr_berryvp_open st=%ld\n", (long)st);
    if (st != MR_OK) {
        p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                         "Could not open BerryVP video graph");
        mr_demux_close(d);
        return 20;
    }
    if (ac309i_is_mov) {
        /* VPv149 hardware state: MP4 H.264 feed fragments are 128 KiB.
         * This is independent of presentation PTS and of the host-time gate. */
        bvp->video_chunk_limit = 0x00020000UL;
        DBG_PRINTF("[AC309I] MOV/MP4 H.264 fragment cap: 128 KiB (VPv149 proven)\n");
        n76_hlog("AC309I_MP4_128K_FRAGMENT_CAP");
    }

    if (!p67_paula && ai && ai->valid && (ai->format_tag == MR_AUDIO_FORMAT_AAC || ai->format_tag == MR_AUDIO_FORMAT_AC3 || ai->format_tag == MR_AUDIO_FORMAT_EAC3)) {
        st = mr_berryvp_config_audio(bvp, ai);
        if (st == MR_OK) {
            audio_enabled = 1;
            n73_prime_ready = 1; /* N75: clock/scheduler already armed in ConfigAudio; no host-side video hold. */
            if (ai->format_tag == MR_AUDIO_FORMAT_AC3)
                DBG_PRINTF("[AC307A] AC3/berry_ac3.module audio armed; N97QD host + N79Q library path\n");
            else if (ai->format_tag == MR_AUDIO_FORMAT_EAC3)
                DBG_PRINTF("[AC308N3] E-AC3 stereo/berry_eac3.module audio armed; stereo HDMI renderer\n");
            else
                DBG_PRINTF("[A03] AAC/BerryFAAD03 audio armed; N75 natural scheduler feed enabled\n");
            n76_hlog("N76_AFTER_CONFIG_AUDIO_OK");
        } else {
            printf("Error: audio setup failed; playback stopped.\n");
            p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                             "Audio setup failed");
            mr_berryvp_close(bvp);
            mr_demux_close(d);
            return 20;
        }
    } else {
        if (!P67_FREE)
            printf("Warning: no supported AAC/AC3/E-AC3 audio track; video will continue without audio.\n");
        /* MKA1b: cinema without HDMI sound (MP3/DTS/Opus track, HE-AAC mono,
         * or no audio at all) = exactly the tested NOAUDIO case: the library
         * starts the free Pi clock (CLK81), so mrberry must feed in real time
         * with the host pacer (W65/W66).  Before, frames were sent unpaced
         * and the Pi stopped (still picture, Esc dead) - as in W63/W64. */
        if (!mr_berryvp_stream_flags && !P67_FREE) {
            p63_noaudio = 1;
            n76_hlog("MKA1B_CINEMA_WITHOUT_HDMI_SOUND_AS_NOAUDIO");
        }
        n73_prime_ready = 1;
    }

    /* P96W2: in the P96 window the library shows each frame as soon as it
     * comes back from the ISP, and its own host pacer holds SendVideo to real
     * time.  Without a host-side video queue the demux can never run ahead,
     * so every 16-frame AAC batch reached audio_render ~0.37 s late and was
     * dropped (P96W1 hardware log).  Reuse the proven AC309I queue: video AUs
     * wait here until due, the demux keeps reading, and audio gets ahead until
     * the library's audio slots push back. */
    ac309i_pacer_init(&ac309i_pace, vi,
                      ((ac309i_is_mov || mr_berryvp_stream_flags) && audio_enabled) ||
                      P67_FREE);         /* P96W65: no HDMI sound -> host pacer (real time) */
    if (ac309i_pace.enabled && P67_FREE) {
        ac309i_pace.lead_us = p66_lead_ms * 1000UL;   /* P96W66 */
        if (n76_hostlog) { fprintf(n76_hostlog, "P66_LEAD_MS=%lu\n", p66_lead_ms); fflush(n76_hostlog); }
    }
    if (ac309i_pace.enabled && mr_berryvp_stream_flags) {
        ac309i_pace.lead_us = 50000UL;
        n76_hlog("P96W5_WINDOW_QUEUE_NONREF_DROP_AUDIO_FIRST");
    }
    if (ac309i_pace.enabled) {
        DBG_PRINTF("[AC309I] VPv149 host-time feed pacer: fps=%lu/%lu step=%lu + %lu/%lu us, lead=%lu us\n",
               (unsigned long)vi->rate, (unsigned long)vi->scale,
               (unsigned long)ac309i_pace.step_whole_us,
               (unsigned long)ac309i_pace.step_rem,
               (unsigned long)ac309i_pace.step_den,
               (unsigned long)ac309i_pace.lead_us);
        n76_hlog("AC309I_HOST_TIME_PACER_ACTIVE_LEAD_250MS");
    }

    if (p05_input_open()) n76_hlog("P05_ESC_INPUT_WINDOW_OK");
    else n76_hlog("P05_ESC_INPUT_WINDOW_FAIL");
    {
        char p06_text[128];
        snprintf(p06_text, sizeof p06_text, "%dx%d, %s",
                 vi->width, vi->height, mr_demux_container_name(d));
        p06_status_write(P06_STATE_PLAYING, "H.264 / VideoCore", p06_text);
        strncpy(p45_status_text, p06_text, sizeof p45_status_text - 1);
        n76_hlog("P06_STATUS_PLAYING");
    }

    n76_hlog("N76_BEFORE_DEMUX_LOOP");
    for (;;) {
        p65_hb(sent, n73_vqueued);
        if (p67_pa) mrb_paula_service(p67_pa);
        p70_apply_keys();
        if (p05_input_poll_stop()) {
            n76_hlog("P05_ESC_STOP");
            aborted = 1;
            st = MR_EAGAIN;
            break;
        }
        if (seen < 3 && audio_seen < 3) n76_hlog("N76_BEFORE_SETSIGNAL");
        { ULONG n76_sig = bvp_exec_setsignal(0, 0);
          if (seen < 3 && audio_seen < 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_SETSIGNAL sig=%08lx\n", (unsigned long)n76_sig); fflush(n76_hostlog); }
          if (p58_stop_sig(n76_sig)) {
            aborted = 1;
            st = MR_EAGAIN;
            break;
          }
        }
        {
            int p46 = p45_service(bvp, &ac309i_pace, &eac3_first_wall_us, sent, &audio_sent);
            if (p46 == 2) {
                printf("Error: audio after pause failed.\n");
                p04_free_audio_queue();
                p05_input_close();
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (p46) { aborted = 1; st = MR_EAGAIN; break; }
        }
        /* P96W3: bounded read-ahead.  With the video queue full, service it
         * until there is room again instead of reading the whole file. */
        if ((mr_berryvp_stream_flags || P67_FREE) && ac309i_pace.enabled && n73_prime_ready) {
            int stop_now = 0;
            while (n73_vqueued >= P96W_READ_AHEAD_Q) {
                st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
                if (st != MR_OK) break;
                if (n73_vqueued < P96W_READ_AHEAD_Q) break;
                if (p05_input_poll_stop() ||
                    p58_stop_sig(bvp_exec_setsignal(0, 0))) { stop_now = 1; break; }
                if (p45_service(bvp, &ac309i_pace, &eac3_first_wall_us, sent, &audio_sent)) { stop_now = 1; break; }
                p65_hb(sent, n73_vqueued);
                if (p67_pa) mrb_paula_service(p67_pa);
                p70_apply_keys();
                Delay(1);
            }
            if (st != MR_OK) {
                printf("Error: window video service failed.\n");
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (stop_now) { aborted = 1; st = MR_EAGAIN; break; }
        }
        if (seen < 3 && audio_seen < 3) n76_hlog("N76_BEFORE_DEMUX_NEXT");
        if (seen < 3) DBG_PRINTF("[D10] before demux_next_packet seen=%lu\n", seen);
        st = p50_next_packet(d, &pkt);
        if (seen < 3 && audio_seen < 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_DEMUX_NEXT st=%ld video=%d len=%lu pts=%d\n", (long)st, pkt.is_video, (unsigned long)pkt.len, pkt.has_pts); fflush(n76_hostlog); }
        if (seen < 3) DBG_PRINTF("[D11] after demux_next_packet st=%ld is_video=%d len=%lu\n", (long)st, pkt.is_video, (unsigned long)pkt.len);
        if (st != MR_OK) break;
        if (!pkt.len) continue;
        if (!pkt.is_video) {
            audio_seen++;
            if (p67_pa) {                 /* P96W67: sound through Paula */
                if (!mrb_paula_feed(p67_pa, &pkt))
                    n76_hlog("P67_FEED_NOMEM");
                continue;
            }
            if (!audio_enabled) continue;
            if (pkt.has_pts) { p96w_last_apts = pkt.pts_us; p96w_have_apts = 1; }
            if (audio_seen <= 4)
                DBG_PRINTF("[A10] audio #%lu len=%lu pts=%s\n", audio_seen,
                       (unsigned long)pkt.len, pkt.has_pts ? "yes" : "no");

            /* PiStorm04: retain compressed startup audio until one real
             * H.264 AU has completed SendVideo. */
            if (!p04_first_video_sent || p45_hold_audio) {
                if (!p04_queue_audio(&pkt)) {
                    printf("Error: cannot buffer startup audio packet %lu (%lu bytes).\n",
                           audio_seen, (unsigned long)pkt.len);
                    p04_free_audio_queue();
                    p05_input_close();
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
                if (n76_hostlog && !p04_first_video_sent) {
                    fprintf(n76_hostlog,
                            "P04_STARTUP_AUDIO_QUEUED count=%lu bytes=%lu\n",
                            p04_aqueued, p04_abytes);
                    fflush(n76_hostlog);
                }
                continue;
            }

            if (audio_seen <= 3) n76_hlog("N76_BEFORE_SEND_AUDIO");
            st = mr_berryvp_send_audio_packet(bvp, &pkt);
            if (audio_seen <= 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_SEND_AUDIO st=%ld\n", (long)st); fflush(n76_hostlog); }
            if (st != MR_OK) {
                printf("Error: audio playback failed at packet %lu.\n", audio_seen);
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && pkt.has_pts) {
                if (!eac3_pts_clock_valid) {
                    eac3_first_pts_us = pkt.pts_us;
                    eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                    eac3_pts_clock_valid = 1;
                    if (n76_hostlog) {
                        fprintf(n76_hostlog,
                                "FAST06_EAC3_ANCHOR pts=$%08lx:%08lx wall=%lu\n",
                                (unsigned long)(pkt.pts_us >> 32),
                                (unsigned long)pkt.pts_us,
                                (unsigned long)eac3_first_wall_us);
                        fflush(n76_hostlog);
                    }
                }
                eac3_last_pts_us = pkt.pts_us;
            }
            audio_sent++;
            if (ac309i_pace.enabled && n73_prime_ready && n73_vhead) {
                st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
                if (st != MR_OK) {
                    printf("Error: timed MP4 video service failed after audio packet %lu.\n", audio_seen);
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
            }
            if (!n73_prime_ready && audio_sent >= 4) {
                DBG_PRINTF("[N75] LEGACY-GUARD (unexpected) flushing %lu held video AU(s).\n", n73_vqueued);
                n73_prime_ready = 1;
                st = n73_flush_video_queue(bvp, &sent, &skipped);
                if (st != MR_OK) {
                    printf("Error: queued video flush failed after audio initialization.\n");
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
            }
            if ((audio_sent & 127u) == 0)
                DBG_PRINTF("audio packets sent: %lu\n", audio_sent);
            continue;
        }
        seen++;
        if (seen <= 8) {
            unsigned long phi = (unsigned long)(pkt.pts_us >> 32);
            unsigned long plo = (unsigned long)(pkt.pts_us & 0xffffffffUL);
            DBG_PRINTF("DIAG video #%lu len=%lu annexb=%d has_pts=%d pts=$%08lx:%08lx\n",
                   seen, (unsigned long)pkt.len, pkt.is_annexb, pkt.has_pts, phi, plo);
            fflush(stdout);
        }
        if (!n73_prime_ready) {
            if (!n73_queue_video(&pkt)) {
                printf("Error: cannot buffer video access unit %lu (%lu bytes).\n",
                       seen, (unsigned long)pkt.len);
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (seen <= 8)
                DBG_PRINTF("[N75] hold video #%lu until Audio-Master (%lu bytes, pts=%s)\n",
                       seen, (unsigned long)pkt.len, pkt.has_pts ? "yes" : "no");
            continue;
        }
        if (ac309i_pace.enabled) {
            if (!n73_queue_video(&pkt)) {
                printf("Error: cannot queue timed MP4 video access unit %lu (%lu bytes).\n",
                       seen, (unsigned long)pkt.len);
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (n73_vqueued > ac309i_pace.queue_peak) {
                ac309i_pace.queue_peak = n73_vqueued;
                if (n76_hostlog && (ac309i_pace.queue_peak <= 8u || (ac309i_pace.queue_peak & 15u) == 0u)) {
                    fprintf(n76_hostlog,
                            "AC309I_QUEUE_PEAK=%lu vpts=$%08lx:%08lx\n",
                            ac309i_pace.queue_peak,
                            (unsigned long)(pkt.pts_us >> 32),
                            (unsigned long)pkt.pts_us);
                    fflush(n76_hostlog);
                }
            }
            st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
            if (st != MR_OK) {
                printf("Error: timed MP4 video service failed at video packet %lu.\n", seen);
                p04_free_audio_queue();
                p05_input_close();
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (!p04_first_video_sent && sent) {
                p04_first_video_sent = 1;
                n76_hlog("P04_FIRST_VIDEO_SENT_BEFORE_PCM");
                if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && !eac3_pts_clock_valid &&
                    p04_ahead && p04_ahead->has_pts) {
                    eac3_first_pts_us = p04_ahead->pts_us;
                    eac3_last_pts_us = (p04_atail && p04_atail->has_pts) ?
                                       p04_atail->pts_us : p04_ahead->pts_us;
                    eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                    eac3_pts_clock_valid = 1;
                    if (n76_hostlog) {
                        fprintf(n76_hostlog,
                                "FAST06_EAC3_ANCHOR pts=$%08lx:%08lx wall=%lu\n",
                                (unsigned long)(eac3_first_pts_us >> 32),
                                (unsigned long)eac3_first_pts_us,
                                (unsigned long)eac3_first_wall_us);
                        fflush(n76_hostlog);
                    }
                }
                st = p04_flush_audio_queue(bvp, &audio_sent);
                if (st != MR_OK) {
                    printf("Error: startup audio flush failed after first timed video packet.\n");
                    p04_free_audio_queue();
                    p05_input_close();
                    n73_free_video_queue();
                    mr_berryvp_close(bvp);
                    mr_demux_close(d);
                    return 20;
                }
                if (n76_hostlog) {
                    fprintf(n76_hostlog,
                            "P04_STARTUP_AUDIO_FLUSH_OK sent=%lu remaining=%lu\n",
                            audio_sent, p04_aqueued);
                    fflush(n76_hostlog);
                }
            }
            continue;
        }
        if (seen <= 3) DBG_PRINTF("[D12] before send_packet #%lu\n", seen);
        if (seen <= 3) n76_hlog("N76_BEFORE_SEND_VIDEO");
        {
            /* P96W64 test: NOAUDIO heartbeat, one line per second in N76_HOST.TXT */
            static ULONG p64_t0, p64_last_hb, p64_max_us;
            ULONG p64_a = p63_noaudio ? ac309i_read_sys_us() : 0, p64_b;
            st = mr_berryvp_send_packet(bvp, &pkt);
            if (p63_noaudio && n76_hostlog) {
                p64_b = ac309i_read_sys_us();
                if (!p64_t0) { p64_t0 = p64_a; p64_last_hb = p64_a; }
                if (p64_b - p64_a > p64_max_us) p64_max_us = p64_b - p64_a;
                if (p64_b - p64_a > 1000000UL)
                    fprintf(n76_hostlog, "P64_SLOW_SEND ms=%lu pts_ms=%lu\n",
                            (unsigned long)((p64_b - p64_a) / 1000UL),
                            pkt.has_pts ? (unsigned long)(pkt.pts_us / 1000ULL) : 0UL);
                if (p64_b - p64_last_hb >= 1000000UL) {
                    fprintf(n76_hostlog, "P64_HB wall_ms=%lu sent=%lu pts_ms=%lu max_send_ms=%lu\n",
                            (unsigned long)((p64_b - p64_t0) / 1000UL), (unsigned long)sent,
                            pkt.has_pts ? (unsigned long)(pkt.pts_us / 1000ULL) : 0UL,
                            (unsigned long)(p64_max_us / 1000UL));
                    p64_max_us = 0;
                    p64_last_hb = p64_b;
                }
                fflush(n76_hostlog);
            }
        }
        if (seen <= 3 && n76_hostlog) { fprintf(n76_hostlog, "N76_AFTER_SEND_VIDEO st=%ld\n", (long)st); fflush(n76_hostlog); }
        if (seen <= 3) DBG_PRINTF("[D13] after send_packet #%lu st=%ld\n", seen, (long)st);
        if (st == MR_EAGAIN) {
            skipped++;
            continue;
        }
        if (st != MR_OK) {
            printf("Error: video playback failed at packet %lu.\n", seen);
            mr_berryvp_close(bvp);
            mr_demux_close(d);
            return 20;
        }
        sent++;

        if (p79_still) {
            /* P96W79: first picture in the window -> hold it.  Safety: after
             * 3 s or 100 pictures without a shown frame hold anyway. */
            uint32_t now = (uint32_t)ac309i_read_sys_us();
            if (!p79_have_t0) { p79_t0 = now; p79_have_t0 = 1; }
            if (p79_shown() || (uint32_t)(now - p79_t0) >= 3000000u || sent >= 100) {
                p79_resume = p79_still_loop();
                aborted = 1;
                st = MR_EAGAIN;
                break;
            }
        }

        if (!p04_first_video_sent) {
            p04_first_video_sent = 1;
            n76_hlog("P04_FIRST_VIDEO_SENT_BEFORE_PCM");
            if (ai && ai->format_tag == MR_AUDIO_FORMAT_EAC3 && !eac3_pts_clock_valid &&
                p04_ahead && p04_ahead->has_pts) {
                eac3_first_pts_us = p04_ahead->pts_us;
                eac3_last_pts_us = (p04_atail && p04_atail->has_pts) ?
                                   p04_atail->pts_us : p04_ahead->pts_us;
                eac3_first_wall_us = (uint32_t)ac309i_read_sys_us();
                eac3_pts_clock_valid = 1;
                if (n76_hostlog) {
                    fprintf(n76_hostlog,
                            "FAST06_EAC3_ANCHOR pts=$%08lx:%08lx wall=%lu\n",
                            (unsigned long)(eac3_first_pts_us >> 32),
                            (unsigned long)eac3_first_pts_us,
                            (unsigned long)eac3_first_wall_us);
                    fflush(n76_hostlog);
                }
            }
            st = p04_flush_audio_queue(bvp, &audio_sent);
            if (st != MR_OK) {
                printf("Error: startup audio flush failed after first video packet.\n");
                p04_free_audio_queue();
                p05_input_close();
                n73_free_video_queue();
                mr_berryvp_close(bvp);
                mr_demux_close(d);
                return 20;
            }
            if (n76_hostlog) {
                fprintf(n76_hostlog,
                        "P04_STARTUP_AUDIO_FLUSH_OK sent=%lu remaining=%lu\n",
                        audio_sent, p04_aqueued);
                fflush(n76_hostlog);
            }
        }

        if ((sent & 63u) == 0)
            DBG_PRINTF("video packets sent: %lu\n", sent);
    }

    p54_demux_eof = (st == MR_EAGAIN);
    if (aborted) {
        DBG_PRINTF("Ctrl-C: stopping video and returning to Workbench.\n");
    } else if (st != MR_EAGAIN) {
        /* Current MintVID demuxers use MR_EAGAIN for normal EOF. Preserve a
         * diagnostic if that convention changes or a malformed stream stops us. */
        printf("Error: demux ended with status %ld after %lu video packets.\n",
               (long)st, seen);
    }

    p96w_drain = 1;
    if (ac309i_pace.enabled && n73_vhead && !aborted) {
        /* Natural EOF normally leaves only a tiny tail.  Let the absolute
         * timeline reach it without inventing new PTS or changing the grid.
         * Bound this final CPU-only wait to two seconds of the same 1-MHz
         * timer so malformed timing cannot create an endless close hang. */
        uint32_t drain_start = (uint32_t)ac309i_read_sys_us();
        while (n73_vhead) {
            st = ac309i_service_mov_video(bvp, &ac309i_pace, &sent, &skipped);
            if (st != MR_OK) break;
            if (!n73_vhead) break;
            if (p05_input_poll_stop()) {
                n76_hlog("P05_ESC_STOP_DRAIN");
                aborted = 1;
                break;
            }
            if (p58_stop_sig(bvp_exec_setsignal(0, 0))) { aborted = 1; break; }
            if ((uint32_t)(ac309i_read_sys_us() - drain_start) >=
                (P67_FREE ? 5000000u : 2000000u)) break;
            if (p67_pa) mrb_paula_service(p67_pa);
            s1_tick();                    /* S1: subtitles to the end */
        }
    }
    if (p67_pa && !aborted) {
        /* P96W67: let Paula play what it still holds (max 6 s). */
        uint32_t t0 = (uint32_t)ac309i_read_sys_us();
        while (!mrb_paula_idle(p67_pa)) {
            s1_tick();
            if (p05_input_poll_stop() || p58_stop_sig(bvp_exec_setsignal(0, 0))) break;
            if ((uint32_t)(ac309i_read_sys_us() - t0) >= 6000000u) break;
            Delay(1);
        }
    }
    if (p67_pa && n76_hostlog) {
        unsigned long sil, drop, fix, dec_ms, pk, starv, minbuf;
        mrb_paula_stats(p67_pa, &sil, &drop, &fix);
        mrb_paula_timing(p67_pa, &dec_ms, &pk, &starv, &minbuf);
        fprintf(n76_hostlog, "P67_PAULA_END silence_ms=%lu dropped_ms=%lu corrections=%lu "
                "decode_ms=%lu packets=%lu starvations=%lu min_buffered_ms=%lu\n",
                sil, drop, fix, dec_ms, pk, starv, minbuf);
        fflush(n76_hostlog);
    }
    p67_cleanup();
    if (p50_t_set && n76_hostlog) {
        fprintf(n76_hostlog, "P50 start: t_ms=%lu seek_ms=%lu base_ms=%lu drop_audio=%lu drop_video=%lu\n",
                (unsigned long)(p50_t_us / 1000U), (unsigned long)(p50_seek_us / 1000U),
                (unsigned long)(p50_base_us / 1000U), p50_drop_audio, p50_drop_video);
        fflush(n76_hostlog);
    }
    if (mr_berryvp_stream_flags && n76_hostlog) {
        fprintf(n76_hostlog, "P96W window: dropped=%lu queue_peak=%lu audio_first_holds=%lu\n",
                p96w_dropped, ac309i_pace.queue_peak, p96w_audio_first_holds);
        fprintf(n76_hostlog, "P45 pauses=%lu paused_ms=%lu\n",
                p45_pauses, (unsigned long)(p45_paused_total_us / 1000u));
        if (p96s_off) {
            fprintf(n76_hostlog, "P96W32_SYNC off (NOSYNC, no clock queries)\n");
        } else if (!p96s) {
            fprintf(n76_hostlog, "P96W32_SYNC no clock block from berryvp.library (old library?)\n");
        } else {
            unsigned i;
            fprintf(n76_hostlog,
                    "P96W32_SYNC samples=%lu used=%lu corrections=%lu shift_ms=%ld "
                    "first_err_ms=%ld last_err_ms=%ld min_ms=%ld max_ms=%ld\n",
                    p96s_samples, p96s_used, p96s_corrections, p96s_shift / 1000,
                    p96s_first_err / 1000, p96s_last_err / 1000,
                    p96s_min_err / 1000, p96s_max_err / 1000);
            fprintf(n76_hostlog,
                    "P96W32_LIB requests=%lu replies=%lu failures=%lu status=%lu timeouts=%lu "
                    "rtt_max_us=%lu port_mode=%lu shown=%lu\n",
                    p96s[9], p96s[10], p96s[11], p96s[12], p96s[13], p96s[14], p96s[15], p96s[5]);
            for (i = 0; i < p96s_log_n; i++)
                fprintf(n76_hostlog, "P96W32_ERR t_ms=%ld err_ms=%ld\n", p96s_log_t[i], p96s_log_e[i]);
        }
        fflush(n76_hostlog);
    }
    if (ac309i_pace.enabled) {
        uint32_t elapsed = ac309i_pace.started ?
            (uint32_t)(ac309i_read_sys_us() - ac309i_pace.base_us) : 0;
        DBG_PRINTF("[AC309I] HOST-TIME-PACE sent=%lu elapsed=%lu next=%lu lead=%lu feedlatecnt=%lu maxfeedlate=%lu queuepeak=%lu remaining=%lu\n",
               ac309i_pace.sent_aus, (unsigned long)elapsed,
               (unsigned long)ac309i_pace.target_us, (unsigned long)ac309i_pace.lead_us,
               ac309i_pace.late_count, (unsigned long)ac309i_pace.max_late_us,
               ac309i_pace.queue_peak, n73_vqueued);
    }
    if (n73_vhead) {
        printf("Warning: %lu queued video access unit(s) remained at end of playback and were released.\n", n73_vqueued);
        n73_free_video_queue();
    }

    if (p04_ahead) {
        printf("Warning: %lu startup audio packet(s) remained queued because no video AU was submitted.\n",
               p04_aqueued);
        p04_free_audio_queue();
    }

    /* P32/FAST06: at natural E-AC3 EOF, keep the graph alive until the final
     * scheduled PCM tail has had time to present. ESC remains active here. */
    if (!aborted && st == MR_EAGAIN && eac3_pts_clock_valid && ai && ai->sample_rate) {
        uint64_t frame_us = (1536ULL * 1000000ULL + (uint64_t)ai->sample_rate - 1ULL) /
                            (uint64_t)ai->sample_rate;
        uint64_t expected_us = frame_us + 150000ULL;
        uint32_t elapsed_us = (uint32_t)(ac309i_read_sys_us() - eac3_first_wall_us);
        uint32_t wait_us = 0;
        if (eac3_last_pts_us >= eac3_first_pts_us)
            expected_us += eac3_last_pts_us - eac3_first_pts_us;
        if (expected_us > (uint64_t)elapsed_us) {
            uint64_t rem = expected_us - (uint64_t)elapsed_us;
            if (rem > 15000000ULL) rem = 15000000ULL;
            wait_us = (uint32_t)rem;
        }
        if (n76_hostlog) {
            fprintf(n76_hostlog,
                    "FAST06_EAC3_EOF expected=%lu elapsed=%lu wait=%lu first=$%08lx:%08lx last=$%08lx:%08lx\n",
                    (unsigned long)expected_us, (unsigned long)elapsed_us,
                    (unsigned long)wait_us,
                    (unsigned long)(eac3_first_pts_us >> 32),
                    (unsigned long)eac3_first_pts_us,
                    (unsigned long)(eac3_last_pts_us >> 32),
                    (unsigned long)eac3_last_pts_us);
            fflush(n76_hostlog);
        }
        if (wait_us) {
            uint32_t wait_start = (uint32_t)ac309i_read_sys_us();
            while ((uint32_t)(ac309i_read_sys_us() - wait_start) < wait_us) {
                s1_tick();
                if (p05_input_poll_stop()) {
                    n76_hlog("P05_ESC_STOP_DRAIN");
                    aborted = 1;
                    break;
                }
                if (p58_stop_sig(bvp_exec_setsignal(0, 0))) {
                    aborted = 1;
                    break;
                }
            }
            if (n76_hostlog) {
                fprintf(n76_hostlog, "FAST06_EAC3_EOF_DRAIN_DONE waited=%lu aborted=%d\n",
                        (unsigned long)(uint32_t)(ac309i_read_sys_us() - wait_start), aborted);
                fflush(n76_hostlog);
            }
        }
    }

    DBG_PRINTF("mrberry A/V complete: video=%lu sent=%lu preconfig-skipped=%lu bytes=%lu; "
           "audio=%lu sent=%lu adts-bytes=%lu\n",
           seen, sent, skipped, bvp->bytes, audio_seen, audio_sent, bvp->audio_bytes);
    {
        /* P96W54: stop position in film time, read before the library
         * closes (window: frame on screen, else last frame read) */
        p54_can_seek = mr_demux_can_seek(d);
        if (p54_have_raw) { p54_stop_us = p54_last_vpts_raw; p54_have_stop = 1; }
        if (mr_berryvp_stream_flags && p96s && p96s[5]) {
            uint64_t shown = ((uint64_t)p96s[6] << 32) | p96s[7];
            p54_stop_us = shown + (p50_t_set ? p50_base_us : 0);
            p54_have_stop = 1;
        }
        if (p78_pause_stop && p78_have_stop) {   /* P96W78: Pi clock spot */
            p54_stop_us = p78_stop_us;
            p54_have_stop = 1;
        } else if (aborted && p67_paula && p67_anchored) {
            /* P96W79: Esc/Ctrl-C with Paula: also the picture on screen
             * (Pi clock), not the last frame read (~4 s ahead). */
            uint64_t at = (uint64_t)(uint32_t)((uint32_t)ac309i_read_sys_us() - p78_clock_sys) +
                          (p50_t_set ? p50_base_us : 0);
            if (p54_have_raw && at > p54_last_vpts_raw) at = p54_last_vpts_raw;
            p54_stop_us = at;
            p54_have_stop = 1;
        }
        if (s4_jump) { p54_stop_us = s4_target_us; p54_have_stop = 1; }   /* v96.85 */
    }
    if (s1_on && n76_hostlog) {
        fprintf(n76_hostlog, "S1_STATS shows=%lu hides=%lu offset_ms=%ld clock_replies=%lu\n",
                s1_shows, s1_hides, s1_offset_ms, s1_blk ? (unsigned long)s1_blk[0] : 0UL);
        fflush(n76_hostlog);
    }
    DBG_PRINTF("Closing BerryVP stream (EOS + Workbench return) ...\n");
    st = mr_berryvp_close(bvp);
    p05_input_close();
    mr_demux_close(d);
    if (p79_still)
        n76_hlog("P79_T_NOT_WRITTEN");     /* P96W79: the still keeps the .t */
    else if (aborted && p54_can_seek && p54_have_stop)
        p54_write(argv[1], p54_stop_us, audio_enabled ? "HDMI" : (p70_used ? "PAULA" : "NONE"),
                  (p58_switch || p78_pause_stop) ? 0 : P54_RECAP_US);
    else if (!aborted && p54_demux_eof && p70_used && p70_fine_changed)
        p54_write(argv[1], 0, "PAULA", 0);  /* P96W70: keep the new fine value */
    else if (!aborted && p54_demux_eof)
        n76_hlog("P55_T_KEPT_AT_END");     /* P96W55: .t stays (watched hint) */
    p62_pos_clear();
    if (p63_noaudio && n76_hostlog) { fprintf(n76_hostlog, "P64_END aborted=%d sent=%lu\n", aborted, (unsigned long)sent); fflush(n76_hostlog); }
    if (st != MR_OK) {
        p06_status_write(P06_STATE_ERROR, "H.264 / VideoCore",
                         "VideoCore cleanup failed");
        return 20;
    }

    /* P96W78: "Paused" with ENDED = Paula pause; MintVID-pi offers Play.
     * P96W79: "Resume" with ENDED = Space in the still: Play again. */
    /* v96.82: "Fullscreen" with ENDED = F in the window: MintVID-pi
     * starts the film in full screen at the spot just saved in the .t */
    p06_status_write(P06_STATE_ENDED, "H.264 / VideoCore",
                     p78_pause_stop ? "Paused" : v9682_full ? "Fullscreen" :
                     s4_jump ? "Jump" :
                     (p79_resume ? "Resume" : (aborted ? "Stopped" : "Finished")));
    n76_hlog(p78_pause_stop ? "P06_STATUS_ENDED_PAUSED" :
             p79_resume ? "P06_STATUS_ENDED_RESUME" :
             aborted ? "P06_STATUS_ENDED_STOPPED" : "P06_STATUS_ENDED_FINISHED");
    n76_hlog("N76_END_OK");
    if (n76_hostlog) fclose(n76_hostlog);
    DBG_PRINTF("N76 PASS: host demux trace completed on N75 scheduler base.\n");
    return 0;
}
