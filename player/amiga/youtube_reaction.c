/* Standalone MintVID YouTube search window.
 *
 * Search uses YouTube's public results page (no account or API key), while
 * playback stays in the normal mrplay process and its native live resolver.
 */
#include "../core/mr_http.h"
#include "../core/mr_alloc.h"
#include "../core/mr_play_options.h"
#include "../core/mr_source.h"
#include "../iptv/mr_iptv.h"
#include "mr_player_status.h"
#include "mr_master_options.h"
#include "mr_gui_menu.h"
#include "../youtube/mr_youtube_search.h"

MINTVID_DECLARE_VERSION(ytgui_version_tag, "ytgui");

#include <classes/window.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/libraries.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <gadgets/button.h>
#include <gadgets/chooser.h>
#include <gadgets/layout.h>
#include <gadgets/listbrowser.h>
#include <gadgets/string.h>
#include <images/label.h>
#include <intuition/intuition.h>
#include <proto/button.h>
#include <proto/chooser.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/listbrowser.h>
#include <proto/string.h>
#include <proto/utility.h>
#include <proto/window.h>
#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YT_CLASS_VERSION 44
#define YT_SEARCH_PAGE_MAX (6UL * 1024UL * 1024UL)
#define MRPLAY_STACK_SIZE 320000UL
#define MRPLAY_LOG_FILE "RAM:MintVID.log"

struct IntuitionBase *IntuitionBase;
struct Library *UtilityBase, *WindowBase, *LayoutBase, *ButtonBase;
struct Library *ChooserBase, *ListBrowserBase, *StringBase, *LabelBase;

enum {
    G_QUERY = 1,
    G_SEARCH,
    G_SEARCH_TYPE,
    G_RESULTS,
    G_PLAY,
    G_QUALITY,
    G_PAUSE,
    G_FAST,
    G_FULLSCREEN,
    G_VOLUME_DOWN,
    G_VOLUME_UP,
    G_CHANNEL,
    G_STOP,
    G_LOG,
    G_CLOSE
};

static int mrplay_log_session_open;

static const char *quality_labels[] = {
    "Quality: Low", "Quality: 360p", "Quality: 480p",
    "Quality: 720p", "Quality: 1080p", "Quality: Best"
};

static unsigned quality_from_options(const mr_play_options *options)
{
    if (options->hls_low)
        return 0;
    if (options->hls_max_height) {
        if (options->hls_max_height <= 360) return 1;
        if (options->hls_max_height <= 480) return 2;
        if (options->hls_max_height <= 720) return 3;
        if (options->hls_max_height <= 1080) return 4;
    }
    if (!options->hls_max_height && options->hls_max_width) {
        if (options->hls_max_width <= 640) return 1;
        if (options->hls_max_width <= 854) return 2;
        if (options->hls_max_width <= 1280) return 3;
        if (options->hls_max_width <= 1920) return 4;
    }
    return 5;
}

static void set_quality(mr_play_options *options, unsigned quality)
{
    static const unsigned widths[] = {640, 640, 854, 1280, 1920, 0};
    static const unsigned heights[] = {0, 360, 480, 720, 1080, 0};
    if (quality >= sizeof(widths) / sizeof(widths[0]))
        quality = 0;
    options->hls_low = quality == 0;
    options->hls_max_width = widths[quality];
    options->hls_max_height = heights[quality];
}

static int open_classes(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
    WindowBase = OpenLibrary((CONST_STRPTR)"window.class", YT_CLASS_VERSION);
    LayoutBase = OpenLibrary((CONST_STRPTR)"gadgets/layout.gadget",
                             YT_CLASS_VERSION);
    ButtonBase = OpenLibrary((CONST_STRPTR)"gadgets/button.gadget",
                             YT_CLASS_VERSION);
    ChooserBase = OpenLibrary((CONST_STRPTR)"gadgets/chooser.gadget",
                              YT_CLASS_VERSION);
    ListBrowserBase = OpenLibrary((CONST_STRPTR)"gadgets/listbrowser.gadget",
                                  YT_CLASS_VERSION);
    StringBase = OpenLibrary((CONST_STRPTR)"gadgets/string.gadget",
                             YT_CLASS_VERSION);
    LabelBase = OpenLibrary((CONST_STRPTR)"images/label.image",
                            YT_CLASS_VERSION);
    return IntuitionBase && UtilityBase && WindowBase && LayoutBase &&
           ButtonBase && ChooserBase && ListBrowserBase && StringBase &&
           LabelBase;
}

