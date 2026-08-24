#!/usr/bin/env python3
"""Test harness for comb: runs it under a deterministic pseudo-tty.

Channel layout when `stdin_text` is given (mirrors real-world piping):
  comb stdin  <- pipe filled with stdin_text   (not a tty -> comb opens /dev/tty)
  /dev/tty     <- the pty (controlling terminal): keystrokes, winsize
  comb stdout  <- the pty master: captured frames

Without stdin_text, comb's stdin is the pty itself, like running
`./comb FILE` interactively.

Usage:
  # show final screen of one run
  python3 tests/harness.py --keys 'Gq' -- ./comb tests/sample.log

  # same, feeding stdin (dmesg style)
  python3 tests/harness.py --keys 'q' --stdin - -- ./comb -

  # byte-exact regression diff between two builds
  python3 tests/harness.py --compare /tmp/comb_old ./comb

Unit-level coverage of the pure internals (tag detection, sanitizing,
matching, key decoding, spans) lives in tests/selftest.c: `make check`
runs both. This harness only covers what needs a real process+pty.
"""
import argparse
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time

SAMPLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sample.log')

class RunResult:
    def __init__(self, output, status, rows=24, cols=80):
        self.output = output      # raw bytes from the pty master
        self.status = status      # exit status
        self.rows = rows
        self.cols = cols

    def screen(self):
        """Final visible text grid as a list of row strings."""
        return render_screen(self.output, self.rows, self.cols)

    def text(self):
        """ANSI-free transcript."""
        return re.sub(rb'\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07]*(\x07|\x1b\\)',
                      b'', self.output)

def _child_setup(slave, stdin_pipe_r, env_color=True):
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    os.dup2(stdin_pipe_r if stdin_pipe_r is not None else slave, 0)
    os.dup2(slave, 1)
    os.dup2(slave, 2)
    for f in {slave, stdin_pipe_r} - {None}:
        try:
            os.close(f)
        except OSError:
            pass
    os.environ['TERM'] = 'xterm'
    if env_color:
        os.environ.pop('NO_COLOR', None)
    else:
        os.environ['NO_COLOR'] = '1'

def run(argv, keys=b'', stdin_text=None, rows=24, cols=80, env_color=True,
        settle=0.35, key_delay=0.02, idle=0.25, timeout=5.0, midrun=None,
        resizes=None):
    """Spawn comb, send keys, return RunResult. argv excludes keys."""
    stdin_r = None
    # os.pipe() fds are non-inheritable (PEP 446): at execvp the child's
    # copy of stdin_w vanishes, which is what delivers EOF to comb.
    if stdin_text is not None:
        stdin_r, stdin_w = os.pipe()

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack('HHHH', rows, cols, 0, 0))

    pid = os.fork()
    if pid == 0:
        _child_setup(slave, stdin_r, env_color)
        os.execvp(argv[0], argv)

    os.close(slave)
    if stdin_text is not None:
        os.close(stdin_r)
        os.write(stdin_w, stdin_text if isinstance(stdin_text, bytes)
                 else stdin_text.encode())
        os.close(stdin_w)          # EOF: comb finishes load_all()

    out = bytearray()

    def pump(duration):
        end = time.time() + duration
        while time.time() < end:
            r, _, _ = select.select([master], [], [], min(idle, end - time.time()))
            if r:
                try:
                    d = os.read(master, 65536)
                except OSError:
                    return False
                if not d:
                    return False
                out.extend(d)
        return True

    alive = pump(settle)
    if alive and midrun:
        midrun(master)             # mutate the world (append/truncate files)
        alive = pump(settle)       # give follow mode a poll cycle to notice
    if alive:
        for r, c in (resizes or []):
            fcntl.ioctl(master, termios.TIOCSWINSZ,
                        struct.pack('HHHH', r, c, 0, 0))
            rows, cols = r, c
            if not pump(settle):
                break
    for k in (keys if isinstance(keys, bytes) else keys.encode()):
        if not alive:
            break
        try:
            os.write(master, bytes([k]))
        except OSError:
            break
        alive = pump(key_delay)
    if alive:
        pump(timeout)              # drain until quiet / exit

    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(pid, 0)
    os.close(master)
    return RunResult(bytes(out), status, rows, cols)

