/* MintVID ReAction controller.
 *
 * The GUI stays on Workbench and launches the separate mrplay executable.
 * ReAction classes are opened explicitly, following MintAMP's working setup.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <cybergraphx/cybergraphics.h>
#include <classes/window.h>
#include <gadgets/button.h>
#include <gadgets/checkbox.h>
#include <gadgets/chooser.h>
#include <gadgets/getfile.h>
#include <gadgets/listbrowser.h>
#include <libraries/asl.h>
#include <gadgets/layout.h>
#include <gadgets/string.h>
#include <images/label.h>
#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/cybergraphics.h>
#include <proto/button.h>
#include <proto/checkbox.h>
#include <proto/chooser.h>
#include <proto/getfile.h>
#include <proto/listbrowser.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/string.h>
#include <proto/window.h>
#include <stdio.h>
#include <string.h>
#include "../core/mr_play_options.h"
#include "../iptv/mr_iptv.h"
#include "mr_master_options.h"
#include "mr_akiko.h"
#include "mr_gui_menu.h"
#include "mr_last_dir.h"
#include "mr_player_status.h"
#include "mr_playlist.h"

MINTVID_DECLARE_VERSION(mintvid_version_tag, "MintVID");

#ifndef MRGUI_CLASS_VERSION
#define MRGUI_CLASS_VERSION 44
#endif

#define MRPLAY_STACK_SIZE 320000UL

#if defined(__GNUC__)
static const char mrgui_stack_cookie[] __attribute__((used)) =
    "$STACK:131072";
#endif

/* Runtime library bases.  CyberGfxBase is optional: a planar Workbench must
 * still be able to run the AGA/HAM controller. */
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *UtilityBase;
struct Library *AslBase;
struct Library *CyberGfxBase;
struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *CheckBoxBase;
struct Library *ChooserBase;
struct Library *GetFileBase;
struct Library *ListBrowserBase;
struct Library *StringBase;
struct Library *LabelBase;

enum {
    G_FILE = 1,
    G_PLAY,
    G_PAUSE,
    G_STOP,
    G_FF,
    G_MODE,
    G_C2P,
    G_H264,
    G_LACE,
    G_SCALE,
    G_AUDIO_RATE,
    G_FAST_BUFFER,
    G_NO_AUDIO,
    G_MONO_AUDIO,
    G_VIDEO_MODE,
    G_IPTV,
    G_YOUTUBE,
    G_VOLUME_DOWN,
    G_VOLUME_UP,
    G_PLAYLIST
};

/* Chooser rows are chipset-dependent, so never infer a display mode from a
 * hard-coded row number. This map is populated alongside the labels. */
static mr_display_mode mode_values[7];
static unsigned mode_count;
static mr_c2p_mode c2p_values[5];
static unsigned c2p_count;
static int add_chooser_node(struct List *list, const char *text);

static int chipset_has_aga(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_AA_LISA) != 0;
}

static int chipset_has_ecs_denise(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_HR_DENISE) != 0;
}

static int add_mode_node(struct List *list, const char *text,
                         mr_display_mode value)
{
    if (mode_count >= sizeof mode_values / sizeof mode_values[0] ||
        !add_chooser_node(list, text))
        return 0;
    mode_values[mode_count++] = value;
    return 1;
}

static int add_c2p_node(struct List *list, const char *text,
                        mr_c2p_mode value)
{
    if (c2p_count >= sizeof c2p_values / sizeof c2p_values[0] ||
        !add_chooser_node(list, text))
        return 0;
    c2p_values[c2p_count++] = value;
    return 1;
}

static ULONG c2p_row(mr_c2p_mode value)
{
    unsigned i;
    for (i = 0; i < c2p_count; i++)
        if (c2p_values[i] == value)
            return (ULONG)i;
    return 0;
}

static int open_reaction_classes(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary(
        (CONST_STRPTR)"graphics.library", 39);
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 39);

    /* Optional.  Its presence alone does not mean the current Workbench is
     * RTG; default_screen_is_rtg() checks the actual public-screen mode. */
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);

    WindowBase = OpenLibrary((CONST_STRPTR)"window.class",
                             MRGUI_CLASS_VERSION);
    LayoutBase = OpenLibrary((CONST_STRPTR)"gadgets/layout.gadget",
                             MRGUI_CLASS_VERSION);
    ButtonBase = OpenLibrary((CONST_STRPTR)"gadgets/button.gadget",
                             MRGUI_CLASS_VERSION);
    CheckBoxBase = OpenLibrary((CONST_STRPTR)"gadgets/checkbox.gadget",
                               MRGUI_CLASS_VERSION);
    ChooserBase = OpenLibrary((CONST_STRPTR)"gadgets/chooser.gadget",
                              MRGUI_CLASS_VERSION);
    GetFileBase = OpenLibrary((CONST_STRPTR)"gadgets/getfile.gadget",
                              MRGUI_CLASS_VERSION);
    ListBrowserBase = OpenLibrary((CONST_STRPTR)"gadgets/listbrowser.gadget",
                                  MRGUI_CLASS_VERSION);
    StringBase = OpenLibrary((CONST_STRPTR)"gadgets/string.gadget",
                             MRGUI_CLASS_VERSION);
    LabelBase = OpenLibrary((CONST_STRPTR)"images/label.image",
                            MRGUI_CLASS_VERSION);

    return IntuitionBase && GfxBase && UtilityBase && AslBase &&
           WindowBase && LayoutBase && ButtonBase && CheckBoxBase &&
           ChooserBase && GetFileBase && ListBrowserBase && StringBase &&
           LabelBase;
}

static void close_reaction_classes(void)
{
    if (LabelBase) {
        CloseLibrary(LabelBase);
        LabelBase = NULL;
    }
    if (StringBase) {
        CloseLibrary(StringBase);
        StringBase = NULL;
    }
    if (GetFileBase) {
        CloseLibrary(GetFileBase);
        GetFileBase = NULL;
    }
    if (ListBrowserBase) {
        CloseLibrary(ListBrowserBase);
        ListBrowserBase = NULL;
    }
    if (ChooserBase) {
        CloseLibrary(ChooserBase);
        ChooserBase = NULL;
    }
    if (CheckBoxBase) {
        CloseLibrary(CheckBoxBase);
        CheckBoxBase = NULL;
    }
    if (ButtonBase) {
        CloseLibrary(ButtonBase);
        ButtonBase = NULL;
    }
    if (LayoutBase) {
        CloseLibrary(LayoutBase);
        LayoutBase = NULL;
    }
    if (WindowBase) {
        CloseLibrary(WindowBase);
        WindowBase = NULL;
    }
    if (CyberGfxBase) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
    }
    if (AslBase) {
        CloseLibrary(AslBase);
        AslBase = NULL;
    }
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

static int default_screen_is_rtg(void)
{
    struct Screen *screen;
    ULONG mode_id;
    int is_rtg;

    if (!CyberGfxBase || !GfxBase || !IntuitionBase)
        return 0;

    screen = LockPubScreen(NULL);
    if (!screen)
        return 0;

    mode_id = GetVPModeID(&screen->ViewPort);
    is_rtg = mode_id != (ULONG)INVALID_ID && IsCyberModeID(mode_id);
    UnlockPubScreen(NULL, screen);
    return is_rtg;
}

static int add_chooser_node(struct List *list, const char *text)
{
    struct Node *node;

    node = AllocChooserNode(CNA_Text, (ULONG)text, TAG_END);
    if (!node)
        return 0;

    AddTail(list, node);
    return 1;
}

static void free_chooser_nodes(struct List *list)
{
    struct Node *node;

    while ((node = RemHead(list)) != NULL)
        FreeChooserNode(node);
}

static struct Task *find_player(void)
{
    struct Task *task;

    Forbid();
    task = FindTask((STRPTR)"MintVID player");
    Permit();
    return task;
}

static void signal_player(ULONG mask)
{
    struct Task *task;

    Forbid();
    task = FindTask((STRPTR)"MintVID player");
    if (task)
        Signal(task, mask);
    Permit();
}

static void stop_player_and_wait(void)
{
    int guard;

    signal_player(SIGBREAKF_CTRL_F);
    for (guard = 0; guard < 250 && find_player(); guard++)
        Delay(1);
}

static void set_info(Object *info, struct Window *window, const char *text);

