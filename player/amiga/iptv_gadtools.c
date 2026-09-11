/* MintVID IPTV-GT: OS 3.0 GadTools browser using the shared IPTV core. */
#include "../core/mr_http.h"
#include "../core/mr_play_options.h"
#include "../core/mr_source.h"
#include "../iptv/mr_iptv.h"
#include "mr_gui_menu.h"
#include "mr_master_options.h"
#include "mr_player_status.h"

MINTVID_DECLARE_VERSION(iptvgui_gt_version_tag, "iptvgui-GT");

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
#include <string.h>
#include <time.h>

#define IPTV_FILE_MAX (32UL * 1024UL * 1024UL)
#define CHANNELS_URL "https://iptv-org.github.io/api/channels.json"
#define STREAMS_URL  "https://iptv-org.github.io/api/streams.json"
#define MRPLAY_STACK_SIZE 320000UL
#define WIN_W 640
#define WIN_H 356

#if defined(__GNUC__)
static const char iptvgui_gt_stack_cookie[] __attribute__((used))="$STACK:131072";
#endif

struct IntuitionBase *IntuitionBase;
extern struct Library *GadToolsBase;

enum {
    G_SEARCH=1, G_COUNTRY, G_CATEGORY, G_CHANNELS, G_REFRESH, G_PLAY,
    G_NEXT, G_STOP, G_OPEN, G_DEBUG, G_CLOSE, G_URL, G_SUMMARY, G_STATUS
};

typedef struct channel_node {
    struct Node node;
    mr_iptv_channel *channel;
} channel_node;

typedef struct iptvgt {
    struct Screen *screen;
    struct Window *window;
    APTR visual;
    struct Gadget *gadgets, *search, *country, *category, *channels;
    struct Gadget *url, *debug, *summary, *status;
    struct List labels;
    mr_iptv_directory directory;
    mr_play_options options;
    mr_iptv_channel *active;
    unsigned active_stream;
    int debug_on;
    int log_session_open;
    char cache_dir[128];
    mr_gui_menu menu;
    struct MsgPort *timer_port;
    struct timerequest *timer_io;
    int timer_open, timer_running;
    ULONG status_seq;
    LONG selected_index;
    struct DateStamp last_click_ds;
    LONG last_click_index;
    int have_last_click;
} iptvgt;

/* ~600ms at dos.library's 50 ticks/sec (3000 ticks/minute) - a reasonable
 * double-click window when the toolkit's own event loop doesn't expose the
 * IDCMP message timestamps DoubleClick() (intuition.library) would need. */
#define DOUBLE_CLICK_TICKS 30UL

static ULONG ds_ticks(const struct DateStamp *ds)
{
    return ((ULONG)ds->ds_Days * 24UL * 60UL + (ULONG)ds->ds_Minute) * 3000UL
         + (ULONG)ds->ds_Tick;
}

static STRPTR country_labels[]={(STRPTR)"United Kingdom",(STRPTR)"United States",
                                (STRPTR)"All",NULL};
static const char *country_codes[]={"UK","US",NULL};
static STRPTR category_labels[]={(STRPTR)"All",(STRPTR)"News",
                                 (STRPTR)"Entertainment",NULL};
static STRPTR debug_labels[]={(STRPTR)"Log: Off",(STRPTR)"Log: On",NULL};
static const struct TextAttr topaz={(STRPTR)"topaz.font",8,0,0};

