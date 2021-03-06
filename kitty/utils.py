#!/usr/bin/env python
# vim:fileencoding=utf-8
# License: GPL v3 Copyright: 2016, Kovid Goyal <kovid at kovidgoyal.net>

import math
import os
import re
import shlex
import signal
import string
import subprocess
from contextlib import contextmanager
from functools import lru_cache
from time import monotonic

from .constants import isosx
from .fast_data_types import glfw_get_physical_dpi, wcwidth as wcwidth_impl
from .rgb import Color, to_color


def safe_print(*a, **k):
    try:
        print(*a, **k)
    except Exception:
        pass


def ceil_int(x):
    return int(math.ceil(x))


@lru_cache(maxsize=2**13)
def wcwidth(c: str) -> int:
    try:
        return wcwidth_impl(ord(c))
    except TypeError:
        return wcwidth_impl(ord(c[0]))


@lru_cache()
def pt_to_px(pts):
    dpix, dpiy = get_dpi()['logical']
    dpi = (dpix + dpiy) / 2
    return round(pts * dpi / 72)


@contextmanager
def timeit(name, do_timing=False):
    if do_timing:
        st = monotonic()
    yield
    if do_timing:
        safe_print('Time for {}: {}'.format(name, monotonic() - st))


def sanitize_title(x):
    return re.sub(r'\s+', ' ', re.sub(r'[\0-\x19]', '', x))


@lru_cache()
def load_libx11():
    import ctypes
    from ctypes.util import find_library
    libx11 = ctypes.CDLL(find_library('X11'))
    ans = []

    def cdef(name, restype, *argtypes):
        f = getattr(libx11, name)
        if restype is not None:
            f.restype = restype
        if argtypes:
            f.argtypes = argtypes
        ans.append(f)

    cdef('XOpenDisplay', ctypes.c_void_p, ctypes.c_char_p)
    cdef('XCloseDisplay', ctypes.c_int, ctypes.c_void_p)
    cdef('XResourceManagerString', ctypes.c_char_p, ctypes.c_void_p)
    return ans


def parse_xrdb(raw):
    q = 'Xft.dpi:\t'
    for line in raw.decode('utf-8').splitlines():
        if line.startswith(q):
            return float(line[len(q):])


def x11_dpi_native():
    XOpenDisplay, XCloseDisplay, XResourceManagerString = load_libx11()
    display = XOpenDisplay(None)
    if display is None:
        raise RuntimeError('Could not connect to the X server')
    try:
        raw = XResourceManagerString(display)
        return parse_xrdb(raw)
    finally:
        XCloseDisplay(display)


def x11_dpi():
    try:
        return x11_dpi_native()
    except Exception:
        pass
    try:
        raw = subprocess.check_output(['xrdb', '-query'])
        return parse_xrdb(raw)
    except Exception:
        pass


def get_logical_dpi():
    if not hasattr(get_logical_dpi, 'ans'):
        if isosx:
            # TODO: Investigate if this needs a different implementation on OS X
            get_logical_dpi.ans = glfw_get_physical_dpi()
        else:
            # See https://github.com/glfw/glfw/issues/1019 for why we cant use
            # glfw_get_physical_dpi()
            dpi = x11_dpi()
            if dpi is None:
                get_logical_dpi.ans = glfw_get_physical_dpi()
            else:
                get_logical_dpi.ans = dpi, dpi
    return get_logical_dpi.ans


def get_dpi():
    if not hasattr(get_dpi, 'ans'):
        pdpi = glfw_get_physical_dpi()
        get_dpi.ans = {'physical': pdpi, 'logical': get_logical_dpi()}
    return get_dpi.ans


def color_as_int(val):
    return val[0] << 16 | val[1] << 8 | val[2]


def color_from_int(val):
    return Color((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF)


def parse_color_set(raw):
    parts = raw.split(';')
    for c, spec in [parts[i:i + 2] for i in range(0, len(parts), 2)]:
        try:
            c = int(c)
            if c < 0 or c > 255:
                raise IndexError('Out of bounds')
            r, g, b = to_color(spec)
            yield c, r << 16 | g << 8 | b
        except Exception:
            continue


def pipe2():
    try:
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
    except AttributeError:
        import fcntl
        read_fd, write_fd = os.pipe()
        for fd in (read_fd, write_fd):
            flag = fcntl.fcntl(fd, fcntl.F_GETFD)
            fcntl.fcntl(fd, fcntl.F_SETFD, flag | fcntl.FD_CLOEXEC)
            flag = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, flag | os.O_NONBLOCK)
    return read_fd, write_fd


def handle_unix_signals():
    read_fd, write_fd = pipe2()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda x, y: None)
        signal.siginterrupt(sig, False)
    signal.set_wakeup_fd(write_fd)
    return read_fd


def get_primary_selection():
    if isosx:
        return ''  # There is no primary selection on OS X
    # glfw has no way to get the primary selection
    # https://github.com/glfw/glfw/issues/894
    return subprocess.check_output(['xsel', '-p']).decode('utf-8')


def base64_encode(
    integer,
    chars=string.ascii_uppercase + string.ascii_lowercase + string.digits +
    '+/'
):
    ans = ''
    while True:
        integer, remainder = divmod(integer, 64)
        ans = chars[remainder] + ans
        if integer == 0:
            break
    return ans


def set_primary_selection(text):
    if isosx:
        return  # There is no primary selection on OS X
    if isinstance(text, str):
        text = text.encode('utf-8')
    p = subprocess.Popen(['xsel', '-i', '-p'], stdin=subprocess.PIPE)
    p.stdin.write(text), p.stdin.close()
    p.wait()


def open_url(url, program='default'):
    if program == 'default':
        cmd = ['open'] if isosx else ['xdg-open']
    else:
        cmd = shlex.split(program)
    cmd.append(url)
    subprocess.Popen(cmd).wait()
