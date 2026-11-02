# Threat model and hardening notes

Who/what levari trusts, and what each input path validates.

## Trust boundaries

**Everything in `$HOME` is trusted**, with one exception: the library
index is parsed with input-driven rigor (see Index file), because a
malware or multi-writer scenario could change it while the player runs.

**Untrusted by definition**: any file levari opens as media — decoders
(minimp3, stb_vorbis, dr_flac, dr_wav, dlopen'd opus/libav*, vendored
libmodplug and snes_spc), tag parsers (ID3v1/v2, Vorbis comments, RIFF
INFO, SPC/ID666, module headers), and m3u/CUE playlists. These parse
with bounded buffers; the vendored libs compile with the same hardening
as the first-party code (canaries, FORTIFY, visible warnings) so an
upstream regression cannot land silently.

## Daemon socket / MCP = same-user trust

The control socket is a same-user boundary only:

- bind under `umask 0077`, then `chmod 0600`;
- `SO_PEERCRED` checked on both the accept side and the client side
  (a pre-bound impostor in the world-writable `/tmp` fallback is
  refused, and the real instance publishes its random `/tmp` socket
  name in a pointer file inside the private config dir);
- Windows named pipe with a `D:(A;;GA;;;OW)` owner-only DACL;
- bounded 8 KB request lines on both ends; `SO_RCVTIMEO` /
  `SetCommTimeouts` so a silent client cannot park the service thread.

Local code running as the user can already read every file levari
owns, so the protocol adds no authentication beyond the uid check.

## Index file = parse with care

The index (`~/.levari/index` or `library.index`) is written by the
scanner but read by four loaders. Reads are hardened like tag parsing:

- rows longer than the reader's line buffer are drained and skipped
  (never re-parsed as a garbage row), and the scanner refuses paths
  whose escaped form cannot fit one row;
- `\t`, `\n` and `\\` are escaped in the path field (v2 header marks
  the format; legacy indexes are migrated, never unescaped);
- `unesc_field` carries the destination size as a parameter;
- tag fields from the index are control-stripped and utf8-trimmed on
  read before entering `Track.tags` — they flow to MPRIS (which also
  validates utf-8 itself), daemon `status`/`pl` output and m3u saves;
- allocation failures while loading skip the row instead of storing
  NULL fields that later strcmp/deref would crash on.

## Windows DLL search (packaging caveat)

`lv_dlopen` falls back to bare sonames (`libopus-0.dll`, `avcodec-*.dll`
...), which the default loader resolves **exe-directory first**. A
portable bundle therefore turns the exe directory into a DLL sink:
ship only the intended DLLs next to `levari.exe`, nothing else.

## Cross-thread state

Player display scalars (`pos_frames`, track bounds, rate, vol, ...) are
C11 `_Atomic` — read lock-free by the UI/daemon/MPRIS threads and torn
on 32-bit ARM as `volatile` before. `cfg` field mutation from the
daemon thread runs under the `cfg_save` mutex (`cfg_set_shuffle`);
`cur_pipe` (Windows shutdown) is swapped under a mutex. The playlist
itself stays under `Player.mtx`.

## Known-benign residues

- `scan_thread` (library scan) is detached and keeps touching statics
  after `ui_run` returns; process exit wins (all statics, no TLS).
- `srv_stop` may wait up to the 5 s `SO_RCVTIMEO` if a client read is
  in flight — shutdown stays bounded, not deterministic.
- `vol_prev` is owned by the daemon mute path (no UI unmute path
  exists); its atomics cover the cross-thread read.
