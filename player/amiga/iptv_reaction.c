/* Standalone MintVID IPTV ReAction browser.
 *
 * Keeping this in a second process gives directory parsing its own event loop;
 * selected URLs are still played by the normal mrplay executable.
 */
#include "../core/mr_http.h"
#include "../core/mr_play_options.h"
#include "../core/mr_source.h"
#include "../iptv/mr_iptv.h"
#include "mr_player_status.h"
#include "mr_master_options.h"
#include "mr_gui_menu.h"

MINTVID_DECLARE_VERSION(iptvgui_version_tag, "iptvgui");

#include <classes/window.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/libraries.h>
#include <exec/memory.h>
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
#include <time.h>

#define IPTV_CLASS_VERSION 44
#define IPTV_FILE_MAX (32UL * 1024UL * 1024UL)
#define MRPLAY_STACK_SIZE 320000UL
#define IPTV_CACHE_SECONDS (24UL * 60UL * 60UL)
#define IPTV_CHANNELS_URL "https://iptv-org.github.io/api/channels.json"
#define IPTV_STREAMS_URL "https://iptv-org.github.io/api/streams.json"
/* When the Debug toggle is on, the launched player's diagnostics (with --time)
 * are captured here so a failing stream can be inspected without a Shell.
 * Persistent storage, not RAM: - see the matching comment in
 * youtube_reaction.c for why (a hard lockup needs a reset to clear, which
 * also wipes RAM: and the log with it; UHD0: survives that). This is
 * exactly the scenario that motivated the change: an IPTV stream that hard-
 * locks mrplay during startup is the case where the log is needed most and
 * RAM: was least likely to still have it afterward. */
#define MRPLAY_LOG_FILE "UHD0:MintVID.log"

/* Cleared at GUI launch: the first logged stream truncates the file, later ones
 * append, so a whole session's streams accumulate in one log (see start_stream). */
static int mrplay_log_session_open;
/* Set by start_stream() when stop_player() times out: lets callers show a more
 * specific error instead of the generic "mrplay could not start" message. */
static int last_stop_wedged;

static void clear_player_status_snapshot(void) {
  DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_FILE);
  DeleteFile((CONST_STRPTR)MR_PLAYER_STATUS_TMP);
}

static int read_player_status_snapshot(mr_player_status *out) {
  mr_player_status_snapshot snapshot;
  BPTR file = Open((CONST_STRPTR)MR_PLAYER_STATUS_FILE, MODE_OLDFILE);
  LONG got;
  if (!file)
    return 0;
  got = Read(file, &snapshot, (LONG)sizeof snapshot);
  Close(file);
  if (got != (LONG)sizeof snapshot ||
      !mr_player_status_snapshot_valid(&snapshot))
    return 0;
  *out = snapshot.status;
  out->codec[sizeof(out->codec) - 1] = 0;
  out->text[sizeof(out->text) - 1] = 0;
  return 1;
}

struct IntuitionBase *IntuitionBase;
struct Library *UtilityBase, *WindowBase, *LayoutBase, *ButtonBase;
struct Library *ChooserBase, *ListBrowserBase, *StringBase, *LabelBase;

enum {
  G_SEARCH = 1,
  G_COUNTRY,
  G_CATEGORY,
  G_CHANNELS,
  G_REFRESH,
  G_PLAY,
  G_NEXT_STREAM,
  G_STOP,
  G_OPEN_URL,
  G_DEBUG,
  G_CLOSE
};

typedef struct {
  const char *label;
  const char *iptv_code;
} iptv_country_choice;

static const iptv_country_choice country_choices[] = {
    {"United Kingdom", "UK"},
    {"United States", "US"},
    {"All", NULL},
};

static const char *country_code(ULONG selected) {
  if (selected >= sizeof(country_choices) / sizeof(country_choices[0]))
    selected = 0;
  return country_choices[selected].iptv_code;
}

static int open_classes(void) {
  IntuitionBase = (struct IntuitionBase *)OpenLibrary(
      (CONST_STRPTR) "intuition.library", 39);
  UtilityBase = OpenLibrary((CONST_STRPTR) "utility.library", 39);
  WindowBase = OpenLibrary((CONST_STRPTR) "window.class", IPTV_CLASS_VERSION);
  LayoutBase =
      OpenLibrary((CONST_STRPTR) "gadgets/layout.gadget", IPTV_CLASS_VERSION);
  ButtonBase =
      OpenLibrary((CONST_STRPTR) "gadgets/button.gadget", IPTV_CLASS_VERSION);
  ChooserBase =
      OpenLibrary((CONST_STRPTR) "gadgets/chooser.gadget", IPTV_CLASS_VERSION);
  ListBrowserBase = OpenLibrary((CONST_STRPTR) "gadgets/listbrowser.gadget",
                                IPTV_CLASS_VERSION);
  StringBase =
      OpenLibrary((CONST_STRPTR) "gadgets/string.gadget", IPTV_CLASS_VERSION);
  LabelBase =
      OpenLibrary((CONST_STRPTR) "images/label.image", IPTV_CLASS_VERSION);
  return IntuitionBase && UtilityBase && WindowBase && LayoutBase &&
         ButtonBase && ChooserBase && ListBrowserBase && StringBase &&
         LabelBase;
}

