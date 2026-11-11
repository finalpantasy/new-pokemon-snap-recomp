#!/usr/bin/env python3
"""Run the game's own statically linked Oodle decoder (OodleLZ_Decompress, module offset 0x7240)
under the Unicorn CPU emulator.  Offline extraction tool only; not part of the port.

Needs the private venv:  tools\\venv-oodle\\Scripts\\python -I scripts\\oodle_unicorn.py --selftest
Reads  : analysis/import/v262144-main.elf  (game code; never copied anywhere)
No game bytes are embedded in this file.

Signature (confirmed from the DRP loader call site at 0x5e3b84):
  OodleLZ_Decompress(src, srcLen, dst, dstLen, fuzzSafe=1, checkCRC=0, verbosity=0, decBufBase=0,
                     [stack:] decBufSize=0, cb=0, cbUser=0, decMem=0, decMemSize=0, threadPhase=3)
  returns decompressed byte count (0 on error).  The DRP payload is the 4 bytes 'Oodl' followed by the
  stream; the loader passes src = payload+4, srcLen = csize-4.
"""
import struct
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_CODE, UcError)
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                 UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7,
                                 UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC,
                                 UC_ARM64_REG_TPIDR_EL0, UC_ARM64_REG_TPIDRRO_EL0, UC_ARM64_REG_CPACR_EL1)

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "analysis" / "import" / "v262144-main.elf"
OODLE_DECOMPRESS = 0x7240

BASE = 0x100000000
IMPORT_BASE = 0x20000000       # fake import stubs: each is a 'ret'
STACK_TOP = 0x30000000
STACK_SIZE = 16 << 20
TLS_BASE = 0x31000000
RET_SENTINEL = 0x32000000
HEAP_BASE = 0x38000000
HEAP_SIZE = 256 << 20
IN_BASE = 0x50000000 + 0x40000000   # 0x90000000
OUT_BASE = IN_BASE + 0x10000000
BUF_SIZE = 0x10000000    # 256 MB each (largest entry is 104 MB)
PAGE = 0x1000
RET_INSN = 0xD65F03C0


def al(n, a=PAGE):
    return (n + a - 1) & ~(a - 1)


