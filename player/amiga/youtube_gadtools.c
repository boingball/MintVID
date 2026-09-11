/* MintVID YouTube-GT: OS 3.0 GadTools frontend, shared search/player core. */
#include "../core/mr_alloc.h"
#include "../core/mr_http.h"
#include "../core/mr_play_options.h"
#include "../core/mr_source.h"
#include "../youtube/mr_youtube_search.h"
#include "mr_gui_menu.h"
#include "mr_master_options.h"
#include "mr_player_status.h"

MINTVID_DECLARE_VERSION(ytgui_gt_version_tag, "ytgui-GT");

#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YT_SEARCH_PAGE_MAX (6UL * 1024UL * 1024UL)
#define MRPLAY_STACK_SIZE 320000UL
/* Persistent storage, not RAM: - see the matching comment in
 * youtube_reaction.c for why (a hard lockup needs a reset to clear, which
 * also wipes RAM: and the log with it; UHD0: survives that). */
#define MRPLAY_LOG_FILE "UHD0:MintVID.log"
#define WIN_W 640
#define WIN_H 356

#if defined(__GNUC__)
static const char ytgui_gt_stack_cookie[] __attribute__((used))="$STACK:131072";
#endif

struct IntuitionBase *IntuitionBase;
extern struct Library *GadToolsBase;

enum {
    G_QUERY = 1, G_TYPE, G_SEARCH, G_RESULTS, G_PLAY, G_PAUSE, G_FAST,
    G_VOLDOWN, G_VOLUP, G_FULLSCREEN, G_STOP, G_QUALITY, G_CHANNEL, G_LOG,
    G_CLOSE,
    G_SUMMARY, G_STATUS
};

typedef struct ytgt {
    struct Screen *screen;
    struct Window *window;
    APTR visual;
    struct Gadget *gadgets, *query, *type, *results, *play, *quality, *log;
    struct Gadget *summary, *status;
    struct List labels;
    mr_youtube_search_results found;
    mr_play_options options;
    unsigned quality_index;
    ULONG status_seq;
    mr_gui_menu menu;
    struct MsgPort *timer_port;
    struct timerequest *timer_io;
    int timer_open, timer_running, debug_log, log_session_open;
    LONG selected_index;
    struct DateStamp last_click_ds;
    LONG last_click_index;
    int have_last_click;
} ytgt;

/* ~600ms at dos.library's 50 ticks/sec (3000 ticks/minute) - a reasonable
 * double-click window when the toolkit's own event loop doesn't expose the
 * IDCMP message timestamps DoubleClick() (intuition.library) would need. */
#define DOUBLE_CLICK_TICKS 30UL

static ULONG ds_ticks(const struct DateStamp *ds)
{
    return ((ULONG)ds->ds_Days * 24UL * 60UL + (ULONG)ds->ds_Minute) * 3000UL
         + (ULONG)ds->ds_Tick;
}

static STRPTR type_labels[] = {(STRPTR)"All", (STRPTR)"Videos",
                               (STRPTR)"Live", (STRPTR)"Shorts", NULL};
static const char *quality_labels[] = {
    "Quality: Low", "Quality: 360p", "Quality: 480p",
    "Quality: 720p", "Quality: 1080p", "Quality: Best"
};
static const struct TextAttr topaz = {(STRPTR)"topaz.font", 8, 0, 0};

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static unsigned quality_from_options(const mr_play_options *o)
{
    if (o->hls_low) return 0;
    if (o->hls_max_height <= 360 && o->hls_max_height) return 1;
    if (o->hls_max_height <= 480 && o->hls_max_height) return 2;
    if (o->hls_max_height <= 720 && o->hls_max_height) return 3;
    if (o->hls_max_height <= 1080 && o->hls_max_height) return 4;
    return 5;
}

static void set_quality(mr_play_options *o, unsigned q)
{
    static const unsigned w[] = {640, 640, 854, 1280, 1920, 0};
    static const unsigned h[] = {0, 360, 480, 720, 1080, 0};
    if (q >= 6) q = 0;
    o->hls_low = q == 0;
    o->hls_max_width = w[q];
    o->hls_max_height = h[q];
}

static void set_text(ytgt *app, struct Gadget *gadget, const char *text)
{
    if (app->window && gadget)
        GT_SetGadgetAttrs(gadget, app->window, NULL,
                         GTTX_Text, (ULONG)(text ? text : ""), TAG_DONE);
    if (app->window && gadget)
        RefreshGList(gadget, app->window, NULL, 1);
}

