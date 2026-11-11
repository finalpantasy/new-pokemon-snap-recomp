"""Snap prototype: lift a deterministic ~300-function subset of `main`, compile it, and diff it against Unicorn.

Via TotK Recomp tests/diff/func.py @ 7a37fc3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), adapted from LAS Recomp
(MIT, Copyright (c) 2026 Peiikai). Modified for Snap: functions come from analysis/maps/functions_final.tsv, the ELF is
the input, atomics are included, data/.bss writes are compared through per-case memory windows, results are reported per
function (pass / fail / crash / untested), and the subset is also linked against an abort-on-unresolved runtime.

Selection is deterministic (fixed --seed, address-sorted pools). Categories: leaf_int, fp_simd, jump_table, atomic,
calls (stubbed), loops, nvn (bootstrap), hot (bake-off). Calls (BL/BLR) go to one stub on both sides (see snap_harness.c).

Outputs (generated C and binaries only under generated/ and build/; reports under analysis/recomp/):
  generated/subset/subset_NNN.c, stubs_abort.c, manifest.json
  build/subset/*.o, harness.exe, subset_link.exe
  analysis/recomp/diff_results.tsv, diff_results.json

Run: tools/venv-recomp/Scripts/python -I tests/snap_func.py [--seed 1] [--cases 12] [--functions 300] [--jobs 4]
"""
import argparse
import json
import os
import random
import re
import struct
import subprocess
import sys
import time
import zlib
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / 'recomp'))
sys.path.insert(0, str(HERE))

import numpy as np
import unicorn as uc
from unicorn import arm64_const as A

from diff_common import BUF, BUF_SIZE, MAX_WIN, Emu, compiler, diff_state, rand_fpcr, rand_v, rand_x, run_harness
from snap_common import BASE, MAPS, SDK_BASE, functions, load_module, lower_priority, tsv

# SNAP_MODULE=sdk runs the same differential test on the sdk module: its function extents come from snap_gen.sdk_plan
# (discovery plus PLT stubs), there are no jump-table / NVN maps, and outputs go to *_sdk paths.
MODNAME = os.environ.get('SNAP_MODULE', 'main')
if MODNAME == 'sdk':
    BASE = SDK_BASE
import gen
import capstone
_CS = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

SENTINEL = 0x7210000000  # LR on entry; never executed
PTR_LO, PTR_HI = BUF + 0x40000, BUF + 0xC0000
STACK_TOP = BUF + 0xFF000
MIN_INSNS, MAX_INSNS, BIG_INSNS = 6, 600, 20000
FNV = 0x100000001B3
M64 = (1 << 64) - 1
NVN_LOADERS = (0x910270, 0x254c40, 0x90a0c0)
HOT = (0x5bd940, 0x59ab20)

OUT = ROOT / 'analysis' / 'recomp'
GEN = ROOT / 'generated' / ('subset' if MODNAME == 'main' else 'subset_sdk')
BLD = ROOT / 'build' / ('subset' if MODNAME == 'main' else 'subset_sdk')
if MODNAME == 'sdk':
    OUT = OUT / 'sdk_diff'