class OodleVM:
    def __init__(self, elf_path=ELF):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.err = None
        self.heap_ptr = HEAP_BASE
        self.allocs = {}
        self.import_names = {}
        self.cap_in = 0
        self.cap_out = 0
        self.in_base = IN_BASE
        self.out_base = OUT_BASE
        self.calls = 0
        self._load(elf_path)
        self._setup()

    # ---------------- image ----------------
    def _load(self, path):
        data = Path(path).read_bytes()
        f = ELFFile(open(path, "rb"))
        uc = self.uc
        segs = [s for s in f.iter_segments() if s["p_type"] == "PT_LOAD"]
        top = max(al(s["p_vaddr"] + s["p_memsz"]) for s in segs)
        uc.mem_map(BASE, top, UC_PROT_ALL)
        for s in segs:
            uc.mem_write(BASE + s["p_vaddr"], data[s["p_offset"]:s["p_offset"] + s["p_filesz"]])
        self.image_top = top

        def v2o(va):
            for s in segs:
                if s["p_vaddr"] <= va < s["p_vaddr"] + s["p_filesz"]:
                    return s["p_offset"] + va - s["p_vaddr"]
            raise ValueError(hex(va))
        dyn = {}
        for t in f.get_segment_by_type if False else []:
            pass
        dseg = [s for s in f.iter_segments() if s["p_type"] == "PT_DYNAMIC"][0]
        for t in dseg.iter_tags():
            dyn.setdefault(t.entry.d_tag, t.entry.d_val)
        symtab, strtab = dyn["DT_SYMTAB"], dyn["DT_STRTAB"]

        def sym(i):
            o = v2o(symtab) + i * 24
            name_off, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, o)
            so = v2o(strtab) + name_off
            name = data[so:data.index(b"\0", so)].decode()
            return name, shndx, value
        stubs = {}

        def stub_for(name):
            if name not in stubs:
                stubs[name] = IMPORT_BASE + 4 * len(stubs)
            return stubs[name]
        tables = []
        if "DT_RELA" in dyn:
            tables.append((dyn["DT_RELA"], dyn["DT_RELASZ"]))
        if "DT_JMPREL" in dyn:
            tables.append((dyn["DT_JMPREL"], dyn["DT_PLTRELSZ"]))
        counts = {}
        for addr, size in tables:
            o = v2o(addr)
            for i in range(size // 24):
                r_off, r_info, r_add = struct.unpack_from("<QQq", data, o + i * 24)
                rtype, rsym = r_info & 0xFFFFFFFF, r_info >> 32
                counts[rtype] = counts.get(rtype, 0) + 1
                if rtype == 1027:    # R_AARCH64_RELATIVE
                    val = BASE + r_add
                elif rtype in (257, 1025, 1026):  # ABS64 / GLOB_DAT / JUMP_SLOT
                    name, shndx, value = sym(rsym)
                    val = BASE + value + r_add if shndx != 0 else stub_for(name) + 0
                else:
                    raise RuntimeError("unhandled reloc type %d" % rtype)
                uc.mem_write(BASE + r_off, struct.pack("<Q", val))
        self.reloc_counts = counts
        self.stubs = stubs
        self.import_names = {v: k for k, v in stubs.items()}
        uc.mem_map(IMPORT_BASE, al(4 * len(stubs) + 4), UC_PROT_ALL)
        uc.mem_write(IMPORT_BASE, struct.pack("<I", RET_INSN) * (len(stubs) + 1))

    def _setup(self):
        uc = self.uc
        uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
        uc.mem_map(TLS_BASE, 0x10000, UC_PROT_ALL)
        uc.mem_map(RET_SENTINEL, PAGE, UC_PROT_ALL)
        uc.mem_map(IN_BASE, BUF_SIZE, UC_PROT_ALL)
        uc.mem_map(OUT_BASE, BUF_SIZE, UC_PROT_ALL)
        uc.mem_write(RET_SENTINEL, struct.pack("<I", RET_INSN))
        uc.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_ALL)
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        uc.reg_write(UC_ARM64_REG_TPIDR_EL0, TLS_BASE + 0x1000)
        uc.reg_write(UC_ARM64_REG_TPIDRRO_EL0, TLS_BASE + 0x1000)
        self.sym_to_fn = {
            "memcpy": self._memcpy, "memmove": self._memcpy, "memset": self._memset,
            "memcmp": self._memcmp, "malloc": self._malloc, "calloc": self._calloc,
            "free": self._free, "realloc": self._realloc, "memalign": self._memalign,
            "aligned_alloc": self._memalign, "posix_memalign": self._posix_memalign,
        }
        # 0x80a0 is the Oodle "free" plugin (tail-jumps to the game's TLS-heap free).  The emulated heap is a
        # bump allocator reset per call, so free is a no-op: patch it to 'ret'.
        uc.mem_write(BASE + 0x80A0, struct.pack("<I", RET_INSN))
        lo = IMPORT_BASE
        hi = IMPORT_BASE + 4 * (len(self.stubs) + 1)
        uc.hook_add(UC_HOOK_CODE, self._on_import, begin=lo, end=hi)

    # ---------------- libc surface ----------------
    def _a(self, n):
        return self.uc.reg_read(UC_ARM64_REG_X0 + n)

    def _ret(self, v):
        self.uc.reg_write(UC_ARM64_REG_X0, v & 0xFFFFFFFFFFFFFFFF)

    def _memcpy(self):
        d, s, n = self._a(0), self._a(1), self._a(2)
        if n:
            self.uc.mem_write(d, bytes(self.uc.mem_read(s, n)))
        self._ret(d)

    def _memset(self):
        d, c, n = self._a(0), self._a(1) & 0xFF, self._a(2)
        if n:
            self.uc.mem_write(d, bytes([c]) * n)
        self._ret(d)

    def _memcmp(self):
        a = bytes(self.uc.mem_read(self._a(0), self._a(2)))
        b = bytes(self.uc.mem_read(self._a(1), self._a(2)))
        self._ret((a > b) - (a < b))

    def _malloc_n(self, n, align=64):
        n = al(max(n, 1), 64)
        p = al(self.heap_ptr, max(align, 64))
        if p + n > HEAP_BASE + HEAP_SIZE:
            raise MemoryError("emulated heap exhausted")
        self.heap_ptr = p + n
        self.allocs[p] = n
        return p

    def _malloc(self):
        self._ret(self._malloc_n(self._a(0)))

    def _calloc(self):
        n = self._a(0) * self._a(1)
        p = self._malloc_n(n)
        self.uc.mem_write(p, b"\0" * n)
        self._ret(p)

    def _memalign(self):
        al_, n = self._a(0), self._a(1)
        self._ret(self._malloc_n(n, al_))

    def _posix_memalign(self):
        al_, n = self._a(1), self._a(2)
        p = self._malloc_n(n, al_)
        self.uc.mem_write(self._a(0), struct.pack("<Q", p))
        self._ret(0)

    def _free(self):
        pass

    def _realloc(self):
        p, n = self._a(0), self._a(1)
        q = self._malloc_n(n)
        if p:
            old = min(self.allocs.get(p, 0), n)
            self.uc.mem_write(q, bytes(self.uc.mem_read(p, old)))
        self._ret(q)

    def _on_import(self, uc, address, size, user):
        name = self.import_names.get(address)
        try:
            fn = self.sym_to_fn.get(name)
            if fn is None:
                raise RuntimeError("unhandled import %r called (lr=%#x)" % (
                    name, uc.reg_read(UC_ARM64_REG_LR) - BASE))
            fn()
        except Exception as e:  # fail loudly: stop emulation, re-raise from decompress()
            self.err = e
            uc.emu_stop()

    # ---------------- buffers / call ----------------
    def _ensure(self, which, n):
        """Persistent input/output buffers, mapped once (Unicorn maps lazily, so the reserve is cheap)."""
        if n + 4096 > BUF_SIZE:
            raise MemoryError("buffer too large: %d" % n)

    def decompress(self, comp, raw_len):
        """comp = the stream after the 4-byte 'Oodl' tag. Returns (n_written, bytes)."""
        uc = self.uc
        self._ensure("in", len(comp) + 64)
        self._ensure("out", raw_len)
        uc.mem_write(self.in_base, comp)
        self.heap_ptr = HEAP_BASE
        self.allocs.clear()
        sp = STACK_TOP - 0x1000
        uc.mem_write(sp, b"\0" * 0x30)
        uc.mem_write(sp + 0x28, struct.pack("<Q", 3))     # threadPhase = Unthreaded
        for r, v in ((UC_ARM64_REG_X0, self.in_base), (UC_ARM64_REG_X1, len(comp)),
                     (UC_ARM64_REG_X2, self.out_base), (UC_ARM64_REG_X3, raw_len),
                     (UC_ARM64_REG_X4, 1), (UC_ARM64_REG_X5, 0), (UC_ARM64_REG_X6, 0),
                     (UC_ARM64_REG_X7, 0), (UC_ARM64_REG_SP, sp), (UC_ARM64_REG_LR, RET_SENTINEL)):
            uc.reg_write(r, v)
        self.err = None
        try:
            uc.emu_start(BASE + OODLE_DECOMPRESS, RET_SENTINEL)
        except UcError as e:
            raise RuntimeError("unicorn fault: %s pc=%#x" % (e, uc.reg_read(UC_ARM64_REG_PC) - BASE))
        if self.err:
            raise self.err
        self.calls += 1
        n = uc.reg_read(UC_ARM64_REG_X0)
        if n == 0 or n >= 1 << 40:
            return 0, b""
        return n, bytes(uc.mem_read(self.out_base, n))


def _selftest():
    vm = OodleVM()
    print("loaded; relocs", vm.reloc_counts, "imports", len(vm.stubs))
    print("import names:", sorted(vm.stubs)[:5], "...")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        _selftest()
    else:
        print(__doc__)
