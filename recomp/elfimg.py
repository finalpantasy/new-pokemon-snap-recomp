# Via TotK Recomp recomp/nso.py @ 7a37fc3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), itself adapted from LAS Recomp
# tools/nso.py @ da35b83 (MIT, Copyright (c) 2026 Peiikai). Rewritten for the Snap prototype: the input is the ELF64
# that scripts/nso2elf.py wrote (segments at module base 0, so addresses equal module offsets), not a compressed NSO.
"""ELF64 loader with the interface module.py expects: memory image, text range, .eh_frame_hdr, dynsyms, relocations."""
import struct
import sys
from collections import Counter

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_JMPREL, DT_GNU_HASH = 23, 0x6FFFFEF5
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027
PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME = 1, 2, 0x6474e550
PF_X = 1


class ElfImage:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:4] == b'\x7fELF' and raw[4] == 2 and raw[18:20] == b'\xb7\x00', path
        phoff, = struct.unpack_from('<Q', raw, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', raw, 0x36)
        loads, self.dynamic, self.eh_start = [], 0, 0
        for i in range(phnum):
            t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', raw, phoff + i * phentsize)
            if t == PT_LOAD:
                loads.append((va, off, fsz, msz, fl))
            elif t == PT_DYNAMIC:
                self.dynamic = va
            elif t == PT_GNU_EH_FRAME:
                self.eh_start = va
        loads.sort()
        self.size = (max(v + f for v, _, f, _, _ in loads) + 0xFFF) & ~0xFFF   # file-backed extent
        self.mem_end = (max(v + m for v, _, _, m, _ in loads) + 0xFFF) & ~0xFFF  # with .bss
        img = bytearray(self.size)
        for va, off, fsz, msz, fl in loads:
            img[va:va + fsz] = raw[off:off + fsz]
        self.image = img
        self.text = next((va, fsz) for va, _, fsz, _, fl in loads if fl & PF_X)
        self.rodata = loads[1][0], loads[1][2]
        self.data = loads[2][0], loads[2][2]
        self.bss_size = self.mem_end - self.size
        self.build_id = ''
        self._parse_dynamic()

    def u32(self, off): return struct.unpack_from('<I', self.image, off)[0]
    def u64(self, off): return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_dynamic(self):
        dyn, needed, off = {}, [], self.dynamic
        while True:
            tag, val = self.u64(off), self.u64(off + 8)
            off += 16
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        self.strtab = dyn.get(DT_STRTAB, 0)
        self.needed = [self.cstr(self.strtab + n) for n in needed]
        self.syms = []
        n = self.u32(dyn[DT_HASH] + 4) if DT_HASH in dyn else 0   # nchain = dynsym count
        for i in range(n):
            o = dyn[DT_SYMTAB] + i * 24
            name, info, other, shndx = struct.unpack_from('<IBBH', self.image, o)
            value, size = struct.unpack_from('<QQ', self.image, o + 8)
            self.syms.append((self.cstr(self.strtab + name), info, shndx, value, size))
        self.relocs = []
        for base_tag, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if base_tag in dyn:
                b = dyn[base_tag]
                for i in range(dyn[size_tag] // 24):
                    r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, b + i * 24)
                    self.relocs.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))

    def cstr(self, off):
        return self.image[off:self.image.index(b'\0', off)].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]

    def relocated(self, base):
        """Copy of the image as loaded at base: RELATIVE and defined-ABS64 relocations applied. JUMP_SLOT/GLOB_DAT
        (imports) are left as is; the tests never follow them."""
        img = bytearray(self.image)
        for r_off, typ, sym, add in self.relocs:
            if r_off + 8 > len(img):
                continue
            if typ == R_AARCH64_RELATIVE:
                img[r_off:r_off + 8] = (base + add).to_bytes(8, 'little')
            elif typ == R_AARCH64_ABS64 and self.syms[sym][2]:
                img[r_off:r_off + 8] = ((base + self.syms[sym][3] + add) & (2**64 - 1)).to_bytes(8, 'little')
        return img


if __name__ == '__main__':
    n = ElfImage(sys.argv[1])
    print(f'text {n.text[0]:#x}+{n.text[1]:#x} image {n.size:#x} mem {n.mem_end:#x} eh_hdr {n.eh_start:#x} '
          f'syms {len(n.syms)} imports {len(n.imports())} relocs {dict(Counter(r[1] for r in n.relocs))}')
