#!/usr/bin/env bash
# Fetch and verify the pinned libopus source tree into third_party/opus/.
#
# Why a fetch instead of a vendored commit (design ruling, 2026-11-24): the static
# opus core serves the WIN32 and APPLE flagship builds only — the Linux
# flagship/legacy/Pi/i386 and the Win98 era build stay on the dlopen'd
# system libopus — so the 21 MB upstream tree is not repo content and we
# do not ship it as-is. The repo carries only this pin.
#
#   tools/fetch_opus.sh           # write third_party/opus/ unless the stamp matches
#   tools/fetch_opus.sh --check   # verify stamp + sentinels; touch nothing
#
# CMake hard-fails on WIN32/APPLE configures when the tree is missing, the
# same discipline as vendor/ffmpeg's avformat.h check. Run this at the repo
# root before configuring those trees (mac112: in the synced lv-branchless
# copy; wintest01: wherever the tree travels — curl required).
#
# Pin provenance: sha256 cross-verified identical between downloads.xiph.org
# and the Debian pool opus_1.5.2.orig.tar.gz (2026-11-24). License: BSD-2
# (COPYING inside the fetched tree). Sync duty: this pin IS the vendor
# record — bump version+sha together; no cherry-pick of the tree, the whole
# tarball lands under third_party/opus/ (gitignored; only its CMake read
# set ever builds) so a sync diff against upstream is always trivial and
# no file is ever locally modified (project patch-marking rule).
set -euo pipefail

cd "$(dirname "$0")/.."
DEST=third_party/opus
URL=https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
SHA=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

SENTINELS=(CMakeLists.txt src/opus_decoder.c celt/celt_decoder.c
           silk/decode_frame.c include/opus_defines.h dnn/lpcnet_plc.c)

if [[ ${1:-} == --check ]]; then
    [[ -f $DEST/.lv-opus-stamp ]] || { echo "opus: $DEST not fetched"; exit 1; }
    [[ $(cat "$DEST/.lv-opus-stamp") == "$SHA" ]] || { echo "opus: stamp != pin"; exit 1; }
    for s in "${SENTINELS[@]}"; do
        [[ -f $DEST/$s ]] || { echo "opus: missing $s"; exit 1; }
    done
    echo "opus: --check green ($SHA)"
    exit 0
fi

if [[ -f $DEST/.lv-opus-stamp ]] && [[ $(cat "$DEST/.lv-opus-stamp") == "$SHA" ]]; then
    echo "opus: already at pin, nothing to do"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
T=$WORK/opus-1.5.2

if [[ -n ${LV_OPUS_TARBALL:-} ]]; then
    # offline mode: verified tarball travels over LAN (mac112 cannot reach
    # downloads.xiph.org; the sha gate below is the same, so trust is equal)
    echo "opus: using LV_OPUS_TARBALL=$LV_OPUS_TARBALL"
    cp "$LV_OPUS_TARBALL" "$WORK/t.tar.gz"
else
    echo "opus: fetching $URL"
    curl -fL --connect-timeout 15 --retry 3 -o "$WORK/t.tar.gz" "$URL"
fi
got=$(sha256_of "$WORK/t.tar.gz")
[[ $got == "$SHA" ]] || { echo "opus: sha256 MISMATCH got $got want $SHA"; exit 1; }

tar xzf "$WORK/t.tar.gz" -C "$WORK"
for s in "${SENTINELS[@]}"; do
    [[ -f $T/$s ]] || { echo "opus: tarball lacks $s"; exit 1; }
done
printf '%s\n' "$SHA" > "$T/.lv-opus-stamp"

mkdir -p "$(dirname "$DEST")"
if [[ -d $DEST ]]; then
    # stage the swap through WORK so the trap owns the cleanup (no rm -rf here)
    mv "$DEST" "$WORK/opus-stale"
fi
mv "$T" "$DEST"
echo "opus: wrote $DEST ($SHA)"
