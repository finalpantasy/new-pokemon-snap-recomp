# SPDX-License-Identifier: MIT
# Minimal read-only FlatBuffers helpers (no schema): table field access by vtable slot.
import struct


class Tab:
    def __init__(self, buf, pos):
        self.b, self.p = buf, pos
        vt = pos - struct.unpack_from('<i', buf, pos)[0]
        self.vt = vt
        self.vsz = struct.unpack_from('<H', buf, vt)[0]

    def off(self, slot):
        o = 4 + slot * 2
        if o >= self.vsz:
            return 0
        v = struct.unpack_from('<H', self.b, self.vt + o)[0]
        return self.p + v if v else 0

    def has(self, slot):
        return self.off(slot) != 0

    def u8(self, slot, d=0):
        o = self.off(slot)
        return self.b[o] if o else d

    def u32(self, slot, d=0):
        o = self.off(slot)
        return struct.unpack_from('<I', self.b, o)[0] if o else d

    def f32(self, slot, d=0.0):
        o = self.off(slot)
        return struct.unpack_from('<f', self.b, o)[0] if o else d

    def _ref(self, slot):
        o = self.off(slot)
        return o + struct.unpack_from('<I', self.b, o)[0] if o else 0

    def string(self, slot):
        r = self._ref(slot)
        if not r:
            return None
        n = struct.unpack_from('<I', self.b, r)[0]
        return self.b[r + 4:r + 4 + n].decode('utf-8', 'replace')

    def table(self, slot):
        r = self._ref(slot)
        return Tab(self.b, r) if r else None

    def vec(self, slot):
        """vector of tables"""
        r = self._ref(slot)
        if not r:
            return []
        n = struct.unpack_from('<I', self.b, r)[0]
        out = []
        for i in range(n):
            e = r + 4 + 4 * i
            out.append(Tab(self.b, e + struct.unpack_from('<I', self.b, e)[0]))
        return out

    def svec(self, slot):
        r = self._ref(slot)
        if not r:
            return []
        n = struct.unpack_from('<I', self.b, r)[0]
        out = []
        for i in range(n):
            e = r + 4 + 4 * i
            s = e + struct.unpack_from('<I', self.b, e)[0]
            m = struct.unpack_from('<I', self.b, s)[0]
            out.append(self.b[s + 4:s + 4 + m].decode('utf-8', 'replace'))
        return out


def root(buf):
    return Tab(buf, struct.unpack_from('<I', buf, 0)[0])
