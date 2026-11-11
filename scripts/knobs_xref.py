import os as _os
_ROOT = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
"""Usage: knobs_xref.py REGEX...   (fullmatch on strings in work/v262144-main.strings-offsets.tsv; prints code xrefs)"""
import sys; sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from knobs_lib import *
import re
TSV = _ROOT+'/work/v262144-main.strings-offsets.tsv'
pats = sys.argv[1:]
xr = build_xrefs()
for l in open(TSV):
    a, s = l.rstrip('\n').split('\t', 1)
    if any(re.fullmatch(p, s) for p in pats):
        sites = xr.get(int(a, 16), [])
        print(a, s, [(hex(x), hex(func_of(x)[0]) if func_of(x) else None) for x in sites][:8])
