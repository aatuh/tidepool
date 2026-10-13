#!/usr/bin/env python3
"""PTY exercise of rendering, mouse/keyboard sequences, resize, and TTY cleanup."""
import errno
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time

EXECUTABLE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else './builds/tidepool')


def take_terminal_control():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    os.tcsetpgrp(0, os.getpgrp())


def wait_for_output(master, expected, timeout=3):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while expected not in data:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f'Terminal did not emit {expected!r}'
        if select.select([master], [], [], remaining)[0]:
            chunk = os.read(master, 32768)
            assert chunk, 'Terminal closed before expected output'
            data.extend(chunk)
    return bytes(data)


def run_resize_case(exit_method, *, stalled=False, shared_stdio=True):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    original = termios.tcgetattr(slave)
    original_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
    output_slave = slave if shared_stdio else os.open(os.ttyname(slave), os.O_RDWR | os.O_NOCTTY)
    original_output_flags = fcntl.fcntl(output_slave, fcntl.F_GETFL)
    env = dict(os.environ, TERM='xterm-256color', LC_ALL='C.UTF-8')
    program = subprocess.Popen(
        [EXECUTABLE, '--color', 'truecolor', '--seed', '1'],
        stdin=slave, stdout=output_slave, stderr=output_slave, env=env,
        preexec_fn=take_terminal_control)
    try:
        wait_for_output(master, b'tidepool / lagoon')
        os.write(master, b' ')
        wait_for_output(master, b'paused')
        if stalled:
            # A large redraw fills the PTY queue while its reader is paused,
            # as a terminal emulator can be while its window is being resized.
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 80, 200, 0, 0))
            os.kill(program.pid, signal.SIGWINCH)
            assert select.select([master], [], [], 3)[0], 'Resize did not produce output'
            time.sleep(.1)
            if exit_method == 'interaction':
                # Input must be processed even while the redraw is blocked.
                os.write(master, b'\x1b[A')
                wait_for_output(master, b'tidepool / orchid')
                exit_method = 'ctrl-c'
            elif exit_method == 'resize':
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 34, 91, 0, 0))
                os.kill(program.pid, signal.SIGWINCH)
                wait_for_output(master, b'\x1b[34;1H\x1b[0m\x1b[2K')
                exit_method = 'ctrl-c'
        else:
            # Every SIGWINCH must remain handled, including recovery from a
            # terminal smaller than the minimum garden size.
            for rows, cols in [(34, 91), (40, 120), (5, 20), (24, 80)]:
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
                os.kill(program.pid, signal.SIGWINCH)
                expected = (b'enlarge the terminal to continue' if rows < 8 else
                            f'\x1b[{rows};1H'.encode())
                wait_for_output(master, expected)
        if exit_method == 'ctrl-c':
            os.write(master, b'\x03')
        elif exit_method == 'escape':
            os.write(master, b'\x1b')
        else:
            os.kill(program.pid, signal.SIGTERM)
        # Do not drain output until after exit: cleanup must also remain bounded.
        assert program.wait(timeout=3) == 0
        assert termios.tcgetattr(slave) == original, 'Original terminal flags not restored'
        assert fcntl.fcntl(slave, fcntl.F_GETFL) == original_flags, 'File status flags not restored'
        assert fcntl.fcntl(output_slave, fcntl.F_GETFL) == original_output_flags, 'Output flags not restored'
        data = wait_for_output(master, b'\x1b[?1049l')
        assert b'\x1b[?25h' in data, 'Cursor was not unhidden'
        assert b'\x1b[?1006l' in data, 'Mouse reporting was not disabled'
    finally:
        if program.poll() is None:
            program.kill()
            program.wait(timeout=4)
        os.close(master)
        if output_slave != slave:
            os.close(output_slave)
        os.close(slave)


