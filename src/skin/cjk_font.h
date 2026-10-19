#ifndef LEVARI_CJK_FONT_H
#define LEVARI_CJK_FONT_H

#include <stdint.h>

/* full JIS X 0208 set, 16x16 bitmaps from GNU Unifont, keyed by Unicode
   codepoint; returns 16 row bitmaps (bit 0x8000 = leftmost pixel) or NULL */
#define CJK_GLYPH_W 16
const uint16_t *cjk_glyph(unsigned cp);
int cjk_halfwidth(unsigned cp);
/* half-width ASCII on the same 16px grid (8-wide, left-aligned) */
const uint16_t *ascii16_glyph(unsigned cp);

#endif