/* GadTools BUTTON_KIND gadgets render their label from the internal string
 * GTTX_Text set at creation - Gadget->GadgetText is not what gets drawn, so
 * poking it directly (as this used to do) silently does nothing: the button
 * stays showing its original text no matter what is written there. Only
 * GT_SetGadgetAttrs()/GTTX_Text actually changes what's on screen. */
static void set_button_text(ytgt *app, struct Gadget *gadget,
                            const char *text)
{
    if (!app->window || !gadget)
        return;
    GT_SetGadgetAttrs(gadget, app->window, NULL,
                      GTTX_Text, (ULONG)text, TAG_DONE);
    RefreshGList(gadget, app->window, NULL, 1);
}

static ULONG value(ytgt *app, struct Gadget *gadget, ULONG tag)
{
    ULONG out = 0;
    GT_GetGadgetAttrs(gadget, app->window, NULL, tag, (ULONG)&out, TAG_DONE);
    return out;
}

static void free_labels(ytgt *app)
{
    struct Node *node;
    if (app->window && app->results)
        GT_SetGadgetAttrs(app->results, app->window, NULL,
                         GTLV_Labels, ~0UL, TAG_DONE);
    while ((node = RemHead(&app->labels)) != NULL)
        FreeVec(node);
}

static size_t rebuild_labels(ytgt *app)
{
    size_t i;
    for (i = 0; i < app->found.count; i++) {
        struct Node *node = AllocVec(sizeof(*node), MEMF_PUBLIC | MEMF_CLEAR);
        if (!node)
            break;
        node->ln_Name = (STRPTR)app->found.items[i].row;
        AddTail(&app->labels, node);
    }
    GT_SetGadgetAttrs(app->results, app->window, NULL,
                     GTLV_Labels, (ULONG)&app->labels,
                     GTLV_Selected, i ? 0 : ~0UL, TAG_DONE);
    app->selected_index = i ? 0 : -1;
    RefreshGList(app->results, app->window, NULL, 1);
    GT_SetGadgetAttrs(app->play, app->window, NULL,
                     GA_Disabled, i ? FALSE : TRUE, TAG_DONE);
    return i;
}

static int player_running(void)
{
    int yes;
    Forbid();
    yes = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT) != NULL;
    Permit();
    return yes;
}

static int stop_player(void)
{
    struct MsgPort *port;
    int had, waited;
    Forbid();
    port = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
    if (port) Signal(port->mp_SigTask, SIGBREAKF_CTRL_F);
    had = port != NULL;
    Permit();
    for (waited = 0; had && waited < 250 && player_running(); waited++)
        Delay(1);
    return had ? (player_running() ? -1 : 1) : 0;
}

