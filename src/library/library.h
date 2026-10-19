#ifndef LEVARI_LIBRARY_H
#define LEVARI_LIBRARY_H

#include <stddef.h>
#include <stdio.h>
#include "../common.h"

/* argv[1] == "library"; returns exit code */
int lib_cmd(int argc, char **argv);

/* one library subcommand; all output (incl. errors) goes to `out` */
int lib_run(const char *sub, const char *arg, FILE *out);

/* path of the library index file; creates the directory */
void lib_index_path(char *out, size_t n);

/* incremental scan: append only files not already in the index.
   dirs are expanded (~) and realpath'd; added/seen receive counts.
   with_tags=0 indexes filenames only (skips tag + decoder reads).
   returns 0 on success, 1 if the index cannot be opened for writing. */
int lib_scan_add(const char dirs[][LV_MAX_PATH], int n, int with_tags, int *added, int *seen);

/* one parsed row of the index (fields point into a scratch buffer) */
typedef struct {
    const char *path, *artist, *title, *album;
    int duration;
    int track; /* 0 = unknown */
} LibEntry;

/* call fn(entry, ud) for every row in the index, in file order.
   returns the number of rows visited, or -1 if the index can't be opened. */
int lib_foreach_entry(void (*fn)(const LibEntry *e, void *ud), void *ud);

#endif