static void init_list(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static void set_text(iptvgt *app, struct Gadget *gadget, const char *text)
{
    if (app->window && gadget)
        GT_SetGadgetAttrs(gadget,app->window,NULL,GTTX_Text,
                         (ULONG)(text?text:""),TAG_DONE);
    if (app->window && gadget) RefreshGList(gadget,app->window,NULL,1);
}

static ULONG value(iptvgt *app, struct Gadget *gadget, ULONG tag)
{
    ULONG out=0;
    GT_GetGadgetAttrs(gadget,app->window,NULL,tag,(ULONG)&out,TAG_DONE);
    return out;
}

static int try_cache(iptvgt *app, const char *path)
{
    char probe[192];
    BPTR file;
    strncpy(app->cache_dir,path,sizeof(app->cache_dir)-1);
    app->cache_dir[sizeof(app->cache_dir)-1]=0;
    snprintf(probe,sizeof(probe),"%s.write-test",app->cache_dir);
    file=Open((CONST_STRPTR)probe,MODE_NEWFILE);
    if (!file) return 0;
    Close(file); DeleteFile((CONST_STRPTR)probe); return 1;
}

static int choose_cache(iptvgt *app)
{
    BPTR lock=CreateDir((CONST_STRPTR)"PROGDIR:Cache"); if(lock)UnLock(lock);
    lock=CreateDir((CONST_STRPTR)"PROGDIR:Cache/IPTV"); if(lock)UnLock(lock);
    if (try_cache(app,"PROGDIR:Cache/IPTV/")) return 1;
    lock=CreateDir((CONST_STRPTR)"T:MintVID-IPTV"); if(lock)UnLock(lock);
    return try_cache(app,"T:MintVID-IPTV/");
}

static void cache_path(iptvgt *app, char *out, size_t size, const char *name)
{
    snprintf(out,size,"%s%s",app->cache_dir,name);
}

static int cache_fresh(iptvgt *app)
{
    char path[192]; FILE *file; unsigned long saved; time_t now;
    cache_path(app,path,sizeof(path),"cache.meta"); file=fopen(path,"r");
    if (!file)
        return 0;
    if (fscanf(file, "%lu", &saved) != 1) {
        fclose(file);
        return 0;
    }
    fclose(file);
    now = time(NULL);
    return now != (time_t)-1 && (unsigned long)now >= saved &&
           (unsigned long)now-saved < 24UL*60UL*60UL;
}

static void mark_cache(iptvgt *app)
{
    char path[192]; FILE *file; time_t now=time(NULL); if(now==(time_t)-1)return;
    cache_path(app,path,sizeof(path),"cache.meta");file=fopen(path,"w");
    if(file){fprintf(file,"%lu\n",(unsigned long)now);fclose(file);}
}

static int refresh(iptvgt *app)
{
#if !defined(HAVE_AMISSL)
    set_text(app,app->status,"IPTV refresh needs SSL=1."); return 0;
#else
    char channels[192],streams[192],tmp[200],line[256];
    cache_path(app,channels,sizeof(channels),"channels.json");
    cache_path(app,streams,sizeof(streams),"streams.json");
    snprintf(tmp,sizeof(tmp),"%s.tmp",channels);
    set_text(app,app->status,"Downloading IPTV channel directory...");
    if (!mr_http_download_file(CHANNELS_URL,tmp,IPTV_FILE_MAX)) goto fail;
    DeleteFile((CONST_STRPTR)channels);
    if (!Rename((CONST_STRPTR)tmp,(CONST_STRPTR)channels)) goto fail;
    snprintf(tmp,sizeof(tmp),"%s.tmp",streams);
    set_text(app,app->status,"Downloading IPTV stream directory...");
    if (!mr_http_download_file(STREAMS_URL,tmp,IPTV_FILE_MAX)) goto fail;
    DeleteFile((CONST_STRPTR)streams);
    if (!Rename((CONST_STRPTR)tmp,(CONST_STRPTR)streams)) goto fail;
    mark_cache(app);
    return 1;
fail:
    snprintf(line,sizeof(line),"IPTV download failed: %s",mr_source_last_error());
    set_text(app,app->status,line); DeleteFile((CONST_STRPTR)tmp); return 0;
#endif
}

static void free_labels(iptvgt *app)
{
    struct Node *node;
    if (app->window && app->channels)
        GT_SetGadgetAttrs(app->channels,app->window,NULL,GTLV_Labels,~0UL,TAG_DONE);
    while ((node=RemHead(&app->labels))!=NULL) FreeVec(node);
}

static const char *category_name(ULONG selected)
{
    return selected==1?"News":selected==2?"Entertainment":NULL;
}

static size_t rebuild(iptvgt *app)
{
    STRPTR search=NULL;
    ULONG country=value(app,app->country,GTCY_Active);
    ULONG category=value(app,app->category,GTCY_Active);
    mr_iptv_filter filter;
    size_t i,shown=0;
    GT_GetGadgetAttrs(app->search,app->window,NULL,GTST_String,(ULONG)&search,TAG_DONE);
    filter.country=country<3?country_codes[country]:NULL;
    filter.category=category_name(category);
    filter.search=search?(const char *)search:"";
    free_labels(app);
    for(i=0;i<app->directory.channel_count;i++) {
        channel_node *entry;
        mr_iptv_channel *channel=&app->directory.channels[i];
        if (!mr_iptv_channel_visible(channel,&filter)) continue;
        entry=AllocVec(sizeof(*entry),MEMF_PUBLIC|MEMF_CLEAR);
        if(!entry) break;
        entry->node.ln_Name=(STRPTR)channel->name; entry->channel=channel;
        AddTail(&app->labels,&entry->node); shown++;
    }
    GT_SetGadgetAttrs(app->channels,app->window,NULL,GTLV_Labels,(ULONG)&app->labels,
                     GTLV_Selected,shown?0:~0UL,TAG_DONE);
    app->selected_index = shown ? 0 : -1;
    RefreshGList(app->channels, app->window, NULL, 1);
    return shown;
}

static int load_directory(iptvgt *app)
{
    char channels[192],streams[192],line[256];
    ULONG country=value(app,app->country,GTCY_Active);
    size_t shown;
    cache_path(app,channels,sizeof(channels),"channels.json");
    cache_path(app,streams,sizeof(streams),"streams.json");
    mr_iptv_free(&app->directory); mr_iptv_init(&app->directory);
    if(!mr_iptv_load_country_files(&app->directory,channels,streams,
                                   country<3?country_codes[country]:NULL)) {
        snprintf(line,sizeof(line),"Could not load IPTV cache: %s",mr_iptv_last_error());
        set_text(app,app->status,line); return 0;
    }
    app->active=NULL; app->active_stream=0; shown=rebuild(app);
    snprintf(line,sizeof(line),"Ready: %lu playable channels, %lu streams",
             (unsigned long)shown,(unsigned long)app->directory.matched_stream_count);
    set_text(app,app->status,line); return 1;
}

static mr_iptv_channel *selected(iptvgt *app)
{
    ULONG wanted;
    ULONG index=0;
    struct Node *node;
    if (app->selected_index < 0)
        return NULL;
    wanted = (ULONG)app->selected_index;
    for(node=app->labels.lh_Head;node->ln_Succ;node=node->ln_Succ,index++)
        if(index==wanted) return ((channel_node *)node)->channel;
    return NULL;
}

static int player_running(void)
{
    int yes; Forbid(); yes=FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT)!=NULL;
    Permit(); return yes;
}

