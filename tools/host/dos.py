"""INT 21h services, mapped onto a host directory that plays drive C:."""
import os
import struct
import time

from unicorn.x86_const import *

GAME_DIR = 'C:\\BIFI'


def _host_path(m, name):
    name = name.replace('/', '\\')
    if len(name) > 1 and name[1] == ':':
        name = name[2:]
    if not name.startswith('\\'):
        cwd = m.cwd[2:] if m.cwd[1:2] == ':' else m.cwd
        name = cwd.rstrip('\\') + '\\' + name
    parts = [p for p in name.split('\\') if p]
    path = m.root
    for p in parts:
        # case-insensitive lookup
        cand = None
        if os.path.isdir(path):
            for e in os.listdir(path):
                if e.upper() == p.upper():
                    cand = e
                    break
        path = os.path.join(path, cand or p.upper())
    return path


def _ret_ax(m, v, ok=True):
    m.setreg(UC_X86_REG_AX, v & 0xFFFF)
    m.set_cf(not ok)


def int_20(m, ah):
    raise_exit(m, 0)


def raise_exit(m, code):
    m.exit_code = code
    m.uc.emu_stop()


def int_21(m, ah):
    ax = m.reg(UC_X86_REG_AX)
    bx, cx, dx = m.reg(UC_X86_REG_BX), m.reg(UC_X86_REG_CX), m.reg(UC_X86_REG_DX)
    ds, es = m.reg(UC_X86_REG_DS), m.reg(UC_X86_REG_ES)
    al = ax & 0xFF
    if ah == 0x30:                                  # DOS version 5.0
        m.setreg(UC_X86_REG_AX, 0x0005)
        m.setreg(UC_X86_REG_BX, 0)
    elif ah == 0x25:                                # set vector
        m.wr(al * 4, struct.pack('<HH', dx, ds))
    elif ah == 0x35:                                # get vector
        off, seg = struct.unpack('<HH', m.rd(al * 4, 4))
        m.setreg(UC_X86_REG_BX, off)
        m.setreg(UC_X86_REG_ES, seg)
    elif ah == 0x33:                                # ctrl-break state
        m.setreg(UC_X86_REG_DX, dx & 0xFF00)
    elif ah == 0x1A:
        m.dta = m.lin(ds, dx)
    elif ah == 0x2F:
        m.setreg(UC_X86_REG_ES, m.dta >> 4)
        m.setreg(UC_X86_REG_BX, m.dta & 0xF)
    elif ah == 0x34:                                # InDOS flag address
        m.setreg(UC_X86_REG_ES, 0xF000)
        m.setreg(UC_X86_REG_BX, 0x1FF)
    elif ah == 0x2A:
        t = time.localtime()
        m.setreg(UC_X86_REG_CX, t.tm_year)
        m.setreg(UC_X86_REG_DX, t.tm_mon << 8 | t.tm_mday)
        m.setreg(UC_X86_REG_AX, (t.tm_wday + 1) % 7)
    elif ah == 0x2C:
        t = time.time()
        lt = time.localtime(t)
        m.setreg(UC_X86_REG_CX, lt.tm_hour << 8 | lt.tm_min)
        m.setreg(UC_X86_REG_DX, lt.tm_sec << 8 | int(t * 100) % 100)
    elif ah == 0x3D:                                # open
        path = _host_path(m, m.cstring(m.lin(ds, dx)))
        mode = al & 3
        try:
            f = open(path, 'r+b' if mode else 'rb')
        except OSError:
            m.log(f'open failed: {path}')
            return _ret_ax(m, 2, False)
        h = m.next_handle
        m.next_handle += 1
        m.files[h] = f
        m.log(f'open {path} -> {h}')
        _ret_ax(m, h)
    elif ah == 0x3C:                                # create
        path = _host_path(m, m.cstring(m.lin(ds, dx)))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        f = open(path, 'w+b')
        h = m.next_handle
        m.next_handle += 1
        m.files[h] = f
        m.log(f'create {path} -> {h}')
        _ret_ax(m, h)
    elif ah == 0x3E:                                # close
        f = m.files.pop(bx, None)
        if f:
            f.close()
        _ret_ax(m, 0, f is not None or bx < 5)
    elif ah == 0x3F:                                # read
        f = m.files.get(bx)
        if not f:
            return _ret_ax(m, 0 if bx < 5 else 6, bx < 5)
        data = f.read(cx)
        m.wr(m.lin(ds, dx), data)
        _ret_ax(m, len(data))
    elif ah == 0x40:                                # write
        if bx in (1, 2):
            m.log('stdout: ' + m.rd(m.lin(ds, dx), cx).decode('cp437', 'replace'))
            return _ret_ax(m, cx)
        f = m.files.get(bx)
        if not f:
            return _ret_ax(m, 6, False)
        f.write(m.rd(m.lin(ds, dx), cx))
        _ret_ax(m, cx)
    elif ah == 0x42:                                # seek
        f = m.files.get(bx)
        if not f:
            return _ret_ax(m, 6, False)
        off = struct.unpack('<i', struct.pack('<I', cx << 16 | dx))[0]
        pos = f.seek(off, al)
        m.setreg(UC_X86_REG_DX, pos >> 16)
        _ret_ax(m, pos & 0xFFFF)
    elif ah == 0x41:                                # delete
        path = _host_path(m, m.cstring(m.lin(ds, dx)))
        try:
            os.remove(path)
            _ret_ax(m, 0)
        except OSError:
            _ret_ax(m, 2, False)
    elif ah == 0x39:                                # mkdir
        path = _host_path(m, m.cstring(m.lin(ds, dx)))
        os.makedirs(path, exist_ok=True)
        _ret_ax(m, 0)
    elif ah == 0x3A:                                # rmdir
        path = _host_path(m, m.cstring(m.lin(ds, dx)))
        try:
            os.rmdir(path)
        except OSError:
            pass
        _ret_ax(m, 0)
    elif ah == 0x3B:                                # chdir
        m.cwd = m.cstring(m.lin(ds, dx))
        _ret_ax(m, 0)
    elif ah == 0x47:                                # get cwd
        m.wr(m.lin(ds, m.reg(UC_X86_REG_SI)), m.cwd[3:].encode() + b'\0')
        _ret_ax(m, 0)
    elif ah == 0x19:
        m.setreg(UC_X86_REG_AX, (ax & 0xFF00) | 2)
    elif ah == 0x0E:
        m.setreg(UC_X86_REG_AX, (ax & 0xFF00) | 3)
    elif ah == 0x44:                                # IOCTL
        if al == 0:
            m.setreg(UC_X86_REG_DX, 0x80D3 if bx < 5 else 0x0002)
            _ret_ax(m, ax)
        else:
            _ret_ax(m, 0)
    elif ah == 0x48:                                # allocate
        seg, avail = _alloc(m, bx)
        if seg:
            _ret_ax(m, seg)
        else:
            m.setreg(UC_X86_REG_BX, avail)
            _ret_ax(m, 8, False)
    elif ah == 0x49:                                # free
        m.mem_blocks.pop(es, None)
        _ret_ax(m, 0)
    elif ah == 0x4A:                                # resize
        ok, avail = _resize(m, es, bx)
        if ok:
            _ret_ax(m, 0)
        else:
            m.setreg(UC_X86_REG_BX, avail)
            _ret_ax(m, 8, False)
    elif ah == 0x4C:
        m.log(f'exit {al}')
        raise_exit(m, al)
    elif ah == 0x4E:                                # find first
        pattern = m.cstring(m.lin(ds, dx))
        m.find_results = _find(m, pattern)
        m.log(f"find {pattern!r} -> {m.find_results}")
        _find_next(m)
    elif ah == 0x4F:                                # find next
        _find_next(m)
    elif ah == 0x36:                                # free disk space
        m.setreg(UC_X86_REG_AX, 64)
        m.setreg(UC_X86_REG_BX, 0x4000)
        m.setreg(UC_X86_REG_CX, 512)
        m.setreg(UC_X86_REG_DX, 0x8000)
    elif ah == 0x0B:
        m.setreg(UC_X86_REG_AX, ax & 0xFF00)
    else:
        m.log(f'unhandled INT 21 AH={ah:02x} AX={ax:04x}')
        _ret_ax(m, 1, False)


