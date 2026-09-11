/* MintVID-GT: OS 3.0 GadTools controller for the shared mrplay engine. */
#include "../core/mr_play_options.h"
#include "mr_akiko.h"
#include "mr_gui_menu.h"
#include "mr_master_options.h"
#include "mr_player_status.h"

MINTVID_DECLARE_VERSION(mintvid_gt_version_tag, "MintVID-GT");

#include <cybergraphx/cybergraphics.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <proto/asl.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>

#define MRPLAY_STACK_SIZE 320000UL
/* Keep the OS3 GadTools controller inside a standard 640-pixel A1200
 * Workbench.  The long display cycle still gets enough room for the RTG
 * label, while related controls are grouped across two option rows. */
#define WIN_W 632
#define WIN_H 180

#if defined(__GNUC__)
static const char mrgui_gt_stack_cookie[] __attribute__((used))="$STACK:131072";
#endif

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *AslBase;
struct Library *CyberGfxBase;
extern struct Library *GadToolsBase;

enum {
    G_FILE = 1, G_BROWSE, G_MODE, G_C2P, G_H264, G_LACE, G_2X,
    G_AUDIO_RATE, G_FAST_BUFFER, G_NO_AUDIO, G_MONO_AUDIO,
    G_PLAY, G_PAUSE, G_STOP, G_FAST, G_IPTV, G_YOUTUBE, G_INFO
};

typedef struct gt_app {
    struct Screen *screen;
    struct Window *window;
    APTR visual;
    struct Gadget *gadgets;
    struct Gadget *file, *mode, *c2p, *h264, *lace, *twox, *info;
    struct Gadget *audio_rate, *fast_buffer, *no_audio, *mono_audio;
    struct FileRequester *requester;
    mr_master_options_port *master;
    mr_gui_menu menu;
    mr_display_mode modes[7];
    STRPTR mode_labels[8];
    unsigned mode_count;
    mr_c2p_mode c2p_modes[4];
    STRPTR c2p_labels[5];
    unsigned c2p_count;
    char path[512];
    struct MsgPort *timer_port;
    struct timerequest *timer_io;
    int timer_open, timer_running;
    ULONG status_seq;
} gt_app;

static STRPTR h264_labels[] = {(STRPTR)"H.264: Auto", (STRPTR)"H.264: Quality",
                              (STRPTR)"H.264: Balanced", (STRPTR)"H.264: Fast",
                              (STRPTR)"H.264: Turbo", (STRPTR)"H.264: Turbo+",
                              (STRPTR)"H.264: TurboGT", NULL};
static STRPTR audio_rate_labels[] = {(STRPTR)"Audio: Normal",
                                    (STRPTR)"Audio: Low", NULL};
static STRPTR fast_buffer_labels[] = {(STRPTR)"Fast buffer: Auto",
                                     (STRPTR)"Fast buffer: Off",
                                     (STRPTR)"Fast buffer: 4 MB",
                                     (STRPTR)"Fast buffer: 8 MB",
                                     (STRPTR)"Fast buffer: 16 MB", NULL};
static const struct TextAttr topaz = {(STRPTR)"topaz.font", 8, 0, 0};

static int aga(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_AA_LISA) != 0;
}

static int ecs(void)
{
    return GfxBase && (GfxBase->ChipRevBits0 & GFXF_HR_DENISE) != 0;
}

static void add_c2p_mode(gt_app *app, STRPTR label, mr_c2p_mode value)
{
    if (app->c2p_count >= sizeof app->c2p_modes / sizeof app->c2p_modes[0])
        return;
    app->c2p_labels[app->c2p_count] = label;
    app->c2p_modes[app->c2p_count++] = value;
    app->c2p_labels[app->c2p_count] = NULL;
}

static ULONG c2p_row(const gt_app *app, mr_c2p_mode value)
{
    unsigned i;
    for (i = 0; i < app->c2p_count; i++)
        if (app->c2p_modes[i] == value)
            return (ULONG)i;
    return 0;
}

