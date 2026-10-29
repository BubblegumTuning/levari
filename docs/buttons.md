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
| `REM` | Remove the selected track from the playlist. If it was the current track, playback stops first. |
| `CLO` | Hide the playlist window. |
| `LIB` | Append every entry from the media library index to the playlist (the index built by the options window `SCAN` or `levari library scan`). Does not start playback. |
| `X` | Same as `CLO`. |

Track list: single click selects a track (green), double click plays it,
the mouse wheel scrolls, and the right edge has a draggable scrollbar.
The bottom-right corner is a resize grip; the width snaps to the main
window's width within 12 px.

## Equalizer window

| Control | Action |
|---|---|
| `ON` | Enable/disable EQ processing (green = enabled). |
| Preamp slider | Leftmost slider; click or drag vertically, +12..−20 dB. |
| 10 band sliders | Same click/drag behavior, +12..−20 dB, bands at 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 Hz. |
| `FLAT` | Reset all 10 bands to 0 dB (preamp unchanged). |
| `ROCK` / `POP` / `JAZZ` | Apply the built-in band preset. |
| `X` | Hide the window; the EQ stays enabled/disabled as set. |

## Open browser window

| Control | Action |
|---|---|
| List entry (folder, white with trailing `/`) | Enter that folder. |
| `..` | Go up one directory (stays at `/` when already at the root). |
| List entry (file, gray) | Add that file to the end of the playlist, start playing it, and close the browser. |
| `ADD DIR` | Recursively queue every audio file in the currently shown directory, start playback at the first newly added track, and close the browser. |
| `X` | Close the browser. |

The current directory is shown along the bottom; a scrollbar appears on
the right when the list overflows. The browser opens in the folder of
the current track (or the working directory if nothing is loaded).

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

## Search window

| Control | Action |
|---|---|
| `PL` / `LIB` | Toggle the search scope: current playlist (`PL`) or the media library index (`LIB`, green). Library hits that are not in the playlist are inserted at the current position when jumped to. |
| Result row | Click to jump: it becomes the current track and starts playing if playback is active. |
| `X` | Close the window (same as `Esc`). |

Up/Down arrows move the selection, `Enter` jumps to the selected (or
top) result. See [ui.md](ui.md) for the query syntax and edit-box keys.