# --- tiny VT interpreter: enough of CUP/EL/ED for screen assertions ---

_CSI = re.compile(rb'\x1b\[([0-9;?]*)([a-zA-Z])')

def render_screen(data, rows=24, cols=80):
    grid = [[' '] * cols for _ in range(rows)]
    cr, cc = 0, 0
    i = 0
    while i < len(data):
        m = _CSI.match(data, i)
        if m:
            params, final = m.group(1), m.group(2)
            p = [int(x) for x in params.split(b';') if x.isdigit()]
            if final == b'H':
                cr = (p[0] - 1) if len(p) > 0 else 0
                cc = (p[1] - 1) if len(p) > 1 else 0
            elif final == b'K':
                for c in range(cc, cols):
                    grid[cr][c] = ' '
            elif final == b'J':
                if not p or p[0] == 0:
                    for r in range(cr, rows):
                        for c in range(cc if r == cr else 0, cols):
                            grid[r][c] = ' '
            i = m.end()
            continue
        b = data[i]
        if b == 0x0a:
            cr += 1
        elif b == 0x0d:
            cc = 0
        elif b >= 0x20:
            if cr < rows and cc < cols:
                grid[cr][cc] = chr(b)
            cc += 1
        i += 1
    return [''.join(row).rstrip() for row in grid]


# --- CLI -------------------------------------------------------------

def _parse_args():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keys', default='q', help='keystrokes to send (default: q)')
    ap.add_argument('--stdin', action='store_true',
                    help='pipe the fixture through comb\'s stdin '
                         '(argv keeps "-", so use `-- ./comb -`)')
    ap.add_argument('--rows', type=int, default=24)
    ap.add_argument('--cols', type=int, default=80)
    ap.add_argument('--raw', action='store_true', help='dump raw pty bytes')
    ap.add_argument('--screen', action='store_true',
                    help='render final screen instead of transcript')
    ap.add_argument('--compare', nargs=2, metavar=('OLD', 'NEW'),
                    help='byte-diff two builds over the scenario suite')
    ap.add_argument('--check', action='store_true',
                    help='run absolute behavior assertions against the build '
                         'given in cmd (default when --compare is absent)')
    ap.add_argument('cmd', nargs=argparse.REMAINDER, help='-- ./comb [args]')
    return ap.parse_args()

SCENARIOS = [
    ('bottom+copy',       'jjkG$0ww/err\x1b cq', {}),
    ('wrap+hscroll',      'wjjkkll$0Gq',         {}),
    ('wrap toggle twice', 'wwggjk\x1bGq',        {}),
    ('mark+copy',         ' x x jk c q',         {}),
    ('filter edit',       '/err\x7f\x7f\x7f\x1bGgq', {}),
    ('instant quit',      'q',                   {}),
    ('edges',             'wgg0$q',              {}),
    ('follow+reload',     'jkjkfrrq',            {}),
    # real escape-sequence keys: arrows, PgDn/PgUp, Home/End
    ('arrow keys',        '\x1b[B\x1b[B\x1b[A\x1b[6~\x1b[5~\x1b[H\x1b[Fq', {}),
    ('ctrl paging',       '\x04\x04\x15\x06ggq',  {}),
    # wrap over the CJK/fullwidth lines exercises line_rows()
    ('wrap cjk nav',      'wwjjjjGGgjk0$$qq',    {}),
    ('hscroll cjk $',     'GG$hhhh0$q',          {}),
    ('filter nomatch',    '/zzznomatch\x1bG?gq', {}),
    ('marks sweep up',    'GGkk xkx q',          {}),
    ('esc clears marks',  'GGxkx\x1bg/err\x1bq',  {}),
    # SIGWINCH storm: shrink then grow, pane must re-window without dying
    ('resize storm',      'jjjwwggjkq',          {'resizes': ((24, 80), (10, 40), (30, 120))}),
]