/* MR_IPTV_GUI_PORT - see the matching comment in iptv_reaction.c. Lets
 * MintVID Control know once this window is actually open, not merely that
 * the process exists (LoadSeg()/CreateNewProcTags() returns long before
 * that, and choose_cache()'s channel-cache refresh can itself take a real
 * moment). Plain static struct, not AllocMem'd - AmigaOS tasks share one
 * flat address space, and this only needs to be found (PA_IGNORE, no
 * replies), same as MR_IPTV_PLAYER_PORT above. */
static struct MsgPort iptv_gui_port;
static int iptv_gui_port_added;

static void iptv_gui_port_open(void)
{
    if(iptv_gui_port_added)return;
    iptv_gui_port.mp_MsgList.lh_Head=(struct Node *)&iptv_gui_port.mp_MsgList.lh_Tail;
    iptv_gui_port.mp_MsgList.lh_Tail=NULL;
    iptv_gui_port.mp_MsgList.lh_TailPred=(struct Node *)&iptv_gui_port.mp_MsgList.lh_Head;
    iptv_gui_port.mp_Flags=PA_IGNORE;
    iptv_gui_port.mp_SigTask=FindTask(NULL);
    iptv_gui_port.mp_Node.ln_Name=(char *)MR_IPTV_GUI_PORT;
    iptv_gui_port.mp_Node.ln_Pri=0;
    AddPort(&iptv_gui_port);
    iptv_gui_port_added=1;
}

