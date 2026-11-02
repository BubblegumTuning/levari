tagwords: tag-decoding regression corpus
========================================

Run (from anywhere):

    python3 test/tagwords/tagcheck.py                  # against worktree src/tag/tag.c
    python3 test/tagwords/tagcheck.py --tagc <dir>/tag.c   # baseline an older tag.c

Exit 0 iff every pinned entry decodes byte-exact. Fixtures (ID3v2.3 TIT2
enc=0 frames and ID3v1 tails) are generated into a temp dir; add --keep to
inspect them. probe.c is compiled against the tag.c under test with
-I alongside it, always from source (never a stale build object).

words.txt format: MODE<TAB>TEXT per line; expected decode is TEXT itself.
Modes: l1m (byte-trap matrix: every byte 0x80-0xFF at tail distance 0..4
inside an ascii word), l1 (real western words, latin-1 bytes), cyr
(russian/ukrainian words, cp1251 bytes per tag.c's own cp1251_hi table),
u8 (real UTF-8 payload in an enc=0 field — sloppy but common, must pass
through), u8t (valid UTF-8 with a truncated 4-byte tail — the clamp pins),
v1l/v1c (ID3v1 tail shape, NUL-padded: padding must neutralize the trap),
obs (ambiguous byte strings: reported, never asserted).

Current baselines (1117 pinned entries + 25 observe):
  v0.3.2 (tagged, pre-fix): 270 pinned failures
      l1m 144   every lead-class byte 0xC0-0xF7 at its trap distance
      l1  107   real words: café-type tails, pão/você/caffè/città/blå/kök...
      cyr  18   да не он мы вы я что тот как + Від від Київ він ніч дім хід...
      v1c   1   Від (id3v1 tail shape too)
  v0.3.0 (4a12dee): 45 pinned failures
      l1  38    the adjacency misreads (Hääyö, Grüße, Canções, ação, Größe...)
      v1l  3    same through id3v1
      u8t  4    emoji-tail clamp did not exist yet
  fixed worktree (2026-11-02): 0 pinned failures

The corpus therefore pins BOTH directions: everything 0.3.2 legitimately
fixed (adjacency latin-1, emoji clamp, strict-then-shape ordering controls
u8/u8t/v1) must stay green, and everything 0.3.2 regressed (truncation clamp
eating latin-1 tails AND cyrillic function words; the cp1251 letter-set gap
for i/Ye/Yi/Ge bytes 0x80-0xBF) must turn green. A classifier change that
keeps 0.3.0 at 45 while taking 0.3.2 to 0 is the bar — see
docs/fixlist-0.3.3.md item 2.

Rules:
- NEVER delete a failing line to get green. Fix the code, or move the line
  to the OBSERVE section with a comment if the input is genuinely ambiguous.
- Fixture bytes follow tag.c's cp1251_hi table EXACTLY (it maps 0xAF->Yi,
  0xB3->i, 0xB4->ge, 0x95->Ge — extensions the standard cp1251 codec
  lacks). Keep tagcheck.py's mirror table in sync if tag.c's changes.
- copy_utf8/copy_legacy trim trailing spaces by design; never pin a text
  with a trailing space.