static int screen_is_rtg(struct Screen *screen)
{
    ULONG id;
    if (!screen || !CyberGfxBase)
        return 0;
    id = GetVPModeID(&screen->ViewPort);
    return id != (ULONG)INVALID_ID && IsCyberModeID(id);
}

static void set_info(gt_app *app, const char *text)
{
    if (app->window && app->info)
        GT_SetGadgetAttrs(app->info, app->window, NULL,
                         GTTX_Text, (ULONG)(text ? text : ""), TAG_DONE);
    if (app->window && app->info)
        RefreshGList(app->info, app->window, NULL, 1);
}

/* Mirrors the separate mrplay process's codec/position/error status (the
 * same mechanism the IPTV-GT and YouTube-GT browsers already use) onto
 * Info:, including the "| H:MM:SS"/"| M:SS" playhead mrplay folds on about
 * once a second - see mrplay.c's player_update_position(). Leaves Info:
 * alone once no player is running, so it keeps showing whatever file/type
 * text set_info() last set from play_file(). */
static void poll_status(gt_app *app)
{
    mr_player_status ps;
    char line[300];
    if (!mr_player_status_read(&ps)) { app->status_seq = 0; return; }
    if (ps.seq == app->status_seq) return;
    app->status_seq = ps.seq;
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
        return; /* STARTING/OPENING: leave whatever Info: already shows */
    set_info(app, line);
}

static int status_timer_open(gt_app *app)
{
    app->timer_port = CreateMsgPort();
    if (!app->timer_port) return 0;
    app->timer_io = (struct timerequest *)
        CreateIORequest(app->timer_port, sizeof(*app->timer_io));
    if (!app->timer_io || OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                                     (struct IORequest *)app->timer_io, 0))
        return 0;
    app->timer_open = 1;
    return 1;
}

static void status_timer_start(gt_app *app)
{
    app->timer_io->tr_node.io_Command = TR_ADDREQUEST;
    app->timer_io->tr_time.tv_secs = 0;
    app->timer_io->tr_time.tv_micro = 250000; /* 4 Hz */
    SendIO((struct IORequest *)app->timer_io);
    app->timer_running = 1;
}

static void status_timer_close(gt_app *app)
{
    if (app->timer_io) {
        if (app->timer_running && !CheckIO((struct IORequest *)app->timer_io))
            AbortIO((struct IORequest *)app->timer_io);
        if (app->timer_running)
            WaitIO((struct IORequest *)app->timer_io);
        if (app->timer_open)
            CloseDevice((struct IORequest *)app->timer_io);
        DeleteIORequest((struct IORequest *)app->timer_io);
    }
    if (app->timer_port)
        DeleteMsgPort(app->timer_port);
}

static ULONG gad_value(gt_app *app, struct Gadget *gadget, ULONG tag)
{
    ULONG value = 0;
    GT_GetGadgetAttrs(gadget, app->window, NULL, tag, (ULONG)&value, TAG_DONE);
    return value;
}

static void read_options(gt_app *app, mr_play_options *options)
{
    ULONG mode = gad_value(app, app->mode, GTCY_Active);
    ULONG c2p = gad_value(app, app->c2p, GTCY_Active);
    ULONG h264 = gad_value(app, app->h264, GTCY_Active);
    ULONG audio_rate = gad_value(app, app->audio_rate, GTCY_Active);
    ULONG fast_buffer = gad_value(app, app->fast_buffer, GTCY_Active);
    mr_play_options_default(options);
    options->display = mode < app->mode_count ? app->modes[mode]
                                               : MR_DISPLAY_AGA;
    options->c2p = c2p < app->c2p_count
                 ? app->c2p_modes[c2p] : MR_C2P_STANDARD;
    options->h264_performance = h264 <= MR_H264_PERF_TURBO_GT
                              ? (mr_h264_performance)h264
                              : MR_H264_PERF_AUTO;
    options->laced = gad_value(app, app->lace, GTCB_Checked) != 0;
    options->scale_2x = gad_value(app, app->twox, GTCB_Checked) != 0;
    options->audio_rate = audio_rate == 1
                        ? MR_AUDIO_RATE_LOW : MR_AUDIO_RATE_NORMAL;
    options->fast_buffer = fast_buffer <= MR_FAST_BUFFER_16MB
                         ? (mr_fast_buffer_mode)fast_buffer
                         : MR_FAST_BUFFER_AUTO;
    options->no_audio = gad_value(app, app->no_audio, GTCB_Checked) != 0;
    options->mono_audio = gad_value(app, app->mono_audio, GTCB_Checked) != 0;
}