static void iptv_gui_port_close(void)
{
    if(!iptv_gui_port_added)return;
    RemPort(&iptv_gui_port);
    iptv_gui_port_added=0;
}

static int stop_player(void)
{
    struct MsgPort *port; int had,guard;
    Forbid(); port=FindPort((CONST_STRPTR)MR_IPTV_PLAYER_PORT);
    if(port)
        Signal(port->mp_SigTask,SIGBREAKF_CTRL_F);
    had=port!=NULL;
    Permit();
    for(guard=0;had&&guard<250&&player_running();guard++)Delay(1);
    return had?(player_running()?-1:1):0;
}

static int start_stream(iptvgt *app, const mr_iptv_stream *stream)
{
    mr_play_options options=app->options;
    mr_http_options http;
    char args[3200]; BPTR seglist,log=0,nil=0; struct Process *process;
    if(!stream||!mr_iptv_supported_url(stream->url)||
       !mr_http_options_init(&http,stream->user_agent,stream->http_referrer))return 0;
    mr_master_options_apply(&options);
    if(!mr_build_player_arguments(args,sizeof(args),&options,stream->url,
                                  stream->user_agent,stream->http_referrer))return 0;
    if(app->debug_on){size_t n=strlen(args);if(n&&args[n-1]=='\n')n--;
      if(n+8<sizeof(args))memcpy(args+n," --time\n",9);}
    stop_player(); if(player_running())return 0;
    seglist=LoadSeg((CONST_STRPTR)"PROGDIR:mrplay");
    if(!seglist)
        seglist=LoadSeg((CONST_STRPTR)"mrplay");
    if(!seglist)
        return 0;
    if(app->debug_on){
      if(!app->log_session_open){log=Open((CONST_STRPTR)"RAM:MintVID.log",MODE_NEWFILE);
        if(log)app->log_session_open=1;}
      else{log=Open((CONST_STRPTR)"RAM:MintVID.log",MODE_READWRITE);if(log){
        static const char sep[]="\n\n===== new stream =====\n";Seek(log,0,OFFSET_END);
        Write(log,(APTR)sep,(LONG)(sizeof(sep)-1));}}
      nil=Open((CONST_STRPTR)"NIL:",MODE_NEWFILE);
      if(!log||!nil){if(log)Close(log);if(nil)Close(nil);log=nil=0;}
    }
    if(log&&nil)process=CreateNewProcTags(NP_Seglist,seglist,NP_FreeSeglist,TRUE,
      NP_Arguments,(ULONG)args,NP_StackSize,MRPLAY_STACK_SIZE,NP_Cli,TRUE,
      NP_CommandName,(ULONG)"mrplay",NP_Name,(ULONG)"MintVID player",
      NP_Input,(ULONG)nil,NP_CloseInput,TRUE,NP_Output,(ULONG)log,
      NP_CloseOutput,TRUE,TAG_END);
    else process=CreateNewProcTags(NP_Seglist,seglist,NP_FreeSeglist,TRUE,
      NP_Arguments,(ULONG)args,NP_StackSize,MRPLAY_STACK_SIZE,NP_Cli,TRUE,
      NP_CommandName,(ULONG)"mrplay",NP_Name,(ULONG)"MintVID player",TAG_END);
    if(!process){if(log)Close(log);if(nil)Close(nil);UnLoadSeg(seglist);}
    return process!=NULL;
}

static void play_selected(iptvgt *app)
{
    mr_iptv_channel *channel=selected(app);
    if(!channel||!channel->stream_count){set_text(app,app->status,"Select a playable channel.");return;}
    app->active=channel; app->active_stream=0;
    if(start_stream(app,&channel->streams[0]))set_text(app,app->status,"Connecting to selected stream...");
    else set_text(app,app->status,"Could not start mrplay (or old player is busy).");
}

/* Selecting a channel just updates the list's selection; double-clicking the
 * same row within DOUBLE_CLICK_TICKS also starts it, same as pressing Play. */
