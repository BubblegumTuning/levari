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

int tf_backspace(TField *f) {
    if (f->selall) { f->buf[0] = 0; f->caret = 0; f->selall = 0; return 1; }
    int c = f->caret;
    if (c <= 0) return 0;
    int q = c - 1;
    while (q > 0 && utf8_cont((unsigned char)f->buf[q])) q--;
    memmove(f->buf + q, f->buf + c, strlen(f->buf) - (size_t)c + 1);
    f->caret = q;
    return 1;
}

int tf_del(TField *f) {
    if (f->selall) { f->buf[0] = 0; f->caret = 0; f->selall = 0; return 1; }
    size_t len = strlen(f->buf);
    if (f->caret < 0 || f->caret > (int)len) f->caret = (int)len;
    if (f->caret < (int)len) {
        unsigned cp;
        int n = font_utf8_next(f->buf + f->caret, &cp);
        memmove(f->buf + f->caret, f->buf + f->caret + n,
                len - (size_t)f->caret - (size_t)n + 1);
        return 1;
    }
    return 0;
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
    /* the ONE paste policy, codepoint-level (the old byte loop kept
       continuation bytes, smuggling C1 and CESU-8 surrogates past a
       filter every consumer — query fields, the daemon, D-Bus —
       rejects): keep printable scalars, drop DEL, C1, surrogates and
       any undecodable byte */
    size_t o = 0;
    while (len && *s) {
        unsigned cp;
        int k = font_utf8_next(s, &cp);
        int keep = cp >= 0x20 && cp != 0x7F &&
                   !(cp >= 0x80 && cp <= 0x9F) &&
                   !(cp >= 0xD800 && cp <= 0xDFFF);
        if (keep) {
            if (o + (size_t)k >= n) break;
            memcpy(out + o, s, (size_t)k);
            o += (size_t)k;
        }
        s += k;
        if (len >= (size_t)k) len -= (size_t)k; else len = 0;
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
    char before[LV_MAX_PATH];
    lv_strlcpy(before, f->buf, sizeof before); /* every caller's cap is <= LV_MAX_PATH */
    tf_insert(f, clean);
    return strcmp(before, f->buf) != 0; /* 0 when the buffer was already full */
}

/* the ONE key switch every line editor shares. Edit keys land on the tf_*
   ops; anything else is reported unhandled so the caller keeps its own
   Enter/Escape/navigation semantics. */
int tf_handle_key(TField *f, int sym, int ctrl) {
    switch (sym) {
    case SDLK_LEFT:      tf_left(f);  return 0;
    case SDLK_RIGHT:     tf_right(f); return 0;
    case SDLK_HOME:      tf_home(f);  return 0;
    case SDLK_END:       tf_end(f);   return 0;
    case SDLK_BACKSPACE: return tf_backspace(f);
    case SDLK_DELETE:    return tf_del(f);
    case SDLK_a: if (ctrl) { tf_selectall(f); return 0; } return -1;
    case SDLK_c: if (ctrl) { tf_copy(f); return 0; } return -1;
    case SDLK_x: if (ctrl) return tf_cut(f); return -1;
    case SDLK_v: if (ctrl) return tf_paste(f); return -1;
    default: return -1;
    }
}
