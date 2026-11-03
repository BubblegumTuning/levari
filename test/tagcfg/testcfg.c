/* unit test: tag.legacy_encoding config round-trip (parse, default, save).
   Run with a scratch HOME/XDG_CONFIG_HOME:
     gcc -std=gnu11 -Isrc -Isrc/config -Isrc/tag -o /tmp/tc test/tagcfg/testcfg.c src/config/config.c -lpthread
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "config.h"
#include "tag.h"

static int fails;
static void chk(const char *what, int got, int want) {
    if (got != want) { fails++; printf("FAIL %s: got %d want %d\n", what, got, want); }
    else printf("PASS %s\n", what);
}

int main(void) {
    cfg_load();
    chk("default is auto", cfg.tag_legacy_enc, LV_ENC_AUTO);

    char path[LV_MAX_PATH];
    cfg_path(path, sizeof path);
    char dir[LV_MAX_PATH];
    snprintf(dir, sizeof dir, "%s/levari", getenv("XDG_CONFIG_HOME"));
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (!f) { perror("config write"); return 1; }
    fputs("tag.legacy_encoding = cp1251\n", f);
    fclose(f);
    cfg_load();
    chk("parsed cp1251", cfg.tag_legacy_enc, LV_ENC_CP1251);

    cfg.tag_legacy_enc = LV_ENC_LATIN1;
    if (cfg_save() != 0) { printf("FAIL cfg_save\n"); return 1; }
    f = fopen(path, "r");
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    if (!strstr(buf, "tag.legacy_encoding = latin1")) {
        fails++; printf("FAIL saved file lacks the line\n");
    } else printf("PASS saved file carries the line\n");
    cfg_load();
    chk("reloaded latin1", cfg.tag_legacy_enc, LV_ENC_LATIN1);

    f = fopen(path, "w");
    fputs("tag.legacy_encoding = bogus\n", f); fclose(f);
    cfg_load();
    chk("bogus value keeps auto", cfg.tag_legacy_enc, LV_ENC_AUTO);

    printf(fails ? "FAILURES: %d\n" : "all config round-trip checks pass\n", fails);
    return fails != 0;
}
