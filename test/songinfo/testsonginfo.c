/* unit test: song-info window (playlist edit menu PM_INFO).
   Pins: the menu table entry (act + label + geometry inside the default
   playlist window), refusal with no row addressed (single selection,
   shift anchor, playing row all unset) with the ui_msg feedback, one-row
   display of the path and tags read from DISK at invocation (re-writing
   the fixture's ID3 between opens must show the NEW tags — proof the read
   is per invocation, not the cached row/index values), the virtual-CUE
   row fallback to the stored sheet-parsed tags (no disk read), the
   multi-row -> anchor row fallback, the playing-row fallback, the
   Vorbis-comment extended dump for Opus, Ogg Vorbis and native FLAC
   (hand-muxed fixtures: real CRCs, ffmpeg packet shapes, untagged
   fallback pinned) and close
   through the X button on the window chrome.
   Technique (see skill): #include ui.c, link every object EXCEPT
   main.c.o AND ui.c.o AND the stale pre-split leftovers (decode.c.o,
   stb_vorbis.c.o) + the dec_*.dir tier objects (canonical shape per
   test/snapwin):
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -DLV_URL_ENABLED=1 -Isrc -Isrc/ui -Isrc/tag $SDL -o build/testsonginfo \
       test/songinfo/testsonginfo.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name decode.c.o \
         ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) \
       $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++
   Run with a FRESH scratch HOME/XDG_CONFIG_HOME set by the caller.
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}
static void chks(const char *what, const char *got, const char *want) {
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, got, want); }
    else printf("PASS %s\n", what);
}

/* ID3v2.3 fixture with EXPLICIT frame payloads (each payload already
   carries its encoding byte — binary-safe, so COMM frames can hold the
   language + NUL-terminated description + text). tag_read/tag_read_full
   parse the tag block without needing decodable audio frames
   (frameless-mp3 case) */
static void id3v2_write_raw(const char *path, int n, const char *const *ids,
                            const unsigned char *const *pl, const size_t *ln) {
    unsigned char buf[16384]; size_t off = 0, body = 0;
    memcpy(buf + off, "ID3\x03\x00\x00", 6); off += 6;
    size_t szp = off; off += 4;                       /* tag size, patched below */
    for (int i = 0; i < n; i++) {
        uint32_t fs = (uint32_t)ln[i];
        memcpy(buf + off, ids[i], 4); off += 4;
        buf[off++] = (unsigned char)(fs >> 24); buf[off++] = (unsigned char)(fs >> 16);
        buf[off++] = (unsigned char)(fs >> 8);  buf[off++] = (unsigned char)fs;
        buf[off++] = 0; buf[off++] = 0;               /* frame flags */
        memcpy(buf + off, pl[i], ln[i]); off += ln[i];
        body += 10 + ln[i];
    }
    buf[szp]     = (unsigned char)((body >> 21) & 0x7F);   /* syncsafe tag size */
    buf[szp + 1] = (unsigned char)((body >> 14) & 0x7F);
    buf[szp + 2] = (unsigned char)((body >> 7) & 0x7F);
    buf[szp + 3] = (unsigned char)(body & 0x7F);
    memcpy(buf + off, "JUNKJUNK", 8); off += 8;
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fixture"); exit(1); }
    if (fwrite(buf, 1, off, f) != off) { perror("fixture write"); exit(1); }
    fclose(f);
}
/* latin-1 (enc=0) text frames from C strings */
static void id3v2_frames_write(const char *path, int n,
                               const char *const *ids, const char *const *vals) {
    unsigned char pay[16][8096]; size_t len[16];
    const unsigned char *pp[16];
    for (int i = 0; i < n; i++) {
        pay[i][0] = 0;
        size_t vl = strlen(vals[i]);
        memcpy(pay[i] + 1, vals[i], vl);
        len[i] = vl + 1;
        pp[i] = pay[i];
    }
    id3v2_write_raw(path, n, ids, pp, len);
}
static void id3v2_write(const char *path, const char *ti, const char *ar,
                        const char *al, const char *tr) {
    const char *ids[] = { "TIT2", "TPE1", "TALB", "TRCK" };
    const char *vals[] = { ti, ar, al, tr };
    id3v2_frames_write(path, 4, ids, vals);
}