static void publish_options(gt_app *app)
{
    mr_play_options options;
    read_options(app, &options);
    mr_master_options_publish(app->master, &options);
}

static void update_mode_controls(gt_app *app, int output_changed)
{
    ULONG selected = gad_value(app, app->mode, GTCY_Active);
    ULONG disabled = selected < app->mode_count &&
                     (app->modes[selected] == MR_DISPLAY_CGX ||
                      app->modes[selected] == MR_DISPLAY_P96);
    ULONG selected_c2p = gad_value(app, app->c2p, GTCY_Active);
    mr_c2p_mode selected_c2p_mode = selected_c2p < app->c2p_count
                                  ? app->c2p_modes[selected_c2p]
                                  : MR_C2P_STANDARD;
    int kalms_available;
    int direct_available;

    /* HAM8 uses the normal eight-plane Kalms converter. CPU-specific 040/060
     * GUI builds also keep Kalms selected for the linked HAM6 bitmap kernel.
     * Lower-depth indexed modes snap back to Standard; disabled RTG mode keeps
     * the selection so returning to AGA restores the faster default. */
    kalms_available = selected < app->mode_count &&
                      ((app->modes[selected] == MR_DISPLAY_AGA && aga()) ||
                       app->modes[selected] == MR_DISPLAY_HAM8
#ifdef MR_KALMS_040
                       || app->modes[selected] == MR_DISPLAY_HAM6
#endif
                      );
    /* Direct only ever targets the plain 1:1 8-plane AGA case (no HAM, no
     * resize) - see display_aga.c's aga_supports_yuv_indexed() - so unlike
     * Kalms it is never available for HAM6/HAM8. Only listed at all on a
     * 040/060 build (see add_c2p_mode() below). */
    direct_available = 0;
#ifdef MR_KALMS_040
    direct_available = selected < app->mode_count &&
                       app->modes[selected] == MR_DISPLAY_AGA && aga();
#endif

    /* Restore the fast default after an unsupported output forced Standard.
     * A manual Standard choice remains valid until the output is changed, and
     * an explicit CD32/Akiko selection is never overwritten. Direct is never
     * auto-selected this way - it stays a deliberate, manual opt-in, not a
     * new default over the established Kalms path. */
    if (output_changed && kalms_available &&
        selected_c2p_mode == MR_C2P_STANDARD) {
        GT_SetGadgetAttrs(app->c2p, app->window, NULL,
                         GTCY_Active, c2p_row(app, MR_C2P_KALMS), TAG_DONE);
        selected_c2p_mode = MR_C2P_KALMS;
    }
    if (selected_c2p_mode == MR_C2P_KALMS &&
        !disabled && !kalms_available) {
        GT_SetGadgetAttrs(app->c2p, app->window, NULL,
                         GTCY_Active, c2p_row(app, MR_C2P_STANDARD), TAG_DONE);
    }
    if (selected_c2p_mode == MR_C2P_DIRECT &&
        !disabled && !direct_available) {
        GT_SetGadgetAttrs(app->c2p, app->window, NULL,
                         GTCY_Active, c2p_row(app, MR_C2P_STANDARD), TAG_DONE);
    }

    GT_SetGadgetAttrs(app->c2p, app->window, NULL,
                     GA_Disabled, disabled, TAG_DONE);
    GT_SetGadgetAttrs(app->lace, app->window, NULL,
                     GA_Disabled, disabled, TAG_DONE);
    GT_SetGadgetAttrs(app->twox, app->window, NULL,
                     GA_Disabled, disabled, TAG_DONE);
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

static void stop_player(void)
{
    unsigned guard;
    signal_player(SIGBREAKF_CTRL_F);
    for (guard = 0; guard < 250 && find_player(); guard++)
        Delay(1);
}

static int launch(const char *program, const char *task_name,
                  const char *arguments)
{
    char path[64];
    BPTR seglist;
    struct Process *process;
    snprintf(path, sizeof(path), "PROGDIR:%s", program);
    seglist = LoadSeg((CONST_STRPTR)path);
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)program);
    if (!seglist)
        return 0;
    process = CreateNewProcTags(NP_Seglist, seglist, NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)(arguments ? arguments : ""),
        NP_StackSize, MRPLAY_STACK_SIZE, NP_Cli, TRUE,
        NP_CommandName, (ULONG)program, NP_Name, (ULONG)task_name, TAG_END);
    if (!process)
        UnLoadSeg(seglist);
    return process != NULL;
}