static int start_video(ytgt *app, const mr_youtube_search_result *video)
{
    char url[96], args[640];
    BPTR seglist, log = 0, nil = 0;
    struct Process *process;
    if (!video || !mr_youtube_search_watch_url(url, sizeof(url), video) ||
        !mr_build_player_arguments(args, sizeof(args), &app->options, url,
                                   NULL, NULL))
        return 0;
    if (app->debug_log) {
        size_t length = strlen(args);
        if (length && args[length - 1] == '\n')
            length--;
        if (length + 8 < sizeof(args))
            memcpy(args + length, " --time\n", 9);
    }
    stop_player();
    if (player_running()) return 0;
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrplay");
    if (!seglist) seglist = LoadSeg((CONST_STRPTR)"mrplay");
    if (!seglist) return 0;
    if (app->debug_log) {
        if (!app->log_session_open) {
            log = Open((CONST_STRPTR)MRPLAY_LOG_FILE, MODE_NEWFILE);
            if (log)
                app->log_session_open = 1;
        } else {
            log = Open((CONST_STRPTR)MRPLAY_LOG_FILE, MODE_READWRITE);
            if (log) {
                static const char separator[] =
                    "\n\n===== new YouTube video =====\n";
                Seek(log, 0, OFFSET_END);
                Write(log, (APTR)separator, (LONG)(sizeof(separator) - 1));
            }
        }
        nil = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
        if (!log || !nil) {
            if (log) Close(log);
            if (nil) Close(nil);
            log = nil = 0;
        }
    }
    if (log && nil)
        process = CreateNewProcTags(
            NP_Seglist, seglist, NP_FreeSeglist, TRUE,
            NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
            NP_Cli, TRUE, NP_CommandName, (ULONG)"mrplay",
            NP_Name, (ULONG)"MintVID player",
            NP_Input, (ULONG)nil, NP_CloseInput, TRUE,
            NP_Output, (ULONG)log, NP_CloseOutput, TRUE, TAG_END);
    else
        process = CreateNewProcTags(
            NP_Seglist, seglist, NP_FreeSeglist, TRUE,
            NP_Arguments, (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE,
            NP_Cli, TRUE, NP_CommandName, (ULONG)"mrplay",
            NP_Name, (ULONG)"MintVID player", TAG_END);
    if (!process) {
        if (log) Close(log);
        if (nil) Close(nil);
        UnLoadSeg(seglist);
    }
    return process != NULL;
}

static mr_youtube_search_result *selected(ytgt *app);

/* Shared by the Play button and double-clicking a result - same checks and
 * status text either way. */
static void play_selected(ytgt *app)
{
    mr_youtube_search_result *video = selected(app);
    mr_master_options_apply(&app->options);
    if (!video) set_text(app, app->status, "Select a video first.");
    else if (!start_video(app, video)) set_text(app, app->status, "Could not start mrplay.");
    else set_text(app, app->status, video->live ? "Resolving YouTube Live..." : "Resolving YouTube video...");
}

static void load_url(ytgt *app, const char *url, mr_youtube_search_mode mode,
                     const char *busy, const char *done)
{
#if !defined(HAVE_AMISSL)
    (void)url; (void)mode; (void)busy; (void)done;
    set_text(app, app->status, "YouTube search needs SSL=1.");
#else
    static const char ua[] = "Mozilla/5.0 (X11; Linux x86_64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36";
    mr_http_options http;
    char *document = NULL, line[256];
    size_t size = 0, shown;
    set_text(app, app->status, busy);
    GT_SetGadgetAttrs(app->play, app->window, NULL,
                     GA_Disabled, TRUE, TAG_DONE);
    if (!mr_http_options_init(&http, ua, "https://www.youtube.com/") ||
        !mr_http_fetch_text(url, &http, &document, &size,
                            YT_SEARCH_PAGE_MAX)) {
        snprintf(line, sizeof(line), "YouTube search failed: %s",
                 mr_source_last_error());
        set_text(app, app->status, line);
        return;
    }
    free_labels(app);
    mr_youtube_search_results_free(&app->found);
    if (!mr_youtube_search_parse_mode(&app->found, document, size, mode)) {
        set_text(app, app->status, mr_youtube_search_last_error());
        mr_free(document);
        return;
    }
    mr_free(document);
    shown = rebuild_labels(app);
    snprintf(line, sizeof(line), "%lu %s", (unsigned long)shown, done);
    set_text(app, app->status, shown ? line : mr_youtube_search_last_error());
#endif
}

/* A pasted YouTube video URL in the search field plays directly instead of
 * being searched for - no network fetch, just recognizing the URL and
 * queuing it as a single result via the same rebuild_labels() path a real
 * search uses, so it picks up the existing auto-select-first-result and
 * enable-Play behaviour with no changes to either. */
static int load_pasted_url(ytgt *app, const char *query_text)
{
    mr_youtube_search_result pasted;
    mr_youtube_search_result *items;

    if (!query_text || !mr_youtube_url_parse(&pasted, query_text))
        return 0;
    items = (mr_youtube_search_result *)malloc(sizeof(*items));
    if (!items) {
        set_text(app, app->status, "Not enough memory for this URL.");
        return 1;
    }
    *items = pasted;
    free_labels(app);
    mr_youtube_search_results_free(&app->found);
    app->found.items = items;
    app->found.count = 1;
    rebuild_labels(app);
    set_text(app, app->status, "Video URL recognized - press Play.");
    return 1;
}

static void search(ytgt *app)
{
    static const char *summary[] = {"all results shown", "videos shown",
                                    "live results shown", "Shorts shown"};
    STRPTR query = NULL;
    ULONG mode = value(app, app->type, GTCY_Active);
    char url[1024];
    GT_GetGadgetAttrs(app->query, app->window, NULL,
                     GTST_String, (ULONG)&query, TAG_DONE);
    if (load_pasted_url(app, (const char *)query))
        return;
    if (mode > MR_YOUTUBE_SEARCH_SHORTS) mode = MR_YOUTUBE_SEARCH_ALL;
    if (!mr_youtube_search_build_url_mode(url, sizeof(url),
                                          query ? (const char *)query : "",
                                          (mr_youtube_search_mode)mode)) {
        set_text(app, app->status, mr_youtube_search_last_error());
        return;
    }
    load_url(app, url, (mr_youtube_search_mode)mode,
             "Searching YouTube...", summary[mode]);
}

static mr_youtube_search_result *selected(ytgt *app)
{
    ULONG index;
    if (app->selected_index < 0)
        return NULL;
    index = (ULONG)app->selected_index;
    return index < app->found.count ? &app->found.items[index] : NULL;
}

static void channel(ytgt *app)
{
    mr_youtube_search_result *video = selected(app);
    char url[256], summary[192];
    if (!video || !mr_youtube_channel_videos_url(url, sizeof(url), video)) {
        set_text(app, app->status, "Select a result with a channel first.");
        return;
    }
    snprintf(summary, sizeof(summary), "videos shown from %s", video->channel);
    load_url(app, url, MR_YOUTUBE_SEARCH_ALL, "Loading channel videos...",
             summary);
}

static void poll_status(ytgt *app)
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
        snprintf(line, sizeof(line), "Stream error: %s", ps.text);
    else if (ps.state == MR_PLAYER_STATE_ENDED)
        snprintf(line, sizeof(line), "Video ended");
    else
        snprintf(line, sizeof(line), "%s", ps.text[0] ? ps.text : "Connecting...");
    set_text(app, app->status, line);
}