def peak_rss_mb():
    """Peak working set of this process in MB (Windows)."""
    import ctypes
    from ctypes import wintypes as wt

    class PMC(ctypes.Structure):
        _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                    ('WorkingSetSize', ctypes.c_size_t), ('a', ctypes.c_size_t), ('b', ctypes.c_size_t),
                    ('c', ctypes.c_size_t), ('d', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                    ('PeakPagefileUsage', ctypes.c_size_t)]
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    k = ctypes.windll.kernel32
    k.GetCurrentProcess.restype = wt.HANDLE
    ps = ctypes.WinDLL('psapi')
    ps.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
    ps.GetProcessMemoryInfo(k.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
    return round(pmc.PeakWorkingSetSize / 1e6)


def splitmix(z):
    z = (z + 0x9E3779B97F4A7C15) & M64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


# ------------------------------------------------------------------------------------------------ selection

def call_mask(w):
    return ((w & 0xFC000000) == 0x94000000) | ((w & 0xFFFFFC1F) == 0xD63F0000)   # BL, BLR


def forbidden_mask(w):
    """Words a picked function must not contain: exceptions, system registers (except hints and barriers), LSE."""
    m = (w & 0xFF000000) == 0xD4000000                    # SVC, HVC, SMC, BRK, HLT, DCPS
    sysi = (w & 0xFFC00000) == 0xD5000000                 # MRS, MSR, SYS (DC ZVA), hints, barriers
    hint_or_barrier = ((w & 0xFFFFF000) == 0xD5032000) | ((w & 0xFFFFF000) == 0xD5033000)
    m |= sysi & ~hint_or_barrier
    m |= (w & 0x3F200C00) == 0x38200000                   # LSE atomics (none in main; kept for safety)
    return m


def llsc_mask(w):
    fam = (w & 0x3F000000) == 0x08000000                  # exclusives and load-acquire/store-release
    return fam & (((w >> 23) & 1) == 0)                   # o2 = 0: LDXR/LDAXR/STXR/STLXR/LDXP/STXP family


def cond_mask(w):
    return (((w & 0xFE000010) == 0x54000000) | ((w & 0x7E000000) == 0x34000000) | ((w & 0x7E000000) == 0x36000000))


def simd_mask(w):
    return ((w & 0x0E000000) == 0x0E000000) | (((w & 0x0A000000) == 0x08000000) & (((w >> 26) & 1) == 1))


def call_sites(mod, entry, end):
    return [a for a in range(entry, end, 4) if mod.lift(a).kind in ('bl', 'blr')]


def callees(emitter, entry, end):
    mod = emitter.mod
    out = {mod.lift(a).target for a in call_sites(mod, entry, end)
           if mod.lift(a).kind == 'bl' and mod.lift(a).target in emitter.starts}
    for a in range(entry, end, 4):   # tail calls: direct branches out of the function to another start
        i = mod.lift(a)
        if i.kind in ('b', 'bcond') and not entry <= i.target < end and i.target in emitter.starts:
            out.add(i.target)
    return out


def check_function(emitter, entry, end):
    """Tags of a function the test can run in isolation, or None."""
    mod = emitter.mod
    insns = [mod.lift(a) for a in range(entry, end, 4)]
    if insns[-1].kind not in gen.TERMINATORS or not any(i.kind in ('ret', 'b', 'bcond') for i in insns):
        return None
    tags = set()
    for k, ins in enumerate(insns):
        a = entry + 4 * k
        if ins.kind in ('bl', 'blr'):
            tags.add('calls')
        if ins.kind in ('b', 'bcond'):
            if not entry <= ins.target < end:
                if ins.target not in emitter.starts:
                    return None
                tags.add('tail_call')   # emitted as a musttail call of a stub; Unicorn side stubs it at the block hook
            elif ins.target <= a:
                tags.add('loop')
        if ins.kind == 'br':
            jt = gen.jumptables.resolve(mod, a, entry, end)
            if jt:
                tags.add('jump_table')
            elif gen.jumptables.is_computed(mod, a, entry):
                tags.add('all_label_fallback')
            else:
                return None
        if ins.kind == 'trap':
            tags.add('trap')
    w = mod.words[(entry - mod.lo) >> 2:(end - mod.lo) >> 2]
    if simd_mask(w).any():
        tags.add('simd')
    if llsc_mask(w).any():
        tags.add('atomic')
    return tags


# About a third of the picks cannot be run under Unicorn with random inputs (they dereference values the random state does
# not make valid), so the quotas are 1.5x the ~300 functions the test aims to run; untested picks are still lifted,
# compiled and linked, and reported as `untested`.
QUOTA = [('nvn', 12), ('hot', 2), ('atomic', 75), ('jump_table', 60), ('fp_simd', 90), ('leaf_int', 80), ('calls', 70),
         ('loops', 70)]


def pick_functions(mod, starts_abs, count, r, extra=()):
    """Deterministic subset: [(entry, end, category, tags)] in address order plus the Emitter."""
    w = mod.words
    if MODNAME == 'sdk':
        import snap_gen
        fns = [(s, e) for s, e in snap_gen.sdk_plan()[0]]
    else:
        fns = [(s, e) for s, e, k, _ in functions() if k in ('function', 'thunk')]
    theirs = sorted(starts_abs)
    emitter = gen.Emitter(mod, set(starts_abs))
    ws = lambda mask: np.concatenate([[0], np.cumsum(mask.astype(np.int64))])
    cs_forb, cs_cond, cs_call = ws(forbidden_mask(w)), ws(cond_mask(w)), ws(call_mask(w))
    cs_ll, cs_simd = ws(llsc_mask(w)), ws(simd_mask(w))
    cs_br = ws((w & 0xFFFFFC1F) == 0xD61F0000)

    def rng(cs, s, e):
        return int(cs[e >> 2] - cs[s >> 2])

    import bisect
    jt_rows = tsv(MAPS / 'jump_tables.tsv') if MODNAME == 'main' else []
    jt_fn = {int(x['function'], 16) for x in jt_rows if x['class'] == 'jump_table'}
    nvn_fn = ({int(x['function'], 16) for x in tsv(MAPS / 'nvn_procs.tsv') if x['function'].startswith('0x')} - set(NVN_LOADERS)) if MODNAME == 'main' else set()
    pools = {k: [] for k, _ in QUOTA}
    feats = {}
    for s, e in fns:
        n = (e - s) // 4
        # a generator start strictly inside the function (the 6 functions it splits) is skipped
        i = bisect.bisect_right(theirs, BASE + s)
        if i < len(theirs) and theirs[i] < BASE + e:
            continue
        big = s in NVN_LOADERS or s in HOT
        if rng(cs_forb, s, e) or not (MIN_INSNS <= n <= (BIG_INSNS if big else MAX_INSNS)):
            continue
        calls, ll, simd, br = rng(cs_call, s, e), rng(cs_ll, s, e), rng(cs_simd, s, e), rng(cs_br, s, e)
        feats[s] = (n, calls, ll, simd, br)
        if s in NVN_LOADERS or s in HOT or s in nvn_fn:
            pools['nvn' if s not in HOT else 'hot'].append(s)
        if ll:
            pools['atomic'].append(s)
        if s in jt_fn:
            pools['jump_table'].append(s)
        if simd and simd / n >= 0.30 and n >= 10:
            pools['fp_simd'].append(s)
        if not calls and not simd and not br and not ll:
            pools['leaf_int'].append(s)
        if calls and not simd and not ll and not br:
            pools['calls'].append(s)
        if rng(cs_cond, s, e):
            pools['loops'].append(s)
    picks, seen, out_ = [], set(), []
    end_of = dict(fns)
    for cat, quota in QUOTA:
        pool = sorted(set(pools[cat]))
        if cat == 'nvn':
            first = [s for s in NVN_LOADERS if s in pool]
            rest = [s for s in pool if s not in first]
            r.shuffle(rest)
            pool = first + rest
        elif cat == 'hot':
            pool = [s for s in HOT if s in pool]
        else:
            r.shuffle(pool)
        taken = 0
        for s in pool:
            if taken >= quota or len(picks) >= count:
                break
            if s in seen:
                continue
            entry, end = BASE + s, BASE + end_of[s]
            tags = check_function(emitter, entry, end)
            if tags is None or (cat == 'loops' and 'loop' not in tags):
                continue
            seen.add(s)
            out_.append((entry, end, cat, tags))
            picks.append(s)
            taken += 1
    out = out_
    for s_ in extra:   # forced picks (category jt_check)
        if s_ in end_of and s_ not in seen:
            tags = check_function(emitter, BASE + s_, BASE + end_of[s_])
            if tags is not None:
                seen.add(s_)
                out_.append((BASE + s_, BASE + end_of[s_], 'jt_check', tags))
    out = out_
    # drop callers of another pick (the C side would run that callee instead of the stub)
    picked = {e for e, _, _, _ in out}
    dropped = [o for o in out if callees(emitter, o[0], o[1]) & picked]
    out = sorted(o for o in out if o not in dropped)
    # callers of a picked function can run only where that function is a stub, so a function that calls itself (recursion)
    # or another deferred function cannot be tested here
    dset = {o[0] for o in dropped}
    unrunnable = [o for o in dropped if callees(emitter, o[0], o[1]) & dset]
    dropped = [o for o in dropped if o not in unrunnable]
    # callers of another pick go last, into a chunk and a harness executable of their own, where their picked callees
    # are stubs (0x910270 calls both NVN loaders)
    return out + sorted(dropped), emitter, {k: len(v) for k, v in pools.items()}, len(out)


# ------------------------------------------------------------------------------------------------ build

def compile_all(srcs, work, jobs, cflags):
    cc = compiler()
    flags = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-Wno-unused-value', '-w', '-I',
             str(ROOT / 'runtime')] + cflags.split()
    objs, running, fails = [], [], []
    todo = list(srcs)
    t0 = time.time()
    while todo or running:
        while todo and len(running) < jobs:
            s = todo.pop(0)
            o = work / (s.stem + '.o')
            objs.append(o)
            errf = open(work / (s.stem + '.err'), 'wb')
            running.append((s, subprocess.Popen([cc] + flags + ['-c', str(s), '-o', str(o)], stderr=errf)))
        for item in list(running):
            if item[1].poll() is not None:
                running.remove(item)
                if item[1].returncode:
                    fails.append((item[0].name, (work / (item[0].stem + '.err')).read_bytes().decode(errors='replace')[-1500:]))
        time.sleep(0.05)
    if fails:
        for n, e in fails[:3]:
            print('COMPILE FAIL', n, e)
        raise SystemExit('compile failed')
    return objs, time.time() - t0


