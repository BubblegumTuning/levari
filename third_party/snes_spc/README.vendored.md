snes_spc 0.9.0 (blarggs-audio-libraries/snes_spc main, 2026-10)

LGPL 2.1 (see LICENSE). Full SNES SPC-700 APU emulator: loads and plays
SPC music files, outputs stereo 32 kHz PCM via the fast S-DSP core (the
one blargg recommends for music players); used by src/decode/decode.c
through the C interface in spc.h.

Vendored files: spc.{h,cpp} (C API), SNES_SPC{,_misc,_state}.cpp,
SNES_SPC.h, SPC_CPU.h, SPC_DSP.{h,cpp} (fast DSP), SPC_Filter.{h,cpp},
blargg_{common,config,endian,source}.h. The accurate DSP (dsp.{h,cpp}),
fast_dsp/ duplicate and demo/ are not vendored. No local patches.

The library ships no IPL ROM data (spc_init_rom is unused); like the
reference demo and GME, rips that execute from the ROM area are not
supported. levari's decode.c parses the SPC/ID666 header itself for
tags and length.
