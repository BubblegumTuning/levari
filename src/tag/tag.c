#include "tag.h"
#include "../common.h"
#include <ctype.h>

/* One head read per file: on network / spinning-disk storage every open and
   every seek to a new region costs a round-trip, so tag_probe pulls this
   much up front and parses everything it can from memory. The tail is
   visited only when an ID3v1 record can still add something. */
#define TAG_HEAD (128 * 1024)

void tag_init(Tags *t) {
    t->title[0] = t->artist[0] = t->album[0] = 0;
    t->track = 0;
}

static void copy_fixed(char *dst, size_t dn, const unsigned char *src, int n) {
    /* windows-1252 tier: the bytes 0x80-0x9F where cp1251 and windows-1252
       AGREE (smart quotes, dashes, ellipsis, bullet, euro, per-mille, tm).
       Taggers on Windows wrote these bytes for punctuation in enc=0/id3v1
       fields — extremely common in real titles ("Don\u2019t Start Now");
       latin-1 calls them C1 controls, which are never legitimate tag content
       and render as invisible garbage. The disagreeing bytes (cyrillic
       letters like 0x80 Ђ, 0x9A Њ, 0x9C ѕ/œ, and the undefined 0x90/0x98)
       keep the latin-1 C1 reading: the cp1251 classifier already handles
       them when context says cyrillic, and guessing would move other pins.
       Mirror of test/tagwords/tagcheck.py W1252 — keep in sync. */
    static const struct { unsigned char b; unsigned cp; } w1252[] = {
        {0x82,0x201A},{0x84,0x201E},{0x85,0x2026},{0x86,0x2020},{0x87,0x2021},
        {0x88,0x20AC},{0x89,0x2030},{0x8B,0x2039},
        {0x91,0x2018},{0x92,0x2019},{0x93,0x201C},{0x94,0x201D},{0x95,0x2022},
        {0x96,0x2013},{0x97,0x2014},{0x99,0x2122},{0x9B,0x203A},
    };
    size_t j = 0;
    for (int i = 0; i < n && src[i]; i++) {
        unsigned char c = src[i];
        if (c < 0x20) continue;
        if (c < 0x80) {
            if (j + 1 >= dn) break;
            dst[j++] = (char)c;
            continue;
        }
        unsigned cp = c;                        /* latin-1 -> utf-8 */
        for (unsigned k = 0; k < sizeof w1252 / sizeof w1252[0]; k++)
            if (w1252[k].b == c) { cp = w1252[k].cp; break; }
        if (cp < 0x800) {
            if (j + 2 >= dn) break;
            dst[j++] = (char)(0xC0 | (cp >> 6));
            dst[j++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (j + 3 >= dn) break;
            dst[j++] = (char)(0xE0 | (cp >> 12));
            dst[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[j++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    /* ID3v1 pads its fixed-width fields with spaces; drop the trailing run
       so short tags don't turn into "Artist<24 spaces>" in the title bar */
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = 0;
}

/* vorbis comments are utf-8 by spec: copy the bytes through as-is,
   only dropping control chars (latin-1 -> utf-8 conversion would
   double-encode and mangle non-ascii titles). whole sequences only:
   a field truncated at a fixed boundary (255B frame caps) must not end
   in dangling lead/continuation bytes; invalid utf-8 poisons the
   library index and MPRIS strings */
static void copy_utf8(char *dst, size_t dn, const unsigned char *src, int n) {
    size_t j = 0;
    int i = 0;
    while (i < n && src[i]) {
        unsigned char c = src[i];
        if (c < 0x20 || c == 0x7F) { i++; continue; }
        int len = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (len > 1) {
                if (i + len > n) break;         /* cut mid-sequence: drop it */
                for (int k = 1; k < len; k++)
                    if ((src[i + k] & 0xC0) != 0x80) { len = 1; break; }
            }
        }
        if (j + (size_t)len >= dn) break;
        for (int k = 0; k < len; k++) dst[j++] = (char)src[i + k];
        i += len;
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = 0;
}

/* cp1251 (windows cyrillic) high half -> unicode; 0x98 is undefined there */
static const uint16_t cp1251_hi[128] = {
0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
0xFFFD,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,
0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,
0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
};

/* is the payload valid utf-8 containing at least one multibyte sequence?
   the final sequence may be truncated at n: fixed-width fields (id3v1's
   30 bytes, 255B frame caps) cut emoji in half, and rejecting the whole
   field over that tail re-decodes clean utf-8 as latin-1/cp1251 mojibake.
   *clamped receives the length through the last COMPLETE sequence and
   *mb whether any multibyte sequence is present (a truncated tail counts) */
static int utf8_scan(const unsigned char *src, int n, size_t *clamped, int *mb) {
    size_t i = 0, last = 0;
    int m = 0, badpair = 0;
    while (i < (size_t)n && src[i]) {
        unsigned c = src[i];
        int len;
        if (c < 0x80) { i++; last = i; continue; }
        if (c < 0xC2 || c >= 0xF5) return 0;   /* C0/C1 overlongs and F5+ are not
                                                  UTF-8 lead bytes: accepting F5-F7
                                                  ate real cp1251 words (ц=F6 turned
                                                  "цієї" into one fake 4-byte char) */
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return 0;
        if (i + (size_t)len > (size_t)n) {
            /* truncated tail: accept when the tail carries >=2 real
               continuation bytes (a 4-byte char/emoji cut — the 0.3.2
               clamp case), or when the field ALREADY contains a complete
               multibyte char and no complete 2-byte pair decoded to a
               latin-1 symbol. that pair class (U+0080-0x00BF, ×, ÷) is
               exactly how cp1251 words masquerade ("Від"=C2 B3 E4: a
               pseudo-pair + trailing lead), while real UTF-8 cyrillic/
               cjk pairs decode to letters — accept those and clamp the
               cut. a lone lead-class byte with no continuation behind it
               and no prior multibyte stays the latin-1/cp1251 field-end
               shape (é=0xE9, "да"=E4 E0): reject the whole field and let
               the shape rules read it. */
            size_t k = 1;
            while (i + k < (size_t)n && (src[i + k] & 0xC0) == 0x80) k++;
            if (last > 0 && (k >= 3 || (m && !badpair))) { *clamped = last; *mb = 1; return 1; }
            return 0;
        }
        for (int k = 1; k < len; k++)
            if ((src[i + k] & 0xC0) != 0x80) return 0;
        if (len > 1) m = 1;
        if (len == 2) {                          /* decoded class of a complete
                                                    2-byte pair: latin-1 symbol
                                                    = cp1251 masquerade evidence */
            unsigned cp = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F);
            if (cp <= 0xBF || cp == 0xD7 || cp == 0xF7) badpair = 1;
        }
        i += (size_t)len;
        last = i;
    }
    *clamped = last;
    *mb = m;
    return 1;
}

/* cyrillic words are runs of cp1251 letter bytes that stand as their own
   words. western latin-1 accents sit INSIDE ascii words — doubled or even
   adjacent accents happen (Hääyö, Grüße, Canções) — so a high-byte run
   that touches an ascii letter is latin-1, not cp1251. only letter bytes
   join a run: punctuation (cp1251 « », latin-1 ° ·) separates runs. */
static int cp1251_letter(unsigned char c) {
    if (c == 0xA8 || c == 0xB8) return 1;          /* Ё ё */
    if (c < 0x80) return 0;
    if (c < 0xC0) {
        /* the cp1251-specific cyrillic letters of the 0x80-0xBF block per
           cp1251_hi: ukrainian і ї є ґ Ґ, belarusian ў і, macedonian/serbian
           ѓ ѕ љ њ џ Ћ Ќ and companions. excluding them made "Від" "Київ"
           "їсти" fall to latin-1 — 0.3.0's crude >=0x80 adjacency read them
           fine. the latin-1 punctuation that shares the range (°±µ¹» ¤¦ etc.)
           stays a non-letter so Hääyö/Grüße stay latin-1. 0xBD (cp1251 ќ) is
           deliberately NOT a letter: the position carries latin-1 ½, common
           in titles ("12½"), and ќ is rare; 0xBC/0xBE stay letters (Ќ/џ) */
        static const unsigned char lo[] = {
            0x80,0x81,0x83,0x8A,0x8C,0x8D,0x8E,0x8F,0x90,0x9A,
            0x9C,0x9D,0x9E,0x9F,0xA1,0xA2,0xA3,0xA5,0xAA,0xAF,
            0xB2,0xB3,0xB4,0xBA,0xBC,0xBE,0xBF,
        };
        for (unsigned i = 0; i < sizeof lo; i++)
            if (lo[i] == c) return 1;
        return 0;
    }
    if (c == 0xD7 || c == 0xF7) return 0;          /* × ÷ are not letters */
    return 1;                                      /* А-Я / а-я */
}
static int ascii_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int looks_cp1251(const unsigned char *src, int n) {
    for (int i = 0; i < n && src[i]; ) {
        if (!cp1251_letter(src[i])) { i++; continue; }
        int start = i;
        while (i < n && src[i] && cp1251_letter(src[i])) i++;
        if (i - start < 2) continue;
        unsigned char left = start ? src[start - 1] : ' ';
        unsigned char right = (i < n && src[i]) ? src[i] : ' ';
        if (!ascii_alpha(left) && !ascii_alpha(right)) return 1;
    }
    return 0;
}

/* legacy 8-bit tag fields (id3v2 enc=0, id3v1): the spec says latin-1 but
   russian taggers wrote cp1251 and sloppy ones wrote utf-8 outright.
   classify by shape and decode accordingly; raw copying (the old behavior)
   double-encoded every non-ascii byte into mojibake. */
static void copy_legacy(char *dst, size_t dn, const unsigned char *src, int n) {
    size_t clamped = 0;
    int mb = 0;
    if (utf8_scan(src, n, &clamped, &mb) && mb) {
        copy_utf8(dst, dn, src, (int)clamped);
        return;
    }
    if (!looks_cp1251(src, n)) { copy_fixed(dst, dn, src, n); return; }
    size_t j = 0;
    for (int i = 0; i < n && src[i]; i++) {
        unsigned char c = src[i];
        if (c < 0x20) continue;
        unsigned cp = c < 0x80 ? c : cp1251_hi[c - 0x80];
        if (cp < 0x80) {
            if (j + 1 >= dn) break;
            dst[j++] = (char)cp;
        } else if (cp < 0x800) {
            if (j + 2 >= dn) break;
            dst[j++] = (char)(0xC0 | (cp >> 6));
            dst[j++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (j + 3 >= dn) break;
            dst[j++] = (char)(0xE0 | (cp >> 12));
            dst[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[j++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = 0;
}

static size_t utf16_to_utf8(char *dst, size_t dn, const unsigned char *src, size_t sn) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < sn; i += 2) {
        unsigned cp = src[i] | (src[i + 1] << 8);
        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < sn) {
            unsigned lo = src[i + 2] | (src[i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp == 0xFEFF) continue;                  /* stray BOM */
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* unpaired surrogate:
                                                          raw 3-byte output would
                                                          be invalid utf-8 */
        if (cp < 0x80) {
            if (o + 1 >= dn) break;
            dst[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= dn) break;
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= dn) break;
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= dn) break;
            dst[o++] = (char)(0xF0 | (cp >> 18));
            dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[o] = 0;
    return o;
}

static void text_frame(Tags *t, const char *id, const unsigned char *p, size_t n) {
    if (n < 1) return;
    unsigned enc = p[0];
    const unsigned char *v = p + 1;
    size_t vn = n - 1;
    char *dst = NULL;
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = t->title;
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = t->artist;
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = t->album;
    else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) {
        char buf[32] = {0};
        if (enc == 1 || enc == 2) utf16_to_utf8(buf, sizeof buf, v, vn);
        else { size_t k = vn < 31 ? vn : 31; memcpy(buf, v, k); }
        t->track = atoi(buf);
        return;
    }
    else return;
    if (enc == 1 || enc == 2) {
        if (enc == 1 && vn >= 2 && v[0] == 0xFF && v[1] == 0xFE) { v += 2; vn -= 2; }
        else if (enc == 1 && vn >= 2 && v[0] == 0xFE && v[1] == 0xFF) {
            /* big endian: skip the BOM (the LE branch above does too —
             * without this the swapped FE FF becomes U+FEFF in the text),
             * swap into scratch; zero-init so a truncated odd-length
             * field has no uninitialized tail byte */
            v += 2; vn -= 2;
            unsigned char sw[512] = {0};
            size_t k = vn < sizeof sw ? vn : sizeof sw;
            for (size_t i = 0; i + 1 < k; i += 2) { sw[i] = v[i + 1]; sw[i + 1] = v[i]; }
            /* a BOM written into an enc=2 field swaps to FF FE: skip it */
            size_t bo = (k >= 2 && sw[0] == 0xFF && sw[1] == 0xFE) ? 2 : 0;
            utf16_to_utf8(dst, 256, sw + bo, k - bo);
            return;
        }
        utf16_to_utf8(dst, 256, v, vn);
    } else {
        copy_legacy(dst, 256, v, (int)(vn < 255 ? vn : 255));
    }
}

static unsigned be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}

/* walk ID3v2 frames (ver 2/3/4) from a memory buffer */
static void id3v2_frames(const unsigned char *buf, size_t sz, int ver, int flags, Tags *t) {
    size_t i = 0;
    /* extended header (flag 0x40): without skipping it the frame walk
       starts inside the ext header and loses every frame. v3: 4 plain
       bytes, the size EXCLUDES the size field itself (skip = size + 4).
       v4: 4 syncsafe bytes, the size INCLUDES them (skip = size). */
    if (ver >= 3 && (flags & 0x40)) {
        if (sz < 4) return;
        size_t eh;
        if (ver == 4)
            eh = ((size_t)buf[0] << 21) | ((size_t)buf[1] << 14) |
                 ((size_t)buf[2] << 7) | buf[3];
        else
            eh = be32(buf);
        i = ver == 4 ? eh : eh + 4;
        if (i > sz) return;
    }
    int idlen = ver == 2 ? 3 : 4;
    while (i + (size_t)(idlen + (ver == 2 ? 3 : 6)) <= sz) {
        char id[5] = {0};
        memcpy(id, buf + i, idlen);
        size_t fsz;
        if (ver == 2) {
            fsz = ((size_t)buf[i + 3] << 16) | ((size_t)buf[i + 4] << 8) | buf[i + 5];
            i += 6;
        } else {
            if (ver == 4)
                fsz = ((size_t)buf[i + 4] << 21) | ((size_t)buf[i + 5] << 14) |
                      ((size_t)buf[i + 6] << 7) | buf[i + 7];
            else
                fsz = ((size_t)buf[i + 4] << 24) | ((size_t)buf[i + 5] << 16) |
                      ((size_t)buf[i + 6] << 8) | buf[i + 7];
            i += 10;
        }
        if (fsz == 0 || i + fsz > sz) break;
        if (id[0] == 'T') text_frame(t, id, buf + i, fsz);
        i += fsz;
    }
}

/* ID3v1 fills only what the head parse left empty: ID3v2 wins when both
   exist (the old code let the v1 tail overwrite the v2 values) */
static void fill_fixed(char *dst, size_t dn, const unsigned char *src, int n) {
    if (dst[0]) return;
    copy_legacy(dst, dn, src, n);
}

static void read_id3v1(FILE *f, Tags *t) {
    if (fseek(f, -128, SEEK_END) != 0) return;
    unsigned char r[128];
    if (fread(r, 1, 128, f) != 128) return;
    if (memcmp(r, "TAG", 3) != 0) return;
    fill_fixed(t->title, sizeof t->title, r + 3, 30);
    fill_fixed(t->artist, sizeof t->artist, r + 33, 30);
    fill_fixed(t->album, sizeof t->album, r + 63, 30);
    /* v1.1: comment[28]==0 && comment[29]!=0 -> track number. the comment
       field starts at r[97] (title 3..32, artist 33..62, album 63..92,
       year 93..96) — the old 93-based offset read four bytes early. */
    if (!t->track && r[97 + 28] == 0 && r[97 + 29] != 0) t->track = r[97 + 29];
}

/* skip the tail seek entirely once the display fields are present: an
   album/track carried only by ID3v1 is rare, and on spinning rust the
   extra seek costs more than the field is worth */
static int tags_complete(const Tags *t) {
    return t->title[0] && t->artist[0];
}

static void vorbis_comment(Tags *t, const unsigned char *p, size_t n) {
    /* p points after 0x03 "vorbis" */
    if (n < 8) return;
    unsigned rd = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
    p += 4; n -= 4;
    if (rd > n) return;
    p += rd; n -= rd;
    if (n < 4) return;
    unsigned cnt = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
    p += 4; n -= 4;
    for (unsigned c = 0; c < cnt && n >= 4; c++) {
        unsigned cl = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
        p += 4; n -= 4;
        if (cl > n) break;
        char key[32] = {0};
        size_t kl = cl < 31 ? cl : 31;
        size_t eq = 0;
        while (eq < kl && p[eq] != '=') eq++;
        if (eq >= kl) { p += cl; n -= cl; continue; } /* no '=': not a KEY=VALUE entry */
        memcpy(key, p, eq);
        for (size_t i = 0; key[i]; i++) key[i] = (char)toupper((unsigned char)key[i]);
        const unsigned char *val = p + eq + 1;
        size_t vl = cl - eq - 1;
        if (!strcmp(key, "TITLE")) copy_utf8(t->title, sizeof t->title, val, (int)(vl < 255 ? vl : 255));
        else if (!strcmp(key, "ARTIST")) copy_utf8(t->artist, sizeof t->artist, val, (int)(vl < 255 ? vl : 255));
        else if (!strcmp(key, "ALBUM")) copy_utf8(t->album, sizeof t->album, val, (int)(vl < 255 ? vl : 255));
        else if (!strcmp(key, "TRACKNUMBER")) {
            char b[32] = {0};
            size_t k = vl < 31 ? vl : 31;
            memcpy(b, val, k);
            t->track = atoi(b);
        }
        p += cl; n -= cl;
    }
}

/* STREAMINFO body: min/max blocksize (2B each), min/max framesize (3B each),
   then sample rate 20b | channels-1 3b | bitsample-1 5b | total samples 36b
   (fields byte-aligned as libFLAC/ffmpeg write them: sr spans p[10..12]
   upper nibble, total samples spans p[13] lower nibble through p[17]).
   Returns seconds, 0 = unusable. */
static int flac_streaminfo_dur(const unsigned char *p, size_t n) {
    if (n < 18) return 0;
    unsigned sr = ((unsigned)p[10] << 12) | ((unsigned)p[11] << 4) | (p[12] >> 4);
    unsigned long long ns = ((unsigned long long)(p[13] & 0x0F) << 32) |
                            ((unsigned)p[14] << 24) | ((unsigned)p[15] << 16) |
                            ((unsigned)p[16] << 8) | p[17];
    if (!sr || !ns || ns / sr > 86400ULL) return 0;
    return (int)(ns / sr);
}

/* walk a FLAC metadata block chain (b points after the "fLaC" magic):
   4B header = last|type, 24-bit BE length. Fills tags from the VORBIS_COMMENT
   block (type 4), returns duration from STREAMINFO (type 0), 0 = unknown. */
static int flac_blocks(const unsigned char *b, size_t n, Tags *t) {
    size_t i = 0;
    int dur = 0;
    for (;;) {
        if (i + 4 > n) break;
        int last = b[i] & 0x80;
        unsigned type = b[i] & 0x7F;
        size_t blen = ((size_t)b[i + 1] << 16) | ((size_t)b[i + 2] << 8) | b[i + 3];
        i += 4;
        if (blen > n - i) break;
        if (type == 0) dur = flac_streaminfo_dur(b + i, blen);
        else if (type == 4) vorbis_comment(t, b + i, blen);
        i += blen;
        if (last || (dur && tags_complete(t))) break;
    }
    return dur;
}

/* parse Ogg comments from the head buffer. Vorbis carries the comment in a
   0x03 "vorbis" packet, Opus in "OpusTags" (identical framing after the
   magic); Ogg FLAC opens with 0x7F "FLAC" and then one metadata block per
   packet, same block types as native FLAC (ffmpeg additionally embeds the
   fLaC magic + first blocks inside the marker packet). Returns duration
   seconds (only FLAC can provide it here), 0 = unknown. */
static int read_ogg(const unsigned char *buf, size_t n, Tags *t) {
    size_t i = 0;
    unsigned char pkt[65536];
    size_t pn = 0;
    int flac = 0, dur = 0;
    while (i + 27 <= n) {
        if (memcmp(buf + i, "OggS", 4) != 0) { i++; continue; }
        unsigned char nseg = buf[i + 26];
        if (i + 27 + nseg > n) break;
        const unsigned char *seg = buf + i + 27;
        size_t page_body = i + 27 + nseg;
        size_t body = 0;
        for (unsigned s = 0; s < nseg; s++) body += seg[s];
        if (page_body + body > n) break;
        /* continue current packet with this page's segments */
        size_t bo = page_body;
        for (unsigned s = 0; s < nseg; s++) {
            size_t sl = seg[s];
            if (pn + sl <= sizeof pkt) { memcpy(pkt + pn, buf + bo, sl); pn += sl; }
            bo += sl;
            if (sl < 255) {
                /* packet complete */
                if (pn > 7 && pkt[0] == 0x03 && memcmp(pkt + 1, "vorbis", 6) == 0) {
                    vorbis_comment(t, pkt + 7, pn - 7);
                    return dur;
                } else if (pn >= 8 && memcmp(pkt, "OpusTags", 8) == 0) {
                    vorbis_comment(t, pkt + 8, pn - 8);
                    return dur;
                } else if (pn >= 5 && pkt[0] == 0x7F && memcmp(pkt + 1, "FLAC", 4) == 0) {
                    flac = 1;
                    /* marker = 0x7F "FLAC" + major + minor + 2B header-packet
                       count; ffmpeg embeds the fLaC magic + first blocks here */
                    if (pn >= 13 && memcmp(pkt + 9, "fLaC", 4) == 0)
                        dur = flac_blocks(pkt + 13, pn - 13, t);
                } else if (flac && pn >= 4) {
                    unsigned type = pkt[0] & 0x7F;
                    if (type == 0) dur = flac_streaminfo_dur(pkt + 4, pn - 4);
                    else if (type == 4) {
                        vorbis_comment(t, pkt + 4, pn - 4);
                        return dur;
                    }
                }
                pn = 0;
            }
        }
        i = page_body + body;
    }
    return dur;
}

/* RIFF stays FILE-based: LIST INFO usually sits after the data chunk, so
   the walk must be able to seek past audio the head buffer cannot hold */
static void read_riff(FILE *f, Tags *t) {
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12) return;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
    unsigned char ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        unsigned sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((unsigned)ch[7] << 24);
        /* 64-bit: on Windows long is 32-bit and a chunk size >= 0x80000000
           turned `next` negative, so the skip seek moved backwards, failed,
           and the loop re-read the same header forever. fseeko/off_t is
           64-bit on both targets (tag.c already uses fseeko above). */
        long long next = ((long long)sz + 1) & ~1LL;
        if (!memcmp(ch, "LIST", 4)) {
            /* a LIST chunk must carry a 4-byte form type: sz < 4 made the
               non-INFO branch fseeko back (next - 4 <= 0) and "re-sync"
               onto its own header by luck — refuse instead */
            if (sz < 4) return;
            unsigned char lt[4];
            if (fread(lt, 1, 4, f) != 4) return;
            if (!memcmp(lt, "INFO", 4)) {
                off_t end = ftello(f) + (off_t)sz - 4;
                while (ftello(f) + 8 <= end) {
                    unsigned char sh[8];
                    if (fread(sh, 1, 8, f) != 8) return;
                    unsigned ssz = sh[4] | (sh[5] << 8) | (sh[6] << 16) | ((unsigned)sh[7] << 24);
                    if (ssz > 65536) return;
                    unsigned char *sb = malloc(ssz);
                    if (!sb) return;
                    if (fread(sb, 1, ssz, f) != ssz) { free(sb); return; }
                    if (!memcmp(sh, "INAM", 4)) copy_legacy(t->title, sizeof t->title, sb, (int)(ssz < 255 ? ssz : 255));
                    else if (!memcmp(sh, "IART", 4)) copy_legacy(t->artist, sizeof t->artist, sb, (int)(ssz < 255 ? ssz : 255));
                    else if (!memcmp(sh, "IPRD", 4)) copy_legacy(t->album, sizeof t->album, sb, (int)(ssz < 255 ? ssz : 255));
                    else if (!memcmp(sh, "ITRK", 4)) {
                        char b[32] = {0};
                        size_t k = ssz < 31 ? ssz : 31;
                        memcpy(b, sb, k);
                        t->track = atoi(b);
                    }
                    free(sb);
                    if (ssz & 1) fgetc(f);
                }
                return;
            } else {
                fseeko(f, (off_t)(next - 4), SEEK_CUR);
            }
        } else {
            fseeko(f, (off_t)next, SEEK_CUR);
        }
    }
}

static void read_module(const unsigned char *h, size_t n, Tags *t) {
    static const char *modmag[] = { "M.K.", "M!K!", "M&K!", "N4C4", "FLT4", "FLT8",
                                    "4CHN", "6CHN", "8CHN", "16CH", "24CH", "32CH",
                                    "CD81", "OKTA", "FEST" };
    if (n < 4) return;
    if (!memcmp(h, "IMPM", 4) && n >= 0x0C + 26)
        copy_fixed(t->title, sizeof t->title, h + 0x0C, 26);
    else if (n >= 17 && !memcmp(h, "Extended Module: ", 17) && n >= 0x35 + 20)
        copy_fixed(t->title, sizeof t->title, h + 0x35, 20);
    else if (n >= 0x2C + 4 && !memcmp(h + 0x2C, "SCRM", 4))
        copy_fixed(t->title, sizeof t->title, h, 20);
    else if (n >= 1084)
        for (size_t i = 0; i < sizeof modmag / sizeof modmag[0]; i++)
            if (!memcmp(h + 1080, modmag[i], 4)) { copy_fixed(t->title, sizeof t->title, h, 20); break; }
}

/* Duration from the first MPEG frame header inside the head buffer:
   XING/Info frame count (exact for VBR), else VBRI, else a size-based
   estimate using the average bitrate measured over every complete frame
   in the head window (single frame only: that frame's bitrate). Never
   reads the whole file. Returns seconds, 0 = unknown. off = first byte
   after ID3v2 within this buffer (0 if none or buffer refilled);
   skip = file bytes preceding this buffer (0 unless the buffer was
   re-filled past an oversized ID3v2); fsize = size hint from the
   caller's stat (0 = stat via f when absent). */
static int mp3_head_duration(const unsigned char *b, size_t n, size_t off,
                             long long skip, long long fsize, FILE *f) {
    static const int br_v1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int br_v2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int sr_base[3] = {44100, 48000, 32000};
    size_t i = off;
    for (; i + 4 <= n; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        int ver = (b[i + 1] >> 3) & 3;    /* 3=MPEG1 2=MPEG2 0=MPEG2.5, 1=invalid */
        int layer = (b[i + 1] >> 1) & 3;  /* 1=Layer III */
        int bri = (b[i + 2] >> 4) & 0xF;
        int sri = (b[i + 2] >> 2) & 3;
        if (ver == 1 || layer != 1 || sri == 3 || bri == 0 || bri == 15) continue;
        int bitrate = (ver == 3 ? br_v1 : br_v2)[bri] * 1000;
        int rate = sr_base[sri] / (ver == 3 ? 1 : ver == 2 ? 2 : 4);
        int spf = ver == 3 ? 1152 : 576;
        size_t body = i + 4;
        /* XING/Info follows the frame header and side info; scan a small
           window instead of computing the exact side-info length */
        for (size_t k = body; k + 12 <= n && k < body + 96; k++) {
            /* markers are mixed-case: "Xing"/"Info" (VBRI below is all-caps
               by spec). the old all-caps compare never matched, so every
               VBR mp3 fell through to the CBR size estimate. */
            if (memcmp(b + k, "Xing", 4) && memcmp(b + k, "Info", 4)) continue;
            if (be32(b + k + 4) & 1) {
                unsigned frames = be32(b + k + 8);
                if (frames && (long long)frames * spf < (long long)rate * 86400)
                    return (int)((long long)frames * spf / rate);
            }
            break;
        }
        /* VBRI sits at a fixed offset: frame body + 32; frames at +12 */
        if (body + 48 <= n && !memcmp(b + body + 32, "VBRI", 4)) {
            unsigned frames = be32(b + body + 44);
            if (frames && (long long)frames * spf < (long long)rate * 86400)
                return (int)((long long)frames * spf / rate);
        }
        if (fsize <= 0 && fseeko(f, 0, SEEK_END) == 0) fsize = ftello(f);
        long long audbytes = fsize - skip - (long long)i;
        if (audbytes <= 0) return 0;
        /* No Xing/VBRI. A VBR stream without a tag often opens on a tiny
           silence frame (8 kbps over ~72 kbps average reads ~9x too long),
           so one frame's bitrate must not drive the estimate: walk every
           complete frame in the head window (L3 frame length =
           144000*kbps/rate for MPEG1, 72000*kbps/rate for MPEG2/2.5,
           plus the pad bit) and extrapolate the window's measured
           bytes-per-second to the rest of the file. The walk stops at the
           first non-frame byte; a lone parseable frame just reproduces
           the old single-bitrate estimate. */
        long long wb = 0, ws = 0;
        for (size_t p = i; p + 4 <= n; ) {
            if (b[p] != 0xFF || (b[p + 1] & 0xE0) != 0xE0) break;
            int v = (b[p + 1] >> 3) & 3;
            int l = (b[p + 1] >> 1) & 3;
            int br = (b[p + 2] >> 4) & 0xF;
            int sr = (b[p + 2] >> 2) & 3;
            if (v == 1 || l != 1 || sr == 3 || br == 0 || br == 15) break;
            int r = sr_base[sr] / (v == 3 ? 1 : v == 2 ? 2 : 4);
            if (r != rate) break;   /* mixed-rate stream: stop at the seam */
            int flen = (v == 3 ? 144000 : 72000) * (v == 3 ? br_v1 : br_v2)[br] / r
                       + ((b[p + 2] >> 1) & 1);
            if (flen < 4) break;
            wb += flen;
            ws += v == 3 ? 1152 : 576;
            p += (size_t)flen;
        }
        if (wb > 0) {
            long long dur = audbytes * ws / (wb * rate);
            if (dur > 0 && dur < 86400) return (int)dur;
        }
        return (int)(audbytes * 8 / bitrate);
    }
    return 0;
}

/* SPC/ID666 length field: 3 bytes at 0xA9, text digits ("180") or binary
   LE, sometimes ambiguous. Digits first, fall back to LE16, reject over
   0x1FFF seconds (GME's proven decode). decode.c reuses this on its
   slurped copy to size the play/fade window. */
int spc_tag_duration(const unsigned char *h, long n) {
    if (n < 0x100) return 0;
    int secs = 0;
    for (int i = 0; i < 3; i++) {
        unsigned d = (unsigned)h[0xA9 + i] - '0';
        if (d > 9) break;
        secs = secs * 10 + (int)d;
    }
    if (!secs || secs > 0x1FFF)
        secs = h[0xA9] | (h[0xAA] << 8);
    if (secs <= 0 || secs > 0x1FFF) return 0;
    return secs;
}

/* ID666 tag block of the SPC v0.30 header (fixed offsets). Returns
   duration in seconds (0 = unknown). */
static int read_spc(const unsigned char *b, long n, Tags *t) {
    if (n < 0x100) return 0;
    copy_fixed(t->title, sizeof t->title, b + 0x2E, 32);   /* song */
    copy_fixed(t->album, sizeof t->album, b + 0x4E, 32);   /* game */
    /* author at 0xB0: rippers sometimes prefix a marker byte (official
       SPC spec quirk, handled the same way by GME): skip it when the
       first byte is not text */
    int off = (b[0xB0] < ' ' || (b[0xB0] >= '0' && b[0xB0] <= '9')) ? 1 : 0;
    copy_fixed(t->artist, sizeof t->artist, b + 0xB0 + off, 32 - off);
    return spc_tag_duration(b, n);
}

int tag_probe(const char *path, long long size_hint, Tags *t) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *b = malloc(TAG_HEAD);
    if (!b) { fclose(f); return 0; }
    size_t n = fread(b, 1, TAG_HEAD, f);
    int dur = 0;
    if (n >= 3 && b[0]=='I' && b[1]=='D' && b[2]=='3') {
        size_t tagend = 0;
        if (n >= 10 && b[3] >= 2 && b[3] <= 4) {
            size_t sz = ((size_t)b[6] << 21) | ((size_t)b[7] << 14) |
                        ((size_t)b[8] << 7) | b[9];
            if (sz <= (1u << 24)) {
                if (10 + sz <= n) {
                    id3v2_frames(b + 10, sz, b[3], b[5], t);
                } else {
                    /* tag larger than the head buffer: one follow-up read */
                    unsigned char *tb = malloc(sz);
                    if (tb && fseek(f, 10, SEEK_SET) == 0 &&
                        fread(tb, 1, sz, f) == sz)
                        id3v2_frames(tb, sz, b[3], b[5], t);
                    free(tb);
                }
                tagend = 10 + sz;
            }
        }
        /* ID3v2 larger than the head window (big cover art) leaves no
           audio bytes in it: mp3_head_duration would scan past the
           buffer end and report 0:00 in the library. Re-fill the SAME
           buffer at the audio start — still one fixed 128KB read, one
           extra seek; tagend==0 (no/absurd v2 header) keeps the old
           whole-buffer scan. */
        long long skip = 0;
        if (tagend && tagend + 4 > n &&
            fseek(f, (off_t)tagend, SEEK_SET) == 0) {
            skip = (long long)tagend;   /* tag bytes precede the new window */
            n = fread(b, 1, TAG_HEAD, f);
            tagend = 0;
        }
        dur = mp3_head_duration(b, n, tagend, skip, size_hint, f);
        if (!tags_complete(t)) read_id3v1(f, t);
    } else if (n >= 4 && b[0]=='O' && b[1]=='g' && b[2]=='g' && b[3]=='S') {
        dur = read_ogg(b, n, t);
    } else if (n >= 4 && b[0]=='f' && b[1]=='L' && b[2]=='a' && b[3]=='C') {
        dur = flac_blocks(b + 4, n - 4, t);
    } else if (n >= 4 && b[0]=='R' && b[1]=='I' && b[2]=='F' && b[3]=='F') {
        fseek(f, 0, SEEK_SET);
        read_riff(f, t);
    } else if (n >= 27 && !memcmp(b, "SNES-SPC700 Sound File Data", 27)) {
        dur = read_spc(b, (long)n, t);
    } else {
        dur = mp3_head_duration(b, n, 0, 0, size_hint, f);
        if (!tags_complete(t)) read_id3v1(f, t);
        if (!t->title[0]) read_module(b, n, t);
    }
    free(b);
    fclose(f);
    return dur;
}

void tag_read(const char *path, Tags *t) {
    tag_probe(path, 0, t);
}
