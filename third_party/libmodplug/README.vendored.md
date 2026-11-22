libmodplug 0.8.9.1+ (Konstanty/libmodplug master, 2026-10)

Public domain (see COPYING). Renders mod/s3m/xm/it and other module
formats to 44.1 kHz PCM; used by src/decode/decode.c.

Vendored files: src/{fastmix,load_it,load_mod,load_s3m,load_wav,load_xm,mmcmp,modplug,snd_dsp,snd_flt,snd_fx,sndfile,sndmix}.cpp,
src/{modplug,tables}.h, src/libmodplug/*.h, plus a hand-written config.h
for glibc/x86-64. Built with MODPLUG_BASIC_SUPPORT: the exotic loaders
(669/abc/amf/ams/dbm/dmf/dsm/far/j2b/mdl/med/mid/mt2/mtm/okt/pat/psm/
ptm/stm/ult/umx) are not vendored; such files fall back to the optional
libav* backend in levari's decoder.

Local-patch ledger: load_it.cpp and sndfile.cpp carry the four guard lines
of upstream commit e74d5d7 ("IT: clamp stereo unpack offset and bound block
header skip", 2026-11-02) — our 2026-11-02 snapshot predates that commit;
verified byte-identical to master for those hunks on 2026-11-15. NOT taken
from master: the later pPluginData reload fix in load_it.cpp (separate
upstream commit) — revisit on the next vendor refresh, together with a
full re-sync that would let these two ledgers drop.
load_it.cpp ALSO carries a LOCAL security guard NOT in upstream master
(re-apply after any refresh): four lines after the nVol* envelope rejects
in ITInstrToMPT dropping ENV_PAN{LOOP,SUSTAIN}/ENV_PITCH{LOOP,SUSTAIN}
when their file-provided BYTE indices reach 25 — ReadNote indexes the
32-entry PanPoints[]/PitchPoints[] with them and the high indices read
past the per-instrument allocation. Repro + verification in the adding
commit ("libmodplug: guard IT pan/pitch envelope loop and sustain
indices", 2026-11-21).
