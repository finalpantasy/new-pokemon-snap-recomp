"""LMBB reader and prepend-patcher for the game's Lumen behavior files (`<scene>.lmb`, FlatBuffers, file id "LMBB").

A scene is <scene>.lm (Lumen/Flash layout: movie clips, frame labels, text fields), <scene>.lme and <scene>.lmb. The .lmb is
the screen's whole UI behavior, as data:

  Root    f0 [Macro]   named action lists        f1 [State]  screen states (pages)        f2 str  first state
  State   f0 name      f1 [Action] run on enter  f2 [Element] focusable buttons/areas
          f6 u32       f7 str (next state)       f8 [Group]  focus groups (d-pad links)   f9 str  first group
  Element f0 name (the event the game's C++ receives on decide)   f1 [Action]   f3 hit/anchor clip path
          f5 u32   f11 str group   f13/f14 i32 (25, 25 on option rows)   (other fields: see dump; kept as-is)
  Group   f0 name  f1 [Link{f0 u32 dir (1 up, 2 down; 3/4 left/right in some scenes), f1 str target group}]  f2 str
  Action  f0 u32 trigger (10 enter, 3 focus, 4 unfocus, 1 decide, 2 leave ...)   f1 u32 op (1 goto label, 7 set text,
          10 play sound, 12 wait ...)   f2 str clip path   f3 str label/text/sound   f4 str
Text and paths may hold `{Var}` (variables the C++ sets per state, e.g. {Line6Type}) or `{File.Label}` (MSBT message).

The patcher never rewrites existing objects. FlatBuffers offsets point forward, so new objects are written in front of the old
data (right after the 8-byte header) and the old bytes are shifted by a multiple of 16; offsets between old objects stay
valid, new objects point forward into the old ones, and a new root table replaces the old root (which stays as dead data).

  python -I scripts/ui/lmb.py dump <file.lmb> [state]       # readable dump (game text: keep it local)
Only code lives here; no game data.
"""
import struct, sys

def U32(b, o): return struct.unpack_from('<I', b, o)[0]
def I32(b, o): return struct.unpack_from('<i', b, o)[0]
def U16(b, o): return struct.unpack_from('<H', b, o)[0]

ACTION = {0: 'u32', 1: 'u32', 2: 'str', 3: 'str', 4: 'str'}
LINK = {0: 'u32', 1: 'str'}
GROUP = {0: 'str', 1: ('vec', LINK), 2: 'str'}
MACRO = {0: 'str', 1: ('vec', ACTION)}
ELEMENT = {0: 'str', 1: ('vec', ACTION), 2: 'str', 3: 'str', 4: 'str', 9: 'str', 10: 'u8', 11: 'str', 12: 'u8',
           13: 'i32', 14: 'i32', 15: 'u32', 17: 'u32', 18: 'u8', 19: 'u8'}
STATE = {0: 'str', 1: ('vec', ACTION), 2: ('vec', ELEMENT), 3: 'str', 6: 'u32', 7: 'str', 8: ('vec', GROUP), 9: 'str', 10: 'u8'}
ROOT = {0: ('vec', MACRO), 1: ('vec', STATE), 2: 'str'}


class Ref:
    """A pointer to an existing object of the original file (absolute offset in the original)."""
    def __init__(self, off):
        self.off = off


class Raw:
    """A field copied byte-for-byte (scalar of unknown meaning) or by reference (offset field of unknown type)."""
    def __init__(self, data=None, ref=None):
        self.data, self.ref = data, ref


class Table:
    def __init__(self, fields):
        self.fields = fields      # {slot: value}; value: int (with 'u32'/'i32'/'u8' type in .types), str, Table, list, Ref, Raw
        self.types = {}


