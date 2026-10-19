#ifndef LEVARI_PLAYLIST_H
#define LEVARI_PLAYLIST_H

#include <pthread.h>
#include "../tag/tag.h"
#include "../common.h"

typedef struct {
    char path[LV_MAX_PATH];
    Tags tags;
    int duration; /* seconds, 0 unknown */
    double start_sec, end_sec; /* CUE virtual track bounds, 0 = whole file */
} Track;

typedef struct {
    Track *items;
    int count, cap;
} Playlist;

void pl_init(Playlist *p);
void pl_clear(Playlist *p);
void pl_add(Playlist *p, const char *path);
void pl_insert(Playlist *p, int idx, const char *path);
void pl_remove(Playlist *p, int idx);
int  pl_load_m3u(Playlist *p, const char *m3u);
int  pl_save_m3u(Playlist *p, const char *m3u);
int  pl_load_cue(Playlist *p, const char *cue);
void pl_scan_tags(Playlist *p, pthread_mutex_t *mtx);

#endif