static void close_classes(void)
{
#define CLOSE_BASE(base) do {                                                 \
    if (base) { CloseLibrary((struct Library *)base); base = NULL; }           \
} while (0)
    CLOSE_BASE(LabelBase);
    CLOSE_BASE(StringBase);
    CLOSE_BASE(ListBrowserBase);
    CLOSE_BASE(ChooserBase);
    CLOSE_BASE(ButtonBase);
    CLOSE_BASE(LayoutBase);
    CLOSE_BASE(WindowBase);
    CLOSE_BASE(UtilityBase);
    CLOSE_BASE(IntuitionBase);
#undef CLOSE_BASE
}

static void free_nodes(struct List *list)
{
    struct Node *node;
    while ((node = RemHead(list)) != NULL)
        FreeListBrowserNode(node);
}

static int add_search_type(struct List *list, const char *label)
{
    struct Node *node = AllocChooserNode(CNA_Text, (ULONG)label, TAG_END);
    if (!node)
        return 0;
    AddTail(list, node);
    return 1;
}

static void free_search_types(struct List *list)
{
    struct Node *node;
    while ((node = RemHead(list)) != NULL)
        FreeChooserNode(node);
}

static size_t build_nodes(struct List *list,
                          mr_youtube_search_results *results)
{
    size_t i;
    for (i = 0; i < results->count; i++) {
        struct Node *node = AllocListBrowserNode(
            1, LBNA_UserData, (ULONG)&results->items[i], LBNA_Column, 0,
            LBNCA_Text, (ULONG)results->items[i].row, TAG_END);
        if (!node)
            break;
        AddTail(list, node);
    }
    return i;
}

static void set_status(Object *status, struct Window *window, const char *text)
{
    SetGadgetAttrs((struct Gadget *)status, window, NULL,
                   STRINGA_TextVal, (ULONG)(text ? text : ""), TAG_DONE);
}

/* mrplay runs as a separate process. Wake the ReAction loop periodically and
 * mirror its shared codec/status block so silent audio is explained in ytgui,
 * rather than only in the player's Shell output. */
#define STATUS_POLL_MICROS 200000UL

static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static int timer_device_open;
static int timer_running;

static int poll_timer_open(void)
{
    timer_port = CreateMsgPort();
    if (!timer_port)
        return 0;
    timer_io = (struct timerequest *)CreateIORequest(timer_port,
                                                     sizeof(*timer_io));
    if (!timer_io)
        return 0;
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)timer_io, 0) != 0)
        return 0;
    timer_device_open = 1;
    return 1;
}

static void poll_timer_start(void)
{
    if (!timer_io)
        return;
    timer_io->tr_node.io_Command = TR_ADDREQUEST;
    timer_io->tr_time.tv_secs = 0;
    timer_io->tr_time.tv_micro = STATUS_POLL_MICROS;
    SendIO((struct IORequest *)timer_io);
    timer_running = 1;
}

static void poll_timer_close(void)
{
    if (timer_io) {
        if (timer_running && !CheckIO((struct IORequest *)timer_io))
            AbortIO((struct IORequest *)timer_io);
        if (timer_running)
            WaitIO((struct IORequest *)timer_io);
        timer_running = 0;
        if (timer_device_open) {
            CloseDevice((struct IORequest *)timer_io);
            timer_device_open = 0;
        }
        DeleteIORequest((struct IORequest *)timer_io);
        timer_io = NULL;
    }
    if (timer_port) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
    }
}

static void poll_player_status(Object *status, struct Window *window,
                               ULONG *seq, char *out, size_t out_size)
{
    mr_player_status ps;
    if (!mr_player_status_read(&ps)) {
        *seq = 0;
        return;
    }
    if (ps.seq == *seq)
        return;
    *seq = ps.seq;
    switch (ps.state) {
    case MR_PLAYER_STATE_STARTING:
    case MR_PLAYER_STATE_OPENING:
        snprintf(out, out_size, "%s", ps.text[0] ? ps.text : "Connecting...");
        break;
    case MR_PLAYER_STATE_PLAYING:
        if (ps.codec[0] && ps.text[0])
            snprintf(out, out_size, "Playing (%.47s): %.190s", ps.codec,
                     ps.text);
        else if (ps.codec[0])
            snprintf(out, out_size, "Playing: %s", ps.codec);
        else
            snprintf(out, out_size, "Playing");
        break;
    case MR_PLAYER_STATE_UNSUPPORTED:
        snprintf(out, out_size, "Not supported: %s",
                 ps.text[0] ? ps.text : "codec not implemented");
        break;
    case MR_PLAYER_STATE_ERROR:
        snprintf(out, out_size, "Stream error: %s",
                 ps.text[0] ? ps.text : "playback failed");
        break;
    case MR_PLAYER_STATE_ENDED:
        snprintf(out, out_size, "Video ended");
        break;
    default:
        return;
    }
    set_status(status, window, out);
}