# Absolute assertions against a single build (no reference binary needed):
# each returns None on pass, or a detail string on failure.
def _check_empty_stdin(binary):
    scr = run([binary, '-'], keys='q', stdin_text='').screen()
    if not any('(empty)' in row for row in scr):
        return '(empty) not shown for empty stdin'

def _check_no_trailing_newline(binary):
    scr = run([binary, '-'], keys='gq', stdin_text='last line has no newline').screen()
    if not any('last line has no newline' in row for row in scr):
        return 'final partial line was dropped'

def _check_filter_nomatch_status(binary):
    # at narrow widths the left status side yields to the keybinding hint,
    # so give the pane enough columns for the suffix to survive
    res = run([binary, '-e', 'zzznomatch', SAMPLE], keys='q', cols=140)
    if not any('(no matches)' in row for row in res.screen()):
        return '(no matches) missing from status line'

def _check_filter_narrows(binary):
    scr = run([binary, '-e', 'NetworkManager', SAMPLE], keys='gq').screen()
    if not any('NetworkManager' in r for r in scr):
        return 'filtered view shows no matching lines'
    if not any(re.search(r'\d+/\d+', r) for r in scr):
        return 'status counter missing'

def _check_ansi_sanitized(binary):
    dirty = '\x1b[31mRED\x1b[0m plain \x1b]0;title\x07tail\n'
    scr = run([binary, '-'], keys='gq', stdin_text=dirty).screen()
    joined = '\n'.join(scr)
    if 'RED plain tail' not in joined:
        return f'input ANSI leaked into display: {joined!r}'

def _check_copy_osc52(binary):
    out = run([binary, SAMPLE], keys='cq').output
    if b'\x1b]52;c;' not in out:
        return 'OSC 52 sequence missing after copy'

def _no_hl_sgr(out):
    # severity/palette/token SGRs: bright reds/magenta/yellow + 256-palette fg
    return not re.search(rb'\x1b\[(1;9[15]|2;|38;5;\d+)m', out)

def _check_no_color_flag(binary):
    out = run([binary, '--no-color', SAMPLE], keys='Gq').output
    if not _no_hl_sgr(out):
        return '--no-color still emits highlighting SGRs'

def _check_no_color_env(binary):
    out = run([binary, SAMPLE], keys='Gq', env_color=False).output
    if not _no_hl_sgr(out):
        return 'NO_COLOR env still emits highlighting SGRs'

def _check_wrap_long_line(binary):
    # a line wider than the pane must span multiple rows when wrapped,
    # and exactly one row (clipped) when not
    long = 'x' * 200 + '\n'
    wrapped = run([binary, '-'], keys='wgq', stdin_text=long)
    rows_used = sum(1 for r in wrapped.screen() if set(r.strip()) <= {'x'} and r.strip())
    if rows_used < 2:
        return f'long line occupied {rows_used} rows with wrap on, expected >= 2'
    plain = run([binary, '-'], keys='gq', stdin_text=long).screen()
    # count only rows made purely of x's: ignores the top/input bars
    if sum(1 for r in plain if set(r.strip()) <= {'x'} and r.strip()) != 1:
        return 'unwrapped long line spilled over multiple rows'

def _tmplog(lines):
    tf = tempfile.NamedTemporaryFile(delete=False, suffix='.log')
    tf.write((''.join(lines)).encode())
    tf.close()
    return tf.name

def _check_follow_append(binary):
    name = _tmplog(['one\n', 'two\n'])

    def grow(fd):
        with open(name, 'a') as f:
            for i in range(50):
                f.write(f'appended {i}\n')

    scr = run([binary, name], keys='q', midrun=grow).screen()
    # no movement keys: comb starts at the bottom, follow must keep it there
    os.unlink(name)
    if not any('appended 49' in r for r in scr):
        return 'appended lines never appeared'
    if not any('/52' in r for r in scr):
        return 'cursor did not stick to bottom while following'

