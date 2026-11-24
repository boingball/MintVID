#ifndef MR_MKV_TRACKS_H
#define MR_MKV_TRACKS_H

/* MKA1: audio track list of the opened MKV (mrberry overlay, mr_mkv.c).
 * Every audio track of the file in file order, also ones mrberry cannot
 * play.  Set mr_mkv_audio_want (1-based, 0 = automatic) before opening;
 * mr_mkv_audio_chosen tells the track that is played (1-based, 0 = none). */

#include "mr_demux.h"
#include <stdint.h>

#define MR_MKV_ATRACK_MAX 16

typedef struct {
    uint64_t number, duration_ns;   /* Matroska TrackNumber, DefaultDuration */
    mr_audio_info ai;               /* valid when supported */
    int supported, is_default;
    char codec[20];                 /* Matroska CodecID, e.g. A_AAC, A_DTS */
    char lang[12];                  /* ISO 639-2 ("ger") or BCP 47 ("de") */
    char name[48];                  /* track name, Latin-1 */
} mr_mkv_atrack;

extern mr_mkv_atrack mr_mkv_atracks[MR_MKV_ATRACK_MAX];
extern unsigned mr_mkv_atrack_count;
extern unsigned mr_mkv_audio_want;
extern unsigned mr_mkv_audio_chosen;
extern int mr_mkv_audio_want_bad;
/* MKA1c: optional extra test "can this track play with the chosen output"
 * (mrberry sets it for HDMI sound).  NULL = every supported track. */
extern int (*mr_mkv_audio_usable)(const mr_audio_info *ai);

/* v96.84: subtitle tracks of the opened MKV (text: S_TEXT/UTF8 = SRT,
 * S_TEXT/ASS and S_TEXT/SSA; image formats are listed but not shown).
 * mr_mkv_sub_track = Matroska track number whose blocks go to
 * mr_mkv_sub_cb (0 = none); may change while the film plays. */
#define MR_MKV_STRACK_MAX 16
#define MR_MKV_SUB_SRT 1
#define MR_MKV_SUB_ASS 2
typedef struct {
    uint64_t number;
    int kind;                       /* MR_MKV_SUB_*, 0 = not supported */
    int is_default, is_forced;
    char codec[24];
    char lang[12];
    char name[48];
} mr_mkv_strack;
extern mr_mkv_strack mr_mkv_stracks[MR_MKV_STRACK_MAX];
extern unsigned mr_mkv_strack_count;
extern uint64_t mr_mkv_sub_track;
extern void (*mr_mkv_sub_cb)(uint64_t pts_us, uint64_t dur_us,
                             const uint8_t *data, size_t len, int kind);

/* v96.85: chapter start times (us, sorted) of the first edition, from the
 * Chapters element before the first cluster. */
#define MR_MKV_CHAPTER_MAX 256
extern uint64_t mr_mkv_chapters_us[MR_MKV_CHAPTER_MAX];
extern unsigned mr_mkv_chapter_count;

#endif