static int player_is_running(void)
{
    int running;
    Forbid();
    running = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT) != NULL;
    Permit();
    return running;
}

static int stop_player(void)
{
    struct MsgPort *port;
    int had_player, waited;
    Forbid();
    port = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
    if (port)
        Signal(port->mp_SigTask, SIGBREAKF_CTRL_F);
    had_player = port != NULL;
    Permit();
    if (!had_player)
        return 0;
    for (waited = 0; waited < 250 && player_is_running(); waited++) {
        Delay(1);
        if (waited == 100) {
            Forbid();
            port = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
            if (port)
                Signal(port->mp_SigTask, SIGBREAKF_CTRL_C);
            Permit();
        }
    }
    return player_is_running() ? -1 : 1;
}

static void args_add_time(char *arguments, size_t size)
{
    size_t length = strlen(arguments);
    if (length && arguments[length - 1] == '\n')
        length--;
    if (length + 8 < size)
        memcpy(arguments + length, " --time\n", 9);
}

static int start_video(const mr_youtube_search_result *video,
                       const mr_play_options *options, int debug_log)
{
    char watch_url[96], arguments[640];
    BPTR seglist, log = 0, nil = 0;
    struct Process *process;

    if (!video ||
        !mr_youtube_search_watch_url(watch_url, sizeof(watch_url), video) ||
        !mr_build_player_arguments(arguments, sizeof(arguments), options,
                                   watch_url, NULL, NULL))
        return 0;
    if (debug_log)
        args_add_time(arguments, sizeof(arguments));
    stop_player();
    if (player_is_running())
        return 0;
    seglist = LoadSeg((CONST_STRPTR)"PROGDIR:mrplay");
    if (!seglist)
        seglist = LoadSeg((CONST_STRPTR)"mrplay");
    if (!seglist)
        return 0;
    if (debug_log) {
        if (!mrplay_log_session_open) {
            log = Open((CONST_STRPTR)MRPLAY_LOG_FILE, MODE_NEWFILE);
            if (log)
                mrplay_log_session_open = 1;
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
            NP_Arguments, (ULONG)arguments,
            NP_StackSize, MRPLAY_STACK_SIZE, NP_Cli, TRUE,
            NP_CommandName, (ULONG)"mrplay",
            NP_Name, (ULONG)"MintVID player",
            NP_Input, (ULONG)nil, NP_CloseInput, TRUE,
            NP_Output, (ULONG)log, NP_CloseOutput, TRUE,
            TAG_END);
    else
        process = CreateNewProcTags(
            NP_Seglist, seglist, NP_FreeSeglist, TRUE,
            NP_Arguments, (ULONG)arguments,
            NP_StackSize, MRPLAY_STACK_SIZE, NP_Cli, TRUE,
            NP_CommandName, (ULONG)"mrplay",
            NP_Name, (ULONG)"MintVID player", TAG_END);
    if (!process) {
        if (log) Close(log);
        if (nil) Close(nil);
        UnLoadSeg(seglist);
        return 0;
    }
    return 1;
}

static void load_results_url(const char *url, mr_youtube_search_mode mode,
                             const char *busy_text, const char *summary_text,
                             Object *list, Object *play_button, Object *status,
                             struct Window *window, struct List *nodes,
                             mr_youtube_search_results *results)
{
    static const char user_agent[] =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36";
    mr_http_options http_options;
    char status_text[256];
    char *document = NULL;
    size_t document_size = 0, shown;

#if !defined(HAVE_AMISSL)
    set_status(status, window,
               "YouTube search requires HTTPS; rebuild with SSL=1.");
    return;
#else
    set_status(status, window, busy_text);
    SetGadgetAttrs((struct Gadget *)play_button, window, NULL,
                   GA_Disabled, TRUE, TAG_DONE);
    Delay(1);
    if (!mr_http_options_init(&http_options, user_agent,
                              "https://www.youtube.com/") ||
        !mr_http_fetch_text(url, &http_options, &document,
                            &document_size, YT_SEARCH_PAGE_MAX)) {
        snprintf(status_text, sizeof(status_text), "YouTube search failed: %s",
                 mr_source_last_error());
        set_status(status, window, status_text);
        return;
    }
    SetGadgetAttrs((struct Gadget *)list, window, NULL,
                   LISTBROWSER_Labels, ~0UL, TAG_DONE);
    free_nodes(nodes);
    mr_youtube_search_results_free(results);
    if (!mr_youtube_search_parse_mode(results, document, document_size,
                                      mode)) {
        set_status(status, window, mr_youtube_search_last_error());
        mr_free(document);
        SetGadgetAttrs((struct Gadget *)list, window, NULL,
                       LISTBROWSER_Labels, (ULONG)nodes, TAG_DONE);
        return;
    }
    mr_free(document);
    shown = build_nodes(nodes, results);
    SetGadgetAttrs((struct Gadget *)list, window, NULL,
                   LISTBROWSER_Labels, (ULONG)nodes, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)play_button, window, NULL,
                   GA_Disabled, shown ? FALSE : TRUE, TAG_DONE);
    if (!shown) {
        set_status(status, window, mr_youtube_search_last_error());
    } else {
        snprintf(status_text, sizeof(status_text),
                 "%lu %s", (unsigned long)shown, summary_text);
        set_status(status, window, status_text);
    }
#endif
}

