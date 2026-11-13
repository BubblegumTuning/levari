#ifndef LEVARI_TEXTEDIT_H
#define LEVARI_TEXTEDIT_H

#include <stddef.h>

/* Shared single-line text-field editing: one buffer, a byte-offset caret
   always kept on a UTF-8 codepoint boundary, and a select-all flag (the
   next edit replaces the selection). No drag selection: the selection is
   either the whole buffer or nothing. */
typedef struct {
    char *buf;
    size_t cap;   /* buffer size including the NUL */
    int caret;    /* byte offset, codepoint boundary */
    int selall;   /* ctrl-a: whole buffer selected, next edit replaces it */
} TField;

/* edit ops (pure: no SDL calls, unit-testable). The destructive ones
   report whether the buffer actually changed (1) — callers that must
   react to edits (search re-filtering) key off that. */
int tf_backspace(TField *f);
int tf_del(TField *f);
void tf_insert(TField *f, const char *utf8);
void tf_left(TField *f);
void tf_right(TField *f);
void tf_home(TField *f);
void tf_end(TField *f);
void tf_selectall(TField *f);

/* paste sanitization (pure) — the ONE paste policy for every line
   field: trim leading/trailing whitespace incl \r\n (file managers
   paste trailing newlines), strip one pair of surrounding double
   quotes (Explorer copies quoted paths), then keep only printable
   scalars (DEL, C1, surrogates and undecodable bytes dropped — the
   predicate search's own typed filter guards too). Returns the output
   length; out is always NUL-terminated. */
size_t tf_sanitize_paste(const char *in, char *out, size_t n);

/* clipboard ops (need the SDL video subsystem): 1 = did something */
int tf_copy(TField *f);   /* copy the whole buffer */
int tf_cut(TField *f);    /* copy + clear, only when selall; 1 = buffer changed */
int tf_paste(TField *f);  /* sanitized clipboard inserted at the caret */

/* the ONE key switch every line editor shares (file/URL/options/search):
   arrows, Home/End, Backspace/Delete, and ctrl-a/c/x/v. sym/ctrl come
   from SDL_Event (ints, so this header stays SDL-free).
   Returns 1 buffer changed, 0 handled without change, -1 not an edit key
   (the caller's own keys — Enter/Escape/arrows-as-nav — then apply). */
int tf_handle_key(TField *f, int sym, int ctrl);

#endif