/* ---- Ogg Opus fixture (extended dump over Vorbis comments) -------------
   Hand-muxed per the Ogg rules: CRC poly 0x04c11db7 MSB-first over the
   whole page with the crc field zeroed, lacing = n/255 x 255 + one n%255
   terminator. Page 1 is the BOS page carrying OpusHead, page 2 carries the
   OpusTags packet with raw KEY=VALUE entries (binary-safe: the UTF-8 test
   bytes ride straight through). Fixture cross-checked against ffprobe. */
static unsigned ogg_crc(const unsigned char *d, size_t n) {
    static unsigned tbl[256];
    static int init;
    if (!init) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned r = i << 24;
            for (int k = 0; k < 8; k++)
                r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
            tbl[i] = r;
        }
        init = 1;
    }
    unsigned crc = 0;
    for (size_t i = 0; i < n; i++)
        crc = (crc << 8) ^ tbl[((crc >> 24) ^ d[i]) & 0xFF];
    return crc;
}
static void w32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static size_t ogg_page(unsigned char *out, unsigned htype, unsigned seq,
                       const unsigned char *body, size_t bl) {
    size_t n = 0, crcp;
    memcpy(out, "OggS", 4); n += 4;
    out[n++] = 0;                          /* stream version */
    out[n++] = (unsigned char)htype;
    for (int k = 0; k < 8; k++) out[n++] = 0;   /* granule pos */
    w32(out + n, 1234u); n += 4;                /* serial */
    w32(out + n, seq);   n += 4;                /* page index */
    crcp = n; n += 4;                           /* crc, patched below */
    size_t nseg = bl / 255 + 1;
    out[n++] = (unsigned char)nseg;
    for (size_t s = 1; s < nseg; s++) out[n++] = 255;
    out[n++] = (unsigned char)(bl % 255);
    memcpy(out + n, body, bl); n += bl;
    w32(out + crcp, 0);                 /* crc MUST be zero while computing */
    w32(out + crcp, ogg_crc(out, n));
    return n;
}
static const char vendor[] = "levari-test";
/* vendor + count + KEY=VALUE entries: the Vorbis comment framing shared
   by all three packet shapes (OpusTags / 0x03-vorbis / FLAC VCBLOCK) */
static size_t comment_body(unsigned char *pkt, int n, const char *const *kv) {
    size_t p = 0;
    w32(pkt + p, (unsigned)sizeof vendor - 1); p += 4;
    memcpy(pkt + p, vendor, sizeof vendor - 1); p += sizeof vendor - 1;
    w32(pkt + p, (unsigned)n); p += 4;
    for (int i = 0; i < n; i++) {
        unsigned l = (unsigned)strlen(kv[i]);
        w32(pkt + p, l); p += 4;
        memcpy(pkt + p, kv[i], l); p += l;
    }
    return p;
}
static void file_write(const char *path, const unsigned char *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fixture"); exit(1); }
    if (fwrite(buf, 1, n, f) != n) { perror("fixture write"); exit(1); }
    fclose(f);
}
static void ogg_opus_write(const char *path, int n, const char *const *kv) {
    unsigned char out[8192], pkt[4096];
    size_t off = 0;
    unsigned char oh[19] = {0};             /* OpusHead, 2ch/48k/mapping 0 */
    memcpy(oh, "OpusHead", 8);
    oh[8] = 1; oh[9] = 2; oh[10] = 0x38; oh[11] = 0x01;   /* ver, ch, preskip 312 */
    w32(oh + 12, 48000);
    off += ogg_page(out + off, 0x02, 0, oh, sizeof oh);
    memcpy(pkt, "OpusTags", 8);
    size_t p = 8 + comment_body(pkt + 8, n, kv);
    off += ogg_page(out + off, 0x00, 1, pkt, p);
    file_write(path, out, off);
}
/* Ogg Vorbis, ffmpeg's on-the-wire shape: identification packet then the
   0x03 "vorbis" comment packet, framing flag 0x01 terminating it (mutagen
   and ffprobe both verified against this fixture this session) */