static void close_classes(void) {
#define CLOSE_BASE(base)                                                       \
  do {                                                                         \
    if (base) {                                                                \
      CloseLibrary((struct Library *)base);                                    \
      base = NULL;                                                             \
    }                                                                          \
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

static int add_chooser(struct List *list, const char *text) {
  struct Node *node = AllocChooserNode(CNA_Text, (ULONG)text, TAG_END);
  if (!node)
    return 0;
  AddTail(list, node);
  return 1;
}

static void free_chooser(struct List *list) {
  struct Node *node;
  while ((node = RemHead(list)) != NULL)
    FreeChooserNode(node);
}

static void set_status(Object *status, struct Window *window, const char *text);

static char cache_dir[128];

static void cache_path(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s%s", cache_dir, name);
}

static int try_cache_dir(const char *directory) {
  char probe[192];
  FILE *file;
  strncpy(cache_dir, directory, sizeof(cache_dir) - 1);
  cache_dir[sizeof(cache_dir) - 1] = 0;
  cache_path(probe, sizeof(probe), ".write-test");
  file = fopen(probe, "wb");
  if (!file)
    return 0;
  fclose(file);
  remove(probe);
  return 1;
}

static int choose_cache_dir(char *error, size_t error_size) {
  BPTR lock;
  lock = CreateDir((CONST_STRPTR) "PROGDIR:Cache");
  if (lock)
    UnLock(lock);
  lock = CreateDir((CONST_STRPTR) "PROGDIR:Cache/IPTV");
  if (lock)
    UnLock(lock);
  if (try_cache_dir("PROGDIR:Cache/IPTV/"))
    return 1;
  lock = CreateDir((CONST_STRPTR) "T:MintVID-IPTV");
  if (lock)
    UnLock(lock);
  if (try_cache_dir("T:MintVID-IPTV/"))
    return 1;
  snprintf(error, error_size,
           "Cache directory failed: PROGDIR: and T: are not writable");
  cache_dir[0] = 0;
  return 0;
}

static int cache_is_fresh(void) {
  char path[192];
  FILE *meta;
  unsigned long saved;
  time_t now;
  cache_path(path, sizeof(path), "cache.meta");
  meta = fopen(path, "r");
  if (!meta)
    return 0;
  if (fscanf(meta, "%lu", &saved) != 1) {
    fclose(meta);
    return 0;
  }
  fclose(meta);
  now = time(NULL);
  return now != (time_t)-1 && (unsigned long)now >= saved &&
         (unsigned long)now - saved < IPTV_CACHE_SECONDS;
}

static int download_file(const char *url, const char *path) {
  return mr_http_download_file(url, path, IPTV_FILE_MAX);
}

static size_t file_size(const char *path) {
  FILE *file = fopen(path, "rb");
  long size;
  if (!file)
    return 0;
  if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0)
    size = 0;
  fclose(file);
  return (size_t)size;
}

static void download_error(char *error, size_t error_size, const char *stage) {
  const char *detail = mr_source_last_error();
  if (strstr(detail, "download file"))
    snprintf(error, error_size, "Cache write failed: %s", detail);
  else
    snprintf(error, error_size, "%s download failed: %s", stage, detail);
}

static int refresh_cache(char *error, size_t error_size, Object *status,
                         struct Window *window, const char *country,
                         mr_iptv_directory *result) {
  char channels_tmp[192], streams_tmp[192], channels_path[192];
  char streams_path[192], channels_old[192], streams_old[192], meta_path[192];
  char channels_failed[192], streams_failed[192];
  mr_iptv_directory check;
  size_t channels_size = 0, streams_size = 0;
  FILE *meta;
  time_t now;
  int check_initialized = 0, valid = 0;

  if (error_size)
    error[0] = 0;
#if !defined(HAVE_AMISSL)
  snprintf(error, error_size,
           "IPTV requires HTTPS support; rebuild with SSL=1");
  return 0;
#endif
  if (!cache_dir[0] && !choose_cache_dir(error, error_size))
    return 0;
  cache_path(channels_tmp, sizeof(channels_tmp), "channels.json.tmp");
  cache_path(streams_tmp, sizeof(streams_tmp), "streams.json.tmp");
  cache_path(channels_path, sizeof(channels_path), "channels.json");
  cache_path(streams_path, sizeof(streams_path), "streams.json");
  cache_path(channels_old, sizeof(channels_old), "channels.json.old");
  cache_path(streams_old, sizeof(streams_old), "streams.json.old");
  cache_path(meta_path, sizeof(meta_path), "cache.meta");
  cache_path(channels_failed, sizeof(channels_failed), "channels.failed.json");
  cache_path(streams_failed, sizeof(streams_failed), "streams.failed.json");
  remove(channels_tmp);
  remove(streams_tmp);

  set_status(status, window, "Downloading channels.json...");
  printf("IPTV: requesting %s\n", IPTV_CHANNELS_URL);
  if (!download_file(IPTV_CHANNELS_URL, channels_tmp)) {
    download_error(error, error_size, "Channels");
    goto done;
  }
  channels_size = file_size(channels_tmp);
  if (!channels_size) {
    snprintf(error, error_size,
             "Reading temporary files failed: channels.json");
    remove(channels_failed);
    rename(channels_tmp, channels_failed);
    goto done;
  }
  printf("IPTV: downloaded channels.json, %lu bytes\n",
         (unsigned long)channels_size);

  set_status(status, window, "Downloading streams.json...");
  printf("IPTV: requesting %s\n", IPTV_STREAMS_URL);
  if (!download_file(IPTV_STREAMS_URL, streams_tmp)) {
    download_error(error, error_size, "Streams");
    goto done;
  }
  streams_size = file_size(streams_tmp);
  if (!streams_size) {
    snprintf(error, error_size, "Reading temporary files failed: streams.json");
    remove(streams_failed);
    rename(streams_tmp, streams_failed);
    goto done;
  }
  printf("IPTV: downloaded streams.json, %lu bytes\n",
         (unsigned long)streams_size);

  set_status(status, window, "Parsing channel directory...");
  mr_iptv_init(&check);
  check_initialized = 1;
  if (!mr_iptv_load_country_files(&check, channels_tmp, streams_tmp,
                                  country)) {
    snprintf(error, error_size, "%s", mr_iptv_last_error());
    remove(channels_failed);
    rename(channels_tmp, channels_failed);
    remove(streams_failed);
    rename(streams_tmp, streams_failed);
    goto done;
  }
  if (!check.channel_count) {
    snprintf(error, error_size,
             "Parsing channels.json failed: empty directory");
    remove(channels_failed);
    rename(channels_tmp, channels_failed);
    goto done;
  }

  set_status(status, window, "Saving IPTV cache...");
  remove(channels_old);
  remove(streams_old);
  rename(channels_path, channels_old);
  rename(streams_path, streams_old);
  if (rename(channels_tmp, channels_path) != 0 ||
      rename(streams_tmp, streams_path) != 0) {
    remove(channels_path);
    remove(streams_path);
    rename(channels_old, channels_path);
    rename(streams_old, streams_path);
    snprintf(error, error_size, "Renaming cache files failed (IoErr %ld)",
             (long)IoErr());
    goto done;
  }
  remove(channels_old);
  remove(streams_old);
  now = time(NULL);
  meta = fopen(meta_path, "w");
  if (!meta) {
    snprintf(error, error_size,
             "Writing cache.meta failed: cannot create file");
    goto done;
  }
  if (fprintf(meta, "%lu\n", (unsigned long)now) < 0 || fclose(meta) != 0) {
    snprintf(error, error_size, "Writing cache.meta failed: write error");
    goto done;
  }
  mr_iptv_free(result);
  *result = check;
  check_initialized = 0;
  valid = 1;
done:
  if (check_initialized)
    mr_iptv_free(&check);
  remove(channels_tmp);
  remove(streams_tmp);
  return valid;
}

static int load_cache(mr_iptv_directory *directory, const char *country) {
  char channels_path[192], streams_path[192];
  if (!cache_dir[0])
    return 0;
  cache_path(channels_path, sizeof(channels_path), "channels.json");
  cache_path(streams_path, sizeof(streams_path), "streams.json");
  return mr_iptv_load_country_files(directory, channels_path, streams_path,
                                    country);
}

static void free_nodes(struct List *list) {
  struct Node *node;
  while ((node = RemHead(list)) != NULL)
    FreeListBrowserNode(node);
}

static size_t rebuild_nodes(struct List *list, mr_iptv_directory *directory,
                            const char *search, ULONG country, ULONG category) {
  mr_iptv_filter filter;
  size_t i, shown = 0;
  filter.country = country_code(country);
  if (!filter.country)
    filter.country = "All";
  filter.category = category == 1   ? "news"
                    : category == 2 ? "entertainment"
                                    : "All";
  filter.search = search;
  free_nodes(list);
  for (i = 0; i < directory->channel_count; i++) {
    struct Node *node;
    if (!mr_iptv_channel_visible(&directory->channels[i], &filter))
      continue;
    node = AllocListBrowserNode(
        1, LBNA_UserData, (ULONG)&directory->channels[i], LBNA_Column, 0,
        LBNCA_Text, (ULONG)directory->channels[i].name, TAG_END);
    if (!node)
      break;
    AddTail(list, node);
    shown++;
  }
  return shown;
}

static void set_status(Object *status, struct Window *window,
                       const char *text) {
  SetGadgetAttrs((struct Gadget *)status, window, NULL, STRINGA_TextVal,
                 (ULONG)text, TAG_DONE);
}

static void report_list_fast_ram(void) {
  printf("IPTV: Fast RAM after list nodes: %lu KB; largest %lu KB\n",
         (unsigned long)(AvailMem(MEMF_FAST) / 1024),
         (unsigned long)(AvailMem(MEMF_FAST | MEMF_LARGEST) / 1024));
}

/* The player publishes MR_IPTV_PLAYER_PORT while it runs. Its presence is the
 * canonical "a stream is playing" indicator. */
static int player_is_running(void) {
  int running;
  Forbid();
  running = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT) != NULL;
  Permit();
  return running;
}

