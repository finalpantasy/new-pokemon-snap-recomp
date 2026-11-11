import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
"""Usage: knobs_levelb_keys.py OUT.tsv  -- count identifier-like strings across all levelb files (count, string, first file)."""
import os, re, collections, sys
root = _ROOT+'/extracted/drp-v262144'
c = collections.Counter(); ex = {}
for d, _, fs in os.walk(root):
    for f in fs:
        if f.endswith('.levelb'):
            b = open(os.path.join(d, f), 'rb').read()
            for s in set(re.findall(rb'[A-Za-z_][A-Za-z0-9_\.:/ %-]{3,60}(?=\x00)', b)):
                c[s.decode()] += 1; ex.setdefault(s.decode(), f)
open(sys.argv[1], 'w').write('\n'.join('%d\t%s\t%s' % (n, s, ex[s]) for s, n in c.most_common()))
