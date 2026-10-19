#include "playlist.h"
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

void pl_init(Playlist *p) { p->items = NULL; p->count = p->cap = 0; }

void pl_clear(Playlist *p) {
    free(p->items);
    p->items = NULL; p->count = p->cap = 0;
}

static int ensure(Playlist *p, int need) {
    if (need <= p->cap) return 0;
    int cap = p->cap ? p->cap * 2 : 64;
    while (cap < need) cap *= 2;
    Track *items = realloc(p->items, (size_t)cap * sizeof(Track));
    if (!items) return -1;
    p->items = items;
    p->cap = cap;
    return 0;
}

static int audio_ext(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !lv_strcasecmp(e, ".mp3") || !lv_strcasecmp(e, ".ogg") ||
           !lv_strcasecmp(e, ".oga") || !lv_strcasecmp(e, ".opus") ||
           !lv_strcasecmp(e, ".flac") || !lv_strcasecmp(e, ".wav") ||
           !lv_strcasecmp(e, ".aac") || !lv_strcasecmp(e, ".m4a") ||
           !lv_strcasecmp(e, ".wma") || !lv_strcasecmp(e, ".mod") ||
           !lv_strcasecmp(e, ".s3m") || !lv_strcasecmp(e, ".xm") ||
           !lv_strcasecmp(e, ".it");
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void pl_add_dir(Playlist *p, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, (size_t)cap * sizeof *names);
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmpstr);
    for (int i = 0; i < n; i++) {
        char full[LV_MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        struct stat st;
        /* lstat: never descend into a symlinked directory (cycle guard) */
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode))
            pl_add_dir(p, full);
        else if (audio_ext(names[i]))
            pl_add(p, full);
        free(names[i]);
    }
    free(names);
}

void pl_add(Playlist *p, const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        pl_add_dir(p, path);
        return;
    }
    if (ensure(p, p->count + 1) != 0) return;
    Track *t = &p->items[p->count++];
    if (path[0] == '/') {
        lv_strlcpy(t->path, path, sizeof t->path);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd))
            snprintf(t->path, sizeof t->path, "%s/%s", cwd, path);
        else
            lv_strlcpy(t->path, path, sizeof t->path);
    }
    tag_init(&t->tags);
    t->duration = 0;
    t->start_sec = t->end_sec = 0;
}

void pl_insert(Playlist *p, int idx, const char *path) {
    if (idx < 0) idx = 0;
    if (idx > p->count) idx = p->count;
    if (ensure(p, p->count + 1) != 0) return;
    memmove(&p->items[idx + 1], &p->items[idx], (size_t)(p->count - idx) * sizeof(Track));
    p->count++;
    Track *t = &p->items[idx];
    lv_strlcpy(t->path, path, sizeof t->path);
    tag_init(&t->tags);
    t->duration = 0;
    t->start_sec = t->end_sec = 0;
}

void pl_remove(Playlist *p, int idx) {
    if (idx < 0 || idx >= p->count) return;
    memmove(&p->items[idx], &p->items[idx + 1], (size_t)(p->count - idx - 1) * sizeof(Track));
    p->count--;
}

int pl_load_m3u(Playlist *p, const char *m3u) {
    FILE *f = fopen(m3u, "r");
    if (!f) return -1;
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, m3u, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else lv_strlcpy(dir, ".", sizeof dir);

    char line[LV_MAX_PATH + 64];
    int pending_dur = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!*s) continue;
        if (!strncmp(s, "#EXTINF:", 8)) {
            pending_dur = atoi(s + 8);
            continue;
        }
        if (*s == '#') continue;
        char full[LV_MAX_PATH];
        if (s[0] == '/') lv_strlcpy(full, s, sizeof full);
        else snprintf(full, sizeof full, "%s/%s", dir, s);
        pl_add(p, full);
        if (pending_dur) p->items[p->count - 1].duration = pending_dur;
        pending_dur = 0;
    }
    fclose(f);
    return 0;
}

int pl_save_m3u(Playlist *p, const char *m3u) {
    FILE *f = fopen(m3u, "w");
    if (!f) return -1;
    fputs("#EXTM3U\n", f);
    for (int i = 0; i < p->count; i++) {
        Track *t = &p->items[i];
        if (t->tags.title[0] || t->tags.artist[0])
            fprintf(f, "#EXTINF:%d,%s - %s\n", t->duration,
                    t->tags.artist[0] ? t->tags.artist : "?",
                    t->tags.title[0] ? t->tags.title : "?");
        fprintf(f, "%s\n", t->path);
    }
    fclose(f);
    return 0;
}

int pl_load_cue(Playlist *p, const char *cue) {
    FILE *f = fopen(cue, "r");
    if (!f) return -1;
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, cue, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else lv_strlcpy(dir, ".", sizeof dir);

    char line[2048];
    char file[LV_MAX_PATH] = "";
    int idx = -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!strncmp(s, "FILE ", 5)) {
            char *q1 = strchr(s, '"');
            if (!q1) continue;
            char *q2 = strchr(q1 + 1, '"');
            if (!q2) continue;
            *q2 = 0;
            if (q1[1] == '/') lv_strlcpy(file, q1 + 1, sizeof file);
            else snprintf(file, sizeof file, "%s/%s", dir, q1 + 1);
        } else if (!strncmp(s, "TRACK ", 6)) {
            idx = p->count;
            pl_add(p, file);
        } else if (!strncmp(s, "TITLE ", 6) && idx >= 0) {
            char *q1 = strchr(s, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) *q2 = 0;
                lv_strlcpy(p->items[idx].tags.title, q1 + 1, sizeof p->items[idx].tags.title);
            }
        } else if (!strncmp(s, "PERFORMER ", 10) && idx >= 0) {
            char *q1 = strchr(s, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) *q2 = 0;
                lv_strlcpy(p->items[idx].tags.artist, q1 + 1, sizeof p->items[idx].tags.artist);
            }
        } else if (!strncmp(s, "INDEX 01 ", 9)) {
            int mm, ss, ff;
            if (sscanf(s + 9, "%d:%d:%d", &mm, &ss, &ff) == 3) {
                double sec = mm * 60 + ss + ff / 75.0;
                if (idx >= 0) {
                    if (idx >= 1 && p->items[idx - 1].end_sec == 0)
                        p->items[idx - 1].end_sec = sec;
                    p->items[idx].start_sec = sec;
                }
            }
        }
    }
    fclose(f);
    for (int i = 0; i < p->count; i++) {
        Track *t = &p->items[i];
        if (t->start_sec > 0 || t->end_sec > 0)
            t->duration = (int)(t->end_sec - t->start_sec + 0.5);
    }
    return 0;
}

void pl_scan_tags(Playlist *p, pthread_mutex_t *mtx) {
    for (int i = 0; i < p->count; i++) {
        if (p->items[i].tags.title[0]) continue;
        /* per-track locking: tag_read touches tags the player thread may
           be reading in load_track; one lock for the whole playlist would
           stall track loads on big lists */
        if (mtx) pthread_mutex_lock(mtx);
        tag_read(p->items[i].path, &p->items[i].tags);
        if (mtx) pthread_mutex_unlock(mtx);
    }
}