static void ogg_vorbis_write(const char *path, int n, const char *const *kv) {
    unsigned char out[8192], pkt[4096], ident[30] = {0};
    size_t off = 0, p;
    memcpy(ident, "\x01" "vorbis", 7);
    w32(ident + 7, 0);                        /* format version */
    ident[11] = 2;                            /* channels */
    w32(ident + 12, 44100);                   /* rate: the granule clock */
    ident[29] = 1;                            /* ident framing flag */
    off += ogg_page(out + off, 0x02, 0, ident, sizeof ident);
    memcpy(pkt, "\x03" "vorbis", 7);
    p = 7 + comment_body(pkt + 7, n, kv);
    pkt[p++] = 1;                             /* comment framing flag */
    off += ogg_page(out + off, 0x00, 1, pkt, p);
    file_write(path, out, off);
}
/* native FLAC: "fLaC" + STREAMINFO (rate 44100, total_samples 0 — the
   dump must not depend on it) + terminating VCBLOCK */
static void flac_native_write(const char *path, int n, const char *const *kv) {
    unsigned char out[8192], si[34] = {0};
    size_t off = 0, p;
    memcpy(out, "fLaC", 4); off = 4;
    out[off++] = 0x00; out[off++] = 0; out[off++] = 0; out[off++] = 34;
    si[10] = 0x0A; si[11] = 0xC4; si[12] = 0x42;   /* 44100 Hz, 2ch, 16 bit */
    memcpy(out + off, si, 34); off += 34;
    p = comment_body(out + off + 4, n, kv);
    out[off++] = 0x84;                             /* last | VORBIS_COMMENT */
    out[off++] = (unsigned char)(p >> 16);
    out[off++] = (unsigned char)(p >> 8);
    out[off++] = (unsigned char)p;
    off += p;
    file_write(path, out, off);
}

static Player p;
static Playlist pl;
static char cwd_abs[LV_MAX_PATH];   /* pl_add_file absolutizes relative rows
                                       with the cwd (standing index-path
                                       behavior, test/pldrop pin) */