def _check_copytruncate(binary):
    name = _tmplog([f'stale line {i}\n' for i in range(100)])

    def cut(fd):
        with open(name, 'w') as f:
            f.write('fresh start\n')

    scr = run([binary, name], keys='gq', midrun=cut).screen()
    os.unlink(name)
    if not any('fresh start' in r for r in scr):
        return 'truncated-in fresh line not shown'
    if any('stale line' in r for r in scr):
        return 'stale lines survived truncation'

def _check_rename_rotation_no_dupes(binary):
    # rename+recreate rotation with overlapping content: the fresh inode
    # is loaded through the mmap window; the fd must not re-read the same
    # bytes through the buffered path afterwards
    name = _tmplog(['AAA original one\n', 'BBB original two\n'])

    def rotate(fd):
        os.rename(name, name + '.1')
        with open(name, 'w') as f:
            f.write('AAA original one\nBBB original two\nCCC brand new\n')

    scr = run([binary, name], keys='gq', settle=0.6, midrun=rotate).screen()
    os.unlink(name)
    os.unlink(name + '.1')
    if not any('CCC' in r for r in scr):
        return 'rotated-in line never appeared'
    for needle in ('AAA', 'BBB', 'CCC'):
        n = sum(1 for r in scr if needle in r)
        if n != 1:
            return f'{needle!r} appears {n} times after rotation (expected 1)'

def _check_mark_empty_lines(binary):
    res = run([binary, '-'], keys='Gxkxcq', stdin_text='\n\n\ncontent\n')
    if b'\x1b]52;c;' not in res.output:
        return 'copying marked empty lines produced no OSC 52'
    if not any('copied' in r for r in res.screen()):
        return 'no copy confirmation in status bar'

def _check_hscroll_caps_at_content(binary):
    # scrolling right must stop at the widest line, not wander into
    # blankness that looks like unread log
    text = ('short\n' + ''.join(str(i % 10) for i in range(300)) +
            '\nshort2\n')
    s30 = run([binary, '-'], keys='g' + 'l' * 30 + 'q', stdin_text=text).screen()
    s200 = run([binary, '-'], keys='g' + 'l' * 200 + 'q', stdin_text=text).screen()
    if s30 != s200:
        return 'view changed between 30 and 200 right-taps; cap not engaged'
    pane = next((r for r in s30 if r.strip() and r.strip()[0].isdigit()), '')
    if not pane.rstrip().endswith('9'):
        return f'right edge of widest line not shown: {pane[-12:]!r}'

def _check_filter_accepts_utf8(binary):
    # multi-byte query characters must reach regcomp intact; assert on the
    # match result -- the status bar legitimately elides long queries
    text = 'Sch\u00f6ne Gr\u00fc\u00dfe\nplain line\n'
    res = run([binary, '-'], keys='g/Gr\u00fc\u00dfe\rq',
              stdin_text=text)
    scr = '\n'.join(res.screen())
    want = 'Sch\u00f6ne Gr\u00fc\u00dfe'.encode('utf-8').decode('latin-1')
    if want not in scr:
        return f'UTF-8 query did not match: {scr!r}'
    if 'plain line' in scr:
        return 'filter did not narrow: unmatched line still visible'

def _check_filter_invert(binary):
    # Ctrl-V in the prompt excludes matching lines
    text = ('ERROR disk failure\nINFO all good\nERROR second fault\n'
            'plain line\n')
    res = run([binary, '-'], keys='g/err\x16\rq', stdin_text=text)
    scr = '\n'.join(res.screen())
    if 'disk failure' in scr or 'second fault' in scr:
        return f'inverted filter still shows matching lines: {scr!r}'
    if 'all good' not in scr or 'plain line' not in scr:
        return f'inverted filter dropped non-matching lines: {scr!r}'
    if '(!)' not in scr:
        return f'(!) badge missing while excluding: {scr!r}'
    # extending an inverted query must resurrect lines the shorter one hid:
    # "ERROR " hides both ERRORs; "ERROR d" no longer matches "second
    # fault", so it must come back (disk failure stays out). This
    # exercises the suppressed superset-narrowing fast path.
    res2 = run([binary, '-'], keys='g/ERROR \x16d\rq', stdin_text=text)
    scr2 = '\n'.join(res2.screen())
    if 'second fault' not in scr2:
        return f'inverted query extension did not resurrect lines: {scr2!r}'
    if 'disk failure' in scr2 or 'all good' not in scr2:
        return f'extended inverted filter kept wrong lines: {scr2!r}'

