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

#endif
