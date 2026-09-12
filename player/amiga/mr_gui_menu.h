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
 * shipped beside every binary by the release target) the same way every
 * other MintVID GUI launches a support binary (mrplay/iptvgui/ytgui):
 * LoadSeg() + CreateNewProcTags() running the standard AmigaOS "AmigaGuide"
 * command (normally C:AmigaGuide) as its own process, passing the guide's
 * path as its argument, rather than calling amigaguide.library directly -
 * see mr_gui_menu.c for why. `menu` is unused but kept so the call site
 * mirrors mr_gui_show_about()'s signature shape. */
void mr_gui_open_guide(mr_gui_menu *menu, struct Window *window);

#endif
