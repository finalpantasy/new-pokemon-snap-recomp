import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
"""Read-only helpers for static knob hunting in the v262144 main ELF (module base 0). Source only."""
import struct, re, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
ELF = _ROOT+'/analysis/import/v262144-main.elf'
data = open(ELF, 'rb').read()
# segments: (vaddr, fileoff, filesz, memsz)
SEGS = [(0x0, 0x1000, 0xa38e40), (0xa39000, 0xa3a000, 0x227bc4), (0xc61000, 0xc62000, 0x6a908)]
def rd(addr, n):
    for va, fo, sz in SEGS:
        if va <= addr < va + sz:
            return data[fo + addr - va: fo + addr - va + n]
    return b'\0' * n
def u32(a): return struct.unpack('<I', rd(a, 4))[0]
def u64(a): return struct.unpack('<Q', rd(a, 8))[0]
def f32(a): return struct.unpack('<f', rd(a, 4))[0]
def cstr(a, n=200):
    b = rd(a, n); return b.split(b'\0')[0].decode('latin1')
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = False; md.skipdata = True
def dis(start, end):
    return list(md.disasm(rd(start, end - start), start))

def all_insns():
    """cached full-.text disassembly as list"""
    global _ALL
    try: return _ALL
    except NameError: pass
    _ALL = list(md.disasm(rd(0, 0xa365b0), 0))
    return _ALL

def nvn_slots(loader):
    """map proc name -> bss slot address by scanning a getProcAddress loader"""
    out = {}; name = None; pg = {}
    for i in md.disasm(rd(loader, 0x4000), loader):
        if i.mnemonic == 'adrp': pg[i.op_str.split(',')[0]] = int(i.op_str.split('#')[1], 16)
        if i.mnemonic == 'add' and i.op_str.startswith('x1, x1, #'):
            name = cstr(pg['x1'] + int(i.op_str.split('#')[1], 16))
        if i.mnemonic == 'str' and name and i.op_str.startswith('x0, [x8, #'):
            off = int(i.op_str.split('#')[1].rstrip(']'), 16)
            out[name] = pg['x8'] + off; name = None
        if i.mnemonic == 'ret': break
    return out

def nvn_got_map(loader):
    """GOT-indirect loader variant: proc name -> GOT entry address"""
    out = {}; name = None; pg = {}; got = None
    for i in md.disasm(rd(loader, 0x4000), loader):
        m = i.mnemonic; o = i.op_str
        if m == 'adrp': pg[o.split(',')[0]] = int(o.split('#')[1], 16)
        elif m == 'add' and o.startswith('x1, x1, #'):
            name = cstr(pg['x1'] + int(o.split('#')[1], 16))
        elif m == 'ldr' and o.startswith('x8, [x8, #'):
            got = pg['x8'] + int(o.split('#')[1].rstrip(']'), 16)
            if name: out[name] = got; name = None
        elif m == 'ret': break
    return out

def find_loads(addr_map, lo=0, hi=0xa365b0):
    """addr_map: {address: label}. Return list of (site, label) for adrp+ldr/add of those addresses."""
    res = []; pg = {}
    for i in md.disasm(rd(lo, hi - lo), lo):
        m = i.mnemonic; o = i.op_str
        if m == 'adrp':
            r, v = o.split(', #'); pg[r] = int(v, 16)
        elif m in ('ldr', 'add') and '[' in o or m == 'add':
            try:
                if m == 'ldr':
                    r = o.split('[')[1].split(',')[0].rstrip(']'); off = int(o.split('#')[1].rstrip(']'), 16) if '#' in o.split('[')[1] else 0
                else:
                    p = o.split(', '); r = p[1]; off = int(p[2].lstrip('#'), 16)
                if r in pg:
                    a = pg[r] + off
                    if a in addr_map: res.append((i.address, addr_map[a]))
            except Exception: pass
    return res

def callers(target):
    """sites of bl/b to target"""
    res = []
    for i in md.disasm(rd(0, 0xa365b0), 0):
        if i.mnemonic in ('bl', 'b') and i.op_str == '#0x%x' % target: res.append(i.address)
    return res

_XR = None
def build_xrefs():
    """map address -> [sites] for every adrp+add / adrp+ldr(literal address) pair in .text"""
    global _XR
    if _XR is not None: return _XR
    import collections
    _XR = collections.defaultdict(list); pg = {}
    for i in md.disasm(rd(0, 0xa365b0), 0):
        m = i.mnemonic; o = i.op_str
        if m == 'adrp':
            r, v = o.split(', #'); pg[r] = int(v, 16)
        elif m == 'add' and o.count('#') == 1 and not o.startswith('sp'):
            p = o.split(', ')
            if len(p) == 3 and p[1] in pg and p[2].startswith('#'):
                _XR[pg[p[1]] + int(p[2][1:], 16)].append(i.address)
    return _XR

def func_of(addr):
    import bisect
    global _FS
    try: _FS
    except NameError:
        _FS = []
        for l in open(_ROOT+'/analysis/maps/functions_final.tsv'):
            if l.startswith('#'): continue
            p = l.rstrip('\n').split('\t')
            if len(p) > 2 and p[2] in ('function', 'thunk'): _FS.append((int(p[0], 16), int(p[1], 16)))
        _FS.sort()
    k = bisect.bisect_right(_FS, (addr, 1 << 60)) - 1
    if k >= 0 and _FS[k][0] <= addr < _FS[k][1]: return _FS[k]
    return None
