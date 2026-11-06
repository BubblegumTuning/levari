#ifndef LEVARI_TAG_H
#define LEVARI_TAG_H

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    int  track;
} Tags;

void tag_init(Tags *t);
/* fills whatever it can find; never fails hard */
void tag_read(const char *path, Tags *t);
/* tags + duration estimate in one open and one head read: MP3 duration
   comes from XING/VBRI or a CBR size/bitrate estimate (size_hint from the
   caller's stat, 0 = stat internally). Returns duration in seconds,
   0 = unknown (other formats need the decoder for that). */
int tag_probe(const char *path, long long size_hint, Tags *t);
/* SPC/ID666 length field from a >=0x100-byte header buffer; seconds,
   0 = unknown. decode.c reuses it on its slurped copy. */
int spc_tag_duration(const unsigned char *head, long n);
/* tag.legacy_encoding override for legacy 8-bit fields (id3v2 enc=0 or
   invalid marker, id3v1, RIFF INFO): LV_ENC_AUTO decides from block
   evidence; forced utf8 decodes utf-8 where valid and the guesser for
   the rest. Call after cfg_load (main.c) and on every options change;
   tag.c never reads cfg itself (probe.c builds it standalone). */
#define LV_ENC_AUTO   -1
#define LV_ENC_LATIN1  0
#define LV_ENC_CP1251  1
#define LV_ENC_UTF8    2
/* SHIFT-JIS/CP932 verdict for mislabelled legacy fields (see the SJIS
   corpus section in test/tagwords/words.txt). Added ahead of the tag.c
   reader so the tagwords probe builds; tag.c honors the value once the
   sjis classifier lands. */
#define LV_ENC_SJIS    3
void tag_set_legacy_default(int enc);

#endif
