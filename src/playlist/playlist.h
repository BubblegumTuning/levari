#ifndef LEVARI_PLAYLIST_H
#define LEVARI_PLAYLIST_H

#include <pthread.h>
#include "../tag/tag.h"
#include "../common.h"

typedef struct {
    char path[LV_MAX_PATH];
    Tags tags;
    int duration; /* seconds, 0 unknown */
    int notag;    /* index says this file carries no tags: skip re-reads */
    double start_sec, end_sec; /* CUE virtual track bounds, 0 = whole file */
} Track;

typedef struct {
    Track *items;
    int count, cap;
} Playlist;

void pl_init(Playlist *p);
void pl_clear(Playlist *p);
int  pl_add(Playlist *p, const char *path); /* 0 appended, -1 out of memory */
int  pl_add_file(Playlist *p, const char *path); /* no dir probe: path is a file */
void pl_insert(Playlist *p, int idx, const char *path);
void pl_insert_track(Playlist *p, int idx, const Track *src); /* full row copy (paste) */
void pl_remove(Playlist *p, int idx);
int  pl_load_m3u(Playlist *p, const char *m3u);
int  pl_load_pls(Playlist *p, const char *pls);
int  pl_save_m3u(Playlist *p, const char *m3u);
int  pl_load_cue(Playlist *p, const char *cue);
void pl_scan_tags(Playlist *p, pthread_mutex_t *mtx);

/* ---- shared load-format classifier ----
   One truth for the extension dispatch in main.c argv, ui.c drop and
   daemon.c play — .m3u/.m3u8 parse identically (bytes are transparent
   to pl_load_m3u); .pls is Winamp's INI format */
typedef enum { PL_NONE = 0, PL_M3U, PL_PLS, PL_CUE } PlKind;
PlKind pl_ext_kind(const char *path);

/* ---- edit primitives (playlist editing depth) ----
   Every mutation takes the caller's cur index and remaps it through the
   operation so the current-track pointer lands on the SAME MUSIC after
   (the caller posts CMD_STOP itself when the cur row is destroyed —
   same contract as the multi-REM path). CUE virtual rows share a path
   with their parent and form an ATOMIC GROUP: sort/reverse/shuffle move
   a group as one unit sorted by its parent row, copy covers the whole
   group, paste keeps rows adjacent, crop keeps any group it touches
   whole. Callers expand selections to whole groups with pl_group_span.
   Lock discipline per caller: these touch only the array, never I/O
   (pl_load_pls is the exception — open it like pl_load_m3u). */
enum { PL_SORT_TITLE, PL_SORT_ARTIST, PL_SORT_ALBUM, PL_SORT_TRACK, PL_SORT_FILE };

/* group = maximal run of adjacent rows sharing one path where at least
   one row carries CUE bounds. Writes [a,b) row span of idx's group. */
void pl_group_span(Playlist *p, int idx, int *a, int *b);

/* expand a sorted unique row set to whole groups; returns the new count,
   or -1 on OOM (out untouched). out needs room for p->count ints. */
int  pl_sel_expand(Playlist *p, const int *sel, int n, int *out);

void pl_sort(Playlist *p, int key, int *cur);
void pl_reverse(Playlist *p, int *cur);
void pl_shuffle(Playlist *p, int *cur); /* rand()-driven, like Winamp */

/* bubble the (group-expanded) selection past the unselected rows by
   steps (+1 down / -1 up), clamped at the ends; sel remapped in place
   and stays sorted, cur remapped */
void pl_move(Playlist *p, int *sel, int n, int steps, int *cur);

/* drop the (group-expanded) selection at gap index at (0..count: rows
   before at stay above the block, rows at/after at fall below it —
   relative order preserved on both sides). A gap inside a CUE group
   snaps forward exactly like pl_clip_paste; a gap inside the dragged
   block's own span is a no-op by construction (identity permutation).
   Non-adjacent selected units land as one contiguous block (shell
   semantics — the dragged set rides together). sel remaps in place to
   the landed rows and stays sorted; cur rides the permutation. */
void pl_drop(Playlist *p, int *sel, int n, int at, int *cur);

/* remove a sorted unique row set, walking high-to-low (the multi-REM
   engine: cur drops by the removed rows below it; caller checks cur-in-
   set before calling, as PB_REM does). Parity pinned by the tests. */
void pl_remove_set(Playlist *p, const int *sel, int n, int *cur);
/* crop = keep the (group-expanded) selection, remove everything else */
void pl_crop(Playlist *p, const int *sel, int n, int *cur);

/* INTERNAL row clipboard (survives window focus, not app restart):
   copy refuses the WHOLE copy on OOM (never partial, pl_add's -1
   contract) and keeps the previous clipboard. Cut = copy + remove_set;
   paste inserts at index at (snapped forward off a group boundary in
   progress), cur shifts only when the paste lands at/above it.
   Selections passed here must be group-expanded. */
int  pl_clip_copy(Playlist *p, const int *sel, int n);
void pl_clip_cut(Playlist *p, const int *sel, int n, int *cur);
int  pl_clip_paste(Playlist *p, int at, int *cur); /* -1 OOM, list intact */
int  pl_clip_count(void);                          /* 0 = nothing to paste */

#endif
