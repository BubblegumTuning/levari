# UI

For a per-button reference of every control in each window, see
[buttons.md](buttons.md).

## Windows

- **Main** — time box, scrolling title (CJK-capable), khz/kbps readout,
  VOL/BIAS sliders, seek slider, transport buttons, SHUFFLE/REPEAT/VIS
  toggles, A/B repeat points, `LST` (playlist), `SRCH` (search).
- **Playlist** — docked under main by default; drag its title bar away to
  undock, drop it near the main window's bottom edge to re-dock. Resize by
  dragging the bottom-right grip; width snaps to the main window's width
  within 12 px. Double-click a track to play it. `SHUF/RPT/ADD/REM/CLO`
  buttons, plus `LIB` which appends the whole media library index to the
  playlist.
- **Equalizer** (`EQ` button or `e`) — 10 bands + preamp, ON/OFF,
  FLAT/ROCK/POP/JAZZ presets. Opens docked flush above the main window,
  same width as it.
- **Open browser** (`O` button or `o`) — built-in file/folder picker;
  `ADD DIR` queues a whole directory recursively.
- **Search** (`SRCH` button) — see below.
- **Options** (right-click the main window's title bar → `OPTIONS...`) —
  opens docked flush to the main window's right edge, top-aligned, as wide
  as the main+playlist stack plus a margin for the `SCAN` button.
  Edits the config file (see [usage.md](usage.md)): library scan dirs
  `:`-separated, hover for the tooltip), library index path
  (default `~/.levari/index`), libav* directory, audio backend +
  device, daemon socket path, and startup window toggles
  (playlist/EQ/VIS/search).
  `SAVE` writes `~/.config/levari/config` immediately; audio backend/device
  take effect on the next track, libav dir and socket path on restart.
  `SCAN` (beside the library dirs field) indexes those dirs in a background
  thread, adding only files not already in the index (the dirs typed in the
  field are used, without saving them); the button reads `BUSY` while it
  runs and a status line below reports `added N (M seen)`. A `TAGS` checkbox
  next to it controls whether the scan also reads tags + duration (on) or
  just filenames (off, much faster). `CLEAR` removes the index file so the
  next `SCAN` rebuilds it from scratch.
  `Esc` or `CLOSE` hides the window.

## Search

Type to filter live. Query tokens (space-separated) must each appear as a
substring in title, artist, album, or filename, in any order. ASCII case
and fullwidth/halfwidth forms are folded; CJK matches as-is. Results are
ranked: title matches beat artist, beat album, beat filename; field-start
and word-start matches score higher.

- `PL`/`LIB` button toggles scope: current playlist or the media library
  index. Library hits not in the playlist are inserted at the current
  position when jumped to.
- Click a result, or press `Enter` to jump to the top (or arrow-selected)
  result: it becomes the current track; if playback is active it starts
  playing immediately.
- Edit box: full UTF-8/IME input; `Ctrl+A` selects all, then `Backspace`
  or typing replaces it. `Esc` closes the window.

## Window snapping

Dragging the EQ, file browser, search, or options window within 12 px of the main
window's left or right side snaps it flush against that side; if its top
is also within 12 px of the main window's top, the tops align. Dragging it
within 12 px above the main window (x aligned) snaps it flush on top. A
snapped window then moves together with the main window until dragged away.

Dragging the playlist gets the same live snap on the bottom side: within
12 px of the main window's bottom edge it snaps flush under it (x
aligning when close), and while docked it follows the main window;
dragging away undocks it.

The search window opens docked flush to the main window's left edge,
top-aligned, and is exactly as tall as the main+playlist stack, so the
three windows form one rectangle (it falls back to the right side when
there is no room on the left).

## Hotkeys

    x play   c pause   Space play/pause   v stop   b prev   n next
    s shuffle   r repeat   Ctrl+L rescan tags
    e equalizer   o open browser
    Left/Right seek ±5s   Up/Down volume
    Esc quit

While the search window has keyboard focus its keys go to the search box
instead (see Search); playback hotkeys work again once another window is
focused.