static void channel_clicked(iptvgt *app, ULONG code)
{
    struct DateStamp now;
    int dbl;
    app->selected_index=(LONG)code;
    GT_SetGadgetAttrs(app->channels,app->window,NULL,GTLV_Selected,code,TAG_DONE);
    DateStamp(&now);
    dbl=app->have_last_click && (LONG)code==app->last_click_index &&
        ds_ticks(&now)-ds_ticks(&app->last_click_ds)<=DOUBLE_CLICK_TICKS;
    app->last_click_ds=now; app->last_click_index=(LONG)code; app->have_last_click=1;
    if(dbl){play_selected(app); app->have_last_click=0;}
}

static void next_stream(iptvgt *app)
{
    if(!app->active||app->active_stream+1>=app->active->stream_count){
        set_text(app,app->status,"No more alternate streams for this channel.");return;
    }
    app->active_stream++;
    if(start_stream(app,&app->active->streams[app->active_stream]))
        set_text(app,app->status,"Trying next stream...");
    else set_text(app,app->status,"Could not start the alternate stream.");
}

static void open_url(iptvgt *app)
{
    STRPTR url=NULL; mr_iptv_stream stream;
    GT_GetGadgetAttrs(app->url,app->window,NULL,GTST_String,(ULONG)&url,TAG_DONE);
    memset(&stream,0,sizeof(stream));
    if(!url||strlen(url)>=sizeof(stream.url)){set_text(app,app->status,"Enter a valid URL first.");return;}
    strcpy(stream.url,url);
    if(start_stream(app,&stream))set_text(app,app->status,"Connecting to manual URL...");
    else set_text(app,app->status,"Could not start that URL.");
}

static void poll_status(iptvgt *app)
{
    mr_player_status ps; char line[300];
    if(!mr_player_status_read(&ps)){app->status_seq=0;return;}
    if(ps.seq==app->status_seq)
        return;
    app->status_seq=ps.seq;
    if(ps.state==MR_PLAYER_STATE_PLAYING)
        snprintf(line,sizeof(line),"Playing%s%s%s%s",ps.codec[0]?" (":"",ps.codec,
                 ps.codec[0]?"): ":"",ps.text);
    else if(ps.state==MR_PLAYER_STATE_UNSUPPORTED)snprintf(line,sizeof(line),"Not supported: %s",ps.text);
    else if(ps.state==MR_PLAYER_STATE_ERROR)snprintf(line,sizeof(line),"Stream error: %s",ps.text);
    else if(ps.state==MR_PLAYER_STATE_ENDED)snprintf(line,sizeof(line),"Stream ended");
    else snprintf(line,sizeof(line),"%s",ps.text[0]?ps.text:"Connecting...");
    set_text(app,app->status,line);
}

static int timer_open(iptvgt *app)
{
    app->timer_port=CreateMsgPort(); if(!app->timer_port)return 0;
    app->timer_io=(struct timerequest *)CreateIORequest(app->timer_port,sizeof(*app->timer_io));
    if(!app->timer_io||OpenDevice((CONST_STRPTR)"timer.device",UNIT_VBLANK,
       (struct IORequest *)app->timer_io,0))
        return 0;
    app->timer_open=1;
    return 1;
}

static void timer_start(iptvgt *app)
{
    app->timer_io->tr_node.io_Command=TR_ADDREQUEST; app->timer_io->tr_time.tv_secs=0;
    app->timer_io->tr_time.tv_micro=200000; SendIO((struct IORequest *)app->timer_io);
    app->timer_running=1;
}

static struct Gadget *add(iptvgt *app,struct Gadget *previous,ULONG kind,UWORD id,
                          int x,int y,int w,int h,const char *label,ULONG tag,ULONG data)
{
    struct NewGadget ng; memset(&ng,0,sizeof(ng)); ng.ng_LeftEdge=x;ng.ng_TopEdge=y;
    ng.ng_Width=w;ng.ng_Height=h;ng.ng_GadgetText=(STRPTR)label;
    ng.ng_TextAttr=(struct TextAttr *)&topaz;ng.ng_GadgetID=id;
    ng.ng_VisualInfo=app->visual;ng.ng_Flags=kind==BUTTON_KIND?PLACETEXT_IN:
      kind==CHECKBOX_KIND?PLACETEXT_RIGHT:PLACETEXT_LEFT;
    if (kind == LISTVIEW_KIND)
        return CreateGadget(kind,previous,&ng,tag,data,
                            GTLV_Selected,~0UL,
                            GTLV_ShowSelected,(ULONG)NULL,
                            GA_RelVerify,TRUE,TAG_DONE);
    return CreateGadget(kind,previous,&ng,tag,data,TAG_DONE);
}

