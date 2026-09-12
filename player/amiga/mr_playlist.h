#ifndef MR_PLAYLIST_H
#define MR_PLAYLIST_H

#include <exec/lists.h>
#include <string.h>

#define MR_PLAYLIST_MAX 128
#define MR_PLAYLIST_PATH_MAX 512
#define MR_PLAYLIST_NAME_MAX 80

typedef struct mr_playlist {
    char paths[MR_PLAYLIST_MAX][MR_PLAYLIST_PATH_MAX];
    char names[MR_PLAYLIST_MAX][MR_PLAYLIST_NAME_MAX];
    int count;
    int selected;
    int current;
} mr_playlist;

static inline void mr_playlist_init(mr_playlist *playlist)
{
    memset(playlist, 0, sizeof(*playlist));
    playlist->selected = -1;
    playlist->current = -1;
}

static inline const char *mr_playlist_base_name(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (p && *p) {
        if (*p == '/' || *p == ':')
            last = p + 1;
        p++;
    }
    return last ? last : "";
}


static inline void mr_playlist_list_init(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = (struct Node *)0;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
    list->lh_Type = 0;
}

static inline void mr_playlist_copy(char *dst, unsigned long size,
                                    const char *src)
{
    unsigned long i = 0;
    if (!dst || size == 0)
        return;
    if (!src)
        src = "";
    while (i + 1 < size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static inline int mr_playlist_add(mr_playlist *playlist, const char *path)
{
    int n;
    if (!playlist || !path || !*path || playlist->count >= MR_PLAYLIST_MAX)
        return 0;
    n = playlist->count++;
    mr_playlist_copy(playlist->paths[n], MR_PLAYLIST_PATH_MAX, path);
    mr_playlist_copy(playlist->names[n], MR_PLAYLIST_NAME_MAX,
                     mr_playlist_base_name(path));
    if (playlist->selected < 0)
        playlist->selected = 0;
    return 1;
}

static inline void mr_playlist_remove(mr_playlist *playlist, int index)
{
    int i;
    if (!playlist || index < 0 || index >= playlist->count)
        return;
    for (i = index; i + 1 < playlist->count; i++) {
        memcpy(playlist->paths[i], playlist->paths[i + 1],
               sizeof(playlist->paths[i]));
        memcpy(playlist->names[i], playlist->names[i + 1],
               sizeof(playlist->names[i]));
    }
    playlist->count--;
    if (playlist->current > index)
        playlist->current--;
    else if (playlist->current == index)
        playlist->current = -1;
    if (playlist->selected > index)
        playlist->selected--;
    else if (playlist->selected >= playlist->count)
        playlist->selected = playlist->count - 1;
}

static inline void mr_playlist_clear(mr_playlist *playlist)
{
    if (!playlist)
        return;
    playlist->count = 0;
    playlist->selected = -1;
    playlist->current = -1;
}

#endif
