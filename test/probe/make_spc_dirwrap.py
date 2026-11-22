#!/usr/bin/env python3
"""Minimal reproducer for the snes_spc voice_V2 directory OOB (LEVARI PATCH hunk).

Mechanism (third_party/snes_spc/SPC_DSP.cpp, fast DSP): voice_V1 computes the
sample-directory entry address as t_dir * 0x100 + t_srcn * 4 UNMASKED. Both
operands are register bytes, so the pair (DIR=0xFF, SRCN=0xFF) yields 0x102FC
— past the SNES_SPC object's ram[0x10000] + padding2[0x100] (the padding is
sized for the echo pointer wrap, which IS masked at & 0xFFFF). voice_V2 reads
two bytes at that address on every voice clock, unconditionally — no SPC700
program, no key-on, no non-zero RAM is needed; one play frame is enough.
Result: heap-buffer-overflow up to 0x2FC past the 68K emulator object
(ASan; fuzz night runs 2026-11-14/15, three artifacts one signature).
Release builds read heap padding instead — garbage sample pointers, no trap.
Upstream snes_spc main is byte-identical and unfixed (EOL 0.9.0).

The file is the format's hard floor (spc_min_file_size = 0x10180):
  0x0000  signature (first 27 bytes checked by load_spc)
  0x0023  has_id666=0  0x0024 version=0  0x0025.. CPU regs all 0 (PC=0; the
          CPU harmlessly executes zero-opcodes while the DSP clocks run)
  0x002C  text[212] all zero
  0x0100  RAM, 64K of zero — never read by the crash, required by the layout
  0x10100 DSP image (indices are the SPC file image layout, see the r_*/v_*
          enums in SPC_DSP.h): all zero except
            0x04  voice 0 SRCN = 0xFF   (v_srcn)
            0x5D  DIR          = 0xFF   (r_dir)

Gate: write this file and replay it through the SPC decode path under ASan
(the harness pattern is open -> read -> frames of playback; see
third_party/snes_spc/README.vendored.md). Pre-fix it aborts with
heap-buffer-overflow at SPC_DSP.cpp voice_V2; post-fix it plays clean.

Usage: python3 test/probe/make_spc_dirwrap.py [out.spc]   (default: spc-dirwrap.spc)
"""
import sys

SIG = b"SNES-SPC700 Sound File Data v0.30\x1a\x1a"  # first 27 bytes are checked
hdr = bytearray(0x100)
hdr[0:len(SIG)] = SIG
hdr[0x23] = 0          # has_id666
hdr[0x24] = 0x00       # version
hdr[0x25:0x2C] = b"\x00" * 7  # pcl pch a x y psw sp(2)
ram = bytes(0x10000)
dsp = bytearray(0x80)
dsp[0x04] = 0xFF       # voice 0 SRCN (v_srcn)
dsp[0x5D] = 0xFF       # DIR (r_dir)
out = bytes(hdr) + ram + bytes(dsp)
assert len(out) == 0x10180, len(out)
name = sys.argv[1] if len(sys.argv) > 1 else "spc-dirwrap.spc"
open(name, "wb").write(out)
print(f"wrote {name} ({len(out)} bytes)")