/* A pasted YouTube video URL in the search field plays directly instead of
 * being searched for - no network fetch, just recognizing the URL and
 * queuing it as a single result so the existing Play/double-click flow
 * (which reads the selected mr_youtube_search_result from the list) needs
 * no changes at all. */
static int load_pasted_url(const char *query_text, Object *list,
                           Object *play_button, Object *status,
                           struct Window *window, struct List *nodes,
                           mr_youtube_search_results *results)
{
    mr_youtube_search_result pasted;
    mr_youtube_search_result *items;

    if (!query_text || !mr_youtube_url_parse(&pasted, query_text))
        return 0;
    items = (mr_youtube_search_result *)malloc(sizeof(*items));
    if (!items) {
        set_status(status, window, "Not enough memory for this URL.");
        return 1;
    }
    *items = pasted;
    SetGadgetAttrs((struct Gadget *)list, window, NULL,
                   LISTBROWSER_Labels, ~0UL, TAG_DONE);
    free_nodes(nodes);
    mr_youtube_search_results_free(results);
    results->items = items;
    results->count = 1;
    build_nodes(nodes, results);
    SetGadgetAttrs((struct Gadget *)list, window, NULL,
                   LISTBROWSER_Labels, (ULONG)nodes,
                   LISTBROWSER_SelectedNode, (ULONG)nodes->lh_Head, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)play_button, window, NULL,
                   GA_Disabled, FALSE, TAG_DONE);
    set_status(status, window, "Video URL recognized - press Play.");
    return 1;
}

static void run_search(Object *query, Object *type_chooser, Object *list,
                       Object *play_button, Object *status,
                       struct Window *window, struct List *nodes,
                       mr_youtube_search_results *results)
{
    static const char *const result_summaries[] = {
        "all video results shown", "long-form videos shown",
        "live results shown", "Shorts shown"
    };
    STRPTR query_text = NULL;
    ULONG selected = MR_YOUTUBE_SEARCH_LIVE;
    mr_youtube_search_mode mode;
    char search_url[1024];
    GetAttr(STRINGA_TextVal, query, (ULONG *)&query_text);
    if (load_pasted_url((char *)query_text, list, play_button, status,
                        window, nodes, results))
        return;
    GetAttr(CHOOSER_Selected, type_chooser, &selected);
    if (selected > MR_YOUTUBE_SEARCH_SHORTS)
        selected = MR_YOUTUBE_SEARCH_ALL;
    mode = (mr_youtube_search_mode)selected;
    if (!mr_youtube_search_build_url_mode(
            search_url, sizeof(search_url),
            query_text ? (char *)query_text : "", mode)) {
        set_status(status, window, mr_youtube_search_last_error());
        return;
    }
    load_results_url(search_url, mode, "Searching YouTube...",
                     result_summaries[selected],
                     list, play_button, status, window, nodes, results);
}

static void run_channel(const mr_youtube_search_result *selected, Object *list,
                        Object *play_button, Object *status,
                        struct Window *window, struct List *nodes,
                        mr_youtube_search_results *results)
{
    char url[256], summary[192];
    if (!selected) {
        set_status(status, window, "Select a video from the channel first.");
        return;
    }
    if (!mr_youtube_channel_videos_url(url, sizeof(url), selected)) {
        set_status(status, window,
                   "That result did not include a usable channel link.");
        return;
    }
    snprintf(summary, sizeof(summary), "videos shown from %s",
             selected->channel[0] ? selected->channel : "selected channel");
    load_results_url(url, MR_YOUTUBE_SEARCH_ALL,
                     "Loading channel videos...", summary,
                     list, play_button, status, window, nodes, results);
}

