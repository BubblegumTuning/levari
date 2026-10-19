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

#endif