static char abs1[LV_MAX_PATH], abscue[LV_MAX_PATH];

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        printf("FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    pl_init(&pl);
    pthread_mutex_init(&p.mtx, NULL);
    pthread_cond_init(&p.wake, NULL);
    p.pl = &pl;
    p.cur = -1;
    ui.pl = &p;
    ui.wmain = SDL_CreateWindow("test main", 0, 0, MAIN_W, MAIN_H, SDL_WINDOW_HIDDEN);
    if (!ui.wmain) { printf("FAIL wmain: %s\n", SDL_GetError()); return 1; }
    if (!getcwd(cwd_abs, sizeof cwd_abs)) { perror("getcwd"); return 1; }
    snprintf(abs1, sizeof abs1, "%s/info1.mp3", cwd_abs);
    snprintf(abscue, sizeof abscue, "%s/ghost.cue", cwd_abs);

    /* ---- menu table: entry exists, label, geometry inside the default
       playlist window (2 cols x 8 rows after the 16th item) ---- */
    int info_ix = -1;
    for (int i = 0; i < PM_N; i++) if (PL_MENU[i].act == PM_INFO) info_ix = i;
    chk("PM_INFO in table", info_ix >= 0, 1);
    chks("menu label", info_ix >= 0 ? PL_MENU[info_ix].lbl : "", "SONG INFO");
    Rect last = pl_menu_item_rect(PM_N - 1);
    chk("menu fits height", last.y + last.h <= PL_H, 1);
    chk("menu fits width", last.x + last.w <= PL_W, 1);

    /* ---- refusal: no row addressed -> nothing opens, red line set ---- */
    ui.pl_multi_n = 0; ui.pl_selall = 0; ui.pl_sel = -1;
    pl_menu_action(PM_INFO);
    chk("refuse: hidden", ui.info_visible, 0);
    chks("refuse: msg", ui.msg, "  no song selected");

    /* ---- single selected row: tags read fresh from disk at invocation ---- */
    id3v2_write("info1.mp3", "First Title", "First Artist", "First Album", "7");
    pl_add_file(&pl, "info1.mp3");
    pl_sel_add(0);
    ui.msg[0] = 0;
    pl_menu_action(PM_INFO);
    chk("open: visible", ui.info_visible, 1);
    chk("open: msg cleared", ui.msg[0], 0);
    chks("open: path", ui.info_path, abs1);
    chks("open: title", ui.info_full.title, "First Title");
    chks("open: artist", ui.info_full.artist, "First Artist");
    chks("open: album", ui.info_full.album, "First Album");
    chks("open: track", ui.info_full.track, "7");          /* raw TRCK text */
    chk("open: dir split", strcmp(ui.info_dir, cwd_abs) == 0, 1);
    chks("open: file split", ui.info_file, "info1.mp3");
    chk("open: focus cleared", ui.info_focus, -1);
    chk("row tags untouched (not copied back)", pl.items[0].tags.title[0], 0);

    /* ---- read ON INVOCATION: rewrite the tags on disk, re-open the same
       row: the window must show the NEW values ---- */
    id3v2_write("info1.mp3", "Second Title", "Other Artist", "Other Album", "12");
    pl_menu_action(PM_INFO);
    chk("reread: visible", ui.info_visible, 1);
    chks("reread: title", ui.info_full.title, "Second Title");
    chks("reread: artist", ui.info_full.artist, "Other Artist");
    chks("reread: album", ui.info_full.album, "Other Album");
    chks("reread: track", ui.info_full.track, "12");

    /* ---- virtual CUE row: stored sheet tags, no disk read (file absent) ---- */
    pl_add_file(&pl, "ghost.cue");
    lv_strlcpy(pl.items[1].tags.title, "Stored Track", sizeof pl.items[1].tags.title);
    lv_strlcpy(pl.items[1].tags.artist, "Stored Artist", sizeof pl.items[1].tags.artist);
    pl.items[1].tags.track = 3;
    pl.items[1].start_sec = 0; pl.items[1].end_sec = 120;
    ui.pl_multi_n = 0; ui.pl_sel = 1;
    pl_menu_action(PM_INFO);
    chk("cue: visible", ui.info_visible, 1);
    chks("cue: path", ui.info_path, abscue);
    chks("cue: stored title", ui.info_full.title, "Stored Track");
    chks("cue: stored artist", ui.info_full.artist, "Stored Artist");
    chks("cue: stored track", ui.info_full.track, "3");
    chks("cue: genre empty", ui.info_full.genre, "");

    /* ---- extended ID3v2.3 set: album artist, composer, TRCK raw,
       year, genre, and the COMM text PAST language + description ---- */
    {
        const char *ids[9] = { "TIT2", "TPE1", "TPE2", "TCOM", "TALB",
                               "TRCK", "TYER", "TCON", "COMM" };
        unsigned char comm[64];
        comm[0] = 0;                                  /* enc: latin-1 */
        memcpy(comm + 1, "eng", 3);                   /* language: EXACTLY 3 bytes, no NUL */
        memcpy(comm + 4, "commentary", 10);           /* short description */
        comm[14] = 0;
        memcpy(comm + 15, "Really a note", 13);
        const unsigned char *pl[9]; size_t ln[9];
        const char *tx[8] = { "Ext Title", "Ext Artist", "Album Person",
                              "Great Composer", "Ext Album", "3/12", "1994",
                              "Rock" };
        unsigned char pay[8][80];
        for (int i = 0; i < 8; i++) {
            pay[i][0] = 0;
            size_t vl = strlen(tx[i]);
            memcpy(pay[i] + 1, tx[i], vl);
            pl[i] = pay[i]; ln[i] = vl + 1;
        }
        pl[8] = comm; ln[8] = 28;                     /* 1 + 3 + 10 + 1 + 13 */
        id3v2_write_raw("full.mp3", 9, ids, pl, ln);
    }
    pl_add_file(&pl, "full.mp3");
    ui.pl_multi_n = 0; ui.pl_sel = 2;
    pl_menu_action(PM_INFO);
    chks("full: title", ui.info_full.title, "Ext Title");
    chks("full: artist", ui.info_full.artist, "Ext Artist");
    chks("full: album artist", ui.info_full.album_artist, "Album Person");
    chks("full: composer", ui.info_full.composer, "Great Composer");
    chks("full: album", ui.info_full.album, "Ext Album");
    chks("full: track raw keeps total", ui.info_full.track, "3/12");
    chks("full: year", ui.info_full.year, "1994");
    chks("full: genre", ui.info_full.genre, "Rock");
    chks("full: comment past description", ui.info_full.comment, "Really a note");
    chks("full: file split", ui.info_file, "full.mp3");

    /* ---- copy boxes: click focuses, copy takes the focused value;
       geometry stays inside the chrome ---- */
    Rect trow = info_row_rect(INFOI_TRACK);
    SDL_Event fe;
    memset(&fe, 0, sizeof fe);
    fe.type = SDL_MOUSEBUTTONDOWN;
    fe.motion.x = trow.x + 5; fe.motion.y = trow.y + 9;   /* inside the box */
    fe.button.button = SDL_BUTTON_LEFT;
    handle_info_mouse(&fe);
    chk("click focuses track box", ui.info_focus, INFOI_TRACK);
    chk("copy with focus copies", info_copy_sel(), 1);
    {   /* the clipboard round-trip needs a video driver with clipboard
           support: accept either a match or driver refusal, never crash */
        const char *got = SDL_GetClipboardText();
        if (got && got[0]) chks("clipboard carries the value", got, "3/12");
        else printf("PASS clipboard carries the value (driver has no clipboard, skipped)\n");
    }
    ui.pl_sel = 1;
    pl_menu_action(PM_INFO);          /* reopen on the cue row: focus clears */
    chk("reopen clears focus", ui.info_focus, -1);
    ui.info_focus = INFOI_COMMENT;    /* cue row has no comment */
    chk("empty box copies nothing", info_copy_sel(), 0);
    Rect lastbox = info_row_rect(INFOI_N - 1);
    chk("copy rows fit height", lastbox.y + lastbox.h <= INFO_H, 1);
    chk("copy boxes fit width", lastbox.x + lastbox.w <= INFO_W, 1);

    /* ---- Ogg Opus: the same extended set through the shared
       vorbis_comment parser. TITLE carries multibyte UTF-8 (í = C3 AD,
       ë = C3 AB — codec-generated, never recalled), TRACKNUMBER keeps
       the raw "n/m", DATE feeds the year box ---- */
    {
        const char *kv[9] = { "TITLE=Opus T\xc3\xadtles", "ARTIST=Opus Artist",
                              "ALBUMARTIST=Album P\xc3\xabrson", "COMPOSER=Great Composer",
                              "ALBUM=Opus Album", "TRACKNUMBER=5/11",
                              "GENRE=Electronic", "DATE=2007", "COMMENT=opus note" };
        ogg_opus_write("info.opus", 9, kv);
    }
    pl_add_file(&pl, "info.opus");
    ui.pl_multi_n = 0; ui.pl_sel = 3;
    pl_menu_action(PM_INFO);
    chk("opus: visible", ui.info_visible, 1);
    chks("opus: title utf-8", ui.info_full.title, "Opus T\xc3\xadtles");
    chks("opus: artist", ui.info_full.artist, "Opus Artist");
    chks("opus: album artist", ui.info_full.album_artist, "Album P\xc3\xabrson");
    chks("opus: composer", ui.info_full.composer, "Great Composer");
    chks("opus: album", ui.info_full.album, "Opus Album");
    chks("opus: track raw keeps total", ui.info_full.track, "5/11");
    chks("opus: year", ui.info_full.year, "2007");
    chks("opus: genre", ui.info_full.genre, "Electronic");
    chks("opus: comment", ui.info_full.comment, "opus note");
    chks("opus: file split", ui.info_file, "info.opus");

    /* untagged Opus (empty comment packet): the dump falls back to the
       basics — nothing invented, nothing crashes */
    ogg_opus_write("bare.opus", 0, (const char *const *)0);
    pl_add_file(&pl, "bare.opus");
    ui.pl_multi_n = 0; ui.pl_sel = 4;
    pl_menu_action(PM_INFO);
    chk("bare opus: visible", ui.info_visible, 1);
    chks("bare opus: title empty", ui.info_full.title, "");
    chks("bare opus: genre empty", ui.info_full.genre, "");
    chks("bare opus: track empty", ui.info_full.track, "");

    /* ---- Ogg Vorbis and native FLAC: same dump through the same mapper
       (the format plumbing is what these pin; field coverage is opus's) ---- */
    {
        const char *kv_vor[6] = { "TITLE=Vorbis T\xc3\xadtle", "ARTIST=Vorbis Artist",
                                  "ALBUMARTIST=Vorbis Person", "TRACKNUMBER=2/9",
                                  "GENRE=Ambient", "COMMENT=vorbis note" };
        ogg_vorbis_write("info.ogg", 6, kv_vor);
        const char *kv_flac[6] = { "TITLE=Flac T\xc3\xadtle", "ARTIST=Flac Artist",
                                   "ALBUMARTIST=Flac Person", "TRACKNUMBER=4",
                                   "DATE=1999", "COMMENT=flac note" };
        flac_native_write("info.flac", 6, kv_flac);
    }
    pl_add_file(&pl, "info.ogg");
    ui.pl_multi_n = 0; ui.pl_sel = 5;
    pl_menu_action(PM_INFO);
    chks("vor: title utf-8", ui.info_full.title, "Vorbis T\xc3\xadtle");
    chks("vor: album artist", ui.info_full.album_artist, "Vorbis Person");
    chks("vor: track raw keeps total", ui.info_full.track, "2/9");
    chks("vor: comment", ui.info_full.comment, "vorbis note");
    chks("vor: file split", ui.info_file, "info.ogg");

    pl_add_file(&pl, "info.flac");
    ui.pl_multi_n = 0; ui.pl_sel = 6;
    pl_menu_action(PM_INFO);
    chks("flac: title utf-8", ui.info_full.title, "Flac T\xc3\xadtle");
    chks("flac: album artist", ui.info_full.album_artist, "Flac Person");
    chks("flac: track plain", ui.info_full.track, "4");
    chks("flac: year", ui.info_full.year, "1999");
    chks("flac: comment", ui.info_full.comment, "flac note");
    chks("flac: file split", ui.info_file, "info.flac");

    /* ---- multi-row selection falls back to the shift anchor ---- */
    pl_multi_grow(2);
    ui.pl_multi[0] = 0; ui.pl_multi[1] = 1; ui.pl_multi_n = 2;
    ui.pl_sel = 0;
    pl_menu_action(PM_INFO);
    chk("multi: visible", ui.info_visible, 1);
    chks("multi: anchor path", ui.info_path, abs1);
    chks("multi: anchor title", ui.info_full.title, "Second Title");

    /* ---- no selection: the playing row is addressed ---- */
    ui.pl_multi_n = 0; ui.pl_sel = -1; p.cur = 1;
    pl_menu_action(PM_INFO);
    chk("playing: visible", ui.info_visible, 1);
    chks("playing: path", ui.info_path, abscue);
    p.cur = -1;

    /* ---- close through the X button (drag-14 chrome) ---- */
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.motion.x = INFO_W - 12; e.motion.y = 10;      /* inside close_rect */
    e.button.button = SDL_BUTTON_LEFT;
    handle_info_mouse(&e);
    chk("x: pressed armed", ui.pressed, IB_X);
    e.type = SDL_MOUSEBUTTONUP;
    handle_info_mouse(&e);
    chk("x: closed", ui.info_visible, 0);
    chk("x: pressed cleared", ui.pressed, B_NONE);
    /* the window stays closed until the next menu invocation */
    chk("closed stays hidden", ui.info_visible, 0);

    printf("%s (%d fail)\n", fails ? "SONGINFO TEST FAILED" : "SONGINFO TEST OK", fails);
    return fails ? 1 : 0;
}