/* ~600ms at dos.library's 50 ticks/sec (3000 ticks/minute) - a reasonable
 * double-click window when RA_HandleInput()'s abstraction doesn't expose
 * the raw IDCMP message timestamps DoubleClick() (intuition.library) would
 * need. */
#define DOUBLE_CLICK_TICKS 30UL

static ULONG ds_ticks(const struct DateStamp *ds)
{
    return ((ULONG)ds->ds_Days * 24UL * 60UL + (ULONG)ds->ds_Minute) * 3000UL +
           (ULONG)ds->ds_Tick;
}

/* True if the row currently selected in the results list is the same one
 * clicked last time, within the double-click window - so double-clicking a
 * result starts it, the same as pressing Play. */
static int video_was_double_clicked(Object *results_list)
{
    static struct DateStamp last_ds;
    static const mr_youtube_search_result *last_video;
    static int have_last;
    struct Node *node = NULL;
    mr_youtube_search_result *video = NULL;
    struct DateStamp now;
    int dbl;

    GetAttr(LISTBROWSER_SelectedNode, results_list, (ULONG *)&node);
    if (node)
        GetListBrowserNodeAttrs(node, LBNA_UserData, (ULONG)&video, TAG_DONE);
    if (!video)
        return 0;
    DateStamp(&now);
    dbl = have_last && video == last_video &&
          ds_ticks(&now) - ds_ticks(&last_ds) <= DOUBLE_CLICK_TICKS;
    last_ds = now;
    last_video = video;
    have_last = 1;
    if (dbl)
        have_last = 0; /* don't chain a third click into another double-click */
    return dbl;
}

