import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
"""List numeric immediates / float constants and string refs inside an address range."""
import sys; sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from knobs_lib import *
import struct
lo = int(sys.argv[1], 16); hi = int(sys.argv[2], 16); pg = {}
for i in md.disasm(rd(lo, hi - lo), lo):
    m = i.mnemonic; o = i.op_str; note = None
    if m == 'adrp':
        r, v = o.split(', #'); pg[r] = int(v, 16)
    elif m == 'add' and o.count('#') == 1 and not o.startswith('sp'):
        p = o.split(', ')
        if len(p) == 3 and p[1] in pg:
            a = pg[p[1]] + int(p[2][1:], 16); s = cstr(a, 60)
            if 0xa39000 <= a < 0xc60000 and len(s) > 3 and s.isprintable(): note = 'str "%s"' % s
    elif m == 'ldr' and o.startswith(('s', 'd')) and '[x' in o and o.split('[')[1].split(',')[0] in pg and '#' in o:
        r = o.split('[')[1].split(',')[0]; a = pg[r] + int(o.split('#')[1].rstrip(']'), 16)
        note = 'f32=%g' % f32(a) if o[0] == 's' else 'f64=%g' % struct.unpack('<d', rd(a, 8))[0]
    elif m == 'fmov' and '#' in o: note = 'fimm ' + o
    elif m in ('mov', 'movk') and '#' in o and not o.startswith(('x29', 'sp')):
        tok = o.split('#')[1].split(',')[0]; v = int(tok, 16) if '0x' in tok else int(tok)
        if o.startswith('w') and (m == 'movk' or abs(v) >= 2): note = 'imm %s' % o
    if note: print('%x: %s %s ; %s' % (i.address, m, o, note))
