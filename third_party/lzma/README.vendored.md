LZMA decoder, 7-Zip SDK (github.com/ip7z/7zip, C/ directory, master 2026-11-20)

Public domain (Igor Pavlov). Raw-LZMA1 decoder only: used by
src/skin/cjk_font.c to inflate the embedded glyph blob at first lookup
(LzmaDec_Allocate/Init/DecodeToBuf with our own ISzAlloc), no container,
no allocator pool, no BCJ filters. Era-safe plain C, no SIMD.

Vendored files: LzmaDec.{c,h}, 7zTypes.h, Compiler.h, Precomp.h (as
fetched from raw.githubusercontent master 2026-11-20; per-file pin:
LzmaDec.c   4e6ec665a6df01f1722e2b2fc36dc5e3aa7c3890e0f8cb11c6a07c9a01f3309b
7zTypes.h   a5d03b8cb65cd3a2a56bde46dd68fb356d2ebacd9d70283e7d4d20b4e0a64713
Compiler.h  d5e42be77d26beaa8c81d3e62ef99fc5685736cfc079779fdb9154d05fa8bc4a
LzmaDec.h   3aaf07b4ae4173a2d103179455dc7089b5ddbc7fc3db3c0e40964a7499c69266
Precomp.h   c8903f2e36a771a272d8981e1e9ffed5ba16ca3d2f3dca6d1414d2340a6829d0
). The encoder (LzmaEnc.*) is NOT vendored — streams are produced at
build time by python stdlib lzma in tools/gen_cjk_font.py (FORMAT_RAW,
filters LZMA1 preset 9|extreme, lc=3 lp=0 pb=0, dict 512 KiB), which is
the only producer of the bytes this decoder reads.

No local patches. Verify after any refresh: diff the five files against
upstream C/ (the sha256s above are the gate).
