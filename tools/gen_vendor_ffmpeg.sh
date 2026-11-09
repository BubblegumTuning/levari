#!/usr/bin/env bash
# Regenerate the vendored libav* public header sets in vendor/ffmpeg/ff<N>/
# from the pinned official FFmpeg release tarballs. The file list per lib is
# exactly the HEADERS stanza of that library's Makefile (the same rule the
# committed sets follow, validated byte-exact at 80002a5/fe865eb), plus the
# fixed little-endian libavutil/avconfig.h below.
#
#   tools/gen_vendor_ffmpeg.sh          # write vendor/ffmpeg/ff<N>/include
#   tools/gen_vendor_ffmpeg.sh --check  # regenerate to a temp dir and diff -r
#                                         # against the tree (touches nothing)
#
# A slim distribution (e.g. the GitHub package) can ship this script instead
# of the 624 header files; run it once at the repo root before configuring,
# and --check must stay green on this line so the slim tree builds identical
# variants.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD
V=vendor/ffmpeg

# Pinned majors — keep byte-identical to vendor/ffmpeg/README.md.
MAJORS="5 6 7 8 9"
ver_of()  { case $1 in 5) echo 5.1.10;; 6) echo 6.1.6;; 7) echo 7.1.5;; 8) echo 8.1.3;; 9) echo 9.0.2;; esac; }
sha_of()  { case $1 in
  5) echo 392306d6fc45dab0e9e0ea55381e071842e83a2fb31d320aeda40477a7766293;;
  6) echo d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a;;
  7) echo de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f;;
  8) echo 7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3;;
  9) echo 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e;;
esac; }

MODE=write
[[ ${1:-} == --check ]] && MODE=check

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for major in $MAJORS; do
  v=$(ver_of "$major"); sha=$(sha_of "$major")
  echo "==> ffmpeg $v (major $major)"
  tarf=$WORK/ffmpeg-$v.tar.xz
  curl -fsSL -o "$tarf" "https://ffmpeg.org/releases/ffmpeg-$v.tar.xz"
  echo "$sha  $tarf" | sha256sum -c - >/dev/null || { echo "sha256 MISMATCH for $v" >&2; exit 1; }
  src=$WORK/src-$v
  mkdir "$src"
  tar xJf "$tarf" -C "$src" --strip-components=1

  out=$V/ff$major/include
  [[ $MODE == check ]] && out=$WORK/ff$major/include
  rm -rf "${out:?}"
  mkdir -p "$out"

  for lib in libavutil libavcodec libavformat; do
    # Line-based stanza reader. TRAP (fe865eb): libavcodec's HEADERS stanza
    # ends with a backslash followed by an EMPTY line — a greedy
    # backslash+whitespace continuation join swallows the NEXT variable
    # (OBJS). A blank line always terminates the stanza; a non-blank line
    # without a trailing backslash terminates it after itself.
    awk '
      inb { handle($0); next }
      /^HEADERS[[:space:]]*=/ { sub(/^HEADERS[[:space:]]*=[[:space:]]*/, ""); handle($0); inb = 1; next }
      function handle(l) {
        if (l ~ /^[[:space:]]*$/) { inb = 0; return }
        if (l ~ /\\[[:space:]]*$/) { sub(/\\[[:space:]]*$/, "", l); emit(l); return }
        emit(l); inb = 0
      }
      function emit(l,  a, n, i) {
        n = split(l, a, /[[:space:]]+/)
        for (i = 1; i <= n; i++) if (a[i] ~ /\.h$/) print a[i]
      }
    ' "$src/$lib/Makefile" > "$WORK/headers-$v-$lib.txt"

    while IFS= read -r h; do
      mkdir -p "$out/$lib/$(dirname "$h")"
      cp "$src/$lib/$h" "$out/$lib/$h"
    done < "$WORK/headers-$v-$lib.txt"
  done

  # Generated-header stand-in, identical to what ffmpeg configure emits for
  # our little-endian fast-unaligned targets (x86_64, ARM, ARM64).
  mkdir -p "$out/libavutil"
  cat > "$out/libavutil/avconfig.h" <<'EOF'
/* Generated header stand-in for x86_64-linux-gnu gcc builds (identical
   to what ffmpeg configure emits there): endian + alignment facts only. */
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif
EOF

  if [[ $MODE == check ]]; then
    diff -r "$out" "$V/ff$major/include" || {
      echo "CHECK FAILED: ff$major regenerated set differs from the tree" >&2; exit 1; }
    echo "    ff$major byte-identical ($(find "$out" -type f | wc -l) files)"
  fi
done

if [[ $MODE == check ]]; then
  echo "CHECK OK: all regenerated sets byte-identical to $V"
else
  echo "wrote $V/ff{$(echo $MAJORS | tr ' ' ',')}/include from the pinned tarballs"
fi