static int window_open(iptvgt *app)
{
    struct Gadget *g; struct NewWindow nw;
    app->screen=LockPubScreen(NULL);if(!app->screen)return 0;
    app->visual=GetVisualInfoA(app->screen,NULL);
    if(!app->visual||!CreateContext(&app->gadgets))return 0;
    g=app->gadgets;
    app->search=g=add(app,g,STRING_KIND,G_SEARCH,8,20,260,15,"",GTST_MaxChars,MR_IPTV_NAME_MAX);
    app->country=g=add(app,g,CYCLE_KIND,G_COUNTRY,274,20,170,15,"",GTCY_Labels,(ULONG)country_labels);
    app->category=g=add(app,g,CYCLE_KIND,G_CATEGORY,450,20,178,15,"",GTCY_Labels,(ULONG)category_labels);
    app->channels=g=add(app,g,LISTVIEW_KIND,G_CHANNELS,8,43,620,192,"",GTLV_Labels,(ULONG)&app->labels);
    app->url=g=add(app,g,STRING_KIND,G_URL,8,242,620,15,"",GTST_MaxChars,MR_IPTV_URL_MAX);
    g=add(app,g,BUTTON_KIND,G_REFRESH,8,265,86,17,"Refresh",TAG_IGNORE,0);
    g=add(app,g,BUTTON_KIND,G_PLAY,98,265,76,17,"Play",TAG_IGNORE,0);
    g=add(app,g,BUTTON_KIND,G_NEXT,178,265,100,17,"Next stream",TAG_IGNORE,0);
    g=add(app,g,BUTTON_KIND,G_STOP,282,265,72,17,"Stop",TAG_IGNORE,0);
    g=add(app,g,BUTTON_KIND,G_OPEN,358,265,96,17,"Open URL",TAG_IGNORE,0);
    app->debug=g=add(app,g,CYCLE_KIND,G_DEBUG,458,265,88,17,"",GTCY_Labels,(ULONG)debug_labels);
    g=add(app,g,BUTTON_KIND,G_CLOSE,550,265,78,17,"Close",TAG_IGNORE,0);
    app->summary=g=add(app,g,TEXT_KIND,G_SUMMARY,8,290,620,15,"",GTTX_Text,(ULONG)"Playback options");
    app->status=g=add(app,g,TEXT_KIND,G_STATUS,8,311,620,15,"",GTTX_Text,(ULONG)"Loading IPTV directory...");
    if(!g)
        return 0;
    memset(&nw,0,sizeof(nw));
    nw.LeftEdge=(app->screen->Width-WIN_W)/2;nw.TopEdge=(app->screen->Height-WIN_H)/2;
    nw.Width=WIN_W;nw.Height=WIN_H;nw.DetailPen=0;nw.BlockPen=1;
    /* LISTVIEWIDCMP (gadtools.h) supplies the mouse-button/move, gadget-down
     * and IntuiTicks events GadTools' composite scroller and auto-repeat
     * up/down arrow gadgets consume internally - IDCMP_GADGETUP alone leaves
     * the channel listview's arrows drawn but unresponsive (same fix as
     * MintAMP's amiga_mp3gui.c radio browser window). */
    nw.IDCMPFlags=LISTVIEWIDCMP|IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW|IDCMP_MENUPICK;
    nw.Flags=WFLG_CLOSEGADGET|WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_ACTIVATE|WFLG_SMART_REFRESH;
    nw.FirstGadget=app->gadgets;nw.Title=(UBYTE *)"MintVID IPTV-GT";
    nw.Screen=app->screen;nw.Type=CUSTOMSCREEN;app->window=OpenWindow(&nw);
    return app->window!=NULL;
}

