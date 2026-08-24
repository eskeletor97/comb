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
  Ctrl-R in the prompt switches to POSIX ERE; the status bar shows (R)
  while regex mode is on
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

    python3 tests/harness.py --check            # behavior assertions
    python3 tests/harness.py --compare OLD NEW  # byte-diff two builds

## Usage

    comb [-e REGEX] [--no-color] [FILE]

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
| /                | filter (incremental literal, smart case)  |
| Ctrl-r (in prompt) | toggle literal / regex (ERE) filtering   |
| ?                | clear filter                              |
| Space / x        | mark / unmark line, sweep in last direction |
| c or y           | copy marked lines, else current line      |
| Enter            | scroll down (accepts filter in prompt)    |
| Esc              | cancel editing; clear marks or filter     |
| f                | toggle follow                             |
| w                | toggle line wrap                          |
| r                | reload file                               |
| q, Ctrl-c        | quit                                      |

Change controls in the `keymap[]` table near the top of `comb.c`.

## License

0BSD (see LICENSE).
