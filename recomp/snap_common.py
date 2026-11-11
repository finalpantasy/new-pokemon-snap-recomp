"""Snap project inputs for the prototype: the ELF image as a generator Module, and the map files (module offsets)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))

ELF = ROOT / 'analysis' / 'import' / 'v262144-main.elf'
SDK_ELF = ROOT / 'analysis' / 'import' / 'v262144-sdk.elf'
MAPS = ROOT / 'analysis' / 'maps'
BASE = 0x7100000000   # guest base of main; the ELF is linked at 0, so address = BASE + map offset
SDK_BASE = 0x7110000000   # guest base of sdk (main's image plus bss is 0x2892000 bytes, so 256 MB apart is ample)
MODULES = {'main': (ELF, BASE), 'sdk': (SDK_ELF, SDK_BASE)}


def load_module(use_map_jump_tables=False, name='main'):
    """The ELF as a generator Module. With use_map_jump_tables the recovered tables of analysis/maps/jump_tables.tsv
    (class jump_table) override the generator's own resolver (jumptables.resolve consults mod.jt_override)."""
    from module import Module
    # Module loads the ELF through elfimg.ElfImage (module.py imports it as Nso).
    path, base = MODULES[name]
    mod = Module(name, path, base, 'c')
    if use_map_jump_tables and name == 'main':
        mod.jt_override = {BASE + int(r['br_site'], 16): sorted({BASE + int(x, 16) for x in r['targets'].split(';') if x})
                           for r in tsv(MAPS / 'jump_tables.tsv') if r['class'] == 'jump_table'}
    return mod


def elf_sections(path):
    """{section name: (address, size)} from the ELF section headers (module offsets)."""
    from elftools.elf.elffile import ELFFile
    with open(path, 'rb') as f:
        return {s.name: (s['sh_addr'], s['sh_size']) for s in ELFFile(f).iter_sections()}


def tsv(path):
    """Rows of a map .tsv as dicts (comment lines skipped, first non-comment line is the header)."""
    rows, head = [], None
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        cells = line.rstrip('\n').split('\t')
        if head is None:
            head = cells
            continue
        rows.append(dict(zip(head, cells + [''] * (len(head) - len(cells)))))
    return rows


def functions():
    """[(start, end, kind, tags)] module offsets from functions_final.tsv, kinds function/thunk/plt_stub/unresolved."""
    out = []
    for r in tsv(MAPS / 'functions_final.tsv'):
        if r['kind'] in ('function', 'thunk', 'plt_stub', 'unresolved'):
            out.append((int(r['start'], 16), int(r['end'], 16), r['kind'], r['tags']))
    return out


def lower_priority():
    """BelowNormal for this process (children inherit it)."""
    import ctypes
    k = ctypes.windll.kernel32
    k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)


class MemSampler:
    """Samples system-wide available physical memory every 0.5 s; peak_used_mb = baseline minus the lowest sample (all
    processes, so it includes worker pools and compiler children, and any other load on the machine)."""

    def __init__(self):
        import threading
        self.stop = threading.Event()
        self.base = self._avail()
        self.low = self.base
        self.t = threading.Thread(target=self._run, daemon=True)

    @staticmethod
    def _avail():
        import ctypes
        from ctypes import wintypes

        class MS(ctypes.Structure):
            _fields_ = [('dwLength', wintypes.DWORD), ('dwMemoryLoad', wintypes.DWORD), ('ullTotalPhys', ctypes.c_ulonglong),
                        ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong),
                        ('ullAvailPageFile', ctypes.c_ulonglong), ('ullTotalVirtual', ctypes.c_ulonglong),
                        ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(m)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys

    def _run(self):
        while not self.stop.wait(0.5):
            self.low = min(self.low, self._avail())

    def __enter__(self):
        self.t.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        self.t.join()
        self.low = min(self.low, self._avail())

    @property
    def peak_used_mb(self):
        return round((self.base - self.low) / 2**20)
