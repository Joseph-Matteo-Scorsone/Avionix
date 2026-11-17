# Run on Windows: python tests/conpty_paste.py zig-out/bin/avionix-tests.exe
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import time
import sys

k = c.WinDLL('kernel32', use_last_error=True)
class COORD(c.Structure):
    _fields_ = [('X', c.c_short), ('Y', c.c_short)]
class SI(c.Structure):
    _fields_ = [('cb', w.DWORD), ('reserved', w.LPWSTR), ('desktop', w.LPWSTR), ('title', w.LPWSTR),
                ('x', w.DWORD), ('y', w.DWORD), ('xs', w.DWORD), ('ys', w.DWORD),
                ('xc', w.DWORD), ('yc', w.DWORD), ('fill', w.DWORD), ('flags', w.DWORD),
                ('show', w.WORD), ('reserved2size', w.WORD), ('reserved2', c.c_void_p),
                ('stdin', w.HANDLE), ('stdout', w.HANDLE), ('stderr', w.HANDLE)]
class SIX(c.Structure):
    _fields_ = [('si', SI), ('attrs', c.c_void_p)]
class PI(c.Structure):
    _fields_ = [('process', w.HANDLE), ('thread', w.HANDLE), ('pid', w.DWORD), ('tid', w.DWORD)]
k.CreatePipe.argtypes = [c.POINTER(w.HANDLE), c.POINTER(w.HANDLE), c.c_void_p, w.DWORD]
k.CreatePseudoConsole.argtypes = [COORD, w.HANDLE, w.HANDLE, w.DWORD, c.POINTER(w.HANDLE)]
k.CreatePseudoConsole.restype = c.c_long
k.InitializeProcThreadAttributeList.argtypes = [c.c_void_p, w.DWORD, w.DWORD, c.POINTER(c.c_size_t)]
k.UpdateProcThreadAttribute.argtypes = [c.c_void_p, w.DWORD, c.c_size_t, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p]
k.CreateProcessW.argtypes = [w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p, w.BOOL, w.DWORD, c.c_void_p, w.LPCWSTR, c.POINTER(SIX), c.POINTER(PI)]
k.PeekNamedPipe.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.c_void_p, c.POINTER(w.DWORD), c.c_void_p]
k.ReadFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
k.WriteFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
k.GetExitCodeProcess.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
k.CloseHandle.argtypes = [w.HANDLE]
k.ClosePseudoConsole.argtypes = [w.HANDLE]
k.DeleteProcThreadAttributeList.argtypes = [c.c_void_p]
k.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
def ok(value):
    if not value:
        raise c.WinError(c.get_last_error())
ir, iw, or_, ow, hp = [w.HANDLE() for _ in range(5)]
ok(k.CreatePipe(c.byref(ir), c.byref(iw), None, 0))
ok(k.CreatePipe(c.byref(or_), c.byref(ow), None, 0))
assert k.CreatePseudoConsole(COORD(100, 30), ir, ow, 0, c.byref(hp)) == 0
size = c.c_size_t()
k.InitializeProcThreadAttributeList(None, 1, 0, c.byref(size))
attrs = c.create_string_buffer(size.value)
ok(k.InitializeProcThreadAttributeList(attrs, 1, 0, c.byref(size)))
ok(k.UpdateProcThreadAttribute(attrs, 0, 0x20016, hp, c.sizeof(hp), None, None))
si = SIX()
si.si.cb = c.sizeof(SIX)
si.attrs = c.cast(attrs, c.c_void_p)
pi = PI()
exe = Path(sys.argv[1]).resolve()
os.environ['AVIONIX_CONPTY_PASTE_TEST'] = '1'
command = c.create_unicode_buffer(f'"{exe}" terminal.conpty_multiline_paste')
ok(k.CreateProcessW(None, command, None, None, False, 0x80000, None, None, c.byref(si), c.byref(pi)))
k.CloseHandle(ir)
k.CloseHandle(ow)
output = bytearray()
sent = False
exit_code = w.DWORD(259)
try:
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        available = w.DWORD()
        if k.PeekNamedPipe(or_, None, 0, None, c.byref(available), None) and available.value:
            buf = c.create_string_buffer(available.value)
            read = w.DWORD()
            ok(k.ReadFile(or_, buf, len(buf), c.byref(read), None))
            output.extend(buf.raw[:read.value])
        if not sent and b'AVIONIX_PASTE_READY' in output:
            # Paste travels through ConPTY, followed by actual modified key records.
            payload = b'\x1b[200~Aurora\r\n\r\nRifts\r\nCosmos\x1b[201~\x1b[13;28;13;1;16;1_\x1b[13;28;13;1;0;1_'
            written = w.DWORD()
            ok(k.WriteFile(iw, payload, len(payload), c.byref(written), None))
            assert written.value == len(payload)
            sent = True
        ok(k.GetExitCodeProcess(pi.process, c.byref(exit_code)))
        if exit_code.value != 259:
            time.sleep(.1)
            if k.PeekNamedPipe(or_, None, 0, None, c.byref(available), None) and available.value:
                buf = c.create_string_buffer(available.value)
                read = w.DWORD()
                ok(k.ReadFile(or_, buf, len(buf), c.byref(read), None))
                output.extend(buf.raw[:read.value])
            break
        time.sleep(.02)
    assert sent and exit_code.value == 0, (sent, exit_code.value, output[-3000:])
    print('PASS ConPTY multiline paste, Shift+Enter, and Enter')
finally:
    if exit_code.value == 259:
        k.TerminateProcess(pi.process, 1)
    k.CloseHandle(iw)
    k.ClosePseudoConsole(hp)
    k.CloseHandle(or_)
    k.CloseHandle(pi.thread)
    k.CloseHandle(pi.process)
    k.DeleteProcThreadAttributeList(attrs)