int main(int argc, char **argv)
{
    Object *winobj = NULL, *layout = NULL, *search_row = NULL;
    Object *query = NULL, *type_chooser = NULL, *results_list = NULL;
    Object *status = NULL, *playback_summary = NULL;
    Object *transport_buttons = NULL, *option_buttons = NULL;
    Object *search_button = NULL, *play_button = NULL;
    Object *quality_button = NULL, *pause_button = NULL, *fast_button = NULL;
    Object *fullscreen_button = NULL, *volume_down_button = NULL;
    Object *volume_up_button = NULL, *channel_button = NULL;
    Object *stop_button = NULL, *log_button = NULL, *close_button = NULL;
    Object *query_label = NULL;
    struct Window *window = NULL;
    mr_gui_menu app_menu;
    struct List result_nodes;
    struct List search_types;
    mr_youtube_search_results results;
    mr_play_options play_options;
    ULONG sigmask, signals, result;
    ULONG player_status_seq = 0, timermask = 0;
    UWORD code;
    char option_error[160], playback_text[160], player_status_text[256];
    int rc = RETURN_FAIL, debug_log = 0;
    unsigned quality_index;

    memset(&app_menu, 0, sizeof(app_menu));
    result_nodes.lh_Head = (struct Node *)&result_nodes.lh_Tail;
    result_nodes.lh_Tail = NULL;
    result_nodes.lh_TailPred = (struct Node *)&result_nodes.lh_Head;
    search_types.lh_Head = (struct Node *)&search_types.lh_Tail;
    search_types.lh_Tail = NULL;
    search_types.lh_TailPred = (struct Node *)&search_types.lh_Head;
    mr_youtube_search_results_init(&results);
    mr_play_options_default(&play_options);
    if (!mr_play_options_parse(&play_options, argc, argv, option_error,
                               sizeof(option_error))) {
        fprintf(stderr, "ytgui: %s\n", option_error);
        return RETURN_FAIL;
    }
    quality_index = quality_from_options(&play_options);
    mr_play_options_summary(&play_options, playback_text, sizeof(playback_text));
    if (!open_classes()) {
        fprintf(stderr, "ytgui: ReAction V%ld classes are not available.\n",
                (long)YT_CLASS_VERSION);
        goto cleanup;
    }

    query = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, G_QUERY,
                                GA_RelVerify, TRUE, STRINGA_MaxChars, 200,
                                TAG_DONE);
    if (!add_search_type(&search_types, "All") ||
        !add_search_type(&search_types, "Videos") ||
        !add_search_type(&search_types, "Live") ||
        !add_search_type(&search_types, "Shorts"))
        goto cleanup;
    type_chooser = (Object *)NewObject(
        CHOOSER_GetClass(), NULL, GA_ID, G_SEARCH_TYPE, GA_RelVerify, TRUE,
        CHOOSER_Labels, (ULONG)&search_types,
        CHOOSER_Selected, MR_YOUTUBE_SEARCH_LIVE, TAG_DONE);
    search_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_SEARCH, GA_Text, (ULONG)"Search",
        GA_RelVerify, TRUE, TAG_DONE);
    results_list = (Object *)NewObject(
        LISTBROWSER_GetClass(), NULL, GA_ID, G_RESULTS, GA_RelVerify, TRUE,
        LISTBROWSER_Labels, (ULONG)&result_nodes, LISTBROWSER_AutoFit, TRUE,
        LISTBROWSER_ShowSelected, TRUE, LISTBROWSER_MinVisible, 15,
        TAG_DONE);
    play_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_PLAY, GA_Text, (ULONG)"Play",
        GA_RelVerify, TRUE, GA_Disabled, TRUE, TAG_DONE);
    quality_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_QUALITY, GA_Text,
        (ULONG)quality_labels[quality_index], GA_RelVerify, TRUE, TAG_DONE);
    pause_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_PAUSE, GA_Text, (ULONG)"Pause",
        GA_RelVerify, TRUE, TAG_DONE);
    fast_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_FAST, GA_Text, (ULONG)"Fast",
        GA_RelVerify, TRUE, TAG_DONE);
    fullscreen_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_FULLSCREEN,
        GA_Text, (ULONG)"Fullscreen", GA_RelVerify, TRUE, TAG_DONE);
    volume_down_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_VOLUME_DOWN,
        GA_Text, (ULONG)"Vol -", GA_RelVerify, TRUE, TAG_DONE);
    volume_up_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_VOLUME_UP,
        GA_Text, (ULONG)"Vol +", GA_RelVerify, TRUE, TAG_DONE);
    channel_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_CHANNEL,
        GA_Text, (ULONG)"Channel videos", GA_RelVerify, TRUE, TAG_DONE);
    stop_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_STOP, GA_Text, (ULONG)"Stop",
        GA_RelVerify, TRUE, TAG_DONE);
    log_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_LOG, GA_Text, (ULONG)"Log: Off",
        GA_RelVerify, TRUE, TAG_DONE);
    close_button = (Object *)NewObject(
        BUTTON_GetClass(), NULL, GA_ID, G_CLOSE, GA_Text, (ULONG)"Close",
        GA_RelVerify, TRUE, TAG_DONE);
    status = (Object *)NewObject(
        STRING_GetClass(), NULL, GA_ReadOnly, TRUE, STRINGA_TextVal,
        (ULONG)"Search public live streams and recorded videos.",
        STRINGA_MaxChars, 256, TAG_DONE);
    playback_summary = (Object *)NewObject(
        STRING_GetClass(), NULL, GA_ReadOnly, TRUE, STRINGA_TextVal,
        (ULONG)playback_text, STRINGA_MaxChars, sizeof(playback_text), TAG_DONE);
    query_label = (Object *)NewObject(LABEL_GetClass(), NULL, LABEL_Text,
                                      (ULONG)"YouTube", TAG_DONE);
    if (!query || !type_chooser || !search_button || !results_list ||
        !play_button || !quality_button || !pause_button || !fast_button ||
        !fullscreen_button || !volume_down_button || !volume_up_button ||
        !channel_button || !stop_button || !log_button || !close_button ||
        !status || !playback_summary || !query_label)
        goto cleanup;

    search_row = (Object *)NewObject(
        LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_AddChild, (ULONG)query, CHILD_Label, (ULONG)query_label,
        LAYOUT_AddChild, (ULONG)type_chooser,
        LAYOUT_AddChild, (ULONG)search_button, TAG_DONE);
    transport_buttons = (Object *)NewObject(
        LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_EvenSize, TRUE, LAYOUT_AddChild, (ULONG)play_button,
        LAYOUT_AddChild, (ULONG)pause_button,
        LAYOUT_AddChild, (ULONG)fast_button,
        LAYOUT_AddChild, (ULONG)volume_down_button,
        LAYOUT_AddChild, (ULONG)volume_up_button,
        LAYOUT_AddChild, (ULONG)fullscreen_button,
        LAYOUT_AddChild, (ULONG)stop_button,
        TAG_DONE);
    option_buttons = (Object *)NewObject(
        LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_EvenSize, TRUE,
        LAYOUT_AddChild, (ULONG)quality_button,
        LAYOUT_AddChild, (ULONG)channel_button,
        LAYOUT_AddChild, (ULONG)log_button,
        LAYOUT_AddChild, (ULONG)close_button, TAG_DONE);
    if (!search_row || !transport_buttons || !option_buttons)
        goto cleanup;
    layout = (Object *)NewObject(
        LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_SpaceOuter, TRUE, LAYOUT_SpaceInner, TRUE,
        LAYOUT_AddChild, (ULONG)search_row, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)results_list,
        LAYOUT_AddChild, (ULONG)transport_buttons, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)option_buttons, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)playback_summary, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)status, CHILD_WeightedHeight, 0, TAG_DONE);
    if (!layout)
        goto cleanup;
    winobj = (Object *)NewObject(
        WINDOW_GetClass(), NULL, WA_Title, (ULONG)"MintVID YouTube",
        WA_Activate, TRUE, WA_DepthGadget, TRUE, WA_DragBar, TRUE,
        WA_CloseGadget, TRUE, WA_SizeGadget, TRUE, WA_IDCMP,
        IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_MENUPICK |
            IDCMP_IDCMPUPDATE,
        WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_ParentGroup, (ULONG)layout, TAG_DONE);
    if (!winobj)
        goto cleanup;
    window = (struct Window *)RA_OpenWindow(winobj);
    if (!window)
        goto cleanup;
    mr_gui_menu_open(&app_menu, window);
    GetAttr(WINDOW_SigMask, winobj, &sigmask);
    if (poll_timer_open()) {
        timermask = 1UL << timer_port->mp_SigBit;
        poll_timer_start();
    }

    for (;;) {
        signals = Wait(sigmask | timermask | SIGBREAKF_CTRL_C);
        if (signals & SIGBREAKF_CTRL_C)
            break;
        if (timermask && (signals & timermask)) {
            while (GetMsg(timer_port))
                ;
            timer_running = 0;
            poll_player_status(status, window, &player_status_seq,
                               player_status_text,
                               sizeof(player_status_text));
            poll_timer_start();
        }
        while ((result = RA_HandleInput(winobj, &code)) != WMHI_LASTMSG) {
            ULONG gadget;
            if ((result & WMHI_CLASSMASK) == WMHI_CLOSEWINDOW)
                goto done;
            if ((result & WMHI_CLASSMASK) == WMHI_MENUPICK) {
                int action = mr_gui_menu_action(&app_menu, code);
                if (action == MR_GUI_MENU_ABOUT)
                    mr_gui_show_about(window, "YouTube ReAction edition");
                else if (action == MR_GUI_MENU_QUIT)
                    goto done;
                continue;
            }
            if ((result & WMHI_CLASSMASK) != WMHI_GADGETUP)
                continue;
            gadget = result & WMHI_GADGETMASK;
            if (gadget == G_CLOSE)
                goto done;
            if (gadget == G_SEARCH || gadget == G_QUERY) {
                run_search(query, type_chooser, results_list, play_button,
                           status, window, &result_nodes, &results);
            } else if (gadget == G_STOP) {
                int stopped = stop_player();
                set_status(status, window, stopped > 0 ? "Playback stopped."
                           : stopped < 0 ? "Player did not stop cleanly."
                                         : "No video is playing.");
            } else if (gadget == G_LOG) {
                debug_log = !debug_log;
                SetGadgetAttrs((struct Gadget *)log_button, window, NULL,
                               GA_Text,
                               (ULONG)(debug_log ? "Log: On" : "Log: Off"),
                               TAG_DONE);
                set_status(status, window,
                           debug_log
                           ? "Timing log ON: next Play writes RAM:MintVID.log"
                           : "Timing log off.");
            } else if (gadget == G_PAUSE) {
                set_status(status, window,
                    mr_player_control_send(MR_PLAYER_COMMAND_PAUSE)
                    ? "Pause toggled." : "No video is playing.");
            } else if (gadget == G_FAST) {
                set_status(status, window,
                    mr_player_control_send(MR_PLAYER_COMMAND_FAST)
                    ? "Fast playback toggled." : "No video is playing.");
            } else if (gadget == G_FULLSCREEN) {
                set_status(status, window,
                    mr_player_control_send(MR_PLAYER_COMMAND_FULLSCREEN)
                    ? "RTG fullscreen toggled." : "No video is playing.");
            } else if (gadget == G_VOLUME_DOWN || gadget == G_VOLUME_UP) {
                ULONG command = gadget == G_VOLUME_DOWN
                              ? MR_PLAYER_COMMAND_VOLUME_DOWN
                              : MR_PLAYER_COMMAND_VOLUME_UP;
                set_status(status, window, mr_player_control_send(command)
                           ? "Volume adjusted." : "No video is playing.");
            } else if (gadget == G_CHANNEL) {
                struct Node *node = NULL;
                mr_youtube_search_result *video = NULL;
                GetAttr(LISTBROWSER_SelectedNode, results_list, (ULONG *)&node);
                if (node)
                    GetListBrowserNodeAttrs(node, LBNA_UserData,
                                            (ULONG)&video, TAG_DONE);
                run_channel(video, results_list, play_button, status, window,
                            &result_nodes, &results);
            } else if (gadget == G_QUALITY) {
                quality_index = (quality_index + 1) %
                    (sizeof(quality_labels) / sizeof(quality_labels[0]));
                set_quality(&play_options, quality_index);
                SetGadgetAttrs((struct Gadget *)quality_button, window, NULL,
                               GA_Text, (ULONG)quality_labels[quality_index],
                               TAG_DONE);
                mr_play_options_summary(&play_options, playback_text,
                                        sizeof(playback_text));
                SetGadgetAttrs((struct Gadget *)playback_summary, window, NULL,
                               STRINGA_TextVal, (ULONG)playback_text, TAG_DONE);
                set_status(status, window,
                           "Quality changed; applies to the next Play.");
            } else if (gadget == G_PLAY ||
                       (gadget == G_RESULTS &&
                        video_was_double_clicked(results_list))) {
                struct Node *node = NULL;
                mr_youtube_search_result *video = NULL;
                GetAttr(LISTBROWSER_SelectedNode, results_list, (ULONG *)&node);
                if (node)
                    GetListBrowserNodeAttrs(node, LBNA_UserData,
                                            (ULONG)&video, TAG_DONE);
                player_status_seq = 0;
                if (!video)
                    set_status(status, window, "Select a video first.");
                else {
                    if (mr_master_options_apply(&play_options)) {
                        mr_play_options_summary(&play_options, playback_text,
                                                sizeof(playback_text));
                        SetGadgetAttrs((struct Gadget *)playback_summary,
                                       window, NULL, STRINGA_TextVal,
                                       (ULONG)playback_text, TAG_DONE);
                    }
                    if (!start_video(video, &play_options, debug_log))
                        set_status(status, window,
                                   "Could not start mrplay (or old player is busy).");
                    else
                        set_status(status, window, video->live
                                   ? "Resolving YouTube Live..."
                                   : (quality_index >= 3
                                      ? "Resolving YouTube 720p (360p fallback)..."
                                      : "Resolving YouTube 360p video..."));
                }
            }
        }
    }