static void play_file(gt_app *app)
{
    mr_play_options options;
    char args[1600];
    STRPTR entered = NULL;
    GT_GetGadgetAttrs(app->file, app->window, NULL,
                     GTST_String, (ULONG)&entered, TAG_DONE);
    if (entered) {
        strncpy(app->path, (const char *)entered, sizeof(app->path) - 1);
        app->path[sizeof(app->path) - 1] = 0;
    }
    if (!app->path[0]) {
        set_info(app, "Choose a video first.");
        return;
    }
    if (mr_path_is_audio_only(app->path)) {
        set_info(app, "Audio-only file: use MintAMP instead.");
        return;
    }
    if (find_player()) {
        set_info(app, "A MintVID player is already running; stop it first.");
        return;
    }
    read_options(app, &options);
    if (!mr_build_player_arguments(args, sizeof(args), &options, app->path,
                                   NULL, NULL) ||
        !launch("mrplay", "MintVID player", args))
        set_info(app, "Could not start mrplay (keep it beside MintVID-GT).");
    else
        set_info(app, "Starting playback...");
}

static void browse(gt_app *app)
{
    if (!app->requester)
        app->requester = AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText, (ULONG)"Choose a video", ASLFR_DoSaveMode, FALSE,
            ASLFR_RejectIcons, TRUE, ASLFR_DoPatterns, TRUE,
            ASLFR_InitialPattern, (ULONG)MR_VIDEO_FILE_PATTERN, TAG_DONE);
    if (!app->requester ||
        !AslRequestTags(app->requester, ASLFR_Window, (ULONG)app->window,
                        TAG_DONE))
        return;
    strncpy(app->path, (const char *)app->requester->fr_Drawer,
            sizeof(app->path) - 1);
    app->path[sizeof(app->path) - 1] = 0;
    if (!AddPart((STRPTR)app->path, app->requester->fr_File,
                 sizeof(app->path))) {
        set_info(app, "The selected path is too long.");
        return;
    }
    GT_SetGadgetAttrs(app->file, app->window, NULL,
                     GTST_String, (ULONG)app->path, TAG_DONE);
    set_info(app, app->path);
}

static void open_browser(gt_app *app, int youtube)
{
    mr_play_options options;
    char args[512];
    const char *program = youtube ? "ytgui-GT" : "iptvgui-GT";
    const char *task = youtube ? "MintVID YouTube GT" : "MintVID IPTV GT";
    read_options(app, &options);
    publish_options(app);
    if (!mr_build_iptv_arguments(args, sizeof(args), &options) ||
        !launch(program, task, args))
        set_info(app, youtube ? "Could not start ytgui-GT."
                              : "Could not start iptvgui-GT.");
}

