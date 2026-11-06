# Buttons

Every button is drawn by the skin and shows a pressed state while the
mouse button is held. Toggle buttons light up **green** when active.
Every window has an `X` close button in its top-right corner; on the
main window it quits the whole application, on the other windows it only
hides that window (its state is kept).

## Main window

### Transport (icons)

| Button | Action |
|---|---|
| `▶` Play | Stopped: start playback at the current track (first track if none is current). Paused: resume. |
| `❚❚` Pause | Toggle pause/play. |
| `■` Stop | Stop playback. With the options window's `FADE` toggle on (the default), a stop while playing first fades the volume to silence over 0.5 s; pressing stop again stops at once. Stopping while paused stops at once. |
| `\|◀` Prev | Jump to the previous track. |
| `▶\|` Next | Jump to the next track. |
| folder icon | Open the file browser window (same as the `o` hotkey or `O`). |

### Labeled buttons

| Button | Action |
|---|---|
| `SHUFFLE` | Toggle shuffle mode (green = on). Next/prev navigation then follows the shuffled order. With the options window's `SHUF` startup toggle on, the state is saved to the config and restored at startup. |
| `REPEAT` | Toggle playlist repeat (green = on); the last track wraps back to the first. |
| `EQ` | Show/hide the equalizer window. The label is green while EQ processing is enabled, regardless of window visibility. |
| `VIS` | Toggle the spectrum analyzer drawn over the scrolling title strip (green = on). |
| `LST` | Show/hide the playlist window. If the playlist is docked, it re-docks flush under the main window. |
| `SRCH` | Open/close the search window (green while open). |
| `RPT1` | Toggle repeat-one mode (green = on): the current track loops endlessly. `NEXT`/`PREV` still advance to another track (shuffle-aware) and the loop follows it. Mutually exclusive with `REPEAT`. |
| `A` | Mark **A**: set the A–B repeat start point at the current playback position (green while set). |
| `B` | Mark **B**: set the repeat end point — only takes effect if `A` is set and playback is already past it. Playback then loops A→B. Clicking `B` a second time clears both points. |

### Sliders

| Slider | Action |
|---|---|
| `VOL` | Click or drag the knob to set volume (0–128). Gain is quadratic: `(vol/128)²`, so the slider tracks perceived loudness better than linear (64 ≈ −12 dB). |
| `BIAS` | Click or drag to set left/right balance (−32..+32). |
| Seek bar | Click anywhere or drag the (double-width) knob to seek within the current track. For module files (mod/s3m/xm/it) seeking is approximate — it jumps to the nearest pattern order position. |

## Playlist window

