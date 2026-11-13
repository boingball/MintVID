/* S1: SRT subtitles on mrberry's OSD layer (see mrb_sub.c). */
#ifndef MRB_SUB_H_INCLUDED
#define MRB_SUB_H_INCLUDED
#include <stddef.h>
#include <stdint.h>

#define MRB_SUB_W 1600              /* multiple of 32 */
#define MRB_SUB_H 128               /* multiple of 16, two lines */

#define MRB_SUB_KEEP 0
#define MRB_SUB_SHOW 1              /* new picture drawn: send it, show */
#define MRB_SUB_HIDE 2              /* no text now: hide the layer */

unsigned mrb_sub_load(const char *film, const char *explicit_name, char *used, size_t used_size);
void mrb_sub_free(void);
int mrb_sub_tick(uint32_t film_ms);
const uint8_t *mrb_sub_pixels(void);
unsigned mrb_sub_count(void);
const char *mrb_sub_text(int i);
int mrb_sub_shown(void);
void mrb_sub_reset(void);
/* v96.83: key S */
int mrb_sub_buffers(void);
void mrb_sub_clear(void);
void mrb_sub_banner(const char *text);
/* v96.84: MKV subtitle block -> cue (ass = ASS/SSA block) */
int mrb_sub_add(uint32_t start_ms, uint32_t end_ms, const uint8_t *data, size_t len, int ass);
/* S2: several SRT files per film */
#define MRB_SUB_FILES 16
int mrb_sub_file_label(const char *base, const char *fname, char *label, size_t size);
int mrb_sub_is_forced(const char *label);
int mrb_sub_lang_match(const char *label, const char *lang);
int mrb_sub_pick_default(const char labels[][64], unsigned n, const char *lang);

#endif