static void read_play_options(Object *mode, Object *c2p, Object *h264,
                              Object *lace, Object *scale,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio,
                              Object *video_mode,
                              mr_play_options *options)
{
    ULONG selected = 0, selected_c2p = 0, selected_h264 = 0;
    ULONG checked_lace = 0, selected_scale = 0;
    ULONG selected_audio_rate = 0, selected_fast_buffer = 0;
    ULONG checked_no_audio = 0, checked_mono = 0;
    ULONG selected_video_mode = 0;
    mr_play_options_default(options);
    GetAttr(CHOOSER_Selected, mode, &selected);
    GetAttr(CHOOSER_Selected, c2p, &selected_c2p);
    GetAttr(CHOOSER_Selected, h264, &selected_h264);
    GetAttr(CHECKBOX_Checked, lace, &checked_lace);
    GetAttr(CHOOSER_Selected, scale, &selected_scale);
    GetAttr(CHOOSER_Selected, audio_rate, &selected_audio_rate);
    GetAttr(CHOOSER_Selected, fast_buffer, &selected_fast_buffer);
    GetAttr(CHECKBOX_Checked, no_audio, &checked_no_audio);
    GetAttr(CHECKBOX_Checked, mono_audio, &checked_mono);
    GetAttr(CHOOSER_Selected, video_mode, &selected_video_mode);
    options->display = selected < mode_count
                     ? mode_values[selected] : MR_DISPLAY_AGA;
    options->c2p = selected_c2p < c2p_count
                 ? c2p_values[selected_c2p] : MR_C2P_STANDARD;
    options->laced = checked_lace != 0;
    /* Scale rows are fixed and never chipset-dependent - unlike mode/c2p,
     * no value-array indirection is needed: 0=None, 1=2x, 2=Copper 2x (see
     * where scale_modes is built in main()). update_mode_controls() is what
     * keeps row 2 from ever landing here when it wouldn't actually engage. */
    options->scale_2x = selected_scale >= 1;
    options->copper_vdouble = selected_scale == 2;
    options->h264_performance = selected_h264 <= MR_H264_PERF_TURBO_GT
                              ? (mr_h264_performance)selected_h264
                              : MR_H264_PERF_AUTO;
    options->audio_rate = selected_audio_rate == 1
                        ? MR_AUDIO_RATE_LOW : MR_AUDIO_RATE_NORMAL;
    options->fast_buffer = selected_fast_buffer <= MR_FAST_BUFFER_16MB
                         ? (mr_fast_buffer_mode)selected_fast_buffer
                         : MR_FAST_BUFFER_AUTO;
    options->no_audio = checked_no_audio != 0;
    options->mono_audio = checked_mono != 0;
    /* Video rows are fixed, like Scale: 0=All Frames (throughput on), 1=Skip
     * Frames (throughput off) - see where video_modes is built in main(). */
    options->throughput = selected_video_mode == 0;
}

static void publish_play_options(mr_master_options_port *master_options,
                                 Object *mode, Object *c2p, Object *h264,
                                 Object *lace, Object *scale,
                                 Object *audio_rate, Object *fast_buffer,
                                 Object *no_audio,
                                 Object *mono_audio, Object *video_mode)
{
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    mr_master_options_publish(master_options, &options);
}

static void open_iptv_browser(Object *mode, Object *c2p, Object *h264,
                              Object *lace, Object *scale,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio, Object *video_mode,
                              Object *info,
                              struct Window *window)
{
    BPTR seglist;
    struct Process *process;
    char arguments[512];
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    if (!mr_build_iptv_arguments(arguments, sizeof(arguments), &options)) {
        set_info(info, window, "Could not build IPTV playback options.");
        return;
    }

    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:iptvgui");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"iptvgui");
    if (!seglist) {
        set_info(info, window,
                 "Could not load iptvgui (keep it beside MintVID).");
        return;
    }

    process = CreateNewProcTags(
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)arguments,
        NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"iptvgui",
        NP_Name, (ULONG)"MintVID IPTV",
        TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the iptvgui process.");
    }
}

static void open_youtube_browser(Object *mode, Object *c2p, Object *h264,
                                 Object *lace, Object *scale,
                                 Object *audio_rate, Object *fast_buffer,
                                 Object *no_audio,
                                 Object *mono_audio, Object *video_mode,
                                 Object *info,
                                 struct Window *window)
{
    BPTR seglist;
    struct Process *process;
    char arguments[512];
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    if (!mr_build_iptv_arguments(arguments, sizeof(arguments), &options)) {
        set_info(info, window, "Could not build YouTube playback options.");
        return;
    }
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:ytgui");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"ytgui");
    if (!seglist) {
        set_info(info, window,
                 "Could not load ytgui (keep it beside MintVID).");
        return;
    }
    process = CreateNewProcTags(
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)arguments,
        NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"ytgui",
        NP_Name, (ULONG)"MintVID YouTube",
        TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the ytgui process.");
    }
}

static void set_info(Object *info, struct Window *window, const char *text)
{
    if (!info || !window)
        return;

    SetGadgetAttrs((struct Gadget *)info, window, NULL,
                   STRINGA_TextVal, (ULONG)(text ? text : ""),
                   STRINGA_BufferPos, 0,
                   STRINGA_DispPos, 0,
                   TAG_DONE);
}

static void update_file_info(Object *file, Object *info,
                             struct Window *window)
{
    static char text[640];
    STRPTR path;
    BPTR lock;
    struct FileInfoBlock fib;
    const char *ext;

    path = NULL;
    GetAttr(GETFILE_FullFile, file, (ULONG *)&path);
    if (!path || !*path)
        return;

    {
        char drawer[256];
        mr_last_dir_from_path((const char *)path, drawer, sizeof(drawer));
        mr_last_dir_save(drawer);
    }

    ext = strrchr((const char *)path, '.');
    lock = Lock(path, ACCESS_READ);
    if (lock && Examine(lock, &fib)) {
        snprintf(text, sizeof(text), "%s | type: %s | %ld bytes", path,
                 ext && ext[1] ? ext + 1 : "unknown",
                 (long)fib.fib_Size);
    } else {
        snprintf(text, sizeof(text), "%s | type: %s", path,
                 ext && ext[1] ? ext + 1 : "unknown");
    }
    if (lock)
        UnLock(lock);

    set_info(info, window, text);
}

static void update_mode_controls(Object *mode, Object *c2p, Object *lace,
                                 Object *scale, struct Window *window,
                                 int output_changed)
{
    ULONG selected;
    ULONG disable_chipset_options;
    ULONG selected_c2p;
    ULONG selected_scale;
    mr_c2p_mode selected_c2p_mode;
    int kalms_available;
    int direct_available;
    int copper_ok;

    selected = 0;
    GetAttr(CHOOSER_Selected, mode, &selected);
    disable_chipset_options = selected < mode_count &&
                              (mode_values[selected] == MR_DISPLAY_CGX ||
                               mode_values[selected] == MR_DISPLAY_P96)
                            ? TRUE : FALSE;

    selected_c2p = 0;
    GetAttr(CHOOSER_Selected, c2p, &selected_c2p);
    selected_c2p_mode = selected_c2p < c2p_count
                      ? c2p_values[selected_c2p] : MR_C2P_STANDARD;
    kalms_available = selected < mode_count &&
                      ((mode_values[selected] == MR_DISPLAY_AGA &&
                        chipset_has_aga()) ||
                       mode_values[selected] == MR_DISPLAY_HAM8
#ifdef MR_KALMS_040
                       || mode_values[selected] == MR_DISPLAY_HAM6
#endif
                      );
    direct_available = 0;
#ifdef MR_KALMS_040
    direct_available = selected < mode_count &&
                       mode_values[selected] == MR_DISPLAY_AGA &&
                       chipset_has_aga();
#endif

    if (output_changed && kalms_available &&
        selected_c2p_mode == MR_C2P_STANDARD) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_KALMS), TAG_DONE);
        selected_c2p_mode = MR_C2P_KALMS;
    }
    if (selected_c2p_mode == MR_C2P_KALMS &&
        !disable_chipset_options && !kalms_available) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_STANDARD), TAG_DONE);
        selected_c2p_mode = MR_C2P_STANDARD;
    }
    if (selected_c2p_mode == MR_C2P_DIRECT &&
        !disable_chipset_options && !direct_available) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_STANDARD), TAG_DONE);
        selected_c2p_mode = MR_C2P_STANDARD;
    }

    SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                   GA_Disabled, disable_chipset_options,
                   TAG_DONE);
    if (disable_chipset_options) {
        SetGadgetAttrs((struct Gadget *)lace, window, NULL,
                       GA_Disabled, TRUE,
                       CHECKBOX_Checked, FALSE,
                       TAG_DONE);
        SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                       GA_Disabled, TRUE,
                       CHOOSER_Selected, 0,
                       TAG_DONE);
        return;
    }
    SetGadgetAttrs((struct Gadget *)lace, window, NULL,
                   GA_Disabled, FALSE,
                   TAG_DONE);
    SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                   GA_Disabled, FALSE,
                   TAG_DONE);

    /* Copper 2x (row 2 of the Scale chooser) only ever does anything for a
     * plain c2p/riva-c2p/Akiko geometry - see display_aga.c's own
     * eligibility check in aga_open(). Kalms/Standard(WPA)/Direct all
     * silently no-op it at runtime instead of erroring, which is exactly
     * what let a real-hardware test "succeed" against plain --2x without the
     * copper path ever actually running - so snap back to row 1 (2x) here
     * instead of leaving a selection that looks chosen but was never
     * honoured. Re-checked on every mode/c2p/scale change (all three call
     * this function) so no ordering of clicks can leave Copper selected
     * under an incompatible combination.
     *
     * HAM6/HAM8 no longer disqualify Copper here (EXPERIMENTAL - see
     * display_aga.c's file header comment for the correctness argument and
     * its unconfirmed-on-real-hardware status): aga_open() downgrades HAM8
     * to HAM6 on a non-AGA chipset before its own copper eligibility check
     * runs, so either HAM selection here always resolves to something the
     * backend can actually honour - there is no chipset case this GUI needs
     * to pre-filter that aga_open() doesn't already handle. */
    copper_ok = selected_c2p_mode == MR_C2P_WPA ||
                selected_c2p_mode == MR_C2P_RIVA ||
                selected_c2p_mode == MR_C2P_AKIKO;
    if (!copper_ok) {
        selected_scale = 0;
        GetAttr(CHOOSER_Selected, scale, &selected_scale);
        if (selected_scale == 2)
            SetGadgetAttrs((struct Gadget *)scale, window, NULL,
                           CHOOSER_Selected, 1, TAG_DONE);
    }
}