def run_paint_case(*, stalled=False, partial_input=False):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 43, 149, 0, 0))
    original = termios.tcgetattr(slave)
    original_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
    env = dict(os.environ, TERM='xterm-256color', LC_ALL='C.UTF-8')
    program = subprocess.Popen(
        [EXECUTABLE, '--color', 'truecolor', '--seed', '1', '--fps', '30'],
        stdin=slave, stdout=slave, stderr=slave, env=env,
        preexec_fn=take_terminal_control)
    data = bytearray()
    finished = threading.Event()
    reader = None

    def collect_slowly():
        while not finished.is_set():
            if select.select([master], [], [], .01)[0]:
                chunk = os.read(master, 4096)
                data.extend(chunk)
                finished.wait(len(chunk) / 256000)

    try:
        wait_for_output(master, b'tidepool / lagoon')
        os.write(master, b' ')
        wait_for_output(master, b'paused')
        if not stalled:
            reader = threading.Thread(target=collect_slowly, daemon=True)
            reader.start()
        for _ in range(3):
            os.write(master, b'\x1b[<0;30;20M')
            for x in range(30, 140, 3):
                os.write(master, f'\x1b[<32;{x};20M'.encode())
                time.sleep(.008)
            os.write(master, b'\x1b[<0;140;20m')
        if partial_input:
            os.write(master, b'\x1b[<32;40;')
        os.write(master, b'\x03')
        assert program.wait(timeout=1) == 0, 'Ctrl-C did not promptly exit while painting'
        assert termios.tcgetattr(slave) == original, 'Terminal settings not restored after painting'
        assert fcntl.fcntl(slave, fcntl.F_GETFL) == original_flags, 'File flags not restored after painting'
        finished.set()
        if reader:
            reader.join(timeout=1)
        if b'\x1b[?1049l' not in data:
            data.extend(wait_for_output(master, b'\x1b[?1049l'))
        assert b'\x1b[?25h' in data and b'\x1b[?1006l' in data
        assert b'\x1b[H\x1b[2J' not in data, 'Painting restarted a full-screen redraw'
    finally:
        finished.set()
        if reader:
            reader.join(timeout=1)
        if program.poll() is None:
            program.kill()
            program.wait(timeout=4)
        os.close(master)
        os.close(slave)


def run_output_budget_case():
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 61, 155, 0, 0))
    original = termios.tcgetattr(slave)
    env = dict(os.environ, TERM='xterm', LC_ALL='C.UTF-8')
    program = subprocess.Popen(
        [EXECUTABLE, '--color', 'truecolor', '--fps', '30', '--seed', '1'],
        stdin=slave, stdout=slave, stderr=slave, env=env,
        preexec_fn=take_terminal_control)
    try:
        wait_for_output(master, b'tidepool / lagoon')
        data = bytearray()
        started = time.monotonic()
        deadline = started + 1
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining > 0 and select.select([master], [], [], remaining)[0]:
                data.extend(os.read(master, 32768))
        assert data, 'Running garden stopped producing updates'
        # Leave room for one read spanning the end of the measurement window.
        assert len(data) <= 256000 + 32768, f'Continuous rendering flooded the terminal: {len(data):,} bytes/s'
        os.write(master, b' \x1b[A')
        wait_for_output(master, b'paused')
        os.write(master, b'\x03')
        assert program.wait(timeout=1) == 0
        assert termios.tcgetattr(slave) == original
        wait_for_output(master, b'\x1b[?1049l')
        return len(data)
    finally:
        if program.poll() is None:
            program.kill()
            program.wait(timeout=4)
        os.close(master)
        os.close(slave)