| Button | Action |
|---|---|
| `SHUF` | Same shuffle toggle as the main window. |
| `RPT` | Same repeat toggle as the main window. |
| `ADD` | Open the file browser (same window as the main window's folder button). |
| `REM` | Remove the selected track(s) from the playlist (multi-selection included). If the current track is among them, playback stops first; the current-track pointer stays on the same music otherwise. `Ctrl+A` + `REM` clears the whole list. |
| `CLO` | Hide the playlist window. |
| `LIB` | Append every entry from the media library index to the playlist (the index built by the options window `SCAN` or `levari library scan`). Does not start playback. |
| `SCAN` | Re-probe every **selected** track (tags + duration) and update each library-index row in place — replaced if present, appended if the file was not indexed. The fresh values land on the playlist rows immediately, so newly tagged or mistagged files no longer need a full `CLEAR` + `SCAN`. With nothing selected, or in the ctrl-a select-all state (a whole-playlist probe could stall on network shares), or on a CUE virtual row, it does nothing. While a background scan runs it refuses (the title strip notes it). A failed row leaves its index entry untouched and is reported to stdout; one `cannot rescan` flash in red over the title strip for 10 s covers the whole batch; a real title change replaces the notice sooner. |
| `X` | Same as `CLO`. |

Track list: single click selects a track (white; the *playing* track is
green), double click plays it, `Ctrl+click` adds or removes one track from
the selection, `Shift+click` selects the whole range from the last clicked
track to the one under the cursor (the anchor keeps its place, so further
shift-clicks extend), and `Ctrl+A` selects everything. `Home`/`End` jump
the selection (and the view) to the first/last track; plain `Up`/`Down`
step the selection by one row (the view follows; `Alt+Up`/`Alt+Down` are
the row-move edit shortcuts, see the edit menu below). `Delete`/`Backspace`
removes the selection (multi included). The mouse wheel scrolls, and the
right edge has a draggable scrollbar.
The bottom-right corner is a resize grip; the width snaps to the main
window's width within 12 px.

Right-click anywhere in the playlist window opens the **edit menu**
(two columns; `Esc` or a click outside closes it, a click on an item
acts and closes):

| Menu item | Shortcut | Action |
|---|---|---|
| `SORT TITLE/ARTIST/ALBUM/TRACK/FILE` | — | Sort the whole playlist by that key (stable: equal keys keep their order; rows missing the tag sort last; album sorts by album, then track, then title). |
| `REVERSE` | — | Reverse the playlist. |
| `RANDOMIZE` | — | Shuffle (re-randomized on every press). |
| `CROP` | — | Keep only the selection, remove everything else. |
| `COPY` / `CUT` / `PASTE` | `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Internal row clipboard (survives focus, not a restart). Paste lands below a single selected row, or appends. |
| `MOVE UP` / `MOVE DOWN` | `Alt+Up` / `Alt+Down` | Move the selection past the rows around it. |
| `SAVE AS...` | `Ctrl+K` | Save the whole playlist as `.m3u` (opens the browser in SAVE mode, see below). |
| `CLEAR` | — | Stop and empty the playlist. |

The current track survives every edit: after a sort, shuffle or paste the
same *music* keeps playing (or playback stops when its row was cut,
cropped or cleared, exactly like `REM`). CUE albums move as one unit in
every operation above — a virtual track never gets split from its sheet.

Rows also drag directly for reordering: press a row and move past 4 px to
pick it up — a green hairline marks the gap it will land at (cursor in a
row's top half lands above it, bottom half below; dragging near the list's
top/bottom edge scrolls the view). Pressing inside a multi-row selection
carries the WHOLE selection (a non-adjacent set lands as one block);
pressing outside selects just that row first. The current track rides the
move like every other edit, and a drag never plays anything: the
double-click play defers to release precisely so a double-click that turns
into a drag eats the play instead of firing it.

Both the main and the playlist window accept files dragged from the
desktop: the dropped batch is appended to the playlist in arrival order
(a `.m3u`/`.m3u8`/`.pls` is expanded relative to its own folder, a `.cue`
is loaded as virtual tracks, a folder is added recursively picking up
audio files, any other file is appended as a path like the CLI `add`),
and playback starts at the first newly added track.
Drops on the other windows are ignored.

## Equalizer window

| Control | Action |
|---|---|
| `ON` | Enable/disable EQ processing (green = enabled). |
| Preamp slider | Leftmost slider; click or drag vertically, +12..−20 dB. |
| 10 band sliders | Same click/drag behavior, +12..−20 dB, bands at 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 Hz. |
| `FLAT` | Reset all 10 bands to 0 dB (preamp unchanged). |
| `ROCK` / `POP` / `JAZZ` | Apply the built-in band preset. |
| `RESET` | Reset all 10 bands to 0 dB (preamp unchanged) — beside the `ON`/`OFF` toggle at the lower right. |
| `X` | Hide the window; the EQ stays enabled/disabled as set. |

## Open browser window

| Control | Action |
|---|---|
| Path field (top row) | Editable twin of the current folder. Click to focus, type/paste a path, `Enter` to act on it (see below). While focused, the text after the last `/` (or `\`) live-filters the list by entry name, case-insensitive — like the song search. `Esc` restores the field to the current folder and clears the filter. |
| `Enter` in the path field | Valid folder: navigate into it. Valid audio file: queue it, start playing, close the browser. Anything else: plain red `path not valid` for ~1 s, then your typed text returns so you can fix it (the list stays put). |
| Paste in the path field | `Ctrl+V` sanitizes: trims surrounding whitespace/newlines, strips one pair of surrounding `"` quotes, drops control chars. `Ctrl+C`/`Ctrl+X` copy/cut the whole field (`Ctrl+A` first, like the options fields). |
| List entry (folder, white with trailing `/`) | Enter that folder. |
| `..` | Go up one directory (stays at `/` when already at the root). |
| List entry (file, gray) | Add that file to the end of the playlist, start playing it, and close the browser. |
| `ADD DIR` | Recursively queue every audio file in the currently shown directory, start playback at the first newly added track, and close the browser. |
| `X` | Close the browser. |

The current directory is shown along the bottom; a scrollbar appears on
the right when the list overflows. The browser opens in the folder of
the current track (or the working directory if nothing is loaded).
Typing in the field never triggers the global playback hotkeys.

On Windows, `..` from a drive root (`C:\`) opens the `COMPUTER`
pseudo-root: one entry per logical drive (`A:\`, `C:\`, ...), nothing
above it; the bottom readout shows `COMPUTER`. Unmapped network shares
(`\\server\share`) are reached by typing or pasting the path into the
field.

**SAVE mode** — opened by the playlist's `SAVE AS...` (`Ctrl+K`): the
title reads `SAVE PLAYLIST`, the list shows *every* file (no audio
filter) so you can aim at an existing `.m3u` to overwrite it, and
`ADD DIR` is hidden. `Enter` with a folder navigates; with anything else
it writes the playlist as `.m3u` at that path (a bare name lands in the
shown folder, a name without an extension gets `.m3u`), clicking a file
overwrites it directly, and the browser closes. Overwriting is silent
(the house precedent for destructive replace is `CLEAR`); an emptied
playlist refuses with a red title-strip message. `.pls` is loaded but
never written — `.m3u` with `#EXTINF` is the save format.

## Options window

Opened from the main window title-bar right-click (`OPTIONS...`). The
`BASIC` / `ADV` tab buttons (small tabs in the title bar, right of the
`OPTIONS` label) switch the page; `SAVE` writes
all fields and toggles to the config, `CLOSE`/`Esc` hides the window.

### BASIC page

| Control | Action |
|---|---|
| `LIBRARY DIRS` | `;`-separated list of scanned directories (hover for the tooltip). |
| `SCAN` | Scan the library dirs into the index (uses the `TAGS` checkbox). |
| `TAGS` | When on (green), SCAN reads tags+duration; off = filenames only. |
| `LIB INDEX` / `LIBAV DIR` / `AUDIO BACKEND` / `AUDIO DEVICE` / `DAEMON SOCKET` | Config text fields; `libav.dir` and the socket path apply on restart. |
| `CLEAR` | Delete the library index. |
| `STARTUP` toggles `UI PL` `UI EQ` `UI VIS` `UI SRCH` `SHUF` `FADE` | Startup window visibility; `SHUF` keeps the shuffle state across sessions; `FADE` fades volume out on stop (on by default). |

### ADV page

| Control | Action |
|---|---|
| `LARGE` | Allow ingest of files over 256 MB (green = on). Off by default: files over the cap are refused by the decoder with a message, protecting against OOM from the whole-file slurp decoders (MP3 seek table, module/Ogg-FLAC buffers). Toggling applies immediately (still press `SAVE` to persist); tag scanning is never capped. |
| `10FPS` | Keep repainting the main window at 10 fps whenever the title is long enough to marquee (green = on). Off by default: the marquee only scrolls while playing. On, it also scrolls while paused, stopped, or unfocused — at the cost of a steady 10 fps repaint. Toggling applies immediately (still press `SAVE` to persist). |
| `LEGACY` | 5-state **cycle button** (not a checkbox): the label shows the current setting — `AUTO`, `LATIN-1`, `CP1251`, `UTF-8` or `SHIFT-JIS`; each click cycles to the next. It forces the codepage for tag fields that carry no declared encoding (id3v2 `enc=0`/invalid marker, ID3v1, RIFF INFO): `AUTO` (default) decides per tag block from byte evidence (see `tag.legacy_encoding` in usage.md), the four forced values decode every such field through that reader. Applying takes effect immediately for future tag reads (no restart); rows already in the library index keep their old values — press `CLEAR` then `SCAN` to re-read. Press `SAVE` to persist. Hovering the button shows the same note. |

## Search window

| Control | Action |
|---|---|
| `PL` / `LIB` | Toggle the search scope: current playlist (`PL`) or the media library index (`LIB`, green). Library hits that are not in the playlist are inserted at the current position when jumped to. |
| Result row | Click to jump: it becomes the current track and starts playing if playback is active. |
| Scrollbar (right edge) | Press and drag to scroll the results (same mapping as the playlist's bar); the mouse wheel scrolls too. |
| `X` | Close the window (same as `Esc`). |

Up/Down arrows move the selection, `Enter` jumps to the selected (or
top) result. See [ui.md](ui.md) for the query syntax and edit-box keys.