static struct Gadget *add_gadget(gt_app *app, struct Gadget *previous,
                                 ULONG kind, UWORD id, int x, int y, int w,
                                 int h, const char *label, ULONG tag1,
                                 ULONG data1)
{
    struct NewGadget ng;
    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = x;
    ng.ng_TopEdge = y;
    ng.ng_Width = w;
    ng.ng_Height = h;
    ng.ng_GadgetText = (STRPTR)label;
    ng.ng_TextAttr = (struct TextAttr *)&topaz;
    ng.ng_GadgetID = id;
    ng.ng_VisualInfo = app->visual;
    ng.ng_Flags = kind == BUTTON_KIND ? PLACETEXT_IN :
                  kind == CHECKBOX_KIND ? PLACETEXT_RIGHT : PLACETEXT_LEFT;
    if (kind == TEXT_KIND)
        return CreateGadget(kind, previous, &ng, tag1, data1,
                            GTTX_Border, TRUE, TAG_DONE);
    return CreateGadget(kind, previous, &ng, tag1, data1, TAG_DONE);
}

static int build_window(gt_app *app)
{
    struct Gadget *g;
    struct NewWindow nw;
    int default_mode = 0;

    app->screen = LockPubScreen(NULL);
    if (!app->screen)
        return 0;
    app->visual = GetVisualInfoA(app->screen, NULL);
    if (!app->visual || !CreateContext(&app->gadgets))
        return 0;

    if (aga()) {
        app->mode_labels[app->mode_count] = (STRPTR)"Display: AGA (256)";
        app->modes[app->mode_count++] = MR_DISPLAY_AGA;
        app->mode_labels[app->mode_count] = (STRPTR)"Display: ECS (32)";
        app->modes[app->mode_count++] = MR_DISPLAY_AGA_ECS32;
        app->mode_labels[app->mode_count] = (STRPTR)"Display: ECS (16)";
        app->modes[app->mode_count++] = MR_DISPLAY_AGA_ECS16;
    } else {
        app->mode_labels[app->mode_count] = ecs() ? (STRPTR)"Display: ECS" :
                                                     (STRPTR)"Display: OCS";
        app->modes[app->mode_count++] = MR_DISPLAY_AGA;
    }
    app->mode_labels[app->mode_count] = (STRPTR)"Display: HAM6";
    app->modes[app->mode_count++] = MR_DISPLAY_HAM6;
    if (aga()) {
        app->mode_labels[app->mode_count] = (STRPTR)"Display: HAM8";
        app->modes[app->mode_count++] = MR_DISPLAY_HAM8;
    }
    if (screen_is_rtg(app->screen)) {
        app->mode_labels[app->mode_count] = (STRPTR)"Display: RTG (WritePixel)";
        app->modes[app->mode_count++] = MR_DISPLAY_CGX;
        default_mode = (int)app->mode_count - 1;
        app->mode_labels[app->mode_count] = (STRPTR)"Display: RTG (P96)";
        app->modes[app->mode_count++] = MR_DISPLAY_P96;
    }
    app->mode_labels[app->mode_count] = NULL;

    add_c2p_mode(app, (STRPTR)"C2P: Standard", MR_C2P_STANDARD);
    if (mr_akiko_available())
        add_c2p_mode(app, (STRPTR)"C2P: CD32", MR_C2P_AKIKO);
    add_c2p_mode(app, (STRPTR)"C2P: Kalms", MR_C2P_KALMS);
#ifdef MR_KALMS_040
    add_c2p_mode(app, (STRPTR)"C2P: Direct", MR_C2P_DIRECT);
#endif

    g = app->gadgets;
    app->file = g = add_gadget(app, g, STRING_KIND, G_FILE, 8, 20, 535, 15,
        "", GTST_MaxChars, sizeof(app->path));
    g = add_gadget(app, g, BUTTON_KIND, G_BROWSE, 549, 20, 74, 15,
                   "Browse...", TAG_IGNORE, 0);

    /* Primary video options: keep C2P right beside Display, followed by H.264. */
    app->mode = g = add_gadget(app, g, CYCLE_KIND, G_MODE, 8, 44, 218, 16,
        "", GTCY_Labels, (ULONG)app->mode_labels);
    app->c2p = g = add_gadget(app, g, CYCLE_KIND, G_C2P, 232, 44, 128, 16,
        "", GTCY_Labels, (ULONG)app->c2p_labels);
    app->h264 = g = add_gadget(app, g, CYCLE_KIND, G_H264, 366, 44, 144, 16,
        "", GTCY_Labels, (ULONG)h264_labels);
    app->lace = g = add_gadget(app, g, CHECKBOX_KIND, G_LACE, 516, 45, 68, 14,
        "Laced", GTCB_Checked, FALSE);
    app->twox = g = add_gadget(app, g, CHECKBOX_KIND, G_2X, 590, 45, 36, 14,
        "2x", GTCB_Checked, FALSE);

    /* Secondary audio/source options. */
    app->audio_rate = g = add_gadget(app, g, CYCLE_KIND, G_AUDIO_RATE, 8, 68,
        135, 16, "", GTCY_Labels, (ULONG)audio_rate_labels);
    app->fast_buffer = g = add_gadget(app, g, CYCLE_KIND, G_FAST_BUFFER,
        149, 68, 190, 16, "", GTCY_Labels, (ULONG)fast_buffer_labels);
    app->no_audio = g = add_gadget(app, g, CHECKBOX_KIND, G_NO_AUDIO, 345, 69,
        94, 14, "No audio", GTCB_Checked, FALSE);
    app->mono_audio = g = add_gadget(app, g, CHECKBOX_KIND, G_MONO_AUDIO, 445,
        69, 110, 14, "Mono audio", GTCB_Checked, FALSE);

    /* Compact transport strip.  ASCII keeps the glyphs available on stock
     * Topaz while making the controls much narrower than word labels. */
    g = add_gadget(app, g, BUTTON_KIND, G_PLAY, 8, 92, 42, 18, ">",
                   TAG_IGNORE, 0);
    g = add_gadget(app, g, BUTTON_KIND, G_PAUSE, 56, 92, 42, 18, "||",
                   TAG_IGNORE, 0);
    g = add_gadget(app, g, BUTTON_KIND, G_STOP, 104, 92, 42, 18, "[]",
                   TAG_IGNORE, 0);
    g = add_gadget(app, g, BUTTON_KIND, G_FAST, 152, 92, 48, 18, ">>",
                   TAG_IGNORE, 0);

    /* Browsers get their own row so transport and service actions are visually
     * distinct and the controller never grows horizontally again. */
    g = add_gadget(app, g, BUTTON_KIND, G_IPTV, 8, 116, 100, 18, "IPTV...",
                   TAG_IGNORE, 0);
    g = add_gadget(app, g, BUTTON_KIND, G_YOUTUBE, 114, 116, 110, 18,
                   "YouTube...", TAG_IGNORE, 0);
    app->info = g = add_gadget(app, g, TEXT_KIND, G_INFO, 8, 140, 615, 16,
        "", GTTX_Text, (ULONG)"No file selected");
    if (!g)
        return 0;

    memset(&nw, 0, sizeof(nw));
    nw.LeftEdge = (app->screen->Width - WIN_W) / 2;
    nw.TopEdge = (app->screen->Height - WIN_H) / 2;
    nw.Width = WIN_W;
    nw.Height = WIN_H;
    nw.DetailPen = 0;
    nw.BlockPen = 1;
    nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
                    IDCMP_MENUPICK;
    nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
               WFLG_ACTIVATE | WFLG_SMART_REFRESH;
    nw.FirstGadget = app->gadgets;
    nw.Title = (UBYTE *)"MintVID Control-GT";
    nw.Screen = app->screen;
    nw.Type = CUSTOMSCREEN;
    app->window = OpenWindow(&nw);
    if (app->window) {
        GT_SetGadgetAttrs(app->mode, app->window, NULL,
                         GTCY_Active, default_mode, TAG_DONE);
        GT_SetGadgetAttrs(app->c2p, app->window, NULL,
                         GTCY_Active, c2p_row(app, MR_C2P_KALMS), TAG_DONE);
        GT_SetGadgetAttrs(app->h264, app->window, NULL,
                         GTCY_Active, MR_H264_PERF_TURBO_GT, TAG_DONE);
    }
    return app->window != NULL;
}