/* MR_IPTV_GUI_PORT lets MintVID Control (the launcher) know once this
 * window is actually open and interactive, not merely that the process
 * exists - LoadSeg()/CreateNewProcTags() returns long before that, and the
 * channel-cache refresh below the window open can itself take a real
 * moment. A plain static struct (not AllocMem'd) is fine: AmigaOS tasks
 * share one flat address space, and this port only needs to be found, not
 * messaged (PA_IGNORE, no replies). Never AddPort() twice - RA_HandleInput's
 * loop can in principle be re-entered, but the window (and this call) only
 * ever runs once per process lifetime here. */
static struct MsgPort iptv_gui_port;
static int iptv_gui_port_added;

static void iptv_gui_port_open(void) {
  if (iptv_gui_port_added)
    return;
  iptv_gui_port.mp_MsgList.lh_Head = (struct Node *)&iptv_gui_port.mp_MsgList.lh_Tail;
  iptv_gui_port.mp_MsgList.lh_Tail = NULL;
  iptv_gui_port.mp_MsgList.lh_TailPred = (struct Node *)&iptv_gui_port.mp_MsgList.lh_Head;
  iptv_gui_port.mp_Flags = PA_IGNORE;
  iptv_gui_port.mp_SigTask = FindTask(NULL);
  iptv_gui_port.mp_Node.ln_Name = (char *)MR_IPTV_GUI_PORT;
  iptv_gui_port.mp_Node.ln_Pri = 0;
  AddPort(&iptv_gui_port);
  iptv_gui_port_added = 1;
}

static void iptv_gui_port_close(void) {
  if (!iptv_gui_port_added)
    return;
  RemPort(&iptv_gui_port);
  iptv_gui_port_added = 0;
}

/* Stop the running player, if any, and wait for it to release its control port
 * so its CGX window and Paula audio are gone before we launch the next stream.
 * Ctrl-F is the player's clean-stop signal; an overloaded player that has not
 * responded within the grace period is forced down with Ctrl-C. Finding the
 * port and signalling under Forbid() keeps the task pointer valid across the
 * call. Returns 1 if a player was asked to stop, 0 if none was running. */
static int stop_player(void) {
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
  for (waited = 0; waited < 250; waited++) { /* up to ~5 s at 1/50 s ticks */
    Delay(1);
    if (!player_is_running())
      break;
    if (waited == 100) { /* ~2 s with no clean exit: force it down */
      Forbid();
      port = FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
      if (port)
        Signal(port->mp_SigTask, SIGBREAKF_CTRL_C);
      Permit();
    }
  }
  return 1;
}

/* Splice "--time" into a built argument line so the player logs full
 * diagnostics. mrplay accepts flags after the URL, so we insert it just before
 * the trailing newline the argument string ends with. */
static void args_add_time(char *args, size_t size) {
  size_t len = strlen(args);
  if (len && args[len - 1] == '\n')
    len--;
  if (len + 8 < size) /* " --time\n" + NUL */
    memcpy(args + len, " --time\n", 9);
}