def _check_regex_toggle_badge(binary):
    # Ctrl-R in the prompt flips to regex mode; the status bar must say so
    out = run([binary, SAMPLE], keys='/\x12err\x1bGq')
    if not any('(R)' in r for r in out.screen()):
        return '(R) badge missing from status bar in regex mode'
    out = run([binary, SAMPLE], keys='/err\x1bGq')
    if any('(R)' in r for r in out.screen()):
        return '(R) badge shown while still in literal mode'

def _check_literal_metachars_match(binary):
    # literal mode treats metachars literally; no bad-regex notice may fire
    text = 'a.c [x] literal\nother line\n'
    res = run([binary, '-'], keys='g/a.c [x]\rq', stdin_text=text)
    scr = '\n'.join(res.screen())
    if 'literal' not in scr:
        return f'literal query did not match: {scr!r}'
    if 'other line' in scr:
        return 'literal filter did not narrow: unmatched line still visible'

CHECKS = [
    ('empty stdin shows placeholder', _check_empty_stdin),
    ('final line without newline kept', _check_no_trailing_newline),
    ('-e no-match status',            _check_filter_nomatch_status),
    ('-e narrows view',               _check_filter_narrows),
    ('input ANSI sanitized',          _check_ansi_sanitized),
    ('copy emits OSC 52',             _check_copy_osc52),
    ('--no-color strips highlighting', _check_no_color_flag),
    ('NO_COLOR env strips highlighting', _check_no_color_env),
    ('wrap splits long lines only in wrap mode', _check_wrap_long_line),
    ('follow picks up appended lines',           _check_follow_append),
    ('copytruncate clears stale lines',          _check_copytruncate),
    ('rename rotation does not duplicate',       _check_rename_rotation_no_dupes),
    ('marked empty lines copy cleanly',          _check_mark_empty_lines),
    ('regex toggle shows (R) badge',  _check_regex_toggle_badge),
    ('literal filter matches metachars', _check_literal_metachars_match),
    ('filter prompt accepts UTF-8 queries',      _check_filter_accepts_utf8),
    ('Ctrl-V inverted filter',                   _check_filter_invert),
    ('hscroll caps at widest line',              _check_hscroll_caps_at_content),
]

def _run_scenario(binary, keys, opts):
    argv = [binary, SAMPLE]
    return run(argv, keys=keys, **opts)

def main():
    a = _parse_args()
    cmd = [c for c in a.cmd if c != '--']

    if a.compare:
        old, new = a.compare
        fails = 0
        for name, keys, opts in SCENARIOS:
            o = _run_scenario(old, keys, opts).output
            n = _run_scenario(new, keys, opts).output
            ok = o == n
            print(('OK   ' if ok else 'DIFF ') + name)
            fails += 0 if ok else 1
        sys.exit(1 if fails else 0)

    # explicit single-run inspection mode
    if cmd and (a.raw or a.screen or not a.check):
        stdin_text = open(SAMPLE).read() if a.stdin and cmd[-1] == '-' else None
        res = run(cmd, keys=a.keys, stdin_text=stdin_text,
                  rows=a.rows, cols=a.cols)
        if a.raw:
            sys.stdout.buffer.write(res.output)
        elif a.screen:
            print('\n'.join(res.screen()))
        else:
            sys.stdout.buffer.write(res.text())
        if res.status:
            print(f'\n[exit status {res.status}]', file=sys.stderr)
        return

    binary = cmd[0] if cmd else './comb'
    fails = 0
    for name, fn in CHECKS:
        try:
            detail = fn(binary)
        except Exception as e:      # report and keep going; one bad check
            detail = f'harness exception: {e!r}'   # shouldn't hide the rest
        if detail is None:
            print('OK   ' + name)
        else:
            fails += 1
            print(f'FAIL {name}: {detail}')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