class Lmb:
    def __init__(self, data):
        self.b = bytes(data)
        if self.b[4:8] != b'LMBB':
            raise ValueError('not an LMBB file')
        self.root = U32(self.b, 0)

    # ---------------------------------------------------------------- reading
    def slots(self, t):
        b = self.b
        vt = t - I32(b, t)
        vs, ts = U16(b, vt), U16(b, vt + 2)
        offs = {f: U16(b, vt + 4 + 2 * f) for f in range((vs - 4) // 2)}
        offs = {f: o for f, o in offs.items() if o}
        return offs, ts

    def field_loc(self, t, f):
        offs, _ = self.slots(t)
        return t + offs[f] if f in offs else None

    def deref(self, loc):
        return loc + U32(self.b, loc)

    def string(self, s):
        n = U32(self.b, s)
        return self.b[s + 4:s + 4 + n].decode('utf-8')

    def vec(self, v):
        return [v + 4 + 4 * i for i in range(U32(self.b, v))]

    def get(self, t, f, kind):
        loc = self.field_loc(t, f)
        if loc is None:
            return None
        if kind == 'u32':
            return U32(self.b, loc)
        if kind == 'i32':
            return I32(self.b, loc)
        if kind == 'u8':
            return self.b[loc]
        if kind == 'str':
            return self.string(self.deref(loc))
        if isinstance(kind, tuple) and kind[0] == 'vec':
            return [self.deref(e) for e in self.vec(self.deref(loc))]
        if kind == 'table':
            return self.deref(loc)
        raise ValueError(kind)

    def states(self):
        return {self.get(s, 0, 'str'): s for s in self.get(self.root, 1, ROOT[1])}

    def elements(self, state):
        return {self.get(e, 0, 'str'): e for e in self.get(state, 2, STATE[2]) or []}

    def groups(self, state):
        return {self.get(g, 0, 'str'): g for g in self.get(state, 8, STATE[8]) or []}

    def action(self, a):
        return tuple(self.get(a, f, k) for f, k in sorted(ACTION.items()))

    def to_table(self, t, schema):
        """Decode a table for re-emitting: schema fields become values, children are referenced (Ref) not copied,
        unknown fields are copied raw (4-aligned 4-byte fields that point at a valid object are kept as references)."""
        offs, ts = self.slots(t)
        out = Table({})
        order = sorted(offs.items(), key=lambda kv: kv[1])
        for i, (f, o) in enumerate(order):
            size = (order[i + 1][1] if i + 1 < len(order) else ts) - o
            loc = t + o
            k = schema.get(f)
            if k in ('u32', 'i32', 'u8'):
                out.fields[f] = self.get(t, f, k)
                out.types[f] = k
            elif k is not None:
                out.fields[f] = Ref(self.deref(loc))
                out.types[f] = 'off'
            else:
                out.fields[f] = Raw(data=self.b[loc:loc + min(size, 8)])
                out.types[f] = 'raw'
        return out

    # ---------------------------------------------------------------- dump
    def dump(self, only=None, out=sys.stdout):
        w = out.write
        w(f'first state: {self.get(self.root, 2, "str")}\n')
        for m in self.get(self.root, 0, ROOT[0]) or []:
            w(f'macro {self.get(m, 0, "str")}: {len(self.get(m, 1, MACRO[1]) or [])} actions\n')
        for name, s in self.states().items():
            if only and name != only:
                continue
            w(f'\n== state {name} @{s:#x} f6={self.get(s, 6, "u32")} f7={self.get(s, 7, "str")} first_group={self.get(s, 9, "str")}\n')
            for a in self.get(s, 1, STATE[1]) or []:
                w(f'  enter {self.action(a)}\n')
            for gname, g in self.groups(s).items():
                links = [(self.get(l, 0, 'u32'), self.get(l, 1, 'str')) for l in self.get(g, 1, GROUP[1]) or []]
                w(f'  group {gname} {links} f2={self.get(g, 2, "str")}\n')
            for ename, e in self.elements(s).items():
                offs, ts = self.slots(e)
                extra = {f: self.b[e + o:e + o + 4].hex() for f, o in offs.items() if f not in (0, 1, 3, 11)}
                w(f'  element {ename} @{e:#x} anchor={self.get(e, 3, "str")} group={self.get(e, 11, "str")} other={extra}\n')
                for a in self.get(e, 1, ELEMENT[1]) or []:
                    w(f'    {self.action(a)}\n')


class Patcher:
    """Builds new tables in front of the original data. Values: int (typed via types), str, list (vector of tables or
    strings), Table, Ref (original object), Raw."""
    def __init__(self, lmb):
        self.l = lmb
        self.objs = []

    def build(self, root):
        b = self.l.b
        front = bytearray()
        fixups = []        # (pos in front, target) target = ('new', obj_id) | ('old', off)
        placed = {}        # id(obj) -> pos in front
        strings = []       # (pos of uoffset field, str) placed after all tables

        def align(n):
            while len(front) % n:
                front.append(0)

        def put_table(t):
            items = sorted(t.fields.items())
            sizes = {}
            for f, v in items:
                k = t.types.get(f)
                if k == 'u8':
                    sizes[f] = 1
                elif k == 'raw' and v.data is not None:
                    sizes[f] = len(v.data)
                else:
                    sizes[f] = 4
            nslots = (max(t.fields) + 1) if t.fields else 0
            # table body layout: soffset, then fields largest first
            layout, pos = {}, 4
            for f, _ in sorted(items, key=lambda kv: -sizes[kv[0]]):
                sz = sizes[f]
                al = 4 if sz >= 4 else 1
                pos = (pos + al - 1) // al * al
                layout[f] = pos
                pos += sz
            tsize = (pos + 3) // 4 * 4
            vt = struct.pack('<HH', 4 + 2 * nslots, tsize) + b''.join(
                struct.pack('<H', layout.get(f, 0)) for f in range(nslots))
            align(2)
            vpos = len(front)
            front.extend(vt)
            align(4)
            tpos = len(front)
            body = bytearray(tsize)
            struct.pack_into('<i', body, 0, tpos - vpos)
            front.extend(body)
            children = []
            for f, v in items:
                at = tpos + layout[f]
                k = t.types.get(f)
                if k == 'u32':
                    struct.pack_into('<I', front, at, v & 0xffffffff)
                elif k == 'i32':
                    struct.pack_into('<i', front, at, v)
                elif k == 'u8':
                    front[at] = v & 0xff
                elif isinstance(v, Raw):
                    front[at:at + len(v.data)] = v.data
                elif isinstance(v, Ref):
                    fixups.append((at, ('old', v.off)))
                elif isinstance(v, str):
                    strings.append((at, v))
                elif isinstance(v, (Table, list)):
                    fixups.append((at, ('new', id(v))))
                    children.append(v)
                else:
                    raise TypeError((f, v))
            placed[id(t)] = tpos
            return children

        def put_vec(v):
            align(4)
            pos = len(front)
            front.extend(struct.pack('<I', len(v)))
            children = []
            for i, e in enumerate(v):
                at = pos + 4 + 4 * i
                front.extend(b'\0\0\0\0')
                if isinstance(e, Ref):
                    fixups.append((at, ('old', e.off)))
                elif isinstance(e, str):
                    strings.append((at, e))
                else:
                    fixups.append((at, ('new', id(e))))
                    children.append(e)
            placed[id(v)] = pos
            return children

        # preorder: every object is written before the objects it points to (offsets must point forward)
        stack = [root]
        seen = set()
        while stack:
            o = stack.pop(0)
            if id(o) in seen:
                raise ValueError('a new object is referenced twice; build a copy instead')
            seen.add(id(o))
            kids = put_table(o) if isinstance(o, Table) else put_vec(o)
            stack = kids + stack
        for at, s in strings:
            align(4)
            pos = len(front)
            enc = s.encode('utf-8')
            front.extend(struct.pack('<I', len(enc)) + enc + b'\0')
            fixups.append((at, ('abs', pos)))
        while (len(front)) % 16:
            front.append(0)
        delta = len(front)
        base = 8                          # front region starts after [root uoffset][LMBB]
        out = bytearray(b[:8]) + front + b[8:]
        for at, (kind, tgt) in fixups:
            src = base + at
            if kind == 'old':
                dst = tgt + delta
            elif kind == 'new':
                dst = base + placed[tgt]
            else:
                dst = base + tgt
            if dst <= src:
                raise ValueError('backward offset')
            struct.pack_into('<I', out, src, dst - src)
        struct.pack_into('<I', out, 0, base + placed[id(root)])
        return bytes(out)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == 'dump':
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        Lmb(open(sys.argv[2], 'rb').read()).dump(sys.argv[3] if len(sys.argv) > 3 else None)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