static int start_stream(const mr_iptv_stream *stream,
                        const mr_play_options *play_options, int debug_log) {
  char args[MR_IPTV_URL_MAX * 4 + MR_HTTP_USER_AGENT_MAX * 2 + 64];
  mr_http_options options;
  BPTR seglist, log = 0, nil = 0;
  int started;
  if (!stream || !mr_iptv_supported_url(stream->url) ||
      !mr_http_options_init(&options, stream->user_agent,
                            stream->http_referrer))
    return 0;
  if (!mr_build_player_arguments(args, sizeof(args), play_options, stream->url,
                                 stream->user_agent,
                                 stream->http_referrer))
    return 0;
  if (debug_log)
    args_add_time(args, sizeof(args));
  /* Never let two players run at once: pressing Play/Next while a stream is up
   * would otherwise double CPU load. Stop the current player and wait for it to
   * release the display and Paula before we spawn the replacement. If the old
   * player is still alive after the full stop-and-wait sequence (e.g. it is
   * wedged decoding a high-res frame), refuse to launch a replacement: starting
   * a second player while the first still owns its high-resolution buffers and
   * the control port causes RAM collapse and an unresponsive GUI. */
  stop_player();
  if (player_is_running()) {
    last_stop_wedged = 1;
    return 0;
  }
  last_stop_wedged = 0;
  clear_player_status_snapshot();
  seglist = LoadSeg((CONST_STRPTR) "PROGDIR:mrplay");
  if (!seglist)
    seglist = LoadSeg((CONST_STRPTR) "mrplay");
  if (!seglist)
    return 0;
  /* Capture the player's stdout to a log file. Needs a real input stream too
   * (a CLI expects one), so pair the log with NIL:. If either file won't open,
   * fall back to an unlogged launch rather than refusing to play.
   *
   * Session-based, appending: truncate once (the first logged stream of this
   * GUI run) so a session starts clean, then append every subsequent stream so
   * the whole session's diagnostics accumulate in one file instead of each
   * stream wiping the last. A divider marks where each new stream begins. */
  if (debug_log) {
    if (!mrplay_log_session_open) {
      log = Open((CONST_STRPTR)MRPLAY_LOG_FILE, MODE_NEWFILE);
      if (log) mrplay_log_session_open = 1;
    } else {
      log = Open((CONST_STRPTR)MRPLAY_LOG_FILE, MODE_READWRITE);
      if (log) {
        static const char sep[] = "\n\n===== new stream =====\n";
        Seek(log, 0, OFFSET_END);
        Write(log, (APTR)sep, (LONG)(sizeof sep - 1));
      }
    }
    nil = Open((CONST_STRPTR) "NIL:", MODE_NEWFILE);
    if (!log || !nil) {
      if (log) Close(log);
      if (nil) Close(nil);
      log = nil = 0;
    }
  }
  if (log && nil)
    started = CreateNewProcTags(
                  NP_Seglist, seglist, NP_FreeSeglist, TRUE, NP_Arguments,
                  (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE, NP_Cli, TRUE,
                  NP_CommandName, (ULONG) "mrplay", NP_Name,
                  (ULONG) "MintVID player", NP_Input, (ULONG)nil, NP_CloseInput,
                  TRUE, NP_Output, (ULONG)log, NP_CloseOutput, TRUE,
                  TAG_END) != NULL;
  else
    started = CreateNewProcTags(
                  NP_Seglist, seglist, NP_FreeSeglist, TRUE, NP_Arguments,
                  (ULONG)args, NP_StackSize, MRPLAY_STACK_SIZE, NP_Cli, TRUE,
                  NP_CommandName, (ULONG) "mrplay", NP_Name,
                  (ULONG) "MintVID player", TAG_END) != NULL;
  if (!started) {
    if (log) Close(log);   /* not consumed by a failed CreateNewProc */
    if (nil) Close(nil);
    UnLoadSeg(seglist);
    return 0;
  }
  return 1;
}

static int start_url(const char *url, const mr_play_options *play_options,
                     int debug_log) {
  mr_iptv_stream stream;
  memset(&stream, 0, sizeof(stream));
  if (!url || strlen(url) >= sizeof(stream.url))
    return 0;
  strcpy(stream.url, url);
  return start_stream(&stream, play_options, debug_log);
}

/* ---- Player-status polling --------------------------------------------- *
 * A running mrplay publishes what it is doing (which codec is on screen, or
 * why a stream refused to play) in the shared block on its control port. We
 * cannot block waiting for it, so a small repeating timer.device request wakes
 * the reactor a few times a second; on each wake we read the block and, when it
 * changed, mirror it onto the status line. */
#define STATUS_POLL_MICROS 200000UL /* ~5 Hz */

static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static int timer_device_open;
static int timer_running;

static int poll_timer_open(void) {
  timer_port = CreateMsgPort();
  if (!timer_port)
    return 0;
  timer_io =
      (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer_io));
  if (!timer_io)
    return 0;
  if (OpenDevice((CONST_STRPTR) "timer.device", UNIT_VBLANK,
                 (struct IORequest *)timer_io, 0) != 0)
    return 0;
  timer_device_open = 1;
  return 1;
}

static void poll_timer_start(void) {
  if (!timer_io)
    return;
  timer_io->tr_node.io_Command = TR_ADDREQUEST;
  timer_io->tr_time.tv_secs = 0;
  timer_io->tr_time.tv_micro = STATUS_POLL_MICROS;
  SendIO((struct IORequest *)timer_io);
  timer_running = 1;
}

