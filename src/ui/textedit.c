#include "textedit.h"
#include "../common.h"
#include "../skin/skin.h"
#include <string.h>
#include <SDL2/SDL.h>

static int utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

void tf_insert(TField *f, const char *utf8) {
    if (f->selall) { f->buf[0] = 0; f->caret = 0; f->selall = 0; }
    size_t tl = strlen(utf8), len = strlen(f->buf);
    if (len + tl >= f->cap - 1) return;
    if (f->caret < 0 || f->caret > (int)len) f->caret = (int)len;
    memmove(f->buf + f->caret + tl, f->buf + f->caret, len - (size_t)f->caret + 1);
    memcpy(f->buf + f->caret, utf8, tl);
    f->caret += (int)tl;
}

void tf_backspace(TField *f) {
    if (f->selall) { f->buf[0] = 0; f->caret = 0; f->selall = 0; return; }
    int c = f->caret;
    if (c <= 0) return;
    int q = c - 1;
    while (q > 0 && utf8_cont((unsigned char)f->buf[q])) q--;
    memmove(f->buf + q, f->buf + c, strlen(f->buf) - (size_t)c + 1);
    f->caret = q;
}

void tf_del(TField *f) {
    if (f->selall) { f->buf[0] = 0; f->caret = 0; f->selall = 0; return; }
    size_t len = strlen(f->buf);
    if (f->caret < 0 || f->caret > (int)len) f->caret = (int)len;
    if (f->caret < (int)len) {
        unsigned cp;
        int n = font_utf8_next(f->buf + f->caret, &cp);
        memmove(f->buf + f->caret, f->buf + f->caret + n,
                len - (size_t)f->caret - (size_t)n + 1);
    }
}

void tf_left(TField *f) {
    if (f->selall) { f->caret = 0; f->selall = 0; return; }
    if (f->caret > 0) {
        int q = f->caret - 1;
        while (q > 0 && utf8_cont((unsigned char)f->buf[q])) q--;
        f->caret = q;
    }
}

void tf_right(TField *f) {
    if (f->selall) { f->caret = (int)strlen(f->buf); f->selall = 0; return; }
    if (f->caret < (int)strlen(f->buf)) {
        unsigned cp;
        f->caret += font_utf8_next(f->buf + f->caret, &cp);
    }
}

void tf_home(TField *f) { f->caret = 0; f->selall = 0; }

void tf_end(TField *f) { f->caret = (int)strlen(f->buf); f->selall = 0; }

void tf_selectall(TField *f) { f->selall = f->buf[0] ? 1 : 0; }

size_t tf_sanitize_paste(const char *in, char *out, size_t n) {
    if (!n) return 0;
    const char *s = in;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                   s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    /* Explorer copies paths quoted: strip one surrounding pair */
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s++; len -= 2; }
    /* keep printable bytes (incl. UTF-8 lead/continuation), drop controls */
    size_t o = 0;
    while (len && *s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c != 0x7F) {
            if (o + 1 >= n) break;
            out[o++] = (char)c;
        }
        s++; len--;
    }
    out[o] = 0;
    return o;
}

int tf_copy(TField *f) {
    if (!f->buf[0]) return 0;
    SDL_SetClipboardText(f->buf);
    return 1;
}

int tf_cut(TField *f) {
    if (!f->selall || !f->buf[0]) return 0;
    SDL_SetClipboardText(f->buf);
    f->buf[0] = 0;
    f->caret = 0;
    f->selall = 0;
    return 1;
}

int tf_paste(TField *f) {
    char *clip = SDL_GetClipboardText();
    if (!clip) return 0;
    char clean[LV_MAX_PATH];
    size_t n = tf_sanitize_paste(clip, clean, sizeof clean);
    SDL_free(clip);
    if (!n) return 0;
    tf_insert(f, clean);
    return 1;
}