def _free_ranges(m):
    used = sorted(m.mem_blocks.items())
    start = 0x0900
    for seg, size in used:
        if seg > start:
            yield start, seg - start
        start = max(start, seg + size + 1)
    if start < m.mem_top:
        yield start, m.mem_top - start


def _alloc(m, paras):
    best = 0
    for seg, size in _free_ranges(m):
        if size >= paras + 1:
            m.mem_blocks[seg + 1] = paras
            return seg + 1, 0
        best = max(best, size - 1)
    return 0, max(best, 0)


def _resize(m, seg, paras):
    if seg not in m.mem_blocks:
        return False, 0
    limit = m.mem_top - seg
    for other in sorted(m.mem_blocks):
        if other > seg:
            limit = other - 1 - seg
            break
    if paras <= limit:
        m.mem_blocks[seg] = paras
        return True, 0
    return False, limit


def _find(m, pattern):
    import fnmatch
    path = _host_path(m, pattern)
    folder, mask = os.path.dirname(path), os.path.basename(path).upper()
    if not os.path.isdir(folder):
        return []
    mask = mask.replace('*.*', '*')
    hits = []
    for e in sorted(os.listdir(folder)):
        if e.startswith('.'):
            continue
        if fnmatch.fnmatch(e.upper(), mask):
            hits.append(os.path.join(folder, e))
    return hits


def _find_next(m):
    if not getattr(m, 'find_results', None):
        return _ret_ax(m, 18, False)
    path = m.find_results.pop(0)
    st = os.stat(path)
    t = time.localtime(st.st_mtime)
    ftime = t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2
    fdate = (t.tm_year - 1980) << 9 | t.tm_mon << 5 | t.tm_mday
    rec = bytearray(43)
    rec[0x15] = 0x10 if os.path.isdir(path) else 0x20
    rec[0x16:0x1A] = struct.pack('<HH', ftime, fdate)
    rec[0x1A:0x1E] = struct.pack('<I', st.st_size)
    name = os.path.basename(path).upper().encode()[:12]
    rec[0x1E:0x1E + len(name)] = name
    m.wr(m.dta, rec)
    _ret_ax(m, 0)
