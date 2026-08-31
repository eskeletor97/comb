# comb

A small terminal log viewer in plain C (libc only, no ncurses).

## Features

- severity highlighting (FATAL/PANIC/CRIT magenta, ERROR red,
  WARN yellow, DEBUG/TRACE dim)
- per-service colors: the syslog tag gets a pastel color, assigned by
  rotation in order of first appearance so adjacent services never share
  a color; works with or without hostname / [pid]
- structural tinting: dim timestamps/host/epoch stamps, colored verbosity
  tokens (<info>, <notice>), accented "quoted" values and (parenthesized)
  context — pure heuristics, no format config
- incremental `/` filter — literal substring by default, smart case
  (lowercase = case-insensitive); `^`/`$` anchor to line start/end.
  Ctrl-R in the prompt switches to POSIX ERE; Ctrl-V excludes matching
  lines instead (grep -v). The status bar shows (R)/(!) while regex
  mode is on or matches are excluded. In regex mode, simple alternations
  of plain literals (`error|warn`, `^foo`) run on the SIMD literal path
  and stay fast; anything fancier uses the libc engine
- highlight-only search: `\\` colors matches in place without narrowing;
  `n`/`N` jump between them (shifting the view to reveal off-screen
  matches), `|` clears, and the right-edge scrollbar marks hit rows
- live follow with `poll()`, handles file truncation/rotation
- `w` toggles soft line wrap (h/l/$ horizontal scrolling applies when
  wrap is off)
- mark lines with space (x unmarks), copy them all with c/y via
  OSC 52 (works over ssh); falls back to xclip / wl-copy / pbcopy /
  termux-clipboard-set when installed

## Build & install

    make
    make install        # to /usr/local/bin (PREFIX=... to change)

## Tests

    make check                                  # selftests + behavior assertions
    python3 tests/harness.py --check ./comb     # end-to-end assertions (pty)
    python3 tests/harness.py --compare OLD NEW  # byte-diff two builds

## Usage

    comb [-e REGEX] [-t N] [--no-color] [FILE]

`-t` / `--threads N` sets the worker-thread count used for parallel file
loading and for parallel filter/search scans. Defaults to `online CPUs - 2`,
clamped to a minimum of 1; `-t 1` disables threading (single-threaded load
and scan).

`--no-color` (or `-C`, or the `NO_COLOR` env var) strips syntax coloring but keeps inverse-video cursor/marks.

Reads FILE, or piped/redirected stdin (`dmesg | comb`, `comb < file`); the explicit `comb -` still works. When reading stdin, keys come from /dev/tty.
Pipe is followed live.

Plain text only: no systemd journal, no binary or compressed logs.
For those, pipe: `journalctl -o short-precise | comb`, `zcat error.log.gz | comb`.

## Keys

| key              | action                                    |
|------------------|-------------------------------------------|
| j/k/e, arrows     | next / previous line (sets sweep dir)     |
| Ctrl-d/u/f/b, PgDn/Up | page down / up                        |
| g/G, Home/End    | top / bottom                              |
| h/l, Left/Right  | scroll sideways (0 = home, $ = end)       |
| /                | filter (incremental literal, smart case); |
|                  | Up/Down/PgUp/PgDn scroll results live     |
| Ctrl-r (in prompt) | toggle literal / regex (ERE) filtering   |
| Ctrl-v (in prompt) | exclude matching lines instead (grep -v)  |
| \\                 | search: highlight matches, don't filter   |
| n / N             | next / previous search match              |
| |                 | clear the highlight search                |
| ?                | clear filter                              |
| Space / x        | mark / unmark line, sweep in last direction |
| c or y           | copy marked lines, else current line      |
| Enter            | scroll down (accepts filter in prompt)    |
| Esc              | leave the prompt (the typed query stays);   |
|                  | clear marks or filter; bail out of an     |
|                  | in-flight filter/search scan               |
| f                | toggle follow                             |
| w                | toggle line wrap                          |
| r                | reload file                               |
| Ctrl-z           | suspend (fg to resume)                    |
| Ctrl-l           | redraw screen                             |
| q, Ctrl-c        | quit                                      |

Change controls in the `keymap[]` table in `config.h`.

## License

0BSD (see LICENSE).
