"""Print string tokens of matinstb files in order (read-only). Usage: python -I matinst_dump.py <file>..."""
import sys, struct
for p in sys.argv[1:]:
    b = open(p, 'rb').read()
    print('==', p.split('//')[-1] if False else p.replace(chr(92), '/').split('/')[-1], len(b))
    i = 0
    out = []
    while i + 5 < len(b):
        n = struct.unpack_from('<I', b, i)[0]
        if 3 <= n < 200 and i + 4 + n < len(b) and b[i + 4 + n] == 0 and all(32 <= c < 127 for c in b[i + 4:i + 4 + n]):
            out.append((i, b[i + 4:i + 4 + n].decode()))
            i = (i + 4 + n + 1 + 3) & ~3
        else:
            i += 4
    for o, s in out:
        print('  %04x %s' % (o, s))
