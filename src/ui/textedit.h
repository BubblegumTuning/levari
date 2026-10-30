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

/* edit ops (pure: no SDL calls, unit-testable) */
void tf_insert(TField *f, const char *utf8);
void tf_backspace(TField *f);
void tf_del(TField *f);
void tf_left(TField *f);
void tf_right(TField *f);
void tf_home(TField *f);
void tf_end(TField *f);
void tf_selectall(TField *f);

/* paste sanitization (pure): trim leading/trailing whitespace incl \r\n
   (file managers paste trailing newlines), strip one pair of surrounding
   double quotes (Explorer copies quoted paths), drop control chars.
   Returns the output length; out is always NUL-terminated. */
size_t tf_sanitize_paste(const char *in, char *out, size_t n);

/* clipboard ops (need the SDL video subsystem): 1 = did something */
int tf_copy(TField *f);   /* copy the whole buffer */
int tf_cut(TField *f);    /* copy + clear, only when selall */
int tf_paste(TField *f);  /* sanitized clipboard inserted at the caret */

#endif