#define STATUS_POLL_MICROS 250000UL
/* iptvgui's own process exists (LoadSeg()/CreateNewProcTags() has already
 * returned) long before its window is actually open and interactive - it
 * still has to load/refresh its channel cache first, which is the real
 * source of the "takes a while to show the window" delay. Polled on the
 * same STATUS_POLL_MICROS tick as poll_player_status() below, so ~60s here
 * is that many ticks, not a separate timer. Bounded so a launch that never
 * completes (missing binary, crash) doesn't leave the button disabled and
 * the busy pointer up forever.
 *
 * Was 60 ticks (~15s), which real A1200/68060 hardware confirmed was too
 * short: a cold channel-directory load (parsing tens of thousands of
 * channels/streams - see iptv/mr_iptv.c) plus a slow/network-backed cache
 * refresh can legitimately take longer than that, and this watchdog fired
 * before iptvgui's window ever opened, showing "IPTV browser did not open
 * (missing binary or crash?)" for a launch that was still genuinely in
 * progress and would have succeeded given more time. Same real-hardware
 * lesson as the live-stream stall this session's CLAUDE.md notes document
 * elsewhere: this target's own network/storage latency is much larger than
 * a desktop-tuned timeout assumes. Quadrupled to 240 ticks (~60s) rather
 * than removed outright - a launch that is genuinely missing/crashed should
 * still be reported eventually, just not this early. */
#define IPTV_LAUNCH_TIMEOUT_TICKS 240UL

static int iptv_launch_pending;
static ULONG iptv_launch_ticks;

static struct MsgPort *status_timer_port;
static struct timerequest *status_timer_io;
static int status_timer_device_open;
static int status_timer_running;
static ULONG player_status_seq;

static int status_timer_open(void)
{
    status_timer_port = CreateMsgPort();
    if (!status_timer_port) return 0;
    status_timer_io = (struct timerequest *)
        CreateIORequest(status_timer_port, sizeof(*status_timer_io));
    if (!status_timer_io) return 0;
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)status_timer_io, 0) != 0)
        return 0;
    status_timer_device_open = 1;
    return 1;
}

static void status_timer_start(void)
{
    if (!status_timer_io) return;
    status_timer_io->tr_node.io_Command = TR_ADDREQUEST;
    status_timer_io->tr_time.tv_secs = 0;
    status_timer_io->tr_time.tv_micro = STATUS_POLL_MICROS;
    SendIO((struct IORequest *)status_timer_io);
    status_timer_running = 1;
}

static void status_timer_close(void)
{
    if (status_timer_io) {
        if (status_timer_running && !CheckIO((struct IORequest *)status_timer_io))
            AbortIO((struct IORequest *)status_timer_io);
        if (status_timer_running)
            WaitIO((struct IORequest *)status_timer_io);
        status_timer_running = 0;
        if (status_timer_device_open) {
            CloseDevice((struct IORequest *)status_timer_io);
            status_timer_device_open = 0;
        }
        DeleteIORequest((struct IORequest *)status_timer_io);
        status_timer_io = NULL;
    }
    if (status_timer_port) {
        DeleteMsgPort(status_timer_port);
        status_timer_port = NULL;
    }
}

static void poll_player_status(Object *info, struct Window *window)
{
    mr_player_status ps;
    char line[300];

    if (!mr_player_status_read(&ps)) { player_status_seq = 0; return; }
    if (ps.seq == player_status_seq) return;
    player_status_seq = ps.seq;

    if (ps.state == MR_PLAYER_STATE_PLAYING)
        snprintf(line, sizeof(line), "Playing%s%s%s%s", ps.codec[0] ? " (" : "",
                 ps.codec, ps.codec[0] ? "): " : "", ps.text);
    else if (ps.state == MR_PLAYER_STATE_UNSUPPORTED)
        snprintf(line, sizeof(line), "Not supported: %s", ps.text);
    else if (ps.state == MR_PLAYER_STATE_ERROR)
        snprintf(line, sizeof(line), "Player error: %s", ps.text);
    else if (ps.state == MR_PLAYER_STATE_ENDED)
        snprintf(line, sizeof(line), "Playback ended");
    else
        return;
    set_info(info, window, line);
}

/* Called on the same status_timer tick as poll_player_status() while an
 * iptvgui launch is pending. Clears the busy indicator once iptvgui's own
 * window is open (MR_IPTV_GUI_PORT appears) or, failing that, once the
 * bounded timeout elapses. */
static void poll_iptv_launch(Object *info, struct Window *window,
                             Object *iptv_button)
{
    int ready;

    if (!iptv_launch_pending)
        return;
    Forbid();
    ready = FindPort((CONST_STRPTR)MR_IPTV_GUI_PORT) != NULL;
    Permit();
    if (!ready && ++iptv_launch_ticks < IPTV_LAUNCH_TIMEOUT_TICKS)
        return;
    iptv_launch_pending = 0;
    SetWindowPointer(window, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)iptv_button, window, NULL,
                   GA_Disabled, FALSE, TAG_DONE);
    set_info(info, window,
            ready ? NULL
                  : "IPTV browser did not open (missing binary or crash?).");
}

/* Immediate feedback for the button click - iptvgui's own process exists
 * as soon as open_iptv_browser() returns, but its window can take a real
 * moment to appear (channel cache load/refresh) with nothing else visible
 * changing in the meantime. */
static void start_iptv_launch(Object *info, struct Window *window,
                              Object *iptv_button)
{
    SetGadgetAttrs((struct Gadget *)iptv_button, window, NULL,
                   GA_Disabled, TRUE, TAG_DONE);
    SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
    set_info(info, window, "Opening IPTV browser...");
    iptv_launch_pending = 1;
    iptv_launch_ticks = 0;
}

static void start_player_path(const char *full_file, Object *mode,
                              Object *c2p, Object *h264,
                              Object *lace, Object *scale,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio, Object *video_mode,
                              Object *info,
                              struct Window *window)
{
    char path[512];
    char args[1600];
    mr_play_options options;
    BPTR seglist;
    struct Process *process;

