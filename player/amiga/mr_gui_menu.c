#include "mr_gui_menu.h"

#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <libraries/amigaguide.h>
#include <dos/dos.h>
#include <proto/amigaguide.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

struct Library *GadToolsBase;
struct Library *AmigaGuideBase;

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
    if (AmigaGuideBase && menu->owns_amigaguide) {
        CloseLibrary(AmigaGuideBase);
        AmigaGuideBase = NULL;
    }
    menu->owns_amigaguide = 0;
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

/*
 * Opens PROGDIR:MintVID.guide via amigaguide.library's asynchronous viewer -
 * a separate process/window the library manages independently, so this
 * never blocks the calling GUI's own event loop and there is no handle to
 * track or close afterwards (CloseAmigaGuide() is for a caller that keeps
 * driving the guide's own message port, which nothing here does).
 * amigaguide.library ships with every AmigaOS 2.1+ install (README states
 * a 3.0+ minimum for this project, so it is always expected to be present),
 * but is opened defensively all the same, matching mr_gui_menu_open()'s own
 * handling of gadtools.library - a missing library or guide file degrades
 * to an EasyRequest rather than silently doing nothing or crashing.
 *
 * Only nag_Name is set on the (zero-initialised) struct NewAmigaGuide -
 * every other field is left at its zeroed default. This is deliberate, not
 * an oversight: there is no real AmigaOS toolchain/NDK on this project's dev
 * host to check the exact field set of the real <libraries/amigaguide.h>
 * against (see CLAUDE.md's "Validate against ffmpeg" section for the same
 * gap affecting every other Amiga-only file), so the safest usage touches
 * only the one field every known example of this call sets - the guide's
 * name - and lets the library default everything else (screen, position,
 * size) itself. Real verification is the same as for any other NDK-only
 * code in this tree: the CI build against the real m68k-amigaos-gcc/NDK
 * toolchain, which fails outright on a wrong field name rather than
 * miscompiling, and a real-hardware run to confirm the viewer actually
 * opens and shows the guide correctly.
 */
void mr_gui_open_guide(mr_gui_menu *menu, struct Window *window)
{
    struct NewAmigaGuide nag;
    BPTR lock;

    if (!menu)
        return;
    if (!AmigaGuideBase) {
        AmigaGuideBase = OpenLibrary((CONST_STRPTR)"amigaguide.library", 0);
        menu->owns_amigaguide = AmigaGuideBase != NULL;
    }
    if (!AmigaGuideBase) {
        struct EasyStruct request;
        request.es_StructSize = sizeof(request);
        request.es_Flags = 0;
        request.es_Title = (UBYTE *)"MintVID Guide";
        request.es_TextFormat = (UBYTE *)
            "amigaguide.library is not available on this system.\n"
            "It ships with a standard AmigaOS 3.0+ installation.\n"
            "You can still read PROGDIR:MintVID.guide as plain text.";
        request.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(window, &request, NULL, NULL);
        return;
    }

    lock = Lock((CONST_STRPTR)"PROGDIR:MintVID.guide", ACCESS_READ);
    if (!lock) {
        struct EasyStruct request;
        request.es_StructSize = sizeof(request);
        request.es_Flags = 0;
        request.es_Title = (UBYTE *)"MintVID Guide";
        request.es_TextFormat = (UBYTE *)
            "MintVID.guide was not found next to this program.\n"
            "Keep it in the same drawer as the MintVID binaries.";
        request.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(window, &request, NULL, NULL);
        return;
    }
    UnLock(lock);

    memset(&nag, 0, sizeof(nag));
    nag.nag_Name = (STRPTR)"PROGDIR:MintVID.guide";
    if (!OpenAmigaGuideAsync(&nag, TAG_DONE)) {
        struct EasyStruct request;
        request.es_StructSize = sizeof(request);
        request.es_Flags = 0;
        request.es_Title = (UBYTE *)"MintVID Guide";
        request.es_TextFormat = (UBYTE *)
            "Could not open MintVID.guide.";
        request.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(window, &request, NULL, NULL);
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
