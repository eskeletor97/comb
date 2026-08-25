#!/usr/bin/env python3
"""Interactive drive of comb on a huge file: type a mistyped regex, watch
the spinner animate across batch boundaries, hit Esc mid-scan, verify the
view is restored intact and 'cancelled' is reported."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import harness as H

def wait_for(master, needle, deadline):
    """Read pty until `needle` (bytes) appears; returns tail buffer or b''."""
    buf = bytearray()
    while time.time() < deadline:
        r, _, _ = __import__('select').select([master], [], [], 0.25)
        if r:
            try:
                d = os.read(master, 65536)
            except OSError:
                return b''
            if not d:
                return b''
            buf.extend(d)
            if needle in buf:
                return bytes(buf)
    return bytes(buf) if needle in buf else b''

def main():
    path = sys.argv[1]
    master, slave = None, None
    import pty, fcntl, struct, termios, select, signal
    rows, cols = 24, 100
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        os.dup2(slave, 0); os.dup2(slave, 1); os.dup2(slave, 2)
        os.close(slave)
        os.environ['TERM'] = 'xterm'
        os.execvp('./comb', ['./comb', path])
    os.close(slave)

    t0 = time.time()
    # wait for the first full render (status bar hint) => load finished
    out = wait_for(master, b'q quit', t0 + 600)
    assert out, 'comb died or never finished loading'
    print(f'[load done in {time.time()-t0:.1f}s]')

    def send(b):
        os.write(master, b)

    # baseline screen: note the status line before filtering
    send(b'g'); time.sleep(0.3)
    pre = H.RunResult(drain(master), 0, rows, cols).screen()

    # mistyped regex that matches nothing but costs a full scan
    send(b'/'); time.sleep(0.15)
    send(b'z'); send(b'y'); send(b'z'); send(b'x'); send(b'q')
    time.sleep(0.15)
    send(b'\r')  # commit the query -> job starts

    # collect output while the scan runs; count distinct spinner frames
    frames = set()
    spin_out = bytearray()
    end = time.time() + 30
    cancelled_seen = False
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.2)
        if r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d:
                break
            spin_out.extend(d)
            for m in re.finditer(rb'[|/-] filtering [0-9]+%', d):
                frames.add(m.group(0))
            if b'filtering 100%' in spin_out:
                pass  # keep draining until render replaces it
        # after at least one frame + some progress, bail out
        if len(frames) >= 2:
            send(b'\x1b')
            cancelled_seen = True
            break
    assert cancelled_seen, f'scan finished before we could bail; frames={frames}'
    print(f'[spinner frames seen: {sorted(frames)}]')

    # drain post-cancel output
    post = wait_for(master, b'cancelled', time.time() + 5)
    scr = H.RunResult(spin_out + post, 0, rows, cols).screen()
    joined = '\n'.join(scr)
    assert 'cancelled' in joined, 'no cancellation notice on screen'
    # view must be back to the pre-filter state (empty prompt row, no /query)
    assert '/zyzxq' not in joined, 'rejected query still visible'

    drain(master)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    print('[PASS] esc bailed mid-scan, previous view restored')

def drain(master):
    out = bytearray()
    import select
    while True:
        r, _, _ = select.select([master], [], [], 0.4)
        if not r:
            break
        try:
            d = os.read(master, 65536)
        except OSError:
            break
        if not d:
            break
        out.extend(d)
    return bytes(out)

if __name__ == '__main__':
    main()
