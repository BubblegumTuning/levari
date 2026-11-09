# Vendored libav* public headers

Purpose: the ffmpeg backend (src/decode/ffab.c) must compile once per
supported libav MAJOR so its struct-field reads match the runtime
library's layout — see the comment at the top of ffab.c for the full
rationale. The system's libav headers are deliberately never used.

Contents: ONLY the public headers of libavutil, libavcodec and
libavformat, taken from the official upstream release tarballs:

    ff5/  ffmpeg 5.1.10   lavu 57 / lavc 59 / lavf 59
    ff6/  ffmpeg 6.1.6    lavu 58 / lavc 60 / lavf 60
    ff7/  ffmpeg 7.1.5    lavu 59 / lavc 61 / lavf 61
    ff8/  ffmpeg 8.1.3    lavu 60 / lavc 62 / lavf 62
    ff9/  ffmpeg 9.0.2    lavu 61 / lavc 63 / lavf 61

Upstream: https://ffmpeg.org/releases/ffmpeg-<ver>.tar.xz (sha256)

    5.1.10 392306d6fc45dab0e9e0ea55381e071842e83a2fb31d320aeda40477a7766293
    6.1.6  d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a
    7.1.5  de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
    8.1.3  7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
    9.0.2  8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

Integrity record (honest): the tarballs were fetched over TLS directly
from ffmpeg.org on 2026-11-07 (5.1.10/8.1.3 re-fetched 2026-11-07; the
9.0.2 sha256 was re-confirmed byte-identical on the second fetch) and
their sha256 computed at that time (the values above). Upstream GPG
signatures were downloaded but NOT
verified on the build host (no dirmngr / release key unavailable) — if
signature confirmation matters, verify .asc files against the ffmpeg
release key elsewhere. The extracted file lists are exactly the HEADERS
stanzas of each library's Makefile (public headers only), plus the
generated libavutil/avconfig.h, whose contents here match what ffmpeg
configure emits for our little-endian fast-unaligned targets (x86_64,
ARM, ARM64): AV_HAVE_BIGENDIAN 0, AV_HAVE_FAST_UNALIGNED 1.

License: LGPL-2.1-or-later (FFmpeg), headers carry the upstream notices
unmodified. We link nothing against them — the libraries are dlopen'd at
runtime; this matches the existing third-party attribution on the ABOUT
page ("ffmpeg (runtime, optional)").

Adding a major (the three touches, keep all in sync):
1. vendor/ffmpeg/ff<abi>/include — extract from the official tarball as above
2. LEVAR_FF_ABIS in CMakeLists.txt
3. LV_FFAPI_LIST in src/decode/ffabi.h
Then extend the matrix in test/ffabi/ and bump FF_SONAME_HI in
src/decode/ffload.h if the new major's soname exceeds it.

Regenerating: `tools/gen_vendor_ffmpeg.sh` (at the repo root) rebuilds all
five sets from the pinned tarballs above — it downloads each, verifies the
sha256, extracts exactly the HEADERS stanzas plus avconfig.h, and is proven
byte-exact against this tree by `tools/gen_vendor_ffmpeg.sh --check` (run it
after any pin change; a slim package may ship the script instead of these
files — then run the script once at the repo root before configuring).