static int timer_open(ytgt *app)
{
    app->timer_port = CreateMsgPort();
    if (!app->timer_port) return 0;
    app->timer_io = (struct timerequest *)CreateIORequest(app->timer_port,
                                                          sizeof(*app->timer_io));
    if (!app->timer_io || OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
        (struct IORequest *)app->timer_io, 0)) return 0;
    app->timer_open = 1;
    return 1;
}

static void timer_start(ytgt *app)
{
    app->timer_io->tr_node.io_Command = TR_ADDREQUEST;
    app->timer_io->tr_time.tv_secs = 0;
    app->timer_io->tr_time.tv_micro = 200000;
    SendIO((struct IORequest *)app->timer_io);
    app->timer_running = 1;
}

static struct Gadget *add(ytgt *app, struct Gadget *previous, ULONG kind,
                          UWORD id, int x, int y, int w, int h,
                          const char *label, ULONG tag, ULONG data)
{
    struct NewGadget ng;
    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge=x; ng.ng_TopEdge=y; ng.ng_Width=w; ng.ng_Height=h;
    ng.ng_GadgetText=(STRPTR)label; ng.ng_TextAttr=(struct TextAttr *)&topaz;
    ng.ng_GadgetID=id; ng.ng_VisualInfo=app->visual;
    ng.ng_Flags=kind==BUTTON_KIND?PLACETEXT_IN:
                kind==CHECKBOX_KIND?PLACETEXT_RIGHT:PLACETEXT_LEFT;
    if (kind == LISTVIEW_KIND)
        return CreateGadget(kind, previous, &ng, tag, data,
                            GTLV_Selected, ~0UL,
                            GTLV_ShowSelected, (ULONG)NULL,
                            GA_RelVerify, TRUE, TAG_DONE);
    return CreateGadget(kind, previous, &ng, tag, data, TAG_DONE);
}

