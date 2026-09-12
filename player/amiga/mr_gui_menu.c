#include "mr_gui_menu.h"

#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

struct Library *GadToolsBase;

/* Stack for the "AmigaGuide" process mr_gui_open_guide() launches - a
 * lightweight system utility, unlike mrplay's $STACK:320000 (libavc H.264
 * needs), so a modest size well above the Shell's own ~4-8 KB default is
 * plenty. */
#define MR_GUIDE_STACK_SIZE 20000UL

#define MR_MENU_USER_GUIDE ((APTR)1)
#define MR_MENU_USER_ABOUT ((APTR)2)
#define MR_MENU_USER_QUIT  ((APTR)3)

static struct NewMenu menu_template[] = {
    {NM_TITLE, (STRPTR)"MintVID", NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR)"Guide...", (STRPTR)"H", 0, 0, MR_MENU_USER_GUIDE},
    {NM_ITEM, (STRPTR)"About MintVID...", (STRPTR)"?", 0, 0,
     MR_MENU_USER_ABOUT},
    {NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, MR_MENU_USER_QUIT},
    {NM_END, NULL, NULL, 0, 0, NULL}
};

int mr_gui_menu_open(mr_gui_menu *menu, struct Window *window)
{
    if (!menu || !window || !window->WScreen)
        return 0;
    memset(menu, 0, sizeof(*menu));
    if (!GadToolsBase) {
        GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
        menu->owns_gadtools = GadToolsBase != NULL;
    }
    if (!GadToolsBase)
        return 0;
    menu->visual_info = GetVisualInfoA(window->WScreen, NULL);
    if (!menu->visual_info)
        goto fail;
    menu->strip = CreateMenusA(menu_template, NULL);
    if (!menu->strip ||
        !LayoutMenusA(menu->strip, menu->visual_info, NULL))
        goto fail;
    SetMenuStrip(window, menu->strip);
    return 1;

fail:
    mr_gui_menu_close(menu, window);
    return 0;
}

void mr_gui_menu_close(mr_gui_menu *menu, struct Window *window)
{
    if (!menu)
        return;
    if (window && menu->strip)
        ClearMenuStrip(window);
    if (menu->strip) {
        FreeMenus(menu->strip);
        menu->strip = NULL;
    }
    if (menu->visual_info) {
        FreeVisualInfo(menu->visual_info);
        menu->visual_info = NULL;
    }
    if (GadToolsBase && menu->owns_gadtools) {
        CloseLibrary(GadToolsBase);
        GadToolsBase = NULL;
    }
    menu->owns_gadtools = 0;
}

int mr_gui_menu_action(mr_gui_menu *menu, UWORD code)
{
    struct MenuItem *item;
    APTR user_data;

    if (!menu || !menu->strip || code == MENUNULL)
        return MR_GUI_MENU_NONE;
    item = ItemAddress(menu->strip, code);
    if (!item)
        return MR_GUI_MENU_NONE;
    user_data = GTMENUITEM_USERDATA(item);
    if (user_data == MR_MENU_USER_ABOUT)
        return MR_GUI_MENU_ABOUT;
    if (user_data == MR_MENU_USER_QUIT)
        return MR_GUI_MENU_QUIT;
    if (user_data == MR_MENU_USER_GUIDE)
        return MR_GUI_MENU_GUIDE;
    return MR_GUI_MENU_NONE;
}

static void guide_error(struct Window *window, const char *text)
{
    struct EasyStruct request;
    request.es_StructSize = sizeof(request);
    request.es_Flags = 0;
    request.es_Title = (UBYTE *)"MintVID Guide";
    request.es_TextFormat = (UBYTE *)text;
    request.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(window, &request, NULL, NULL);
}

/*
 * Opens PROGDIR:MintVID.guide the same way every other MintVID GUI launches
 * a support binary - see open_iptv_browser()/open_youtube_browser()/
 * start_player() in mrgui.c, the already-proven LoadSeg()+
 * CreateNewProcTags() shape used to start iptvgui/ytgui/mrplay - rather
 * than calling amigaguide.library directly. Runs the standard AmigaOS
 * "AmigaGuide" command (normally C:AmigaGuide, part of a standard 2.1+/
 * 3.0+ installation - present on the command path, not shipped beside
 * MintVID's own binaries) with the guide's PROGDIR:-relative path as its
 * one argument; that command itself does whatever amigaguide.library/
 * Multiview plumbing is needed, using the exact toolchain every AmigaGuide-
 * literate utility (including this project's own MintPRINT) already relies
 * on to display its own help - so it needs no NDK struct this project has
 * no way to check against, unlike the first version of this function.
 *
 * The process runs detached (NP_Cli TRUE, no output/error stream wired
 * back) and this function does not wait for it - same fire-and-forget
 * shape as opening a video window and moving on.
 */
void mr_gui_open_guide(mr_gui_menu *menu, struct Window *window)
{
    BPTR lock;
    BPTR seglist;
    struct Process *process;
    char arguments[64];

    (void)menu;

    lock = Lock((CONST_STRPTR)"PROGDIR:MintVID.guide", ACCESS_READ);
    if (!lock) {
        guide_error(window,
            "MintVID.guide was not found next to this program.\n"
            "Keep it in the same drawer as the MintVID binaries.");
        return;
    }
    UnLock(lock);

    seglist = LoadSeg((CONST_STRPTR)"AmigaGuide");
    if (!seglist) {
        guide_error(window,
            "The AmigaGuide command was not found (normally\n"
            "C:AmigaGuide, part of a standard AmigaOS 2.1+/3.0+\n"
            "installation). You can still read MintVID.guide with\n"
            "a text editor.");
        return;
    }

    strcpy(arguments, "PROGDIR:MintVID.guide\n");
    process = CreateNewProcTags(
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)arguments,
        NP_StackSize, MR_GUIDE_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"AmigaGuide",
        NP_Name, (ULONG)"MintVID Guide",
        TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        guide_error(window, "Could not start the AmigaGuide command.");
    }
}

void mr_gui_show_about(struct Window *window, const char *edition)
{
    struct EasyStruct request;
    const char *name = edition && *edition ? edition : "AmigaOS edition";

    request.es_StructSize = sizeof(request);
    request.es_Flags = 0;
    request.es_Title = (UBYTE *)"About MintVID";
    request.es_TextFormat = (UBYTE *)
        "MintVID " MINTVID_VERSION " - Video for classic AmigaOS\n"
        "%s\n"
        "Local files, HLS, IPTV and YouTube playback\n"
        "including WMV7/8, MPEG-1/2/4 and H.264.\n\n"
        "Made by Darren 'boingball' Banfi\n"
        "Copyright 2026\n\n"
        "Source and licence notices:\n"
        "https://github.com/boingball/MintVID\n\n"
        "Support development and the LLM token fund:\n"
        "https://buymeacoffee.com/boingball\n\n"
        "Built with libavc, libmpeg2, MintAMP audio decoders,\n"
        "liba52, Claude and Codex.";
    request.es_GadgetFormat = (UBYTE *)"Cheers!";
    EasyRequestArgs(window, &request, NULL, (APTR)&name);
}