def run_case(options, *, mouse=True, utf8=True, resize=True, terminate=False):
    master, slave = pty.openpty()
    dimensions = struct.pack('HHHH', 28, 85, 0, 0)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, dimensions)
    original = termios.tcgetattr(slave)
    env = os.environ.copy()
    env.update({'TERM': 'xterm-256color', 'COLORTERM': 'truecolor',
                'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'})
    program = subprocess.Popen([EXECUTABLE, *options], stdin=slave,
                               stdout=slave, stderr=slave, env=env,
                               preexec_fn=take_terminal_control)
    buf = bytearray()
    finished = threading.Event()
    def collect():
        while not finished.is_set():
            ready, _, _ = select.select([master], [], [], .1)
            if not ready:
                continue
            try:
                data = os.read(master, 32768)
                if data and len(buf) < 10_000_000:
                    buf.extend(data)
            except OSError as ex:
                if ex.errno not in (errno.EIO, errno.EAGAIN):
                    raise
                if ex.errno == errno.EIO:
                    return
    thread = threading.Thread(target=collect, daemon=True)
    thread.start()
    try:
        time.sleep(.35)
        os.write(master, b'qazp\t  \x1b[C\x1b[D\x1b[A\x1b[B?')
        os.write(master, 'åäö'.encode('utf-8'))
        time.sleep(.14)
        os.write(master, b'?\r')
        if mouse:
            os.write(master, b'\x1b[<0;40;12M')
            os.write(master, b'\x1b[<32;42;14M')
            os.write(master, b'\x1b[<0;42;14m')
            os.write(master, b'\x1b[<2;47;13M')
            os.write(master, b'\x1b[<2;47;13m')
        time.sleep(.24)
        if resize:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 34, 91, 0, 0))
            os.kill(program.pid, signal.SIGWINCH)
            time.sleep(.2)
            # Tiny terminal followed by return to a normal size.
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 5, 20, 0, 0))
            os.kill(program.pid, signal.SIGWINCH)
            time.sleep(.07)
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
            os.kill(program.pid, signal.SIGWINCH)
            time.sleep(.2)
        if terminate:
            os.kill(program.pid, signal.SIGTERM)
        else:
            os.write(master, b'\x1b')
        code = program.wait(timeout=7)
        assert code == 0, (options, code, bytes(buf[-2000:]))
        assert termios.tcgetattr(slave) == original, 'Original terminal flags not restored'
        time.sleep(.08)
        data = bytes(buf)
        assert b'\x1b[?1049h' in data, 'Alternate screen not entered'
        assert b'\x1b[?1049l' in data, 'Alternate screen not left'
        assert b'\x1b[?25h' in data, 'Cursor was not unhidden'
        assert b'\x1b[0m' in data, 'Terminal colors not reset'
        if '--no-hud' not in options:
            assert b'tidepool' in data, (len(data), repr(data[-2000:]), data.count(b'\x1b[2K'))
        if mouse:
            assert b'\x1b[?1006h' in data and b'\x1b[?1006l' in data
        else:
            assert b'\x1b[?1006h' not in data
        if utf8:
            assert '▀'.encode() in data, 'Unicode half-block display missing'
        else:
            assert '▀'.encode() not in data
        if '--color' in options and 'truecolor' in options:
            assert b'\x1b[38;2;' in data, 'Truecolor not present'
        assert b'\x00' not in data, 'NUL bytes emitted to terminal'
        return len(data)
    finally:
        finished.set()
        thread.join(timeout=1)
        if program.poll() is None:
            program.kill(); program.wait(timeout=4)
        os.close(master); os.close(slave)


def main():
    total = run_output_budget_case()
    print(f'PTY continuous rendering: OK ({total:,} bytes/s, responsive input)')
    run_paint_case()
    print('PTY painting: OK (slow output, no full-screen clears, prompt Ctrl-C)')
    run_paint_case(stalled=True, partial_input=True)
    print('PTY painting: OK (stalled output, incomplete mouse report, prompt Ctrl-C)')
    run_resize_case('ctrl-c')
    print('PTY resize: OK (repeated resizes, tiny terminal recovery, Ctrl-C)')
    for exit_method in ['ctrl-c', 'escape', 'sigterm', 'interaction', 'resize']:
        run_resize_case(exit_method, stalled=True)
        print(f'PTY stalled resize: OK ({exit_method})')
    run_resize_case('ctrl-c', stalled=True, shared_stdio=False)
    print('PTY stalled resize: OK (separate stdin/stdout descriptors)')
    for opts, flags in [
        (['--layout', 'fi', '--color', 'truecolor', '--seed', '1', '--fps', '12', '--renewal', 'gentle'], {'mouse': True}),
        (['--ascii', '--color', '256', '--palette', 'orchid', '--no-mouse'], {'mouse': False, 'utf8': False}),
        (['--mono', '--no-mouse', '--no-hud'], {'mouse': False, 'utf8': False, 'resize': False}),
        (['--color', '16', '--seed', '42', '--renewal', 'active'], {'mouse': True, 'terminate': True}),
    ]:
        total_bytes = run_case(opts, **flags)
        print(f'PTY smoke: OK ({" ".join(opts)}; {total_bytes:,} bytes)')
    help_data = subprocess.run([EXECUTABLE,'--help'],capture_output=True,check=True).stdout
    assert b'tidepool' in help_data and b'--palette' in help_data and b'--no-mouse' in help_data
    assert b'--renewal' in help_data and b'gentle' in help_data
    fail = subprocess.run([EXECUTABLE,'--fps','250'],capture_output=True)
    assert fail.returncode==2
    assert subprocess.run([EXECUTABLE, '--renewal', 'unknown'], capture_output=True).returncode == 2
    assert subprocess.run([EXECUTABLE, '--renewal'], capture_output=True).returncode == 2
    print('all PTY smoke tests: OK')


if __name__ == '__main__':
    main()
