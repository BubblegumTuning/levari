#ifndef LEVARI_LIBRARY_H
#define LEVARI_LIBRARY_H

#include <stddef.h>
#include <stdio.h>
#include "../common.h"
#include "../playlist/playlist.h"

/* argv[1] == "library"; returns exit code */
int lib_cmd(int argc, char **argv);

/* one library subcommand; all output (incl. errors) goes to `out` */
int lib_run(const char *sub, const char *arg, FILE *out);

/* path of the library index file; creates the directory */
void lib_index_path(char *out, size_t n);

/* consume the "#levari-index v2" header line if present (path fields in
   the rest of the stream are escaped and must be unescaped after every
   read); returns 1 when the header was consumed, 0 for legacy indexes */
int lib_idx_header(FILE *f);

/* unescape a v2 index path field in place (only shrinks) */
void lib_unesc_path(char *p);

/* fgets + over-long-row guard: a row cut by the line buffer would have
   its tail re-parsed as a garbage row, so the remainder is drained.
   returns 1 for a whole line, 2 for a truncated (skippable) one, 0 at EOF */
int lib_idx_getline(FILE *f, char *line, size_t n);

/* incremental scan: append only files not already in the index.
   dirs are expanded (~) and realpath'd; added/seen receive counts.
   with_tags=0 indexes filenames only (skips tag + decoder reads).
   returns 0 on success, 1 if the index cannot be opened for writing. */
int lib_scan_add(const char dirs[][LV_MAX_PATH], int n, int with_tags, int *added, int *seen);

/* one parsed row of the index (fields point into a scratch buffer) */
typedef struct {
    const char *path, *artist, *title, *album;
    int duration;
    int track;  /* 0 = unknown */
    int notag;  /* 1 = scan probed the file and found no tags at all */
} LibEntry;

/* call fn(entry, ud) for every row in the index, in file order.
   returns the number of rows visited, or -1 if the index can't be opened. */
int lib_foreach_entry(void (*fn)(const LibEntry *e, void *ud), void *ud);

/* whole index in memory, sorted by path. ALL file I/O happens in load,
   so callers that touch a shared playlist can hold their lock only for
   the apply step (the UI locks the player mutex every draw frame). */
typedef struct { char *path, *artist, *title, *album; int dur, track, notag; } LibRow;
typedef struct { LibRow *rows; int n; } LibIndex;
LibIndex lib_index_load(void);              /* n = 0 when no index exists */
void lib_index_free(LibIndex *ix);
/* fill tags/duration for items with an empty title (exact path match).
   caller must hold the playlist lock (or own the playlist outright). */
void lib_fill_playlist_ix(Playlist *p, const LibIndex *ix);
/* convenience load + apply + free; safe for private temp playlists */
void lib_fill_playlist(Playlist *p);

#endif
