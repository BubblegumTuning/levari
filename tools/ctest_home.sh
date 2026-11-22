#!/bin/sh
# ctest_home.sh — the levari headless-rules wrapper for CTest: every test
# runs with a FRESH scratch HOME (the name must contain 'scratch': some
# harnesses guard strstr(HOME,"scratch")), XDG layout, audio.backend=null
# config, /tmp/scratch-p2 present (optenc fixture dir), cwd inside the
# scratch so stray fixtures never land in the tree, SDL_VIDEODRIVER=dummy.
# usage: ctest_home.sh [--scratch-arg | --test-arg] CMD [ARGS...]
#   --scratch-arg : append the scratch dir to the command (durclamp)
#   --test-arg    : append the repo test/ dir (pldrop fixtures)
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TAIL=""
case "$1" in
    --scratch-arg) TAIL=scratch ;;
    --test-arg)    TAIL=repo-test ;;
esac
case "$1" in --scratch-arg|--test-arg) shift ;; esac
[ "$1" = "--" ] && shift
[ $# -ge 1 ] || { echo "ctest_home.sh: no command" >&2; exit 2; }
S=$(mktemp -d /tmp/scratch-ctest-XXXXXX) || exit 2
mkdir -p "$S/xdg/levari" "$S/.levari" /tmp/scratch-p2
# some drivers keep their own scratch under ~/.hermes/cache/scratch
mkdir -p "$S/.hermes/cache/scratch"
printf 'audio.backend = null\ndaemon.socket = %s/levari.sock\n' "$S" > "$S/xdg/levari/config"
cd "$S" || exit 2
case "$TAIL" in
    scratch)   set -- "$@" "$S" ;;
    repo-test) set -- "$@" "$REPO/test" ;;
esac
HOME="$S" XDG_CONFIG_HOME="$S/xdg" XDG_RUNTIME_DIR="$S" SDL_VIDEODRIVER=dummy \
    exec "$@"
