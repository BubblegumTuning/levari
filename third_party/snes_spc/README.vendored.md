snes_spc 0.9.0 (blarggs-audio-libraries/snes_spc main, 2026-10)

LGPL 2.1 (see LICENSE). Full SNES SPC-700 APU emulator: loads and plays
SPC music files, outputs stereo 32 kHz PCM via the fast S-DSP core (the
one blargg recommends for music players); used by src/decode/decode.c
through the C interface in spc.h.

Vendored files: spc.{h,cpp} (C API), SNES_SPC{,_misc,_state}.cpp,
SNES_SPC.h, SPC_CPU.h, SPC_DSP.{h,cpp} (fast DSP), SPC_Filter.{h,cpp},
blargg_{common,config,endian,source}.h. The accurate DSP (dsp.{h,cpp}),
fast_dsp/ duplicate and demo/ are not vendored.

LOCAL PATCHES (verify/re-apply on any vendor refresh, like the stb_vorbis
hunks): SPC_DSP.cpp carries TWO marked local hunks — `LEVARI PATCH`
(voice_V1 sample-directory address mask, below) and `LEVARI PATCH
(PERFORMANCE)` (reciprocal-multiply read_counter, after the SECURITY PORTS
paragraph). Mask hunk: voice_V1 masks the sample-directory address to 16
bits. Upstream computes dir*0x100 + srcn*4
unmasked (max 0x102FC) while the object's padding2[0x100] absorbs only the
echo wrap; a crafted SPC drives the V2 sample-pointer read up to 0x2FC past
the SNES_SPC object (ASan heap-buffer-overflow, fuzz night runs 2026-11-14/
15; upstream main is byte-identical and unfixed, library EOL at 0.9.0).
Reproducer: test/probe/make_spc_dirwrap.py (DIR=0xFF+SRCN=0xFF, all else
zero); gate: replay under any ASan decode harness (~/fuzz-work/driver2
pattern) — must be clean.

SECURITY PORTS (2026-11-22, applied from test-assets/snesspc-levari-secfix.patch;
five fixes ported from libgme, the maintained fork — forms cross-checked
against the current upstream GME core, where STOP, the merged padding, the
$F-register filter and the X/Y clamps appear verbatim), touching
SNES_SPC.{h,cpp}, SNES_SPC_misc.cpp, SPC_CPU.h, SPC_DSP.cpp:
(1) 8-bit register clamps — SET_SP/GET_SP (the old `sp += 3` on RET1 could
push the stack pointer into RAM proper), X on MOV (X)+,A, Y on the multiply
and DA — closing the code-execution class a crafted .spc could reach;
(2) padding merge (libgme b2dc7f0) — m.ram is now `padding1[0x100]` then
`alignas(uint16_t) ram[0x10000 + 0x100]`; the separate padding2[] indexed
from RAM[] was undefined behaviour that ASan/UBSan correctly report as OOB.
This supersedes the padding2 layout the LEVARI paragraph above describes
(its 64K sample-directory mask stays required);
(3) STOP-hang defusal (libgme 775efcd) — the pre-fetch `data = *++pc` makes
GET_PC() read 0x10001 at a padding STOP; the old wrap branch then SET_PC(0)s
and loops the 64K walk forever. It now falls through and halts the core
(m.cpu_error). Pinned by testspcseek pin 6;
(4)+(5) DSP BRR header shifts — `(s * (1 << shift)) >> 1` and `(s >> 25) *
0x800` replace two signed left shifts (the vendored tree compiled emitting
four -Wshift-negative-value warnings at the register-filter lines removed
here; patched build compiles warning-free).
Valid files render bit-identically at the production flag string (-O3 … -Os
-flto); crafted fuzz files change behaviour exactly on the fixed paths
(early halt instead of endless garbage emulation). GATE TRAP: testspcseek
liveness programs must be an unconditional self-JMP $8000 (SPC700 0x5F —
0x80 is SETC and 0x4C is LSR abs here, unlike 6502), never placed at RAM 0:
the DSP echo engine with dir=0 zeroes low RAM ~170 ms in, and the old core
masked that overwrite with the wrap-loop this port deletes.

PERFORMANCE hunk: read_counter()'s `% counter_rates[rate]` becomes a 2^32
reciprocal multiply (`counter_magic` table — exact for every numerator below
2^16, verified exhaustively for all 32 rates; rate 1 stays a branch). Measured
2026-11-22 on real Pi 1 silicon (ARM1176 — no hardware divide) at the exact
shipping flag string (-O3 -DNDEBUG -Os -flto -fPIE -fstack-protector-strong
-D_FORTIFY_SOURCE=2 -mcpu=arm1176jzf-s -std=gnu++11), best-of-11 interleaved
rounds: -1.0..-1.9% on all four SPC fixtures, output FNV bit-identical to
shipped on the Pi and at x86 production flags.

The library ships no IPL ROM data (spc_init_rom is unused); like the
reference demo and GME, rips that execute from the ROM area are not
supported. levari's decode.c parses the SPC/ID666 header itself for
tags and length.
