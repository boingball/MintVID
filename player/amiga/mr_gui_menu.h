#ifndef MR_GUI_MENU_H
#define MR_GUI_MENU_H

#include <exec/types.h>
#include "mintvid_version.h"

struct Menu;
struct Window;

typedef struct mr_gui_menu {
    struct Menu *strip;
    APTR visual_info;
    int owns_gadtools;
    int owns_amigaguide;
} mr_gui_menu;

enum {
    MR_GUI_MENU_NONE = 0,
    MR_GUI_MENU_ABOUT,
    MR_GUI_MENU_QUIT,
    MR_GUI_MENU_GUIDE
};

int mr_gui_menu_open(mr_gui_menu *menu, struct Window *window);
void mr_gui_menu_close(mr_gui_menu *menu, struct Window *window);
int mr_gui_menu_action(mr_gui_menu *menu, UWORD code);
void mr_gui_show_about(struct Window *window, const char *edition);
/* "MintVID > Guide..." - opens PROGDIR:MintVID.guide (the AmigaGuide manual
 * shipped beside every binary by the release target) via amigaguide.library,
 * asynchronously so it does not block the calling GUI's own event loop.
 * amigaguide.library is opened lazily on first use and closed again by
 * mr_gui_menu_close() - see mr_gui_menu.c for the fallback shown when the
 * library or the guide file itself is unavailable. */
void mr_gui_open_guide(mr_gui_menu *menu, struct Window *window);

#endif
