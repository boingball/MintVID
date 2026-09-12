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
#include "mr_player_status.h"

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
    G_2X,
    G_COPPER,
    G_AUDIO_RATE,
    G_FAST_BUFFER,
    G_NO_AUDIO,
    G_MONO_AUDIO,
    G_IPTV,
    G_YOUTUBE
};

/* Chooser rows are chipset-dependent, so never infer a display mode from a
 * hard-coded row number. This map is populated alongside the labels. */
static mr_display_mode mode_values[7];
static unsigned mode_count;
static mr_c2p_mode c2p_values[4];
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
    StringBase = OpenLibrary((CONST_STRPTR)"gadgets/string.gadget",
                             MRGUI_CLASS_VERSION);
    LabelBase = OpenLibrary((CONST_STRPTR)"images/label.image",
                            MRGUI_CLASS_VERSION);

    return IntuitionBase && GfxBase && UtilityBase && AslBase &&
           WindowBase && LayoutBase && ButtonBase && CheckBoxBase &&
           ChooserBase && GetFileBase && StringBase && LabelBase;
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
                              Object *lace, Object *twox, Object *copper,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio,
                              mr_play_options *options)
{
    ULONG selected = 0, selected_c2p = 0, selected_h264 = 0;
    ULONG checked_lace = 0, checked_2x = 0, checked_copper = 0;
    ULONG selected_audio_rate = 0, selected_fast_buffer = 0;
    ULONG checked_no_audio = 0, checked_mono = 0;
    mr_play_options_default(options);
    GetAttr(CHOOSER_Selected, mode, &selected);
    GetAttr(CHOOSER_Selected, c2p, &selected_c2p);
    GetAttr(CHOOSER_Selected, h264, &selected_h264);
    GetAttr(CHECKBOX_Checked, lace, &checked_lace);
    GetAttr(CHECKBOX_Checked, twox, &checked_2x);
    GetAttr(CHECKBOX_Checked, copper, &checked_copper);
    GetAttr(CHOOSER_Selected, audio_rate, &selected_audio_rate);
    GetAttr(CHOOSER_Selected, fast_buffer, &selected_fast_buffer);
    GetAttr(CHECKBOX_Checked, no_audio, &checked_no_audio);
    GetAttr(CHECKBOX_Checked, mono_audio, &checked_mono);
    options->display = selected < mode_count
                     ? mode_values[selected] : MR_DISPLAY_AGA;
    options->c2p = selected_c2p < c2p_count
                 ? c2p_values[selected_c2p] : MR_C2P_STANDARD;
    options->laced = checked_lace != 0;
    options->scale_2x = checked_2x != 0;
    options->copper_vdouble = checked_copper != 0;
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
}

static void publish_play_options(mr_master_options_port *master_options,
                                 Object *mode, Object *c2p, Object *h264,
                                 Object *lace, Object *twox, Object *copper,
                                 Object *audio_rate, Object *fast_buffer,
                                 Object *no_audio,
                                 Object *mono_audio)
{
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, twox, copper, audio_rate, fast_buffer,
                      no_audio, mono_audio, &options);
    mr_master_options_publish(master_options, &options);
}

static void open_iptv_browser(Object *mode, Object *c2p, Object *h264,
                              Object *lace, Object *twox, Object *copper,
                              Object *audio_rate, Object *fast_buffer,
                              Object *no_audio,
                              Object *mono_audio, Object *info,
                              struct Window *window)
{
    BPTR seglist;
    struct Process *process;
    char arguments[512];
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, twox, copper, audio_rate, fast_buffer,
                      no_audio, mono_audio, &options);
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
                                 Object *lace, Object *twox, Object *copper,
                                 Object *audio_rate, Object *fast_buffer,
                                 Object *no_audio,
                                 Object *mono_audio, Object *info,
                                 struct Window *window)
{
    BPTR seglist;
    struct Process *process;
    char arguments[512];
    mr_play_options options;

    read_play_options(mode, c2p, h264, lace, twox, copper, audio_rate, fast_buffer,
                      no_audio, mono_audio, &options);
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
                                 Object *twox, Object *copper,
                                 struct Window *window, int output_changed)
{
    ULONG selected;
    ULONG disable_chipset_options;
    ULONG selected_c2p;
    mr_c2p_mode selected_c2p_mode;
    int kalms_available;
    int direct_available;

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
    }
    if (selected_c2p_mode == MR_C2P_DIRECT &&
        !disable_chipset_options && !direct_available) {
        SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                       CHOOSER_Selected, c2p_row(MR_C2P_STANDARD), TAG_DONE);
    }

    SetGadgetAttrs((struct Gadget *)c2p, window, NULL,
                   GA_Disabled, disable_chipset_options,
                   TAG_DONE);
    if (disable_chipset_options) {
        SetGadgetAttrs((struct Gadget *)lace, window, NULL,
                       GA_Disabled, TRUE,
                       CHECKBOX_Checked, FALSE,
                       TAG_DONE);
        SetGadgetAttrs((struct Gadget *)twox, window, NULL,
                       GA_Disabled, TRUE,
                       CHECKBOX_Checked, FALSE,
                       TAG_DONE);
        SetGadgetAttrs((struct Gadget *)copper, window, NULL,
                       GA_Disabled, TRUE,
                       CHECKBOX_Checked, FALSE,
                       TAG_DONE);
    } else {
        SetGadgetAttrs((struct Gadget *)lace, window, NULL,
                       GA_Disabled, FALSE,
                       TAG_DONE);
        SetGadgetAttrs((struct Gadget *)twox, window, NULL,
                       GA_Disabled, FALSE,
                       TAG_DONE);
        SetGadgetAttrs((struct Gadget *)copper, window, NULL,
                       GA_Disabled, FALSE,
                       TAG_DONE);
    }
}