    if (!full_file || !*full_file) {
        set_info(info, window, "Choose a video first.");
        return;
    }
    if (mr_path_is_audio_only(full_file)) {
        set_info(info, window, "Audio-only file: use MintAMP instead.");
        return;
    }
    if (find_player()) {
        set_info(info, window,
                 "A MintVID player is already running; stop it first.");
        return;
    }
    strncpy(path, full_file, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    read_play_options(mode, c2p, h264, lace, scale, audio_rate, fast_buffer,
                      no_audio, mono_audio, video_mode, &options);
    if (!mr_build_player_arguments(args, sizeof(args), &options, path,
                                   NULL, NULL)) {
        set_info(info, window, "Could not build player arguments.");
        return;
    }
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrplay");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"mrplay");
    if (!seglist) {
        set_info(info, window,
                 "Could not load mrplay (keep it beside MintVID or in PATH).");
        return;
    }
    process = CreateNewProcTags(
        NP_Seglist, seglist, NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE, NP_CommandName, (ULONG)"mrplay",
        NP_Name, (ULONG)"MintVID player", TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the mrplay process.");
    }
}

static void start_player(Object *file, Object *mode, Object *c2p,
                         Object *h264, Object *lace, Object *scale,
                         Object *audio_rate, Object *fast_buffer,
                         Object *no_audio, Object *mono_audio,
                         Object *video_mode, Object *info,
                         struct Window *window)
{
    STRPTR full_file = NULL;
    GetAttr(GETFILE_FullFile, file, (ULONG *)&full_file);
    start_player_path((const char *)full_file, mode, c2p, h264, lace, scale,
                      audio_rate, fast_buffer, no_audio, mono_audio,
                      video_mode, info, window);
}



#define MRG_PL_LIST 200
#define MRG_PL_ADD 201
#define MRG_PL_REMOVE 202
#define MRG_PL_CLEAR 203
#define MRG_PL_PLAY 204
#define MRG_PL_LOAD 205
#define MRG_PL_SAVE 206
#define MRG_PL_CLOSE 207

typedef struct mrg_playlist_window {
    Object *window_object;
    struct Window *window;
    Object *list;
    struct List nodes;
    mr_playlist *playlist;
    ULONG sigmask;
    Object *mode, *c2p, *h264, *lace, *scale;
    Object *audio_rate, *fast_buffer, *no_audio, *mono_audio, *video_mode;
    Object *info;
    struct Window *parent_window;
} mrg_playlist_window;

static void mrg_playlist_free_nodes(mrg_playlist_window *p)
{
    struct Node *node;
    while ((node = RemHead(&p->nodes)) != NULL)
        FreeListBrowserNode(node);
}

static struct Node *mrg_playlist_node_at(mrg_playlist_window *p, int index)
{
    struct Node *node;
    int i = 0;
    for (node = p->nodes.lh_Head; node && node->ln_Succ;
         node = node->ln_Succ) {
        if (i++ == index)
            return node;
    }
    return NULL;
}

static void mrg_playlist_rebuild(mrg_playlist_window *p)
{
    int i;
    mrg_playlist_free_nodes(p);
    for (i = 0; i < p->playlist->count; i++) {
        struct Node *node = AllocListBrowserNode(
            1, LBNA_Column, 0, LBNCA_Text,
            (ULONG)p->playlist->names[i], TAG_END);
        if (!node)
            break;
        AddTail(&p->nodes, node);
    }
}

static void mrg_playlist_refresh(mrg_playlist_window *p)
{
    struct Node *selected;
    mrg_playlist_rebuild(p);
    if (!p->window || !p->list)
        return;
    selected = mrg_playlist_node_at(p, p->playlist->selected);
    SetGadgetAttrs((struct Gadget *)p->list, p->window, NULL,
                   LISTBROWSER_Labels, (ULONG)&p->nodes,
                   LISTBROWSER_SelectedNode, (ULONG)selected, TAG_DONE);
}

static void mrg_playlist_set_selected(mrg_playlist_window *p)
{
    struct Node *selected = NULL;
    struct Node *node;
    int i = 0;
    GetAttr(LISTBROWSER_SelectedNode, p->list, (ULONG *)&selected);
    for (node = p->nodes.lh_Head; node && node->ln_Succ;
         node = node->ln_Succ, i++)
        if (node == selected) {
            p->playlist->selected = i;
            return;
        }
}

static void mrg_playlist_add_files(mrg_playlist_window *p)
{
    struct FileRequester *req;
    req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Add videos to playlist",
        ASLFR_DoMultiSelect, TRUE, ASLFR_DoPatterns, TRUE,
        ASLFR_InitialPattern, (ULONG)MR_VIDEO_FILE_PATTERN, TAG_DONE);
    if (!req)
        return;
    if (AslRequestTags(req, ASLFR_Window, (ULONG)p->window,
                       ASLFR_SleepWindow, TRUE, TAG_DONE)) {
        char path[MR_PLAYLIST_PATH_MAX];
        int i;
        if (req->fr_NumArgs > 0 && req->fr_ArgList) {
            for (i = 0; i < (int)req->fr_NumArgs; i++) {
                strncpy(path, req->fr_Drawer ? req->fr_Drawer : "",
                        sizeof(path) - 1);
                path[sizeof(path) - 1] = 0;
                if (req->fr_ArgList[i].wa_Name &&
                    AddPart((STRPTR)path, req->fr_ArgList[i].wa_Name,
                            sizeof(path)))
                    mr_playlist_add(p->playlist, path);
            }
        } else if (req->fr_File && req->fr_File[0]) {
            strncpy(path, req->fr_Drawer ? req->fr_Drawer : "",
                    sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            if (AddPart((STRPTR)path, req->fr_File, sizeof(path)))
                mr_playlist_add(p->playlist, path);
        }
    }
    FreeAslRequest(req);
    mrg_playlist_refresh(p);
}

static void mrg_playlist_load_m3u(mrg_playlist_window *p)
{
    struct FileRequester *req;
    char m3u[MR_PLAYLIST_PATH_MAX], drawer[MR_PLAYLIST_PATH_MAX];
    char line[MR_PLAYLIST_PATH_MAX];
    FILE *file;
    req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Load M3U Playlist",
        ASLFR_DoPatterns, TRUE, ASLFR_InitialPattern, (ULONG)"#?.m3u",
        TAG_DONE);
    if (!req)
        return;
    if (!AslRequestTags(req, ASLFR_Window, (ULONG)p->window,
                       ASLFR_SleepWindow, TRUE, TAG_DONE)) {
        FreeAslRequest(req);
        return;
    }
    strncpy(m3u, req->fr_Drawer ? req->fr_Drawer : "", sizeof(m3u) - 1);
    m3u[sizeof(m3u) - 1] = 0;
    if (req->fr_File && req->fr_File[0])
        AddPart((STRPTR)m3u, req->fr_File, sizeof(m3u));
    FreeAslRequest(req);
    if (!m3u[0] || !(file = fopen(m3u, "r")))
        return;
    mr_last_dir_from_path(m3u, drawer, sizeof(drawer));
    while (fgets(line, sizeof(line), file)) {
        char path[MR_PLAYLIST_PATH_MAX];
        char *eol = strpbrk(line, "\r\n");
        if (eol) *eol = 0;
        if (!line[0] || line[0] == '#')
            continue;
        if (strchr(line, ':') || line[0] == '/') {
            strncpy(path, line, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
        } else {
            strncpy(path, drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            if (!AddPart((STRPTR)path, line, sizeof(path)))
                continue;
        }
        mr_playlist_add(p->playlist, path);
    }
    fclose(file);
    mrg_playlist_refresh(p);
}

static void mrg_playlist_save_m3u(mrg_playlist_window *p)
{
    struct FileRequester *req;
    char m3u[MR_PLAYLIST_PATH_MAX];
    FILE *file;
    int i;
    req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Save M3U Playlist",
        ASLFR_DoSaveMode, TRUE, ASLFR_InitialFile, (ULONG)"playlist.m3u",
        TAG_DONE);
    if (!req)
        return;
    if (!AslRequestTags(req, ASLFR_Window, (ULONG)p->window,
                       ASLFR_SleepWindow, TRUE, TAG_DONE)) {
        FreeAslRequest(req);
        return;
    }
    strncpy(m3u, req->fr_Drawer ? req->fr_Drawer : "", sizeof(m3u) - 1);
    m3u[sizeof(m3u) - 1] = 0;
    if (req->fr_File && req->fr_File[0])
        AddPart((STRPTR)m3u, req->fr_File, sizeof(m3u));
    FreeAslRequest(req);
    if (!m3u[0] || !(file = fopen(m3u, "w")))
        return;
    fprintf(file, "#EXTM3U\n");
    for (i = 0; i < p->playlist->count; i++)
        fprintf(file, "%s\n", p->playlist->paths[i]);
    fclose(file);
}

static void mrg_playlist_close(mrg_playlist_window *p)
{
    if (p->window_object) {
        if (p->window)
            RA_CloseWindow(p->window_object);
        DisposeObject(p->window_object);
    }
    mrg_playlist_free_nodes(p);
    p->window_object = NULL;
    p->window = NULL;
    p->list = NULL;
    p->sigmask = 0;
}

static int mrg_playlist_open(mrg_playlist_window *p, mr_playlist *playlist,
                             Object *mode, Object *c2p, Object *h264,
                             Object *lace, Object *scale,
                             Object *audio_rate, Object *fast_buffer,
                             Object *no_audio, Object *mono_audio,
                             Object *video_mode, Object *info,
                             struct Window *parent)
{
    Object *add, *remove, *clear, *play, *load, *save, *close;
    Object *row1, *row2, *layout;
    memset(p, 0, sizeof(*p));
    NewList(&p->nodes);
    p->playlist = playlist;
    p->mode = mode; p->c2p = c2p; p->h264 = h264;
    p->lace = lace; p->scale = scale;
    p->audio_rate = audio_rate; p->fast_buffer = fast_buffer;
    p->no_audio = no_audio; p->mono_audio = mono_audio;
    p->video_mode = video_mode; p->info = info;
    p->parent_window = parent;
    mrg_playlist_rebuild(p);
    p->list = (Object *)NewObject(LISTBROWSER_GetClass(), NULL,
        GA_ID, MRG_PL_LIST, GA_RelVerify, TRUE,
        LISTBROWSER_Labels, (ULONG)&p->nodes,
        LISTBROWSER_AutoFit, TRUE, LISTBROWSER_ShowSelected, TRUE,
        LISTBROWSER_MinVisible, 12, TAG_DONE);
    add = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_ADD, GA_Text, (ULONG)"Add", GA_RelVerify, TRUE, TAG_DONE);
    remove = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_REMOVE, GA_Text, (ULONG)"Remove", GA_RelVerify, TRUE, TAG_DONE);
    clear = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_CLEAR, GA_Text, (ULONG)"Clear", GA_RelVerify, TRUE, TAG_DONE);
    play = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_PLAY, GA_Text, (ULONG)"Play", GA_RelVerify, TRUE, TAG_DONE);
    load = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_LOAD, GA_Text, (ULONG)"Load M3U", GA_RelVerify, TRUE, TAG_DONE);
    save = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_SAVE, GA_Text, (ULONG)"Save M3U", GA_RelVerify, TRUE, TAG_DONE);
    close = (Object *)NewObject(BUTTON_GetClass(), NULL,
        GA_ID, MRG_PL_CLOSE, GA_Text, (ULONG)"Close", GA_RelVerify, TRUE, TAG_DONE);
    row1 = (Object *)NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)add, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)remove, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)clear, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)play, CHILD_WeightedWidth, 0, TAG_DONE);
    row2 = (Object *)NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)load, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)save, CHILD_WeightedWidth, 0,
        LAYOUT_AddChild, (ULONG)close, CHILD_WeightedWidth, 0, TAG_DONE);
    layout = (Object *)NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE,
        LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)p->list, CHILD_WeightedHeight, 1,
        LAYOUT_AddChild, (ULONG)row1, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)row2, CHILD_WeightedHeight, 0, TAG_DONE);
    p->window_object = (Object *)NewObject(WINDOW_GetClass(), NULL,
        WA_Title, (ULONG)"MintVID Playlist", WA_Activate, TRUE,
        WA_DepthGadget, TRUE, WA_DragBar, TRUE, WA_CloseGadget, TRUE,
        WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                  IDCMP_REFRESHWINDOW,
        WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_ParentGroup, (ULONG)layout, TAG_DONE);
    if (!p->list || !add || !remove || !clear || !play || !load || !save ||
        !close || !row1 || !row2 || !layout || !p->window_object)
        goto fail;
    p->window = (struct Window *)RA_OpenWindow(p->window_object);
    if (!p->window)
        goto fail;
    GetAttr(WINDOW_SigMask, p->window_object, &p->sigmask);
    return 1;
