import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
import sys; sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from knobs_lib import *
a = int(sys.argv[1], 16); b = int(sys.argv[2], 16)
ann = {}
if len(sys.argv) > 3:
    import pickle
a_slots = nvn_slots(0x90a0c0); inv = {v: k for k, v in a_slots.items()}
pg = {}
for i in dis(a, b):
    note = ''
    o = i.op_str
    if i.mnemonic == 'adrp':
        r, v = o.split(', #'); pg[r] = int(v, 16)
    elif i.mnemonic in ('ldr', 'add', 'str', 'ldrb', 'ldrh', 'ldrsw'):
        try:
            if i.mnemonic == 'add': p = o.split(', '); r = p[1]; off = int(p[2].lstrip('#'), 16)
            else: r = o.split('[')[1].split(',')[0].rstrip(']'); off = int(o.split('#')[1].rstrip(']'), 16) if '#' in o.split('[')[1] else 0
            if r in pg:
                ad = pg[r] + off
                if ad in inv: note = '; ' + inv[ad]
                elif ad < 0xcc0000 and 0xa39000 <= ad:
                    note = '; str "%s"' % cstr(ad, 60) if i.mnemonic == 'add' else '; [0x%x]=%s' % (ad, rd(ad, 8).hex())
                else: note = '; =0x%x' % ad
        except Exception: pass
    print('%x: %s %s %s' % (i.address, i.mnemonic, o, note))