static void cleanup(gt_app *app)
{
    status_timer_close(app);
    mr_gui_menu_close(&app->menu, app->window);
    mr_master_options_close(&app->master);
    if (app->window)
        CloseWindow(app->window);
    if (app->gadgets)
        FreeGadgets(app->gadgets);
    if (app->requester)
        FreeAslRequest(app->requester);
    if (app->visual)
        FreeVisualInfo(app->visual);
    if (app->screen)
        UnlockPubScreen(NULL, app->screen);
}

int main(void)
{
    gt_app app;
    ULONG mask, timermask = 0;
    int done = 0, rc = RETURN_FAIL;
    memset(&app, 0, sizeof(app));
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 37);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    if (!IntuitionBase || !GfxBase || !AslBase || !GadToolsBase ||
        !build_window(&app))
        goto out;
    app.master = mr_master_options_open();
    update_mode_controls(&app, TRUE);
    publish_options(&app);
    mr_gui_menu_open(&app.menu, app.window);
    mask = 1UL << app.window->UserPort->mp_SigBit;
    if (status_timer_open(&app)) {
        timermask = 1UL << app.timer_port->mp_SigBit;
        status_timer_start(&app);
    }
    while (!done) {
        struct IntuiMessage *msg;
        ULONG signals = Wait(mask | timermask | SIGBREAKF_CTRL_C);
        if (signals & SIGBREAKF_CTRL_C)
            break;
        if (timermask && (signals & timermask)) {
            while (GetMsg(app.timer_port)) /* remove the replied request */
                ;
            app.timer_running = 0;
            poll_status(&app);
            status_timer_start(&app);
        }
        while ((msg = GT_GetIMsg(app.window->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            struct Gadget *gadget = (struct Gadget *)msg->IAddress;
            UWORD id = gadget ? gadget->GadgetID : 0;
            GT_ReplyIMsg(msg);
            if (cls == IDCMP_CLOSEWINDOW)
                done = 1;
            else if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(app.window);
                GT_EndRefresh(app.window, TRUE);
            } else if (cls == IDCMP_MENUPICK) {
                int action = mr_gui_menu_action(&app.menu, code);
                if (action == MR_GUI_MENU_ABOUT)
                    mr_gui_show_about(app.window, "GadTools edition (OS 3.0)");
                else if (action == MR_GUI_MENU_QUIT)
                    done = 1;
            } else if (cls == IDCMP_GADGETUP) {
                switch (id) {
                case G_BROWSE: browse(&app); break;
                case G_PLAY: play_file(&app); break;
                case G_PAUSE: signal_player(SIGBREAKF_CTRL_D); break;
                case G_STOP: stop_player(); break;
                case G_FAST: signal_player(SIGBREAKF_CTRL_E); break;
                case G_IPTV: open_browser(&app, 0); break;
                case G_YOUTUBE: open_browser(&app, 1); break;
                case G_MODE:
                    update_mode_controls(&app, TRUE);
                    publish_options(&app);
                    break;
                case G_C2P:
                    update_mode_controls(&app, FALSE);
                    publish_options(&app);
                    break;
                case G_H264: case G_LACE: case G_2X:
                case G_AUDIO_RATE: case G_FAST_BUFFER:
                case G_NO_AUDIO: case G_MONO_AUDIO:
                    publish_options(&app); break;
                default: break;
                }
            }
        }
    }
    rc = RETURN_OK;
    stop_player();
out:
    cleanup(&app);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GadToolsBase) { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (AslBase) CloseLibrary(AslBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