#define STATUS_POLL_MICROS 250000UL
/* iptvgui's own process exists (LoadSeg()/CreateNewProcTags() has already
 * returned) long before its window is actually open and interactive - it
 * still has to load/refresh its channel cache first, which is the real
 * source of the "takes a while to show the window" delay. Polled on the
 * same STATUS_POLL_MICROS tick as poll_player_status() below, so ~15s here
 * is that many ticks, not a separate timer. Bounded so a launch that never
 * completes (missing binary, crash) doesn't leave the button disabled and
 * the busy pointer up forever. */
#define IPTV_LAUNCH_TIMEOUT_TICKS 60UL

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

static void start_player(Object *file, Object *mode, Object *c2p,
                         Object *h264,
                         Object *lace, Object *twox, Object *copper,
                         Object *audio_rate, Object *fast_buffer,
                         Object *no_audio,
                         Object *mono_audio, Object *info,
                         struct Window *window)
{
    char path[512];
    char args[1600];
    mr_play_options options;
    STRPTR full_file;
    BPTR seglist;
    struct Process *process;

    full_file = NULL;

    GetAttr(GETFILE_FullFile, file, (ULONG *)&full_file);
    if (!full_file || !*full_file) {
        set_info(info, window, "Choose a video first.");
        return;
    }

    if (mr_path_is_audio_only((const char *)full_file)) {
        set_info(info, window, "Audio-only file: use MintAMP instead.");
        return;
    }

    if (find_player()) {
        set_info(info, window,
                 "A MintVID player is already running; stop it first.");
        return;
    }

