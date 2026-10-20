# Usage

## Starting the player

    levari [flags] [files...]

Files may be audio (`.mp3 .ogg .oga .opus .flac .wav .mod .s3m .xm .it`,
plus `.aac .m4a .wma` when built with the optional libav* backend),
`.m3u` playlists (loaded into the playlist), or `.cue` sheets (virtual
tracks with INDEX offsets). The first file starts playing automatically.

Flags:

    --nopl             start with the playlist window hidden
    --eq               start with the equalizer window visible
    --vis              start with the spectrum visualization on
    --save out.m3u     save the playlist on exit
    --probe files...   print decoder info and exit (no UI)
    --selftest <sec>   run the UI for <sec> seconds, then exit and print
                       player state (used for automated checks)
    --dump <prefix>    save window screenshots <prefix>-main.bmp etc. on exit
    -h, --help         short usage line

## Daemon + remote control

The player listens on `$XDG_RUNTIME_DIR/levari-<uid>.sock`
(fallback `/tmp/levari-<uid>.sock`). A second `levari <command>` talks to
the running instance instead of starting a new one:

    play [files...]        with files: replace the playlist and play
                           (splits on spaces). Alone: resume, or play
                           the current track
    pause / resume / toggle
    stop / next / prev
    seek <sec> | seek +N   absolute or relative seek
    vol <0-128>
    volup / voldown        step volume by 8
    mute                   volume 0; run again to restore the previous volume
    pan <-32..32>          left/right balance
    speed <0.5-2.0>        WSOLA pitch-preserving tempo
    shuffle on|off
    repeat on|off|one
    eq on|off
    status                 current track, state, position
    pl                     print the playlist
    add <file>             append one path without playing (spaces kept)

## Config file

`$XDG_CONFIG_HOME/levari/config` (default `~/.config/levari/config`),
plain `key = value` lines, `#` comments. Edit by hand or through the UI
(right-click the main window's title bar → `OPTIONS...`).

    library.dir = ~/Music         repeatable; default ~/Music; used by `library scan`
    library.index =               index path override (default ~/.levari/index)
    libav.dir =                   dir to dlopen libav*.so from (ffmpeg)
    audio.backend =               "" auto | pulse | alsa | sdl | null
    audio.device =                ALSA device name (default "default")
    daemon.socket =               socket path override (default XDG)
    ui.playlist = on              startup window state (flags override)
    ui.eq = off
    ui.vis = off
    ui.search = off

A leading `~` in any path value expands to the home directory. In the
options window the library dirs field takes several directories as one
`:`-separated line; each dir is scanned recursively.

`libav.dir` points at the libav* **shared libraries** (libavformat/
libavcodec/libavutil .so files), not the ffmpeg binary; they are searched
in that directory before the system paths. Takes effect on restart, as
does `daemon.socket`.

## Media library

    levari library scan [dir]      index audio files recursively (tags +
                                   duration); no dir = every configured
                                   library.dir
    levari library list [filter]   list entries, optional substring filter
    levari library play [query]    send matching entries to the player

The index is a flat tab-separated file at `~/.levari/index`
(overridable via `library.index`).
It is also the data source for the UI search window's `LIB` scope.

The options window has a `SCAN` button (beside the library dirs field) that
runs an incremental scan in a background thread: it appends only files not
already in the index, using the dirs typed in the field without saving them.
A `TAGS` checkbox next to it toggles whether the scan also reads tags +
duration (on) or indexes filenames only (off, faster). A `CLEAR` button
deletes the index so the next scan rebuilds it. The CLI `library scan`
also skips files already in the index, and always reads tags.

## MCP server (optional build)

Build with `cmake -B build -DENABLE_MCP=ON` to get a second binary,
`levari-mcp`: a stdio MCP (Model Context Protocol) server that controls
the running player over the same daemon socket. The release build does
not include it.

Register it in any MCP client, e.g.:

    { "mcpServers": { "levari": { "command": "levari-mcp" } } }

Tools: `status`, `play`, `toggle`, `stop`, `next`, `prev`, `seek`,
`set_volume`, `volume_up`, `volume_down`, `mute`, `set_pan`, `set_speed`, `set_shuffle`, `set_repeat`,
`set_eq`, `playlist`, `add`, `library_scan`, `library_list`,
`library_play`. The player must already be running. `play` splits on
whitespace, so those paths must not contain spaces. CLI `add` keeps
spaces; the MCP `add` tool still rejects them.
