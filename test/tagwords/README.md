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
u8cut/u8cut2 (utf-8 cut at a char boundary in a fixed-width field),
v1cut/v1u8 (id3v1's 30 bytes cutting cp1251 / UTF-8 text mid-char),
bigc/bigl (two-word legacy fields: boundary adjacency rules),
l1w/v1w (windows-1252 bytes of TEXT: the 0x80-0x9F punctuation tag.c's
w1252 tier maps — the bytes where cp1251 and windows-1252 AGREE plus the
documented 0x88→euro exception; the latin-1 fallback must read them as
punctuation, never as C1 controls),
au8/au8t/au8cut/au8cut2 (the u8 shapes in a TPE1 frame instead of TIT2:
the ARTIST field is pinned; probe.c prints title and artist),
mf/vmf/obsmf (multi-frame block specs, SECTION 12: one encoding verdict
per tag block), mf22 (same spec emitted as ID3v2.2: 3-char ids TT2/TP1/
TAL and the COM comment frame), bulk (same as mf with 'ID*N' repetition
suffixes for the evidence-cap stress pins),
obs (ambiguous byte strings: reported, never asserted).

Current baselines (1187 pinned entries + 32 observe, 2026-11-03):
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
keeps 0.3.0 at 45 while taking 0.3.2 to 0 is the bar.

Rules:
- NEVER delete a failing line to get green. Fix the code, or move the line
  to the OBSERVE section with a comment if the input is genuinely ambiguous.
- Fixture bytes follow tag.c's cp1251_hi table EXACTLY (it maps 0xAF->Yi,
  0xB3->i, 0xB4->ge, 0x95->Ge — extensions the standard cp1251 codec
  lacks). Keep tagcheck.py's mirror table in sync if tag.c's changes.
- copy_utf8/copy_legacy trim trailing spaces by design; never pin a text
  with a trailing space.

Deep dive (one-shot sweeps, NOT part of the constant gate)
----------------------------------------------------------
divegen.py expands top-N frequency words into temporary corpora
(test-assets/corpus/dive-<lang>.txt, git-ignored) and tagcheck runs them
with --corpus. Sweep of 2026-11-02: fi/ja 5000 words + ru/uk/fr/de/no
1000 = 55,879 fixtures; found ~8.5k real failures, all in the UTF-8
lead-byte/truncation family (plus the Ukrainian "цієї" eaten by a fake
4-byte lead). Fixed classes are pinned in SECTION 10; the irreducible
residue (a word whose only multibyte char is cut at a field end — byte-
identical to the latin-1 accent) is documented there with counts.
Run:  python3 test/tagwords/divegen.py && for L in fi ja ru uk fr de no; do python3 test/tagwords/tagcheck.py --corpus test-assets/corpus/dive-$L.txt; done

kworb sweep 2026-11-02 (second one-shot, also NOT part of the gate): the
top-2500 streamed songs' TITLES AND ARTISTS from kworb.net/spotify/songs.html
(2413 unique titles, 1023 unique artists) expanded into 20,412 fixtures
including TPE1/id3v1-artist variants. Zero new decode bug classes — the
UTF-8 path was spotless; it exposed the windows-1252 tier instead: a lone
smart-quote byte (usually 0x92) in a legacy field decoded as a latin-1 C1
control (18/2413 titles carry one). The fix pins the 17 tier bytes — the
16 cp1251/w1252-agreed plus the documented exception 0x88→euro (cp1251's
€, common in titles; w1252's lone ˆ never occurs) — as l1w/v1w
(SECTION 11); the 85 matrix lines for those bytes moved
l1m->l1w with the contract, the disagreeing bytes keep the C1 reading and
stay l1m. The tool lived in a scratch dir (one-shot, corpus lists are not
kept in git); SECTION 11 documents the counts and artist-field proof.

Thanks
------
Word lists from Hermit Dave's FrequencyWords project
(github.com/hermitdave/FrequencyWords, mirrored on our Gitea as
AI/FrequencyWords; code MIT, content CC-by-sa-4.0 — the lists are NOT
kept in this repo, they are fetched and cached in the git-ignored
test-assets/corpus/, sha256-pinned in divegen.py). The large sweeps
against these lists are what surfaced the lead-byte decoding bugs
pinned in SECTION 10 — many thanks to the project's creator.
Real-world song title/artist shapes from kworb.net (top Spotify streams
tables) drove the 2026-11-02 sweep that produced the windows-1252 tier.