    strncpy(path, (const char *)full_file, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;

    read_play_options(mode, c2p, h264, lace, twox, copper, audio_rate, fast_buffer,
                      no_audio, mono_audio, &options);
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
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)args,
        NP_StackSize, MRPLAY_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"mrplay",
        NP_Name, (ULONG)"MintVID player",
        TAG_END);

    if (!process) {
        UnLoadSeg(seglist);
        set_info(info, window, "Could not create the mrplay process.");
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
    Object *twox;
    Object *copper;
    Object *audio_rate;
    Object *fast_buffer;
    Object *no_audio;
    Object *mono_audio;
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
    Object *audio_rate_label;
    Object *fast_buffer_label;
    Object *play_button;
    Object *pause_button;
    Object *stop_button;
    Object *ff_button;
    Object *iptv_button;
    Object *youtube_button;
    struct Window *window;
    mr_master_options_port *master_options;
    mr_gui_menu app_menu;
    struct List modes;
    struct List c2p_modes;
    struct List h264_modes;
    struct List audio_rate_modes;
    struct List fast_buffer_modes;
    ULONG sigmask;
    ULONG result;
    ULONG signals;
    ULONG timermask;
    UWORD code;
    int status;
    int have_rtg;
    int default_mode;

    window_object = NULL;
    timermask = 0;
    file = NULL;
    mode = NULL;
    c2p = NULL;
    h264 = NULL;
    lace = NULL;
    twox = NULL;
    copper = NULL;
    audio_rate = NULL;
    fast_buffer = NULL;
    no_audio = NULL;
    mono_audio = NULL;
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
    play_button = NULL;
    pause_button = NULL;
    stop_button = NULL;
    ff_button = NULL;
    iptv_button = NULL;
    youtube_button = NULL;
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
    audio_rate_modes.lh_Head = (struct Node *)&audio_rate_modes.lh_Tail;
    audio_rate_modes.lh_Tail = NULL;
    audio_rate_modes.lh_TailPred = (struct Node *)&audio_rate_modes.lh_Head;
    fast_buffer_modes.lh_Head = (struct Node *)&fast_buffer_modes.lh_Tail;
    fast_buffer_modes.lh_Tail = NULL;
    fast_buffer_modes.lh_TailPred = (struct Node *)&fast_buffer_modes.lh_Head;

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
        !add_c2p_node(&c2p_modes, "Kalms", MR_C2P_KALMS)
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
    if (!add_chooser_node(&audio_rate_modes, "Normal") ||
        !add_chooser_node(&audio_rate_modes, "Low"))
        goto cleanup;
    if (!add_chooser_node(&fast_buffer_modes, "Auto") ||
        !add_chooser_node(&fast_buffer_modes, "Off") ||
        !add_chooser_node(&fast_buffer_modes, "4 MB") ||
        !add_chooser_node(&fast_buffer_modes, "8 MB") ||
        !add_chooser_node(&fast_buffer_modes, "16 MB"))
        goto cleanup;

    file = (Object *)NewObject(GETFILE_GetClass(), NULL,
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
    twox = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                               GA_ID, G_2X,
                               GA_Text, (ULONG)"2x",
                               GA_RelVerify, TRUE,
                               TAG_DONE);
    copper = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
                                 GA_ID, G_COPPER,
                                 GA_Text, (ULONG)"Copper 2x",
                                 GA_RelVerify, TRUE,
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

    if (!file || !mode || !c2p || !h264 || !lace || !twox || !copper ||
        !audio_rate || !fast_buffer || !no_audio || !mono_audio || !info ||
        !file_label || !display_label || !c2p_label || !h264_label ||
        !audio_rate_label || !fast_buffer_label)
        goto cleanup;

    /* Keep the option area comfortably inside a 640-pixel A1200 Workbench.
     * The tightly-related display/C2P/H.264 controls stay together on the
     * first row; audio and buffering live on the second row. "Copper 2x"
     * sits right after "2x" since it only ever does anything when 2x is
     * also on (display_aga.c silently ignores it otherwise) - four small
     * fixed-width checkboxes/choosers should still fit, but this is the
     * spot to trim first if a real 640-wide Workbench run says otherwise. */
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
                                       LAYOUT_AddChild, (ULONG)twox,
                                       CHILD_WeightedWidth, 0,
                                       LAYOUT_AddChild, (ULONG)copper,
                                       CHILD_WeightedWidth, 0,
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
    if (!play_button || !pause_button || !stop_button || !ff_button ||
        !iptv_button || !youtube_button)
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

    update_mode_controls(mode, c2p, lace, twox, copper, window, TRUE);
    master_options = mr_master_options_open();
    publish_play_options(master_options, mode, c2p, h264, lace, twox, copper,
                         audio_rate, fast_buffer, no_audio, mono_audio);
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
                    update_mode_controls(mode, c2p, lace, twox, copper, window, TRUE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, twox, copper, audio_rate, fast_buffer,
                                         no_audio, mono_audio);
                    break;

                case G_C2P:
                    update_mode_controls(mode, c2p, lace, twox, copper, window, FALSE);
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, twox, copper, audio_rate, fast_buffer,
                                         no_audio, mono_audio);
                    break;

                case G_H264:
                case G_LACE:
                case G_2X:
                case G_COPPER:
                case G_AUDIO_RATE:
                case G_FAST_BUFFER:
                case G_NO_AUDIO:
                case G_MONO_AUDIO:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, twox, copper, audio_rate, fast_buffer,
                                         no_audio, mono_audio);
                    break;

                case G_PLAY:
                    start_player(file, mode, c2p, h264, lace, twox, copper,
                                 audio_rate, fast_buffer, no_audio, mono_audio,
                                 info, window);
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

                case G_IPTV:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, twox, copper, audio_rate, fast_buffer,
                                         no_audio, mono_audio);
                    start_iptv_launch(info, window, iptv_button);
                    open_iptv_browser(mode, c2p, h264, lace, twox, copper,
                                      audio_rate, fast_buffer, no_audio,
                                      mono_audio, info, window);
                    break;

                case G_YOUTUBE:
                    publish_play_options(master_options, mode, c2p, h264,
                                         lace, twox, copper, audio_rate, fast_buffer,
                                         no_audio, mono_audio);
                    open_youtube_browser(mode, c2p, h264, lace, twox, copper,
                                         audio_rate, fast_buffer, no_audio,
                                         mono_audio, info, window);
                    break;

                default:
                    break;
                }
                break;

            default:
                break;
            }
        }
    }

done:
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
                if (twox) DisposeObject(twox);
                if (copper) DisposeObject(copper);
                if (display_label) DisposeObject(display_label);
                if (c2p_label) DisposeObject(c2p_label);
                if (h264_label) DisposeObject(h264_label);
            }
            if (controls_bottom) {
                DisposeObject(controls_bottom);
            } else {
                if (audio_rate) DisposeObject(audio_rate);
                if (fast_buffer) DisposeObject(fast_buffer);
                if (no_audio) DisposeObject(no_audio);
                if (mono_audio) DisposeObject(mono_audio);
                if (audio_rate_label) DisposeObject(audio_rate_label);
                if (fast_buffer_label) DisposeObject(fast_buffer_label);
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
            }
            if (services) {
                DisposeObject(services);
            } else {
                if (iptv_button) DisposeObject(iptv_button);
                if (youtube_button) DisposeObject(youtube_button);
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
    free_chooser_nodes(&audio_rate_modes);
    free_chooser_nodes(&fast_buffer_modes);
    close_reaction_classes();
    return status;
}
