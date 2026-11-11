#!/usr/bin/env python3
"""Parse every unpacked .luo as standard Lua 5.1 bytecode (LE, size_t 8, int 4, number 8); report anomalies."""
import struct, sys, collections
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
class R:
    def __init__(s, d): s.d, s.o = d, 12
    def u32(s): v = struct.unpack_from("<I", s.d, s.o)[0]; s.o += 4; return v
    def u64(s): v = struct.unpack_from("<Q", s.d, s.o)[0]; s.o += 8; return v
    def u8(s): v = s.d[s.o]; s.o += 1; return v
    def string(s):
        n = s.u64(); b = s.d[s.o:s.o + n]; s.o += n; return b
def func(r, st, depth=0):
    src = r.string(); st["src"][len(src) > 0] += 1
    r.u32(); r.u32(); nups = r.u8(); r.u8(); r.u8(); r.u8()
    n = r.u32(); r.o += 4 * n; st["instr"] += n
    k = r.u32()
    for _ in range(k):
        t = r.u8(); st["k%d" % t] += 1
        if t == 1: r.u8()
        elif t == 3: r.o += 8
        elif t == 4: r.string()
        elif t != 0: raise ValueError("bad const type %d" % t)
    p = r.u32()
    for _ in range(p): func(r, st, depth + 1)
    st["funcs"] += 1
    li = r.u32(); r.o += 4 * li; st["lineinfo"] += li
    lv = r.u32()
    for _ in range(lv): r.string(); r.u32(); r.u32()
    st["locvars"] += lv
    uv = r.u32()
    for _ in range(uv): r.string()
    st["upvalue_names"] += uv
st = collections.Counter(); st["src"] = collections.Counter(); bad = 0; n = 0
for p in sorted((ROOT / "extracted/drp-v262144").rglob("*.luo")):
    d = p.read_bytes(); n += 1; r = R(d)
    try:
        func(r, st)
        if r.o != len(d): bad += 1; print("trailing", p.name, r.o, len(d))
    except Exception as e:
        bad += 1; print("parse fail", p.name, e)
print("files", n, "bad", bad, dict(st))
