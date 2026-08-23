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
  python3 tests/harness.py --keys 'Gq' -- ./comb sample.log

  # same, feeding stdin (dmesg style)
  python3 tests/harness.py --keys 'q' --stdin - -- ./comb -

  # byte-exact regression diff between two builds
  python3 tests/harness.py --compare /tmp/comb_old ./comb
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
import termios
import time


class RunResult:
    def __init__(self, output, status):
        self.output = output      # raw bytes from the pty master
        self.status = status      # exit status

    def screen(self):
        """Final visible text grid as a list of row strings."""
        return render_screen(self.output)

    def text(self):
        """ANSI-free transcript."""
        return re.sub(rb'\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07]*(\x07|\x1b\\)',
                      b'', self.output)


def _child_setup(slave, stdin_pipe_r):
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
    os.environ.pop('NO_COLOR', None)


def run(argv, keys=b'', stdin_text=None, rows=24, cols=80, env_color=True,
        settle=0.35, key_delay=0.02, idle=0.25, timeout=5.0):
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
        _child_setup(slave, stdin_r)
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
    return RunResult(bytes(out), status)


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
                    help='pipe the file argument through comb\'s stdin '
                         '(argv keeps "-", so use `-- ./comb -`)')
    ap.add_argument('--rows', type=int, default=24)
    ap.add_argument('--cols', type=int, default=80)
    ap.add_argument('--raw', action='store_true', help='dump raw pty bytes')
    ap.add_argument('--screen', action='store_true',
                    help='render final screen instead of transcript')
    ap.add_argument('--compare', nargs=2, metavar=('OLD', 'NEW'),
                    help='byte-diff two builds over the scenario suite')
    ap.add_argument('cmd', nargs=argparse.REMAINDER, help='-- ./comb [args]')
    return ap.parse_args()


SCENARIOS = [
    ('bottom+copy',       'jjkG$0ww/err\x1b cq', {'stdin_text': None}),
    ('wrap+hscroll',      'wjjkkll$0Gq',         {}),
    ('wrap toggle twice', 'wwggjk\x1bGq',        {}),
    ('mark+copy',         ' x x jk c q',         {}),
    ('filter edit',       '/err\x7f\x7f\x7f\x1bGgq', {}),
    ('instant quit',      'q',                   {}),
    ('edges',             'wgg0$q',              {}),
    ('follow+reload',     'jkjkfrrq',            {}),
]


def _run_scenario(binary, name, keys, opts):
    argv = [binary, 'sample.log']
    return run(argv, keys=keys, **opts)


def main():
    a = _parse_args()
    cmd = [c for c in a.cmd if c != '--']

    if a.compare:
        old, new = a.compare
        fails = 0
        for name, keys, opts in SCENARIOS:
            o = _run_scenario(old, name, keys, opts).output
            n = _run_scenario(new, name, keys, opts).output
            ok = o == n
            print(('OK   ' if ok else 'DIFF ') + name)
            fails += 0 if ok else 1
        sys.exit(1 if fails else 0)

    if not cmd:
        sys.exit('need a command: -- ./comb sample.log')

    stdin_text = open('sample.log').read() if a.stdin and cmd[-1] == '-' else None
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


if __name__ == '__main__':
    main()