done:
    rc = RETURN_OK;
cleanup:
    poll_timer_close();
    mr_gui_menu_close(&app_menu, window);
    if (winobj) {
        if (window)
            RA_CloseWindow(winobj);
        DisposeObject(winobj);
    } else if (layout) {
        DisposeObject(layout);
    } else {
        if (search_row)
            DisposeObject(search_row);
        else {
            if (query) DisposeObject(query);
            if (type_chooser) DisposeObject(type_chooser);
            if (search_button) DisposeObject(search_button);
            if (query_label) DisposeObject(query_label);
        }
        if (transport_buttons)
            DisposeObject(transport_buttons);
        else {
            if (play_button) DisposeObject(play_button);
            if (pause_button) DisposeObject(pause_button);
            if (fast_button) DisposeObject(fast_button);
            if (volume_down_button) DisposeObject(volume_down_button);
            if (volume_up_button) DisposeObject(volume_up_button);
            if (fullscreen_button) DisposeObject(fullscreen_button);
            if (stop_button) DisposeObject(stop_button);
        }
        if (option_buttons)
            DisposeObject(option_buttons);
        else {
            if (quality_button) DisposeObject(quality_button);
            if (channel_button) DisposeObject(channel_button);
            if (log_button) DisposeObject(log_button);
            if (close_button) DisposeObject(close_button);
        }
        if (results_list) DisposeObject(results_list);
        if (playback_summary) DisposeObject(playback_summary);
        if (status) DisposeObject(status);
    }
    free_nodes(&result_nodes);
    free_search_types(&search_types);
    mr_youtube_search_results_free(&results);
    mr_http_net_shutdown();
    close_classes();
    return rc;
}