def link(cc, exe, objs):
    subprocess.check_call([cc, '-fuse-ld=lld', '-o', str(exe)] + [str(o) for o in objs])


def plt_names():
    if MODNAME != 'main':
        return {}
    return {int(r['start'], 16): r['notes'].replace('import:', '') for r in tsv(MAPS / 'functions_final.tsv')
            if r['kind'] == 'plt_stub' and r['notes'].startswith('import:')}


def emit_subset(emitter, picks, nmain, jobs, chunk_fns):
    """Writes generated/subset/subset_NNN.c; returns (files, seconds, referenced prototypes)."""
    GEN.mkdir(parents=True, exist_ok=True)
    for f in GEN.glob('*.c'):
        f.unlink()
    t0 = time.time()
    files, refs, n = [], set(), max(1, chunk_fns)
    jobs_ = [(f'subset_{j // n:03d}.c', j // n, picks[j:min(j + n, nmain)]) for j in range(0, nmain, n)]
    if len(picks) > nmain:
        jobs_.append(('subset_defer.c', 999, picks[nmain:]))
    for name, num, grp in jobs_:
        text = gen.chunk_text('subset', num, emitter, [(e, en) for e, en, _, _ in grp])
        p = GEN / name
        p.write_text(text)
        files.append(p)
        refs |= set(re.findall(r'^void (f_[0-9a-f]+)\(Cpu \*c\);$', text, re.M))
    return files, time.time() - t0, refs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--functions', type=int, default=460)
    ap.add_argument('--cases', type=int, default=12, help='random states tried per function')
    ap.add_argument('--retry-untested', type=int, default=36, help='extra random states for a function with no accepted case')
    ap.add_argument('--steps', type=int, default=200000, help='Unicorn instruction cap per case')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--chunk-fns', type=int, default=25)
    ap.add_argument('--cflags', default='-march=x86-64-v3')
    ap.add_argument('--mutate', type=int, default=0, help='self-test: corrupt one operation in N emitted functions')
    ap.add_argument('--add', default='', help='comma list of hex entry offsets to add as forced picks')
    ap.add_argument('--jt-source', choices=('map', 'generator'), default='map', help='jump table targets: the maps or the generator resolver')
    ap.add_argument('--only', default='', help='comma list of hex entry offsets to restrict the picks (debug)')
    ap.add_argument('--show', type=int, default=25)
    args = ap.parse_args()
    lower_priority()
    r = random.Random(args.seed)
    T = {}
    t0 = time.time()
    mod = load_module(use_map_jump_tables=args.jt_source == 'map', name=MODNAME)
    starts, dstats = gen.discover(mod)
    starts_abs = {mod.lo + int(a) * 4 for a in starts}
    T['discover_s'] = round(time.time() - t0, 1)
    t1 = time.time()
    extra = [int(x, 16) for x in args.add.split(',') if x]
    picks, emitter, pool_sizes, nmain = pick_functions(mod, starts_abs, args.functions, r, extra)
    if args.only:
        want = {BASE + int(x, 16) for x in args.only.split(',')}
        main_set = picks[:nmain]
        picks = [p for p in picks if p[0] in want]
        nmain = sum(1 for p in picks if p in main_set)
    T['select_s'] = round(time.time() - t1, 1)
    cat_n = Counter(p[2] for p in picks)
    print(f'picked {len(picks)} of pools {pool_sizes} ({len(picks) - nmain} callers of another pick, run in a second harness): {dict(cat_n)}',
          flush=True)

    # ---- image (relocated, with .bss) for the harness and Unicorn
    BLD.mkdir(parents=True, exist_ok=True)
    img = bytearray(mod.nso.relocated(BASE)).ljust(mod.nso.mem_end, b'\0')
    # .bss starts as pointers into the scratch buffer (60%) or zero, so singletons reached through the GOT are not null;
    # both sides start from the same bytes (fixed seed).
    rng = np.random.default_rng(args.seed)
    nb = (mod.nso.mem_end - mod.nso.size) // 8
    words = rng.integers(PTR_LO // 16, PTR_HI // 16, nb, dtype=np.uint64) * np.uint64(16)
    words[rng.random(nb) >= 0.9] = 0
    img[mod.nso.size:mod.nso.size + nb * 8] = words.astype('<u8').tobytes()
    img = bytes(img)
    (BLD / 'main.img').write_bytes(img)
    text_end = (mod.nso.text[0] + mod.nso.text[1] + 0xFFF) & ~0xFFF

    # ---- emit + compile
    files, t_emit, refs = emit_subset(emitter, picks, nmain, args.jobs, args.chunk_fns)
    mutated = set()
    if args.mutate:   # self-test of the harness: corrupt the first '+' of every k-th function in the main chunks
        every = max(1, nmain // args.mutate)
        for f in files:
            if f.stem == 'subset_defer':
                continue
            txt = f.read_text()
            parts = re.split(r'(?m)^(?=void f_[0-9a-f]+[(]Cpu [*]c[)] [{])', txt)
            for i, part in enumerate(parts):
                m = re.match(r'void (f_[0-9a-f]+)[(]', part)
                if m and (len(mutated) * every <= int(m.group(1)[2:], 16) % (every * 997)) and part.count(' + ') and len(mutated) < args.mutate:
                    k = part.index(' + ', part.index('{'))
                    parts[i] = part[:k] + ' + 1 + ' + part[k + 3:]
                    mutated.add(int(m.group(1)[2:], 16))
            f.write_text(''.join(parts))
        print(f'mutated {len(mutated)} functions', flush=True)
    T['lift_emit_s'] = round(t_emit, 1)
    csize = sum(f.stat().st_size for f in files)
    for f in BLD.glob('*.o'):
        f.unlink()
    defined = {gen.fname(e) for e, _, _, _ in picks}
    undefined = sorted(refs - defined)
    names = plt_names()
    # abort-stub runtime: every callee outside the subset logs and aborts
    lines = ['// GENERATED-GAME-CODE: unresolved-callee stubs for the subset, generated by tests/snap_func.py. Do not commit.',
             '#include "cpu.h"', 'void rt_unresolved(Cpu *c, uint64_t addr, const char *what);']
    for f in undefined:
        a = int(f[2:], 16)
        what = f'import {names[a - BASE]}' if (a - BASE) in names else 'guest function not in subset'
        lines.append(f'void {f}(Cpu *c) {{ rt_unresolved(c, {a:#x}ull, "{what}"); }}')
    lines.append('GuestFn subset_fns[] = {' + ','.join(gen.fname(e) for e, _, _, _ in picks) + '};')
    lines.append(f'int subset_count = {len(picks)};')
    lines = ['void ' + f'{gen.fname(e)}(Cpu *c);' for e, _, _, _ in picks] + [''] + lines
    lines.insert(0, '#include "cpu.h"')
    (GEN / 'stubs_abort.c').write_text('\n'.join(lines) + '\n')
    (GEN / 'link_main.c').write_text('#include <stdio.h>\nextern int subset_count;\nint main(void){ printf("subset %d functions linked\\n", subset_count); return 0; }\n')
    rt = ROOT / 'runtime' / 'snap_rt_abort.c'
    objs, t_comp = compile_all(files, BLD, args.jobs, args.cflags)
    T['compile_s'] = round(t_comp, 1)
    cc = compiler()
    extra, _ = compile_all([GEN / 'stubs_abort.c', GEN / 'link_main.c', rt], BLD, args.jobs, args.cflags)
    link(cc, BLD / 'subset_link.exe', objs + extra)
    lk = subprocess.run([str(BLD / 'subset_link.exe')], capture_output=True, text=True)
    print(f'lift+emit {t_emit:.1f}s, compile {t_comp:.1f}s ({len(files)} chunks, {csize / 1e6:.2f} MB of C), '
          f'unresolved stubs {len(undefined)}; link run: {lk.stdout.strip()}', flush=True)
    T['c_bytes'] = csize
    T['chunks'] = len(files)
    T['unresolved_stubs'] = len(undefined)

    # ---- harness builds: all callees are the shared digest stub. Group A: the picks that call no other pick; group B:
    # callers of picks (their own object file, picked callees stubbed).
    harness_groups = []
    for gname, idxs, gobjs in (('A', list(range(nmain)), [o for o in objs if o.stem != 'subset_defer']),
                               ('B', list(range(nmain, len(picks))), [o for o in objs if o.stem == 'subset_defer'])):
        if not idxs:
            continue
        gp = [picks[i] for i in idxs]
        called = set()
        for e, en, _, _ in gp:
            called |= callees(emitter, e, en)
        table = GEN / f'table_harness_{gname}.c'
        table.write_text('#include "cpu.h"\n' + ''.join(f'void {gen.fname(e)}(Cpu *c);\n' for e, _, _, _ in gp) +
                         'GuestFn tests[] = {' + ','.join(gen.fname(e) for e, _, _, _ in gp) + '};\n' +
                         f'int ntests = {len(gp)};\n' + 'void rt_stub_call(Cpu *c, uint64_t target);\n' +
                         ''.join(f'void {gen.fname(t)}(Cpu *c) {{ rt_stub_call(c, {hex(t)}ull); }}\n'
                                 for t in sorted(called - {e for e, _, _, _ in gp})))
        hobjs, _ = compile_all([table], BLD, args.jobs, args.cflags)
        hrt, _ = compile_all([ROOT / 'tests' / 'snap_harness.c', ROOT / 'runtime' / 'fma.c'], BLD, args.jobs, args.cflags) if not harness_groups else ([BLD / 'snap_harness.o', BLD / 'fma.o'], 0)
        exe = BLD / f'harness_{gname}.exe'
        link(cc, exe, gobjs + hobjs + hrt)
        harness_groups.append((exe, idxs))

    # ---- Unicorn reference
    sites = {}
    for e, en, _, _ in picks:
        sites[e] = call_sites(mod, e, en)
    buf0 = make_buffer(r)
    runner = Runner(BASE, img, text_end, buf0, starts_abs)
    cases, expect, per_fn, skip_reasons = [], [], {}, {}
    t2 = time.time()
    for k, (e, en, cat, tags) in enumerate(picks):
        per_fn[k] = {'cases': 0, 'skips': Counter()}
        runner.begin(e, en, sites[e])
        for ci in range(args.cases + args.retry_untested):
            if ci >= args.cases and per_fn[k]['cases'] >= 3:
                break          # the extra states only top up a function that has fewer than 3 accepted cases
            case = make_case(r, rich=ci % 2 == 0)
            res = runner.run(case, args.steps)
            if isinstance(res, str):
                per_fn[k]['skips'][res] += 1
                continue
            state, wins, (pcase, patches) = res
            fpcr, x, sp, nzcv, v = pcase
            cases.append((k, fpcr, x, sp, nzcv, v, True, wins, patches))
            expect.append(state)
            per_fn[k]['cases'] += 1
        runner.end()
    T['unicorn_s'] = round(time.time() - t2, 1)
    print(f'unicorn: {len(cases)} cases on {sum(1 for p in per_fn.values() if p["cases"])} functions '
          f'({T["unicorn_s"]}s)', flush=True)

    # ---- host side, resuming after a crash
    t3 = time.time()
    recs_all, crashed = [], {}
    work = BLD / 'run'
    work.mkdir(exist_ok=True)
    for exe, idxs in harness_groups:
        local = {k: n for n, k in enumerate(idxs)}
        remaining = [i for i, c in enumerate(cases) if c[0] in local]
        while remaining:
            sub = [(local[cases[i][0]],) + tuple(cases[i][1:]) for i in remaining]
            recs, err = run_harness(exe, work, buf0, sub, [(BASE, BLD / 'main.img')], stubs=True)
            for jx, rec in enumerate(recs):
                recs_all.append((remaining[jx], rec))
            if not err:
                break
            bad = remaining[len(recs)] if len(recs) < len(remaining) else None
            if bad is None:
                break
            kf = cases[bad][0]
            crashed[kf] = err.replace('\n', ' ')[:300]
            remaining = [i for i in remaining[len(recs) + 1:] if cases[i][0] != kf]
    T['host_s'] = round(time.time() - t3, 1)
    T['python_peak_rss_mb'] = peak_rss_mb()
    T['total_s'] = round(time.time() - t0, 1)

    # ---- compare
    fails = {}
    nfail = 0
    ncmp = Counter()
    for ci, got in recs_all:
        k = cases[ci][0]
        d = diff_state(got, expect[ci], check_pc=False, check_mem=True, check_nzcv=False)
        ncmp[k] += 1
        if d:
            nfail += 1
            fails.setdefault(k, (ci, d))
    rows, status = [], Counter()
    for k, (e, en, cat, tags) in enumerate(picks):
        if k in crashed:
            st = 'crash'
        elif k in fails:
            st = 'fail'
        elif ncmp[k]:
            st = 'pass'
        else:
            st = 'untested'
        status[st] += 1
        detail = ''
        if st == 'fail':
            ci, d = fails[k]
            detail = f'case {ci}: ' + '; '.join(d[:3])
        elif st == 'crash':
            detail = crashed[k]
        elif st == 'untested':
            detail = ', '.join(f'{n}x {m}' for m, n in per_fn[k]['skips'].most_common(3))
        rows.append({'entry': f'{e - BASE:#x}', 'end': f'{en - BASE:#x}', 'insns': (en - e) // 4, 'category': cat,
                     'tags': ','.join(sorted(tags)), 'status': st, 'cases_run': ncmp[k], 'cases_unicorn_ok': per_fn[k]['cases'],
                     'detail': detail})
    OUT.mkdir(parents=True, exist_ok=True)
    cols = list(rows[0])
    with open(OUT / 'diff_results.tsv', 'w', encoding='utf-8') as f:
        f.write('\t'.join(cols) + '\n')
        for row in rows:
            f.write('\t'.join(str(row[c]) for c in cols) + '\n')
    by_cat = {c: Counter(r_['status'] for r_ in rows if r_['category'] == c) for c in cat_n}
    summary = {'seed': args.seed, 'cases_per_function': args.cases, 'picked': len(picks), 'status': dict(status),
               'by_category': {c: dict(v) for c, v in by_cat.items()}, 'cases_compared': len(recs_all),
               'failing_cases': nfail, 'timing': T, 'pools': pool_sizes, 'cflags': args.cflags}
    (OUT / 'diff_results.json').write_text(json.dumps({'summary': summary, 'rows': rows}, indent=1))
    if mutated:
        det = [r_ for r_ in rows if int(r_['entry'], 16) + BASE in mutated]
        summary['mutation_selftest'] = {'mutated': len(det), 'detected_fail': sum(r_['status'] == 'fail' for r_ in det),
                                        'not_tested': sum(r_['status'] == 'untested' for r_ in det),
                                        'missed': sum(r_['status'] == 'pass' for r_ in det)}
    print(json.dumps(summary))
    for row in rows:
        if row['status'] in ('fail', 'crash') and args.show:
            args.show -= 1
            print(f"  {row['status']:5} {row['entry']:>9} {row['insns']:5} insns {row['category']:10} {row['tags']}: {row['detail'][:230]}")
    return 0


def make_buffer(r):
    """Scratch buffer content: 85% pointers into the scratch region, 15% zero (list ends, null checks). No small integers
    and no random 64-bit values: both turn a field read into a wild or null base (the earlier mix left about 25% of the
    functions untested)."""
    out = bytearray()
    for _ in range(BUF_SIZE // 8):
        v = r.randrange(PTR_LO, PTR_HI, 16) if r.random() < 0.85 else 0
        out += v.to_bytes(8, 'little')
    return bytes(out)


def make_case(r, rich=False):
    """rich: every argument register holds a pointer into the scratch buffer; otherwise 85% pointers and small integers
    0..7 in the rest. X8..X28 mostly random pointers or random values."""
    x = [rand_x(r) for _ in range(31)]
    for i in range(29):
        k = r.random()
        if i < 8:
            x[i] = r.randrange(PTR_LO, PTR_HI, 16) if (rich or k < 0.85) else r.randrange(0, 8)
        elif k < 0.5:
            x[i] = r.randrange(PTR_LO, PTR_HI, 16)
    x[30] = SENTINEL
    sp = STACK_TOP - r.randrange(0, 0x100, 16)
    return rand_fpcr(r), x, sp, r.getrandbits(4) << 28, [rand_v(r) for _ in range(32)]


class Runner:
    """Unicorn with the relocated image (text read-only, the rest writable), a block hook that stops when execution
    leaves the function, a code hook on each call site of the current function that stops before the call (for the
    stub), and write hooks outside the scratch buffer that record the windows the host side must compare."""

    MAX_CALLS = 3000

    def __init__(self, base, img, text_end, buf0, starts):
        self.emu = Emu([(base, img)])
        mu = self.emu.mu
        self.base, self.img, self.text_end = base, img, text_end
        self.starts = starts
        mu.mem_protect(base, text_end, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        mu.mem_map(SENTINEL, 0x1000)
        mu.mem_write(BUF, buf0)
        self.buf0 = buf0
        self.lo = self.hi = self.h = 0
        self.left = False
        self.at_call = None
        self.tail = None
        self.writes = []
        self.log = []
        mu.hook_add(uc.UC_HOOK_BLOCK, self._block)
        mu.hook_add(uc.UC_HOOK_MEM_WRITE, self._write, begin=0, end=BUF - 1)
        mu.hook_add(uc.UC_HOOK_MEM_WRITE, self._write, begin=BUF + BUF_SIZE, end=(1 << 64) - 1)
        self.hooks = []

    def begin(self, entry, end, sites):
        self.lo, self.hi = entry, end
        mu = self.emu.mu
        self.hooks = [mu.hook_add(uc.UC_HOOK_CODE, self._call, begin=a, end=a) for a in sorted(sites)]

    def end(self):
        for h in self.hooks:
            self.emu.mu.hook_del(h)
        self.hooks = []

    def _block(self, mu, address, size, user):
        if not self.lo <= address < self.hi and address != SENTINEL:
            if address in self.starts:
                self.tail = address     # a direct branch to another function start: a tail call, stubbed
            else:
                self.left = True
            mu.emu_stop()

    def _call(self, mu, address, size, user):
        if self.lo <= address < self.hi:
            self.at_call = address
            mu.emu_stop()

    def _write(self, mu, access, address, size, value, user):
        self.writes.append((address, size))

    def stub(self, a):
        """snap_harness.c rt_stub for the call at a, which Unicorn has not executed yet. Returns the address to resume at."""
        e, mu = self.emu, self.emu.mu
        w = int.from_bytes(mu.mem_read(a, 4), 'little')
        if (w & 0xFC000000) == 0x94000000:   # BL
            imm = w & 0x03FFFFFF
            target = (a + ((imm - (1 << 26) if imm >> 25 else imm) << 2)) & M64
        else:                                 # BLR
            target = mu.reg_read(e.xregs[(w >> 5) & 31])
        mu.reg_write(A.UC_ARM64_REG_X30, a + 4)
        self.stub_target(target)
        return a + 4

    def stub_target(self, target):
        e, mu = self.emu, self.emu.mu
        h = ((self.h ^ target) * FNV) & M64
        for val in list(mu.reg_read_batch(e.xregs)) + [mu.reg_read(A.UC_ARM64_REG_SP)]:
            h = ((h ^ val) * FNV) & M64
        for val in mu.reg_read_batch(e.qregs[:8]):
            h = ((h ^ (val & M64)) * FNV) & M64
            h = ((h ^ (val >> 64)) * FNV) & M64
        self.h = h
        if os.environ.get('STUBLOG'):
            xs = mu.reg_read_batch(e.xregs)
            self.log.append(f'stub target={target:x} h={h:x} x0={xs[0]:x} x1={xs[1]:x} x2={xs[2]:x} x19={xs[19]:x} x20={xs[20]:x} x21={xs[21]:x} x30={xs[30]:x} sp={mu.reg_read(A.UC_ARM64_REG_SP):x}')
        regs = []
        for i in range(19):
            s = splitmix((h + i) & M64)
            regs.append((e.xregs[i], PTR_LO + ((s >> 4) % ((PTR_HI - PTR_LO) // 16)) * 16 if i < 8 and s & 15 else s))
        regs.append((A.UC_ARM64_REG_NZCV, splitmix((h + 19) & M64) >> 60 << 28))
        for i in range(8):
            regs.append((e.qregs[i], splitmix((h + 33 + 2 * i) & M64) << 64 | splitmix((h + 32 + 2 * i) & M64)))
        mu.reg_write_batch(regs)
        top = BUF + BUF_SIZE - 16
        n = int.from_bytes(mu.mem_read(top, 8), 'little')
        mu.mem_write(top, ((n + 1) & M64).to_bytes(8, 'little') + h.to_bytes(8, 'little'))

    def windows(self):
        """Merged write ranges (at most MAX_WIN) inside the image, or None."""
        ws = sorted((a, a + s) for a, s in self.writes)
        for gap in (16, 256, 4096, 65536):
            merged = []
            for a, b in ws:
                if merged and a <= merged[-1][1] + gap:
                    merged[-1][1] = max(merged[-1][1], b)
                else:
                    merged.append([a, b])
            if len(merged) <= MAX_WIN:
                return [(a, b - a) for a, b in merged]
        return None

    REPAIRS = 24
    MAX_PATCH = 16

    def run(self, case, steps):
        """(state, windows, patched case) or a skip-reason string. A case that faults on an unmapped load is repaired
        and retried: the base register of the faulting access is replaced by a scratch pointer when it still holds its
        initial value, or the 8-byte scratch slot it was loaded from (unmodified so far) is patched to a pointer. The
        host applies the same patches (In.patch), so both sides run the same state."""
        fpcr, x, sp, nzcv, v = case
        x = list(x)
        patches = {}
        self.tried = set()
        for attempt in range(self.REPAIRS + 1):
            res = self._run1((fpcr, x, sp, nzcv, v), patches, steps, False)
            if not (isinstance(res, tuple) and res[0] == 'fault'):
                break
            res = self._run1((fpcr, x, sp, nzcv, v), patches, steps, True)    # same state, traced
            if not (isinstance(res, tuple) and res[0] == 'fault'):
                break
            fix = self._repair(res[1], x, patches, attempt)
            if not fix:
                res = res[2]
                break
        if isinstance(res, tuple) and res[0] == 'fault':
            res = res[2]
        if isinstance(res, str):
            return res
        state, wins = res
        return state, wins, ((fpcr, x, sp, nzcv, v), sorted(patches.items()))

    def _repair(self, info, x, patches, attempt):
        """info: (pc, fault address, initial-register snapshot at fault, loaded {value: addr}, dirty addresses)."""
        pc, addr, regs, loaded, dirty = info
        w = int.from_bytes(self.emu.mu.mem_read(pc, 4), 'little')
        ins = next(_CS.disasm(w.to_bytes(4, 'little'), pc), None)
        if ins is None:
            return False
        m = re.search(r'\[(\w+)(?:,\s*(\w+))?', ins.op_str)
        if not m:
            return False
        for name in (m.group(1), m.group(2)):
            if not name or name[0] not in 'xw' or not name[1:].isdigit():
                continue
            n = int(name[1:])
            val = regs[n] if name[0] == 'x' else regs[n] & 0xFFFFFFFF
            new = PTR_LO + (splitmix((pc + 31 * attempt + n) & M64) % ((PTR_HI - PTR_LO) // 16)) * 16
            if name[0] != 'x' or n >= 31 or PTR_LO <= val < PTR_HI:
                continue
            # value provenance: an initial register holding this value (a copy of an argument) is replaced; the register the
            # access names comes first, then the lowest-numbered one not tried yet
            cands = [i for i in range(29) if x[i] == val and i not in self.tried]
            if cands:
                i = n if n in cands else cands[0]
                self.tried.add(i)
                x[i] = new
                return True
            a = loaded.get(val)
            if a is not None and a not in dirty and len(patches) < self.MAX_PATCH and BUF <= a < BUF + BUF_SIZE - 16:
                patches[a] = new
                return True
        return False

    def _run1(self, case, patches, steps, trace):
        """Raw run. Returns (state, windows), a reason string, or ('fault', info, reason) for an unmapped access."""
        fpcr, x, sp, nzcv, v = case
        e, mu = self.emu, self.emu.mu
        self.left, self.h, self.writes, self.log = False, 0, [], []
        for a_, v_ in patches.items():
            mu.mem_write(a_, v_.to_bytes(8, 'little'))
        loaded, dirty, hk = {}, set(), []
        if trace:
            def rd(mu_, access, address, size, value, user):
                if size == 8:
                    loaded[int.from_bytes(mu_.mem_read(address, 8), 'little')] = address
            def wr(mu_, access, address, size, value, user):
                for o in range(address & ~7, address + size, 8):
                    dirty.add(o)
            hk.append(mu.hook_add(uc.UC_HOOK_MEM_READ, rd, begin=BUF, end=BUF + BUF_SIZE - 1))
            hk.append(mu.hook_add(uc.UC_HOOK_MEM_WRITE, wr, begin=BUF, end=BUF + BUF_SIZE - 1))
        regs_init = list(x[:31])
        fault = None
        mu.reg_write_batch(list(zip(e.xregs, x[:31])) + list(zip(e.qregs, v)) +
                           [(A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_NZCV, nzcv), (A.UC_ARM64_REG_FPCR, fpcr)])
        pc, reason = self.lo, None
        try:
            for _ in range(self.MAX_CALLS):
                self.at_call = self.tail = None
                mu.emu_start(pc, SENTINEL, count=steps)
                if self.tail is not None:       # tail call: stub it, then the callee returns to LR (the sentinel)
                    self.stub_target(self.tail)
                    mu.reg_write(A.UC_ARM64_REG_PC, SENTINEL)
                    break
                if self.at_call is None:
                    if self.left:
                        reason = 'left the function'
                    elif mu.reg_read(A.UC_ARM64_REG_PC) != SENTINEL:
                        reason = 'step cap'
                    break
                pc = self.stub(self.at_call)
            else:
                reason = 'call cap'
        except uc.UcError as ex:
            reason = f'unicorn {ex} @{mu.reg_read(A.UC_ARM64_REG_PC) - self.base:#x}'
            if ex.errno in (uc.UC_ERR_READ_UNMAPPED, uc.UC_ERR_WRITE_UNMAPPED) and trace:
                fault = (mu.reg_read(A.UC_ARM64_REG_PC), 0, list(mu.reg_read_batch(e.xregs)), loaded, dirty)
        for h_ in hk:
            mu.hook_del(h_)
        wins = self.windows() if reason is None else None
        if reason is None and wins is None:
            reason = 'too many write windows'
        if reason is None:
            for a, n in wins:
                if a < self.base or a + n > self.base + len(self.img):
                    reason = 'write outside the image'
        if reason is not None:
            self.restore([])
            if fault is not None or (not trace and 'UNMAPPED' in reason):
                return ('fault', fault, reason)
            return reason
        if self.log:
            print('CASE', *self.log, sep='\n', flush=True)
        o = mu.reg_read_batch(e.out_regs)
        crc = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
        wcrc = 0
        for a, n in wins:
            wcrc = zlib.crc32(bytes(mu.mem_read(a, n)), wcrc)
        self.restore(wins)
        return (list(o[:31]), o[31], o[32] & 0xF0000000, o[33], list(o[34:]), (crc, wcrc if wins else 0)), wins

    def restore(self, wins):
        mu = self.emu.mu
        mu.mem_write(BUF, self.buf0)
        ws = wins or [(a, n) for a, n in self.writes]
        for a, n in ws:
            if self.base <= a and a + n <= self.base + len(self.img):
                mu.mem_write(a, self.img[a - self.base:a - self.base + n])


if __name__ == '__main__':
    sys.exit(main())