fail:
    mrg_playlist_close(p);
    return 0;
}

static void mrg_playlist_handle(mrg_playlist_window *p)
{
    ULONG result;
    UWORD code;
    if (!p->window_object)
        return;
    while ((result = RA_HandleInput(p->window_object, &code)) != WMHI_LASTMSG) {
        switch (result & WMHI_CLASSMASK) {
        case WMHI_CLOSEWINDOW:
            mrg_playlist_close(p);
            return;
        case WMHI_REFRESHWINDOW:
            break;
        case WMHI_GADGETUP:
            switch (result & WMHI_GADGETMASK) {
            case MRG_PL_LIST:
                mrg_playlist_set_selected(p);
                break;
            case MRG_PL_ADD: mrg_playlist_add_files(p); break;
            case MRG_PL_REMOVE:
                mr_playlist_remove(p->playlist, p->playlist->selected);
                mrg_playlist_refresh(p);
                break;
            case MRG_PL_CLEAR:
                mr_playlist_clear(p->playlist);
                mrg_playlist_refresh(p);
                break;
            case MRG_PL_PLAY:
                if (p->playlist->selected >= 0 &&
                    p->playlist->selected < p->playlist->count) {
                    p->playlist->current = p->playlist->selected;
                    start_player_path(
                        p->playlist->paths[p->playlist->selected], p->mode,
                        p->c2p, p->h264, p->lace, p->scale, p->audio_rate,
                        p->fast_buffer, p->no_audio, p->mono_audio,
                        p->video_mode, p->info, p->parent_window);
                }
                break;
            case MRG_PL_LOAD: mrg_playlist_load_m3u(p); break;
            case MRG_PL_SAVE: mrg_playlist_save_m3u(p); break;
            case MRG_PL_CLOSE: mrg_playlist_close(p); return;
            }
            break;
        }
    }
}