static void cleanup(iptvgt *app)
{
    iptv_gui_port_close();
    if(app->timer_io){
      if(app->timer_running&&!CheckIO((struct IORequest *)app->timer_io))
          AbortIO((struct IORequest *)app->timer_io);
      if(app->timer_running)
          WaitIO((struct IORequest *)app->timer_io);
      if(app->timer_open)
          CloseDevice((struct IORequest *)app->timer_io);
      DeleteIORequest((struct IORequest *)app->timer_io);
    }
    if(app->timer_port)DeleteMsgPort(app->timer_port);
    mr_gui_menu_close(&app->menu,app->window);
    free_labels(app);mr_iptv_free(&app->directory);if(app->window)CloseWindow(app->window);
    if(app->gadgets)FreeGadgets(app->gadgets);
    if(app->visual)FreeVisualInfo(app->visual);
    if(app->screen)UnlockPubScreen(NULL,app->screen);
    mr_http_net_shutdown();
}

int main(int argc,char **argv)
{
    iptvgt app;char error[160],summary[160];int done=0,rc=RETURN_FAIL;
    ULONG winmask,timermask=0;memset(&app,0,sizeof(app));init_list(&app.labels);
    app.selected_index=-1;
    mr_iptv_init(&app.directory);mr_play_options_default(&app.options);
    if(!mr_play_options_parse(&app.options,argc,argv,error,sizeof(error)))return RETURN_FAIL;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library",37);
    GadToolsBase=OpenLibrary((CONST_STRPTR)"gadtools.library",37);
    if(!IntuitionBase||!GadToolsBase||!choose_cache(&app)||!window_open(&app))goto out;
    mr_play_options_summary(&app.options,summary,sizeof(summary));set_text(&app,app.summary,summary);
    mr_gui_menu_open(&app.menu,app.window);
    iptv_gui_port_open();
    if(!load_directory(&app)){if(refresh(&app))load_directory(&app);}
    else if(!cache_fresh(&app)){if(refresh(&app))load_directory(&app);}
    if(timer_open(&app)){timermask=1UL<<app.timer_port->mp_SigBit;timer_start(&app);}
    winmask=1UL<<app.window->UserPort->mp_SigBit;
    while(!done){struct IntuiMessage *msg;ULONG signals=Wait(winmask|timermask|SIGBREAKF_CTRL_C);
      if(signals&SIGBREAKF_CTRL_C)break;
      if(timermask&&(signals&timermask)){
        while(GetMsg(app.timer_port)) { }
        app.timer_running=0;poll_status(&app);timer_start(&app);}
      while((msg=GT_GetIMsg(app.window->UserPort))!=NULL){ULONG cls=msg->Class;UWORD code=msg->Code;struct Gadget *gad=(struct Gadget *)msg->IAddress;UWORD id=gad?gad->GadgetID:0;GT_ReplyIMsg(msg);
        if(cls==IDCMP_CLOSEWINDOW)done=1;else if(cls==IDCMP_REFRESHWINDOW){GT_BeginRefresh(app.window);GT_EndRefresh(app.window,TRUE);}
        else if(cls==IDCMP_MENUPICK){int action=mr_gui_menu_action(&app.menu,code);if(action==MR_GUI_MENU_ABOUT)mr_gui_show_about(app.window,"IPTV GadTools edition (OS 3.0)");else if(action==MR_GUI_MENU_QUIT)done=1;}
        else if(cls==IDCMP_GADGETUP){if(id==G_CLOSE)done=1;else if(id==G_CHANNELS)channel_clicked(&app,code);else if(id==G_SEARCH||id==G_CATEGORY)rebuild(&app);else if(id==G_COUNTRY)load_directory(&app);
          else if(id==G_REFRESH){if(refresh(&app))load_directory(&app);}else if(id==G_PLAY)play_selected(&app);else if(id==G_NEXT)next_stream(&app);
          else if(id==G_STOP)set_text(&app,app.status,stop_player()?"Playback stopped.":"No stream is playing.");else if(id==G_OPEN)open_url(&app);
          else if(id==G_DEBUG)app.debug_on=value(&app,app.debug,GTCY_Active)!=0;}
      }}rc=RETURN_OK;
out:cleanup(&app);if(GadToolsBase){CloseLibrary(GadToolsBase);GadToolsBase=NULL;}if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);return rc;
}
