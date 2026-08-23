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
- opens at the end of the file (and after `r` reload) so the newest
  logs are visible immediately; follow mode sticks by default
- incremental `/` filter — POSIX ERE, smart case (lowercase = case-insensitive)
- live follow with `poll()`, handles file truncation/rotation
- `w` toggles soft line wrap (h/l/$ horizontal scrolling applies when
  wrap is off)
- mark lines with space (x unmarks), copy them all with c/y via
  OSC 52 (works over ssh); falls back to xclip / wl-copy / pbcopy /
  termux-clipboard-set when installed
- reads plain files or stdin; input ANSI sequences are stripped so
  logs can't corrupt the display

## Build & install

    make
    make install        # to /usr/local/bin (PREFIX=... to change)

## Test log

`sample.log` is a hand-crafted torture log touching every heuristic:
severity keywords and their false-positive guards (`--debug` flags,
paths, words containing "err"), `<info>`-style verbosity tokens, epoch
stamps vs. `[pid]` groups, quote/paren nesting, apostrophes ("don't"),
embedded / unclosed / lone ANSI escapes, tabs, UTF-8, over-long lines
and tag-less lines.

Try live follow in one terminal:

    ./comb sample.log
    echo "ERROR appended from outside" >> sample.log   # in another

## Usage

    comb [-e REGEX] [--no-color] [FILE]

`--no-color` (or `-C`, or the `NO_COLOR` env var) strips syntax coloring but keeps inverse-video cursor/marks.

Reads FILE, or piped/redirected stdin (`dmesg | comb`, `comb < file`); the explicit `comb -` still works. When reading stdin, keys come from /dev/tty.

## Keys

| key              | action                                    |
|------------------|-------------------------------------------|
| j/k/e, arrows     | next / previous line (sets sweep dir)     |
| Ctrl-d/u/f/b, PgDn/Up | page down / up                        |
| g/G, Home/End    | top / bottom                              |
| h/l, Left/Right  | scroll sideways (0 = home, $ = end)       |
| /                | filter (incremental regex, smart case)    |
| ?                | clear filter                              |
| Space / x        | mark / unmark line, sweep in last direction |
| c or y           | copy marked lines, else current line      |
| Enter            | scroll down (accepts filter in prompt)    |
| Esc              | cancel editing; clear marks or filter     |
| f                | toggle follow                             |
| w                | toggle line wrap                          |
| r                | reload file                               |
| q, Ctrl-c        | quit                                      |

Bindings live in the `keymap[]` table near the top of `comb.c` — edit
and rebuild to taste.

## License

0BSD (see LICENSE).