int main(void)
{
    Object *window_object;
    Object *file;
    Object *mode;
    Object *c2p;
    Object *h264;
    Object *lace;
    Object *scale;
    Object *audio_rate;
    Object *fast_buffer;
    Object *no_audio;
    Object *mono_audio;
    Object *video_mode;
    Object *info;
    Object *layout;
    Object *controls;
    Object *controls_top;
    Object *controls_bottom;
    Object *buttons;
    Object *transport;
    Object *services;
    Object *file_label;
    Object *display_label;
    Object *c2p_label;
    Object *h264_label;
    Object *scale_label;
    Object *audio_rate_label;
    Object *fast_buffer_label;
    Object *video_mode_label;
    Object *play_button;
    Object *pause_button;
    Object *stop_button;
    Object *ff_button;
    Object *iptv_button;
    Object *youtube_button;
    Object *volume_down_button;
    Object *volume_up_button;
    Object *playlist_button;
    struct Window *window;
    mr_playlist playlist;
    mrg_playlist_window playlist_gui;
    mr_master_options_port *master_options;
    mr_gui_menu app_menu;
    struct List modes;
    struct List c2p_modes;
    struct List h264_modes;
    struct List scale_modes;
    struct List audio_rate_modes;
    struct List fast_buffer_modes;
    struct List video_modes;
    ULONG sigmask;
    ULONG result;
    ULONG signals;
    ULONG timermask;
    UWORD code;
    int status;
    int have_rtg;
    int default_mode;
    char initial_drawer[256];
    int have_initial_drawer;

    window_object = NULL;
    timermask = 0;
    file = NULL;
    mode = NULL;
    c2p = NULL;
    h264 = NULL;
    lace = NULL;
    scale = NULL;
    scale_label = NULL;
    audio_rate = NULL;
    fast_buffer = NULL;
    no_audio = NULL;
    mono_audio = NULL;
    video_mode = NULL;
    info = NULL;
    layout = NULL;
    controls = NULL;
    controls_top = NULL;
    controls_bottom = NULL;
    buttons = NULL;
    transport = NULL;
    services = NULL;
    file_label = NULL;
    display_label = NULL;
    c2p_label = NULL;
    h264_label = NULL;
    audio_rate_label = NULL;
    fast_buffer_label = NULL;
    video_mode_label = NULL;
    play_button = NULL;
    pause_button = NULL;
    stop_button = NULL;
    ff_button = NULL;
    iptv_button = NULL;
    youtube_button = NULL;
    volume_down_button = NULL;
    volume_up_button = NULL;
    playlist_button = NULL;
    memset(&playlist_gui, 0, sizeof(playlist_gui));
    mr_playlist_init(&playlist);
    window = NULL;
    master_options = NULL;
    memset(&app_menu, 0, sizeof(app_menu));
    status = RETURN_FAIL;
    have_rtg = 0;
    default_mode = 0;
    mode_count = 0;

    modes.lh_Head = (struct Node *)&modes.lh_Tail;
    modes.lh_Tail = NULL;
    modes.lh_TailPred = (struct Node *)&modes.lh_Head;
    c2p_modes.lh_Head = (struct Node *)&c2p_modes.lh_Tail;
    c2p_modes.lh_Tail = NULL;
    c2p_modes.lh_TailPred = (struct Node *)&c2p_modes.lh_Head;
    h264_modes.lh_Head = (struct Node *)&h264_modes.lh_Tail;
    h264_modes.lh_Tail = NULL;
    h264_modes.lh_TailPred = (struct Node *)&h264_modes.lh_Head;
    scale_modes.lh_Head = (struct Node *)&scale_modes.lh_Tail;
    scale_modes.lh_Tail = NULL;
    scale_modes.lh_TailPred = (struct Node *)&scale_modes.lh_Head;
    audio_rate_modes.lh_Head = (struct Node *)&audio_rate_modes.lh_Tail;
    audio_rate_modes.lh_Tail = NULL;
    audio_rate_modes.lh_TailPred = (struct Node *)&audio_rate_modes.lh_Head;
    fast_buffer_modes.lh_Head = (struct Node *)&fast_buffer_modes.lh_Tail;
    fast_buffer_modes.lh_Tail = NULL;
    fast_buffer_modes.lh_TailPred = (struct Node *)&fast_buffer_modes.lh_Head;
    video_modes.lh_Head = (struct Node *)&video_modes.lh_Tail;
    video_modes.lh_Tail = NULL;
    video_modes.lh_TailPred = (struct Node *)&video_modes.lh_Head;

    if (!open_reaction_classes()) {
        fprintf(stderr, "MintVID: ReAction V%ld classes are not available.\n",
                (long)MRGUI_CLASS_VERSION);
        goto cleanup;
    }

    have_rtg = default_screen_is_rtg();
    if (chipset_has_aga()) {
        if (!add_mode_node(&modes, "AGA (256 colours)", MR_DISPLAY_AGA) ||
            !add_mode_node(&modes, "ECS (32 colours)", MR_DISPLAY_AGA_ECS32) ||
            !add_mode_node(&modes, "ECS (16 colours)", MR_DISPLAY_AGA_ECS16))
            goto cleanup;
    } else if (!add_mode_node(&modes,
                              chipset_has_ecs_denise() ? "ECS (32 colours)" :
                                                         "OCS (32 colours)",
                              MR_DISPLAY_AGA))
        goto cleanup;
    if (!add_mode_node(&modes, "HAM6", MR_DISPLAY_HAM6) ||
        (chipset_has_aga() &&
         !add_mode_node(&modes, "HAM8", MR_DISPLAY_HAM8)) ||
        (have_rtg && !add_mode_node(&modes, "RTG (WritePixel)", MR_DISPLAY_CGX)) ||
        (have_rtg && !add_mode_node(&modes, "RTG (P96)", MR_DISPLAY_P96)))
        goto cleanup;
    if (have_rtg) default_mode = (int)mode_count - 2;
    if (!add_c2p_node(&c2p_modes, "Standard", MR_C2P_STANDARD) ||
        (mr_akiko_available() &&
         !add_c2p_node(&c2p_modes, "CD32", MR_C2P_AKIKO)) ||
        !add_c2p_node(&c2p_modes, "Kalms", MR_C2P_KALMS) ||
        /* The only other --2x-and-scale_2x-independent backend besides
         * Kalms - i.e. the one non-CD32 choice that actually qualifies for
         * Copper 2x (display_aga.c's copper_vdouble excludes Kalms/WPA/
         * Standard entirely, since only this portable mr_c2p8 kernel,
         * --riva-c2p and Akiko take an arbitrary output stride). Was never
         * exposed here before, silently leaving Copper 2x with no reachable
         * qualifying c2p choice on a non-CD32 machine. */
        !add_c2p_node(&c2p_modes, "Portable", MR_C2P_WPA)
#ifdef MR_KALMS_040
        || !add_c2p_node(&c2p_modes, "Direct", MR_C2P_DIRECT)
#endif
       )
        goto cleanup;
    if (!add_chooser_node(&h264_modes, "Auto") ||
        !add_chooser_node(&h264_modes, "Quality") ||
        !add_chooser_node(&h264_modes, "Balanced") ||
        !add_chooser_node(&h264_modes, "Fast") ||
        !add_chooser_node(&h264_modes, "Turbo") ||
        !add_chooser_node(&h264_modes, "Turbo+") ||
        !add_chooser_node(&h264_modes, "TurboGT"))
        goto cleanup;
    /* Rows are fixed, unlike mode/c2p - no value-array indirection needed;
     * read_play_options() and update_mode_controls() both hard-code
     * 0=None, 1=2x, 2=Copper 2x to match this exact order. */
    if (!add_chooser_node(&scale_modes, "None") ||
        !add_chooser_node(&scale_modes, "2x") ||
        !add_chooser_node(&scale_modes, "Copper 2x"))
        goto cleanup;
    if (!add_chooser_node(&audio_rate_modes, "Normal") ||
        !add_chooser_node(&audio_rate_modes, "Low"))
        goto cleanup;
    if (!add_chooser_node(&fast_buffer_modes, "Auto") ||
        !add_chooser_node(&fast_buffer_modes, "Off") ||
        !add_chooser_node(&fast_buffer_modes, "4 MB") ||
        !add_chooser_node(&fast_buffer_modes, "8 MB") ||
        !add_chooser_node(&fast_buffer_modes, "16 MB"))
        goto cleanup;
    /* Rows are fixed, like Scale: 0=All Frames, 1=Skip Frames -
     * read_play_options() hard-codes this exact order. All Frames (never
     * skip a decoded frame purely for falling behind the playback clock -
     * see mr_play_options.h's own throughput field) is the default and the
     * real-hardware-confirmed fix for a live/network stream that can't
     * decode in real time; see CLAUDE.md's "Live HLS playback stall
     * notes". */
    if (!add_chooser_node(&video_modes, "All Frames") ||
        !add_chooser_node(&video_modes, "Skip Frames"))
        goto cleanup;

    /* Seed the embedded file requester's starting drawer from the last one
     * used, saved via mr_last_dir_save() in update_file_info() below.
     * GETFILE_Drawer is a real ReAction getfile.gadget tag, but - like every
     * NDK-only detail in this file - there is no AmigaOS toolchain on this
     * dev host to check its exact behaviour against (see CLAUDE.md's
     * "Validate against ffmpeg" section); the real m68k-amigaos-gcc CI build
     * and a real-hardware run are what actually confirm it. */
    have_initial_drawer = mr_last_dir_load(initial_drawer,
                                           sizeof(initial_drawer));
    file = have_initial_drawer
        ? (Object *)NewObject(GETFILE_GetClass(), NULL,
                              GA_ID, G_FILE,
                              GA_RelVerify, TRUE,
                              GETFILE_TitleText, (ULONG)"Choose a video",
                              GETFILE_Pattern, (ULONG)MR_VIDEO_FILE_PATTERN,
                              GETFILE_DoPatterns, TRUE,
                              GETFILE_ReadOnly, TRUE,
                              GETFILE_DrawersOnly, FALSE,
                              GETFILE_Drawer, (ULONG)initial_drawer,
                              TAG_DONE)
        : (Object *)NewObject(GETFILE_GetClass(), NULL,
                              GA_ID, G_FILE,
                              GA_RelVerify, TRUE,
                              GETFILE_TitleText, (ULONG)"Choose a video",
                              GETFILE_Pattern, (ULONG)MR_VIDEO_FILE_PATTERN,
                              GETFILE_DoPatterns, TRUE,
                              GETFILE_ReadOnly, TRUE,
                              GETFILE_DrawersOnly, FALSE,
                              TAG_DONE);
    mode = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                               GA_ID, G_MODE,
                               GA_RelVerify, TRUE,
                               CHOOSER_Labels, (ULONG)&modes,
                               CHOOSER_Selected, default_mode,
                               TAG_DONE);
    c2p = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                              GA_ID, G_C2P,
                              GA_RelVerify, TRUE,
                              CHOOSER_Labels, (ULONG)&c2p_modes,
                              CHOOSER_Selected, c2p_row(MR_C2P_KALMS),
                              TAG_DONE);
    h264 = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                               GA_ID, G_H264,
                               GA_RelVerify, TRUE,
                               CHOOSER_Labels, (ULONG)&h264_modes,
                               CHOOSER_Selected, MR_H264_PERF_TURBO_GT,
                               TAG_DONE);
    lace = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                               GA_ID, G_LACE,
                               GA_Text, (ULONG)"Laced",
                               GA_RelVerify, TRUE,
                               TAG_DONE);
    scale = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                GA_ID, G_SCALE,
                                GA_RelVerify, TRUE,
                                CHOOSER_Labels, (ULONG)&scale_modes,
                                CHOOSER_Selected, 0,
                                TAG_DONE);
    scale_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                      LABEL_Text, (ULONG)"Scale",
                                      TAG_DONE);
    audio_rate = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                     GA_ID, G_AUDIO_RATE,
                                     GA_RelVerify, TRUE,
                                     CHOOSER_Labels, (ULONG)&audio_rate_modes,
                                     CHOOSER_Selected, MR_AUDIO_RATE_NORMAL,
                                     TAG_DONE);
    fast_buffer = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                      GA_ID, G_FAST_BUFFER,
                                      GA_RelVerify, TRUE,
                                      CHOOSER_Labels, (ULONG)&fast_buffer_modes,
                                      CHOOSER_Selected, MR_FAST_BUFFER_AUTO,
                                      TAG_DONE);
    no_audio = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                                   GA_ID, G_NO_AUDIO,
                                   GA_Text, (ULONG)"No audio",
                                   GA_RelVerify, TRUE,
                                   TAG_DONE);
    mono_audio = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                                     GA_ID, G_MONO_AUDIO,
                                     GA_Text, (ULONG)"Mono audio",
                                     GA_RelVerify, TRUE,
                                     TAG_DONE);
    video_mode = (Object *)NewObject(CHOOSER_GetClass(), NULL,
                                     GA_ID, G_VIDEO_MODE,
                                     GA_RelVerify, TRUE,
                                     CHOOSER_Labels, (ULONG)&video_modes,
                                     CHOOSER_Selected, 0,
                                     TAG_DONE);
    info = (Object *)NewObject(STRING_GetClass(), NULL,
                               GA_ReadOnly, TRUE,
                               STRINGA_TextVal, (ULONG)"No file selected",
                               STRINGA_MaxChars, 640,
                               TAG_DONE);
    file_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                     LABEL_Text, (ULONG)"File",
                                     TAG_DONE);
    display_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                        LABEL_Text, (ULONG)"Display",
                                        TAG_DONE);
    c2p_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                    LABEL_Text, (ULONG)"C2P",
                                    TAG_DONE);
    h264_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                     LABEL_Text, (ULONG)"H.264",
                                     TAG_DONE);
    audio_rate_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                           LABEL_Text, (ULONG)"Audio rate",
                                           TAG_DONE);
    fast_buffer_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                            LABEL_Text, (ULONG)"Fast buffer",
                                            TAG_DONE);
    video_mode_label = (Object *)NewObject(LABEL_GetClass(), NULL,
                                           LABEL_Text, (ULONG)"Video",
                                           TAG_DONE);

    if (!file || !mode || !c2p || !h264 || !lace || !scale ||
        !audio_rate || !fast_buffer || !no_audio || !mono_audio ||
        !video_mode || !info ||
        !file_label || !display_label || !c2p_label || !h264_label ||
        !scale_label || !audio_rate_label || !fast_buffer_label ||
        !video_mode_label)
        goto cleanup;

    /* Keep the option area comfortably inside a 640-pixel A1200 Workbench.
     * The tightly-related display/C2P/H.264 controls stay together on the
     * first row; audio and buffering live on the second row. Scale (None/
     * 2x/Copper 2x) replaces the old separate 2x and Copper 2x checkboxes -
     * Copper only ever does anything on top of 2x, and only for a c2p/
     * display combination that supports it (update_mode_controls() snaps
     * the selection back to plain 2x otherwise), so one three-way chooser
     * reads better than two checkboxes whose relationship wasn't visible. */
    controls_top = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                       LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                       LAYOUT_SpaceInner, TRUE,
                                       LAYOUT_AddChild, (ULONG)mode,
                                       CHILD_Label, (ULONG)display_label,
                                       LAYOUT_AddChild, (ULONG)c2p,
                                       CHILD_Label, (ULONG)c2p_label,
                                       LAYOUT_AddChild, (ULONG)h264,
                                       CHILD_Label, (ULONG)h264_label,
                                       LAYOUT_AddChild, (ULONG)lace,
                                       CHILD_WeightedWidth, 0,
                                       LAYOUT_AddChild, (ULONG)scale,
                                       CHILD_Label, (ULONG)scale_label,
                                       TAG_DONE);
    if (!controls_top)
        goto cleanup;

    controls_bottom = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                          LAYOUT_Orientation,
                                          LAYOUT_ORIENT_HORIZ,
                                          LAYOUT_SpaceInner, TRUE,
                                          LAYOUT_AddChild, (ULONG)audio_rate,
                                          CHILD_Label, (ULONG)audio_rate_label,
                                          LAYOUT_AddChild, (ULONG)fast_buffer,
                                          CHILD_Label, (ULONG)fast_buffer_label,
                                          LAYOUT_AddChild, (ULONG)no_audio,
                                          CHILD_WeightedWidth, 0,
                                          LAYOUT_AddChild, (ULONG)mono_audio,
                                          CHILD_WeightedWidth, 0,
                                          LAYOUT_AddChild, (ULONG)video_mode,
                                          CHILD_Label, (ULONG)video_mode_label,
                                          TAG_DONE);
    if (!controls_bottom)
        goto cleanup;

    controls = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                    LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
                                    LAYOUT_SpaceInner, TRUE,
                                    LAYOUT_AddChild, (ULONG)controls_top,
                                    CHILD_WeightedHeight, 0,
                                    LAYOUT_AddChild, (ULONG)controls_bottom,
                                    CHILD_WeightedHeight, 0,
                                    TAG_DONE);
    if (!controls)
        goto cleanup;

    play_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                      GA_ID, G_PLAY,
                                      GA_Text, (ULONG)">",
                                      GA_RelVerify, TRUE,
                                      TAG_DONE);
    pause_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                       GA_ID, G_PAUSE,
                                       GA_Text, (ULONG)"||",
                                       GA_RelVerify, TRUE,
                                       TAG_DONE);
    stop_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                      GA_ID, G_STOP,
                                      GA_Text, (ULONG)"[]",
                                      GA_RelVerify, TRUE,
                                      TAG_DONE);
    ff_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                    GA_ID, G_FF,
                                    GA_Text, (ULONG)">>",
                                    GA_RelVerify, TRUE,
                                    TAG_DONE);
    iptv_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                      GA_ID, G_IPTV,
                                      GA_Text, (ULONG)"IPTV...",
                                      GA_RelVerify, TRUE,
                                      TAG_DONE);
    youtube_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                         GA_ID, G_YOUTUBE,
                                         GA_Text, (ULONG)"YouTube...",
                                         GA_RelVerify, TRUE,
                                         TAG_DONE);
    volume_down_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                             GA_ID, G_VOLUME_DOWN,
                                             GA_Text, (ULONG)"-",
                                             GA_RelVerify, TRUE, TAG_DONE);
    volume_up_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                           GA_ID, G_VOLUME_UP,
                                           GA_Text, (ULONG)"+",
                                           GA_RelVerify, TRUE, TAG_DONE);
    playlist_button = (Object *)NewObject(BUTTON_GetClass(), NULL,
                                          GA_ID, G_PLAYLIST,
                                          GA_Text, (ULONG)"Playlist",
                                          GA_RelVerify, TRUE, TAG_DONE);
    if (!play_button || !pause_button || !stop_button || !ff_button ||
        !iptv_button || !youtube_button || !volume_down_button ||
        !volume_up_button || !playlist_button)
        goto cleanup;

    transport = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                     LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                     LAYOUT_SpaceInner, TRUE,
                                     LAYOUT_AddChild, (ULONG)play_button,
                                     CHILD_WeightedWidth, 0,
                                     LAYOUT_AddChild, (ULONG)pause_button,
                                     CHILD_WeightedWidth, 0,
                                     LAYOUT_AddChild, (ULONG)stop_button,
                                     CHILD_WeightedWidth, 0,
                                     LAYOUT_AddChild, (ULONG)ff_button,
                                     CHILD_WeightedWidth, 0,
                                     LAYOUT_AddChild, (ULONG)volume_down_button,
                                     CHILD_WeightedWidth, 0,
                                     LAYOUT_AddChild, (ULONG)volume_up_button,
                                     CHILD_WeightedWidth, 0,
                                     TAG_DONE);
    if (!transport)
        goto cleanup;

    services = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                    LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
                                    LAYOUT_SpaceInner, TRUE,
                                    LAYOUT_AddChild, (ULONG)iptv_button,
                                    CHILD_WeightedWidth, 0,
                                    LAYOUT_AddChild, (ULONG)youtube_button,
                                    CHILD_WeightedWidth, 0,
                                    LAYOUT_AddChild, (ULONG)playlist_button,
                                    CHILD_WeightedWidth, 0,
                                    TAG_DONE);
    if (!services)
        goto cleanup;

    buttons = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                   LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
                                   LAYOUT_SpaceInner, TRUE,
                                   LAYOUT_AddChild, (ULONG)transport,
                                   CHILD_WeightedHeight, 0,
                                   LAYOUT_AddChild, (ULONG)services,
                                   CHILD_WeightedHeight, 0,
                                   TAG_DONE);
    if (!buttons)
        goto cleanup;

    layout = (Object *)NewObject(LAYOUT_GetClass(), NULL,
                                  LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
                                  LAYOUT_SpaceOuter, TRUE,
                                  LAYOUT_SpaceInner, TRUE,
                                  LAYOUT_AddChild, (ULONG)file,
                                  CHILD_Label, (ULONG)file_label,
                                  LAYOUT_AddChild, (ULONG)controls,
                                  CHILD_WeightedHeight, 0,
                                  LAYOUT_AddChild, (ULONG)buttons,
                                  CHILD_WeightedHeight, 0,
                                  LAYOUT_AddChild, (ULONG)info,
                                  CHILD_WeightedHeight, 0,
                                  TAG_DONE);
    if (!layout)
        goto cleanup;

    window_object = (Object *)NewObject(WINDOW_GetClass(), NULL,
                                         WA_Title,
                                         (ULONG)"MintVID Control",
                                         WA_Activate, TRUE,
                                         WA_DepthGadget, TRUE,
                                         WA_DragBar, TRUE,
                                         WA_CloseGadget, TRUE,
                                         WA_SizeGadget, TRUE,
                                         WA_IDCMP,
                                         IDCMP_GADGETUP |
                                         IDCMP_CLOSEWINDOW |
                                         IDCMP_MENUPICK |
                                         IDCMP_IDCMPUPDATE |
                                         IDCMP_REFRESHWINDOW,
                                         WINDOW_Position, WPOS_CENTERSCREEN,
                                         WINDOW_ParentGroup, (ULONG)layout,
                                         TAG_DONE);
    if (!window_object)
        goto cleanup;

    window = (struct Window *)RA_OpenWindow(window_object);
    if (!window)
        goto cleanup;
    mr_gui_menu_open(&app_menu, window);

    update_mode_controls(mode, c2p, lace, scale, window, TRUE);
    master_options = mr_master_options_open();
    publish_play_options(master_options, mode, c2p, h264, lace, scale,
                         audio_rate, fast_buffer, no_audio, mono_audio,
                         video_mode);
    GetAttr(WINDOW_SigMask, window_object, &sigmask);
    if (status_timer_open()) {
        timermask = 1UL << status_timer_port->mp_SigBit;
        status_timer_start();
    }

    for (;;) {
        signals = Wait(sigmask | timermask | SIGBREAKF_CTRL_C);
        if (signals & SIGBREAKF_CTRL_C)
            break;
        if (timermask && (signals & timermask)) {
            while (GetMsg(status_timer_port))
                ;
            status_timer_running = 0;
            poll_player_status(info, window);
            poll_iptv_launch(info, window, iptv_button);
            status_timer_start();
        }

        while ((result = RA_HandleInput(window_object, &code)) !=
               WMHI_LASTMSG) {
            switch (result & WMHI_CLASSMASK) {
            case WMHI_CLOSEWINDOW:
                goto done;

            case WMHI_MENUPICK: {
                int action = mr_gui_menu_action(&app_menu, code);
                if (action == MR_GUI_MENU_ABOUT)
                    mr_gui_show_about(window, "ReAction edition");
                else if (action == MR_GUI_MENU_GUIDE)
                    mr_gui_open_guide(&app_menu, window);
                else if (action == MR_GUI_MENU_QUIT)
                    goto done;
                break;
            }

            case WMHI_GADGETUP:
                switch (result & WMHI_GADGETMASK) {
                case G_FILE:
                    if (gfRequestFile(file, window))
                        update_file_info(file, info, window);
                    break;

                case G_MODE:
                    update_mode_controls(mode, c2p, lace, scale, window, TRUE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_C2P:
                    update_mode_controls(mode, c2p, lace, scale, window, FALSE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_SCALE:
                    /* Re-validate here too (not just on G_MODE/G_C2P): picking
                     * an incompatible c2p/mode first and only then Copper 2x
                     * must snap back just as reliably as the reverse order. */
                    update_mode_controls(mode, c2p, lace, scale, window, FALSE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_H264:
                case G_LACE:
                case G_AUDIO_RATE:
                case G_FAST_BUFFER:
                case G_NO_AUDIO:
                case G_MONO_AUDIO:
                case G_VIDEO_MODE:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    break;

                case G_PLAY:
                    start_player(file, mode, c2p, h264, lace, scale,
                                 audio_rate, fast_buffer, no_audio, mono_audio,
                                 video_mode, info, window);
                    break;

                case G_PAUSE:
                    signal_player(SIGBREAKF_CTRL_D);
                    break;

                case G_STOP:
                    stop_player_and_wait();
                    break;

                case G_FF:
                    signal_player(SIGBREAKF_CTRL_E);
                    break;

                case G_VOLUME_DOWN:
                    mr_player_control_send(MR_PLAYER_COMMAND_VOLUME_DOWN);
                    break;

                case G_VOLUME_UP:
                    mr_player_control_send(MR_PLAYER_COMMAND_VOLUME_UP);
                    break;

                case G_PLAYLIST:
                    if (!playlist_gui.window_object &&
                        mrg_playlist_open(&playlist_gui, &playlist, mode, c2p,
                                          h264, lace, scale, audio_rate,
                                          fast_buffer, no_audio, mono_audio,
                                          video_mode, info, window))
                        sigmask |= playlist_gui.sigmask;
                    break;

                case G_IPTV:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    start_iptv_launch(info, window, iptv_button);
                    open_iptv_browser(mode, c2p, h264, lace, scale,
                                      audio_rate, fast_buffer, no_audio,
                                      mono_audio, video_mode, info, window);
                    break;

                case G_YOUTUBE:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, scale, audio_rate, fast_buffer,
                                         no_audio, mono_audio, video_mode);
                    open_youtube_browser(mode, c2p, h264, lace, scale,
                                         audio_rate, fast_buffer, no_audio,
                                         mono_audio, video_mode, info, window);
                    break;

                default:
                    break;
                }
                break;

            default:
                break;
            }
        }
        if (playlist_gui.window_object &&
            (signals & playlist_gui.sigmask))
            mrg_playlist_handle(&playlist_gui);
    }

done:
    mrg_playlist_close(&playlist_gui);
    status = RETURN_OK;
    stop_player_and_wait();

cleanup:
    status_timer_close();
    mr_master_options_close(&master_options);
    mr_gui_menu_close(&app_menu, window);
    if (window_object) {
        if (window)
            RA_CloseWindow(window_object);
        DisposeObject(window_object);
    } else if (layout) {
        DisposeObject(layout);
    } else {
        if (controls) {
            DisposeObject(controls);
        } else {
            if (controls_top) {
                DisposeObject(controls_top);
            } else {
                if (mode) DisposeObject(mode);
                if (c2p) DisposeObject(c2p);
                if (h264) DisposeObject(h264);
                if (lace) DisposeObject(lace);
                if (scale) DisposeObject(scale);
                if (display_label) DisposeObject(display_label);
                if (c2p_label) DisposeObject(c2p_label);
                if (h264_label) DisposeObject(h264_label);
                if (scale_label) DisposeObject(scale_label);
            }
            if (controls_bottom) {
                DisposeObject(controls_bottom);
            } else {
                if (audio_rate) DisposeObject(audio_rate);
                if (fast_buffer) DisposeObject(fast_buffer);
                if (no_audio) DisposeObject(no_audio);
                if (mono_audio) DisposeObject(mono_audio);
                if (video_mode) DisposeObject(video_mode);
                if (audio_rate_label) DisposeObject(audio_rate_label);
                if (fast_buffer_label) DisposeObject(fast_buffer_label);
                if (video_mode_label) DisposeObject(video_mode_label);
            }
        }

        if (buttons) {
            DisposeObject(buttons);
        } else {
            if (transport) {
                DisposeObject(transport);
            } else {
                if (play_button) DisposeObject(play_button);
                if (pause_button) DisposeObject(pause_button);
                if (stop_button) DisposeObject(stop_button);
                if (ff_button) DisposeObject(ff_button);
                if (volume_down_button) DisposeObject(volume_down_button);
                if (volume_up_button) DisposeObject(volume_up_button);
            }
            if (services) {
                DisposeObject(services);
            } else {
                if (iptv_button) DisposeObject(iptv_button);
                if (youtube_button) DisposeObject(youtube_button);
                if (playlist_button) DisposeObject(playlist_button);
            }
        }

        if (file)
            DisposeObject(file);
        if (info)
            DisposeObject(info);
        if (file_label)
            DisposeObject(file_label);
    }

    free_chooser_nodes(&modes);
    free_chooser_nodes(&c2p_modes);
    free_chooser_nodes(&h264_modes);
    free_chooser_nodes(&scale_modes);
    free_chooser_nodes(&audio_rate_modes);
    free_chooser_nodes(&fast_buffer_modes);
    free_chooser_nodes(&video_modes);
    close_reaction_classes();
    return status;
}