static void poll_timer_close(void) {
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

/* Poll the running player and reflect any change onto the status line. `*seq`
 * remembers the last update we showed so we only touch the gadget when the
 * player reports something new; it is reset to 0 whenever no player is running
 * so the controller's own messages are never overwritten while idle. */
static void poll_player_status(Object *status, struct Window *window,
                               ULONG *seq, char *out, size_t out_size) {
  mr_player_status ps;
  int live = mr_player_status_read(&ps);
  if (!live && !read_player_status_snapshot(&ps)) {
    *seq = 0; /* no player: leave the controller's own text alone */
    return;
  }
  if (ps.seq == *seq)
    return; /* nothing new since we last refreshed */
  *seq = ps.seq;
  switch (ps.state) {
  case MR_PLAYER_STATE_OPENING:
    if (!live)
      snprintf(out, out_size, "Player exited during startup: %s",
               ps.text[0] ? ps.text : "connection did not complete");
    else
      snprintf(out, out_size, "%s", ps.text[0] ? ps.text : "Connecting...");
    break;
  case MR_PLAYER_STATE_PLAYING:
    if (!live)
      snprintf(out, out_size, "Player stopped unexpectedly%s%s",
               ps.text[0] ? ": " : "", ps.text[0] ? ps.text : "");
    else if (ps.codec[0] && ps.text[0])
      snprintf(out, out_size, "Playing (%.47s): %.190s", ps.codec, ps.text);
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
    snprintf(out, out_size, "Stream ended%s%s", ps.codec[0] ? " (" : "",
             ps.codec[0] ? ps.codec : "");
    if (ps.codec[0]) {
      size_t n = strlen(out);
      if (n + 1 < out_size) {
        out[n] = ')';
        out[n + 1] = 0;
      }
    }
    break;
  default:
    return; /* STARTING or unknown: nothing worth showing yet */
  }
  set_status(status, window, out);
}

/* ~600ms at dos.library's 50 ticks/sec (3000 ticks/minute) - a reasonable
 * double-click window when RA_HandleInput()'s abstraction doesn't expose
 * the raw IDCMP message timestamps DoubleClick() (intuition.library) would
 * need. */
#define DOUBLE_CLICK_TICKS 30UL

static ULONG ds_ticks(const struct DateStamp *ds) {
  return ((ULONG)ds->ds_Days * 24UL * 60UL + (ULONG)ds->ds_Minute) * 3000UL +
         (ULONG)ds->ds_Tick;
}

/* True if the row currently selected in the channel list is the same one
 * clicked last time, within the double-click window - so double-clicking a
 * channel starts it, the same as pressing Play. */
static int channel_was_double_clicked(Object *channels) {
  static struct DateStamp last_ds;
  static mr_iptv_channel *last_channel;
  static int have_last;
  struct Node *node = NULL;
  mr_iptv_channel *channel = NULL;
  struct DateStamp now;
  int dbl;

  GetAttr(LISTBROWSER_SelectedNode, channels, (ULONG *)&node);
  if (node)
    GetListBrowserNodeAttrs(node, LBNA_UserData, (ULONG)&channel, TAG_DONE);
  if (!channel)
    return 0;
  DateStamp(&now);
  dbl = have_last && channel == last_channel &&
        ds_ticks(&now) - ds_ticks(&last_ds) <= DOUBLE_CLICK_TICKS;
  last_ds = now;
  last_channel = channel;
  have_last = 1;
  if (dbl)
    have_last = 0; /* don't chain a third click into another double-click */
  return dbl;
}

int main(int argc, char **argv) {
  Object *winobj = NULL, *layout = NULL, *search = NULL, *country = NULL;
  Object *category = NULL, *channels = NULL, *status = NULL, *url = NULL;
  Object *playback_summary = NULL;
  Object *buttons = NULL;
  Object *refresh_button = NULL, *play_button = NULL, *next_button = NULL;
  Object *stop_button = NULL;
  Object *open_button = NULL;
  Object *debug_button = NULL;
  Object *close_button = NULL;
  Object *search_label = NULL, *country_label = NULL, *category_label = NULL;
  Object *url_label = NULL;
  struct Window *window = NULL;
  mr_gui_menu app_menu;
  struct List channel_nodes, countries, categories;
  mr_iptv_directory directory, refreshed_directory;
  ULONG sigmask, signals, result, selected, country_index, category_index;
  ULONG player_status_seq = 0, timermask = 0;
  size_t country_choice_index;
  UWORD code;
  char status_text[256], refresh_error[256], cache_error[256];
  char playback_text[160], option_error[160];
  int rc = RETURN_FAIL, loaded, refresh_attempted, cache_ready;
  int debug_log = 0;
  mr_play_options play_options;
  mr_iptv_channel *active_channel = NULL;
  unsigned active_stream = 0;

  memset(&app_menu, 0, sizeof(app_menu));
  channel_nodes.lh_Head = (struct Node *)&channel_nodes.lh_Tail;
  channel_nodes.lh_Tail = NULL;
  channel_nodes.lh_TailPred = (struct Node *)&channel_nodes.lh_Head;
  countries.lh_Head = (struct Node *)&countries.lh_Tail;
  countries.lh_Tail = NULL;
  countries.lh_TailPred = (struct Node *)&countries.lh_Head;
  categories.lh_Head = (struct Node *)&categories.lh_Tail;
  categories.lh_Tail = NULL;
  categories.lh_TailPred = (struct Node *)&categories.lh_Head;
  mr_iptv_init(&directory);
  mr_iptv_init(&refreshed_directory);
  mr_play_options_default(&play_options);
  if (!mr_play_options_parse(&play_options, argc, argv, option_error,
                             sizeof(option_error))) {
    fprintf(stderr, "iptvgui: %s\n", option_error);
    return RETURN_FAIL;
  }
  mr_play_options_summary(&play_options, playback_text, sizeof(playback_text));
  if (!open_classes())
    goto cleanup;
  for (country_choice_index = 0;
       country_choice_index <
       sizeof(country_choices) / sizeof(country_choices[0]);
       country_choice_index++)
    if (!add_chooser(&countries, country_choices[country_choice_index].label))
      goto cleanup;
  if (!add_chooser(&categories, "All") || !add_chooser(&categories, "News") ||
      !add_chooser(&categories, "Entertainment"))
    goto cleanup;

  cache_ready = choose_cache_dir(cache_error, sizeof(cache_error));
  refresh_attempted = cache_ready && !cache_is_fresh();
  loaded =
      cache_ready ? load_cache(&directory, country_choices[0].iptv_code) : 0;
  if (loaded) {
    rebuild_nodes(&channel_nodes, &directory, "", 0, 0);
    report_list_fast_ram();
  }

  search = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, G_SEARCH,
                               GA_RelVerify, TRUE, STRINGA_MaxChars,
                               MR_IPTV_NAME_MAX, TAG_DONE);
  country = (Object *)NewObject(
      CHOOSER_GetClass(), NULL, GA_ID, G_COUNTRY, GA_RelVerify, TRUE,
      CHOOSER_Labels, (ULONG)&countries, CHOOSER_Selected, 0, TAG_DONE);
  category = (Object *)NewObject(
      CHOOSER_GetClass(), NULL, GA_ID, G_CATEGORY, GA_RelVerify, TRUE,
      CHOOSER_Labels, (ULONG)&categories, CHOOSER_Selected, 0, TAG_DONE);
  channels = (Object *)NewObject(
      LISTBROWSER_GetClass(), NULL, GA_ID, G_CHANNELS, GA_RelVerify, TRUE,
      LISTBROWSER_Labels, (ULONG)&channel_nodes, LISTBROWSER_AutoFit, TRUE,
      LISTBROWSER_ShowSelected, TRUE, LISTBROWSER_MinVisible, 15, TAG_DONE);
  url = (Object *)NewObject(STRING_GetClass(), NULL, STRINGA_MaxChars,
                            MR_IPTV_URL_MAX, TAG_DONE);
  if (refresh_attempted)
    strcpy(status_text, "Downloading channel directory...");
  else if (loaded)
    snprintf(status_text, sizeof(status_text), "%lu channels loaded from cache",
             (unsigned long)directory.channel_count);
  else if (!cache_ready)
    strncpy(status_text, cache_error, sizeof(status_text) - 1);
  else
    strcpy(status_text, "No IPTV directory cache available");
  status_text[sizeof(status_text) - 1] = 0;
  status = (Object *)NewObject(STRING_GetClass(), NULL, GA_ReadOnly, TRUE,
                               STRINGA_TextVal, (ULONG)status_text,
                               STRINGA_MaxChars, sizeof(status_text), TAG_DONE);
  playback_summary = (Object *)NewObject(
      STRING_GetClass(), NULL, GA_ReadOnly, TRUE, STRINGA_TextVal,
      (ULONG)playback_text, STRINGA_MaxChars, sizeof(playback_text), TAG_DONE);
  if (!search || !country || !category || !channels || !url || !status ||
      !playback_summary)
    goto cleanup;

  refresh_button = (Object *)NewObject(
      BUTTON_GetClass(), NULL, GA_ID, G_REFRESH, GA_Text,
      (ULONG) "Refresh cache", GA_RelVerify, TRUE, TAG_DONE);
  play_button = (Object *)NewObject(
      BUTTON_GetClass(), NULL, GA_ID, G_PLAY, GA_Text, (ULONG) "Play",
      GA_RelVerify, TRUE, GA_Disabled, loaded ? FALSE : TRUE, TAG_DONE);
  next_button = (Object *)NewObject(
      BUTTON_GetClass(), NULL, GA_ID, G_NEXT_STREAM, GA_Text,
      (ULONG) "Next Stream", GA_RelVerify, TRUE,
      GA_Disabled, loaded ? FALSE : TRUE, TAG_DONE);
  stop_button =
      (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, G_STOP, GA_Text,
                          (ULONG) "Stop", GA_RelVerify, TRUE, TAG_DONE);
  open_button =
      (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, G_OPEN_URL, GA_Text,
                          (ULONG) "Open URL...", GA_RelVerify, TRUE, TAG_DONE);
  debug_button =
      (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, G_DEBUG, GA_Text,
                          (ULONG) "Log: Off", GA_RelVerify, TRUE, TAG_DONE);
  close_button =
      (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, G_CLOSE, GA_Text,
                          (ULONG) "Close", GA_RelVerify, TRUE, TAG_DONE);
  search_label = (Object *)NewObject(LABEL_GetClass(), NULL, LABEL_Text,
                                     (ULONG) "Search", TAG_DONE);
  country_label = (Object *)NewObject(LABEL_GetClass(), NULL, LABEL_Text,
                                      (ULONG) "Country", TAG_DONE);
  category_label = (Object *)NewObject(LABEL_GetClass(), NULL, LABEL_Text,
                                       (ULONG) "Category", TAG_DONE);
  url_label = (Object *)NewObject(LABEL_GetClass(), NULL, LABEL_Text,
                                  (ULONG) "Manual URL", TAG_DONE);
  if (!refresh_button || !play_button || !next_button || !stop_button ||
      !open_button || !debug_button || !close_button ||
      !search_label || !country_label || !category_label || !url_label)
    goto cleanup;

  buttons = (Object *)NewObject(
      LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
      LAYOUT_EvenSize, TRUE, LAYOUT_AddChild, (ULONG)refresh_button,
      LAYOUT_AddChild, (ULONG)play_button, LAYOUT_AddChild, (ULONG)next_button,
      LAYOUT_AddChild, (ULONG)stop_button, LAYOUT_AddChild, (ULONG)open_button,
      LAYOUT_AddChild, (ULONG)debug_button, LAYOUT_AddChild, (ULONG)close_button,
      TAG_DONE);
  if (!buttons)
    goto cleanup;
  layout = (Object *)NewObject(
      LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
      LAYOUT_SpaceOuter, TRUE, LAYOUT_SpaceInner, TRUE, LAYOUT_AddChild,
      (ULONG)search, CHILD_Label, (ULONG)search_label, LAYOUT_AddChild,
      (ULONG)country, CHILD_Label, (ULONG)country_label, LAYOUT_AddChild,
      (ULONG)category, CHILD_Label, (ULONG)category_label, LAYOUT_AddChild,
      (ULONG)channels, LAYOUT_AddChild, (ULONG)url, CHILD_Label,
      (ULONG)url_label, LAYOUT_AddChild, (ULONG)buttons, CHILD_WeightedHeight,
      0, LAYOUT_AddChild, (ULONG)playback_summary, CHILD_WeightedHeight, 0,
      LAYOUT_AddChild, (ULONG)status, CHILD_WeightedHeight, 0, TAG_DONE);
  if (!layout)
    goto cleanup;
  winobj = (Object *)NewObject(
      WINDOW_GetClass(), NULL, WA_Title, (ULONG) "MintVID IPTV", WA_Activate,
      TRUE, WA_DepthGadget, TRUE, WA_DragBar, TRUE, WA_CloseGadget, TRUE,
      WA_SizeGadget, TRUE, WA_IDCMP,
      IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_MENUPICK |
          IDCMP_IDCMPUPDATE, WINDOW_Position,
      WPOS_CENTERSCREEN, WINDOW_ParentGroup, (ULONG)layout, TAG_DONE);
  if (!winobj)
    goto cleanup;
  window = (struct Window *)RA_OpenWindow(winobj);
  if (!window)
    goto cleanup;
  mr_gui_menu_open(&app_menu, window);
  iptv_gui_port_open();
  GetAttr(WINDOW_SigMask, winobj, &sigmask);
  /* Repeating poll so mrplay's codec / "not supported" status reaches the
   * status line. Purely advisory: if the timer.device is unavailable the GUI
   * still works, it just stops mirroring the player's live status. */
  if (poll_timer_open()) {
    timermask = 1UL << timer_port->mp_SigBit;
    poll_timer_start();
  }
  /* The window is open and its loading status is visible before any network
   * operation begins. Delay one tick so Intuition can draw the new window. */
  if (refresh_attempted) {
    Delay(1);
    SetGadgetAttrs((struct Gadget *)play_button, window, NULL, GA_Disabled,
                   TRUE, TAG_DONE);
    SetGadgetAttrs((struct Gadget *)next_button, window, NULL, GA_Disabled,
                   TRUE, TAG_DONE);
    if (!refresh_cache(refresh_error, sizeof(refresh_error), status, window,
                       country_choices[0].iptv_code, &refreshed_directory)) {
      set_status(status, window, refresh_error);
    } else {
      SetGadgetAttrs((struct Gadget *)channels, window, NULL,
                     LISTBROWSER_Labels, ~0UL, TAG_DONE);
      free_nodes(&channel_nodes);
      active_channel = NULL;
      active_stream = 0;
      mr_iptv_free(&directory);
      directory = refreshed_directory;
      mr_iptv_init(&refreshed_directory);
      loaded = directory.channel_count != 0;
      selected = rebuild_nodes(&channel_nodes, &directory, "", 0, 0);
      report_list_fast_ram();
      SetGadgetAttrs((struct Gadget *)channels, window, NULL,
                     LISTBROWSER_Labels, (ULONG)&channel_nodes, TAG_DONE);
      SetGadgetAttrs((struct Gadget *)play_button, window, NULL, GA_Disabled,
                     loaded ? FALSE : TRUE, TAG_DONE);
      SetGadgetAttrs((struct Gadget *)next_button, window, NULL, GA_Disabled,
                     loaded ? FALSE : TRUE, TAG_DONE);
      snprintf(status_text, sizeof(status_text),
               "Ready: %lu playable %s channels; %lu streams matched",
               (unsigned long)selected, country_choices[0].iptv_code,
               (unsigned long)directory.matched_stream_count);
      set_status(status, window, status_text);
    }
  }
  for (;;) {
    signals = Wait(sigmask | timermask | SIGBREAKF_CTRL_C);
    if (signals & SIGBREAKF_CTRL_C)
      break;
    if (timermask && (signals & timermask)) {
      while (GetMsg(timer_port)) /* remove the replied request */
        ;
      timer_running = 0;
      poll_player_status(status, window, &player_status_seq, status_text,
                         sizeof(status_text));
      poll_timer_start();
    }
    while ((result = RA_HandleInput(winobj, &code)) != WMHI_LASTMSG) {
      STRPTR text = NULL;
      struct Node *node = NULL;
      mr_iptv_channel *channel = NULL;
      if ((result & WMHI_CLASSMASK) == WMHI_CLOSEWINDOW)
        goto done;
      if ((result & WMHI_CLASSMASK) == WMHI_MENUPICK) {
        int action = mr_gui_menu_action(&app_menu, code);
        if (action == MR_GUI_MENU_ABOUT)
          mr_gui_show_about(window, "IPTV ReAction edition");
        else if (action == MR_GUI_MENU_QUIT)
          goto done;
        continue;
      }
      if ((result & WMHI_CLASSMASK) != WMHI_GADGETUP)
        continue;
      if ((result & WMHI_GADGETMASK) == G_CLOSE)
        goto done;
      if ((result & WMHI_GADGETMASK) == G_SEARCH ||
          (result & WMHI_GADGETMASK) == G_COUNTRY ||
          (result & WMHI_GADGETMASK) == G_CATEGORY) {
        GetAttr(STRINGA_TextVal, search, (ULONG *)&text);
        GetAttr(CHOOSER_Selected, country, &country_index);
        GetAttr(CHOOSER_Selected, category, &category_index);
        SetGadgetAttrs((struct Gadget *)channels, window, NULL,
                       LISTBROWSER_Labels, ~0UL, TAG_DONE);
        if ((result & WMHI_GADGETMASK) == G_COUNTRY) {
          snprintf(status_text, sizeof(status_text),
                   "Reading channels for %s...",
                   country_code(country_index) ? country_code(country_index)
                                               : "All");
          set_status(status, window, status_text);
          Delay(1);
          free_nodes(&channel_nodes);
          active_channel = NULL;
          active_stream = 0;
          mr_iptv_free(&directory);
          loaded = load_cache(&directory, country_code(country_index));
        }
        selected =
            rebuild_nodes(&channel_nodes, &directory, text ? (char *)text : "",
                          country_index, category_index);
        report_list_fast_ram();
        SetGadgetAttrs((struct Gadget *)channels, window, NULL,
                       LISTBROWSER_Labels, (ULONG)&channel_nodes, TAG_DONE);
        if ((result & WMHI_GADGETMASK) == G_COUNTRY)
          snprintf(status_text, sizeof(status_text),
                   "Ready: %lu playable %s channels; %lu streams matched",
                   (unsigned long)selected,
                   country_code(country_index) ? country_code(country_index)
                                               : "All",
                   (unsigned long)directory.matched_stream_count);
        else
          snprintf(status_text, sizeof(status_text), "%lu channels shown",
                   (unsigned long)selected);
        set_status(status, window, status_text);
      } else if ((result & WMHI_GADGETMASK) == G_REFRESH) {
        STRPTR search_text = NULL;
        GetAttr(CHOOSER_Selected, country, &country_index);
        set_status(status, window, "Downloading channel directory...");
        SetGadgetAttrs((struct Gadget *)play_button, window, NULL, GA_Disabled,
                       TRUE, TAG_DONE);
        SetGadgetAttrs((struct Gadget *)next_button, window, NULL, GA_Disabled,
                       TRUE, TAG_DONE);
        if (!refresh_cache(refresh_error, sizeof(refresh_error), status,
                           window, country_code(country_index),
                           &refreshed_directory)) {
          set_status(status, window, refresh_error);
          SetGadgetAttrs((struct Gadget *)play_button, window, NULL,
                         GA_Disabled, loaded ? FALSE : TRUE, TAG_DONE);
          SetGadgetAttrs((struct Gadget *)next_button, window, NULL,
                         GA_Disabled, loaded ? FALSE : TRUE, TAG_DONE);
        } else {
          GetAttr(STRINGA_TextVal, search, (ULONG *)&search_text);
          GetAttr(CHOOSER_Selected, category, &category_index);
          SetGadgetAttrs((struct Gadget *)channels, window, NULL,
                         LISTBROWSER_Labels, ~0UL, TAG_DONE);
          free_nodes(&channel_nodes);
          active_channel = NULL;
          active_stream = 0;
          mr_iptv_free(&directory);
          directory = refreshed_directory;
          mr_iptv_init(&refreshed_directory);
          loaded = directory.channel_count != 0;
          selected = rebuild_nodes(&channel_nodes, &directory,
                                   search_text ? (char *)search_text : "",
                                   country_index, category_index);
          report_list_fast_ram();
          SetGadgetAttrs((struct Gadget *)channels, window, NULL,
                         LISTBROWSER_Labels, (ULONG)&channel_nodes, TAG_DONE);
          SetGadgetAttrs((struct Gadget *)play_button, window, NULL,
                         GA_Disabled, loaded ? FALSE : TRUE, TAG_DONE);
          SetGadgetAttrs((struct Gadget *)next_button, window, NULL,
                         GA_Disabled, loaded ? FALSE : TRUE, TAG_DONE);
          snprintf(status_text, sizeof(status_text),
                   "Ready: %lu playable %s channels; %lu streams matched",
                   (unsigned long)selected,
                   country_code(country_index) ? country_code(country_index)
                                               : "All",
                   (unsigned long)directory.matched_stream_count);
          set_status(status, window, status_text);
        }
      } else if ((result & WMHI_GADGETMASK) == G_STOP) {
        if (stop_player())
          set_status(status, window, "Playback stopped.");
        else
          set_status(status, window, "No stream is playing.");
      } else if ((result & WMHI_GADGETMASK) == G_DEBUG) {
        debug_log = !debug_log;
        SetGadgetAttrs((struct Gadget *)debug_button, window, NULL, GA_Text,
                       (ULONG)(debug_log ? "Log: On" : "Log: Off"), TAG_DONE);
        set_status(status, window,
                   debug_log ? "Debug log ON: next Play writes " MRPLAY_LOG_FILE
                             : "Debug log off");
      } else if ((result & WMHI_GADGETMASK) == G_OPEN_URL) {
        GetAttr(STRINGA_TextVal, url, (ULONG *)&text);
        if (mr_master_options_apply(&play_options)) {
          mr_play_options_summary(&play_options, playback_text,
                                  sizeof(playback_text));
          SetGadgetAttrs((struct Gadget *)playback_summary, window, NULL,
                         STRINGA_TextVal, (ULONG)playback_text, TAG_DONE);
        }
        if (!start_url(text ? (char *)text : "", &play_options, debug_log))
          set_status(status, window,
                     last_stop_wedged
                         ? "Previous player did not stop; try again shortly."
                         : "Invalid URL or mrplay could not start.");
        else
          player_status_seq = ~0UL;
      } else if ((result & WMHI_GADGETMASK) == G_PLAY ||
                 (result & WMHI_GADGETMASK) == G_NEXT_STREAM ||
                 ((result & WMHI_GADGETMASK) == G_CHANNELS &&
                  channel_was_double_clicked(channels))) {
        GetAttr(LISTBROWSER_SelectedNode, channels, (ULONG *)&node);
        if (node)
          GetListBrowserNodeAttrs(node, LBNA_UserData, (ULONG)&channel,
                                  TAG_DONE);
        if (!channel)
          set_status(status, window, "Select a channel first.");
        else {
          if (channel != active_channel) {
            active_channel = channel;
            active_stream = 0;
          } else if ((result & WMHI_GADGETMASK) == G_NEXT_STREAM) {
            if (active_stream + 1 >= channel->stream_count) {
              set_status(status, window, "No more streams for this channel.");
              continue;
            }
            active_stream++;
          }
          snprintf(status_text, sizeof(status_text),
                   "Starting %s - stream %lu of %lu", channel->name,
                   (unsigned long)active_stream + 1,
                   (unsigned long)channel->stream_count);
          set_status(status, window, status_text);
          if (mr_master_options_apply(&play_options)) {
            mr_play_options_summary(&play_options, playback_text,
                                    sizeof(playback_text));
            SetGadgetAttrs((struct Gadget *)playback_summary, window, NULL,
                           STRINGA_TextVal, (ULONG)playback_text, TAG_DONE);
          }
          if (!start_stream(&channel->streams[active_stream], &play_options,
                            debug_log))
            set_status(status, window,
                       last_stop_wedged
                           ? "Previous player did not stop; try again shortly."
                           : "Invalid stream metadata or mrplay could not start.");
          else
            player_status_seq = ~0UL;
        }
      }
    }
  }
done:
  rc = RETURN_OK;
cleanup:
  iptv_gui_port_close();
  poll_timer_close();
  mr_gui_menu_close(&app_menu, window);
  if (winobj) {
    if (window)
      RA_CloseWindow(winobj);
    DisposeObject(winobj);
  } else if (layout) {
    DisposeObject(layout);
  } else {
    if (buttons) {
      DisposeObject(buttons);
    } else {
      if (refresh_button)
        DisposeObject(refresh_button);
      if (play_button)
        DisposeObject(play_button);
      if (next_button)
        DisposeObject(next_button);
      if (stop_button)
        DisposeObject(stop_button);
      if (open_button)
        DisposeObject(open_button);
      if (debug_button)
        DisposeObject(debug_button);
      if (close_button)
        DisposeObject(close_button);
    }
    if (search)
      DisposeObject(search);
    if (country)
      DisposeObject(country);
    if (category)
      DisposeObject(category);
    if (channels)
      DisposeObject(channels);
    if (url)
      DisposeObject(url);
    if (status)
      DisposeObject(status);
    if (playback_summary)
      DisposeObject(playback_summary);
    if (search_label)
      DisposeObject(search_label);
    if (country_label)
      DisposeObject(country_label);
    if (category_label)
      DisposeObject(category_label);
    if (url_label)
      DisposeObject(url_label);
  }
  free_nodes(&channel_nodes);
  free_chooser(&countries);
  free_chooser(&categories);
  mr_iptv_free(&directory);
  mr_iptv_free(&refreshed_directory);
  close_classes();
  return rc;
}
