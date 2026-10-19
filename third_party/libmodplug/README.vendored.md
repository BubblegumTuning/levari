libmodplug 0.8.9.1+ (Konstanty/libmodplug master, 2024-06)

Public domain (see COPYING). Renders mod/s3m/xm/it and other module
formats to 44.1 kHz PCM; used by src/decode/decode.c.

Vendored files: src/{fastmix,load_it,load_mod,load_s3m,load_wav,load_xm,mmcmp,modplug,snd_dsp,snd_flt,snd_fx,sndfile,sndmix}.cpp,
src/{modplug,tables}.h, src/libmodplug/*.h, plus a hand-written config.h
for glibc/x86-64. Built with MODPLUG_BASIC_SUPPORT: the exotic loaders
(669/abc/amf/ams/dbm/dmf/dsm/far/j2b/mdl/med/mid/mt2/mtm/okt/pat/psm/
ptm/stm/ult/umx) are not vendored; such files fall back to the optional
libav* backend in levari's decoder.