static int window_open(ytgt *app)
{
    struct Gadget *g;
    struct NewWindow nw;
    app->screen = LockPubScreen(NULL);
    if (!app->screen) return 0;
    app->visual = GetVisualInfoA(app->screen, NULL);
    if (!app->visual || !CreateContext(&app->gadgets)) return 0;
    g = app->gadgets;
    app->query = g = add(app,g,STRING_KIND,G_QUERY,8,20,390,15,"",
                         GTST_MaxChars,200);
    app->type = g = add(app,g,CYCLE_KIND,G_TYPE,404,20,108,15,"",
                        GTCY_Labels,(ULONG)type_labels);
    g = add(app,g,BUTTON_KIND,G_SEARCH,518,20,110,15,"Search",TAG_IGNORE,0);
    app->results = g = add(app,g,LISTVIEW_KIND,G_RESULTS,8,43,620,192,"",
                           GTLV_Labels,(ULONG)&app->labels);
    g = add(app,g,BUTTON_KIND,G_PLAY,8,243,82,17,"Play",TAG_IGNORE,0);
    app->play = g;
    g = add(app,g,BUTTON_KIND,G_PAUSE,94,243,82,17,"Pause",TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_FAST,180,243,82,17,"Fast",TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_VOLDOWN,266,243,82,17,"Vol -",TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_VOLUP,352,243,82,17,"Vol +",TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_FULLSCREEN,438,243,100,17,"Fullscreen",TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_STOP,542,243,86,17,"Stop",TAG_IGNORE,0);
    app->quality = g = add(app,g,BUTTON_KIND,G_QUALITY,8,266,150,17,
        quality_labels[app->quality_index],TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_CHANNEL,162,266,220,17,"Channel videos",TAG_IGNORE,0);
    app->log = g = add(app,g,BUTTON_KIND,G_LOG,386,266,110,17,
                       "Log: Off",TAG_IGNORE,0);
    g = add(app,g,BUTTON_KIND,G_CLOSE,500,266,128,17,"Close",TAG_IGNORE,0);
    app->summary = g = add(app,g,TEXT_KIND,G_SUMMARY,8,290,620,15,"",
                           GTTX_Text,(ULONG)"Playback options");
    app->status = g = add(app,g,TEXT_KIND,G_STATUS,8,311,620,15,"",
        GTTX_Text,(ULONG)"Search public live streams and recorded videos.");
    if (!g) return 0;
    memset(&nw,0,sizeof(nw));
    nw.LeftEdge=(app->screen->Width-WIN_W)/2; nw.TopEdge=(app->screen->Height-WIN_H)/2;
    nw.Width=WIN_W; nw.Height=WIN_H; nw.DetailPen=0; nw.BlockPen=1;
    /* LISTVIEWIDCMP (gadtools.h) supplies the mouse-button/move, gadget-down
     * and IntuiTicks events GadTools' composite scroller and auto-repeat
     * up/down arrow gadgets consume internally - IDCMP_GADGETUP alone leaves
     * the results listview's arrows drawn but unresponsive (same fix as
     * MintAMP's amiga_mp3gui.c radio browser window). */
    nw.IDCMPFlags=LISTVIEWIDCMP|IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW|IDCMP_MENUPICK;
    nw.Flags=WFLG_CLOSEGADGET|WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_ACTIVATE|WFLG_SMART_REFRESH;
    nw.FirstGadget=app->gadgets; nw.Title=(UBYTE *)"MintVID YouTube-GT";
    nw.Screen=app->screen; nw.Type=CUSTOMSCREEN;
    app->window=OpenWindow(&nw);
    if (app->window) {
        GT_SetGadgetAttrs(app->type,app->window,NULL,GTCY_Active,
                         MR_YOUTUBE_SEARCH_LIVE,TAG_DONE);
        GT_SetGadgetAttrs(app->play,app->window,NULL,GA_Disabled,TRUE,TAG_DONE);
    }
    return app->window != NULL;
}

static void cleanup(ytgt *app)
{
    if (app->timer_io) {
        if (app->timer_running && !CheckIO((struct IORequest *)app->timer_io))
            AbortIO((struct IORequest *)app->timer_io);
        if (app->timer_running) WaitIO((struct IORequest *)app->timer_io);
        if (app->timer_open) CloseDevice((struct IORequest *)app->timer_io);
        DeleteIORequest((struct IORequest *)app->timer_io);
    }
    if (app->timer_port) DeleteMsgPort(app->timer_port);
    mr_gui_menu_close(&app->menu, app->window);
    free_labels(app);
    mr_youtube_search_results_free(&app->found);
    if (app->window) CloseWindow(app->window);
    if (app->gadgets) FreeGadgets(app->gadgets);
    if (app->visual) FreeVisualInfo(app->visual);
    if (app->screen) UnlockPubScreen(NULL, app->screen);
    mr_http_net_shutdown();
}

int main(int argc, char **argv)
{
    ytgt app;
    char error[160], summary[160];
    int done=0, rc=RETURN_FAIL;
    ULONG winmask, timermask=0;
    memset(&app,0,sizeof(app));
    init_list(&app.labels);
    app.selected_index=-1;
    mr_youtube_search_results_init(&app.found);
    mr_play_options_default(&app.options);
    if (!mr_play_options_parse(&app.options,argc,argv,error,sizeof(error)))
        return RETURN_FAIL;
    app.quality_index=quality_from_options(&app.options);
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library",37);
    GadToolsBase=OpenLibrary((CONST_STRPTR)"gadtools.library",37);
    if (!IntuitionBase || !GadToolsBase || !window_open(&app)) goto out;
    mr_play_options_summary(&app.options,summary,sizeof(summary));
    set_text(&app,app.summary,summary);
    mr_gui_menu_open(&app.menu,app.window);
    if (timer_open(&app)) { timermask=1UL<<app.timer_port->mp_SigBit; timer_start(&app); }
    winmask=1UL<<app.window->UserPort->mp_SigBit;
    while (!done) {
        struct IntuiMessage *msg;
        ULONG signals=Wait(winmask|timermask|SIGBREAKF_CTRL_C);
        if (signals&SIGBREAKF_CTRL_C) break;
        if (timermask && (signals&timermask)) {
            while (GetMsg(app.timer_port)) { }
            app.timer_running=0;
            poll_status(&app); timer_start(&app);
        }
        while ((msg=GT_GetIMsg(app.window->UserPort))!=NULL) {
            ULONG cls=msg->Class; UWORD code=msg->Code;
            struct Gadget *gad=(struct Gadget *)msg->IAddress;
            UWORD id=gad?gad->GadgetID:0; GT_ReplyIMsg(msg);
            if (cls==IDCMP_CLOSEWINDOW) done=1;
            else if (cls==IDCMP_REFRESHWINDOW) { GT_BeginRefresh(app.window); GT_EndRefresh(app.window,TRUE); }
            else if (cls==IDCMP_MENUPICK) {
                int action=mr_gui_menu_action(&app.menu,code);
                if (action==MR_GUI_MENU_ABOUT) mr_gui_show_about(app.window,"YouTube GadTools edition (OS 3.0)");
                else if (action==MR_GUI_MENU_QUIT) done=1;
            } else if (cls==IDCMP_GADGETUP) {
                if (id==G_CLOSE) done=1;
                else if (id==G_RESULTS) {
                    struct DateStamp now;
                    int dbl;
                    app.selected_index=(LONG)code;
                    GT_SetGadgetAttrs(app.results,app.window,NULL,
                                      GTLV_Selected,(ULONG)code,TAG_DONE);
                    DateStamp(&now);
                    dbl = app.have_last_click && (LONG)code==app.last_click_index &&
                          ds_ticks(&now)-ds_ticks(&app.last_click_ds)<=DOUBLE_CLICK_TICKS;
                    app.last_click_ds=now; app.last_click_index=(LONG)code;
                    app.have_last_click=1;
                    if (dbl) { play_selected(&app); app.have_last_click=0; }
                }
                else if (id==G_SEARCH || id==G_QUERY) search(&app);
                else if (id==G_CHANNEL) channel(&app);
                else if (id==G_STOP) set_text(&app,app.status,stop_player()?"Playback stopped.":"No video is playing.");
                else if (id==G_LOG) {
                    app.debug_log=!app.debug_log;
                    set_button_text(&app,app.log,
                                    app.debug_log?"Log: On":"Log: Off");
                    set_text(&app,app.status,app.debug_log
                        ?"Timing log ON: next Play writes " MRPLAY_LOG_FILE
                        :"Timing log off.");
                }
                else if (id==G_PAUSE) mr_player_control_send(MR_PLAYER_COMMAND_PAUSE);
                else if (id==G_FAST) mr_player_control_send(MR_PLAYER_COMMAND_FAST);
                else if (id==G_FULLSCREEN) mr_player_control_send(MR_PLAYER_COMMAND_FULLSCREEN);
                else if (id==G_VOLDOWN) mr_player_control_send(MR_PLAYER_COMMAND_VOLUME_DOWN);
                else if (id==G_VOLUP) mr_player_control_send(MR_PLAYER_COMMAND_VOLUME_UP);
                else if (id==G_QUALITY) {
                    app.quality_index=(app.quality_index+1)%6; set_quality(&app.options,app.quality_index);
                    set_button_text(&app,app.quality,
                                    quality_labels[app.quality_index]);
                    mr_play_options_summary(&app.options,summary,sizeof(summary)); set_text(&app,app.summary,summary);
                } else if (id==G_PLAY) {
                    play_selected(&app);
                }
            }
        }
    }
    rc=RETURN_OK;
out:
    cleanup(&app);
    if (GadToolsBase) { CloseLibrary(GadToolsBase); GadToolsBase=NULL; }
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
