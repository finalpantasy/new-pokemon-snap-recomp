# Via TotK Recomp recomp/gen.py main() @ 7a37fc3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), from LAS Recomp
# (MIT, Copyright (c) 2026 Peiikai). Rewritten for Snap: the function list is analysis/maps/functions_final.tsv, the
# input is the ELF, PLT stubs become weak import stubs, jump tables come from jump_tables.tsv.
"""Generate C for all of `main`: every function of functions_final.tsv plus the PLT stubs.

Usage: tools/venv-recomp/Scripts/python -I recomp/snap_gen.py [--module main|sdk] [--out generated/v262144] [--jobs N] [--no-resume]
                                                             [--chunk-bytes B] [--chunk-fns N] [--big-fn B] [--limit N]

Output under --out (git-ignored; generated game code, never committed):
  main/main_NNNN.c       `void f_<addr>(Cpu *c)` per guest function, about --chunk-bytes of C per file
  main/main_table.c      sorted main_fn_addrs[] / main_fn_ptrs[] / main_fn_count for rt_dispatch (functions + PLT stubs)
  main/plt_stubs.c       one `f_<addr>` per PLT stub that calls the weak `imp_<name>` of its import (default: log and abort)
  main/unresolved.c      log-and-abort definitions for every f_<addr> that is referenced but not defined
  main/manifest.json     chunk list, statistics, timings, peak memory

Chunk contents depend only on the inputs; a chunk whose text did not change is not rewritten (mtime kept), so the
build (recomp/snap_build.py) recompiles only the translation units that changed.
"""
import argparse
import json
import multiprocessing as mp
import os
import re
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snap_common import (BASE, MAPS, MODULES, ROOT, SDK_ELF, MemSampler, elf_sections, functions, load_module,  # noqa: E402
                         lower_priority, tsv)
import ehlsda  # noqa: E402
import gen  # noqa: E402
from gen import Emitter, chunk_text, fname, header, write_if_changed  # noqa: E402
from lifter import hx  # noqa: E402

OUT = ROOT / 'generated' / 'v262144'


def merge_ranges(ranges):
    out = []
    for s, e in sorted(ranges):
        if out and s <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([s, e])
    return out


# ------------------------------------------------------------------------------------------------ worker

_W = {}


def _worker_init(module, starts, use_map_jt, resume, hooks, plt_got, setjmp_targets):
    lower_priority()
    _W.update(module=module, starts=starts, jt=use_map_jt, resume=resume, hooks=hooks, plt_got=plt_got,
              setjmp=setjmp_targets, emitter=None)


def _emitter():
    e = _W['emitter']
    if e is None:
        mod = load_module(use_map_jump_tables=_W['jt'], name=_W['module'])
        lsda = ehlsda.module_pads(mod) if _W['resume'] else {}
        e = _W['emitter'] = Emitter(mod, set(_W['starts']), lsda=lsda, resume=_W['resume'], hooks=_W['hooks'],
                                    plt_got=_W['plt_got'], setjmp_targets=_W['setjmp'])
    return e


def _emit_chunk(task):
    number, path, fns = task
    t0 = time.perf_counter()
    e = _emitter()
    e.stats.clear()
    e.unsupported.clear()
    text = chunk_text(_W['module'], number, e, fns)
    err = None
    try:
        written = write_if_changed(Path(path), text)
    except OSError as ex:
        written, err = False, f'{path}: {ex}'
    refs = set(re.findall(r'^void (f_[0-9a-f]+)\(Cpu \*c\);$', text, re.M))
    return {'number': number, 'file': Path(path).name, 'functions': len(fns), 'bytes': len(text), 'written': written,
            'error': err, 'seconds': time.perf_counter() - t0, 'stats': dict(e.stats), 'unsupported': dict(e.unsupported),
            'refs': sorted(refs), 'peak_mb': peak_rss_mb(), 'insns': sum((en - s) // 4 for s, en in fns)}


# ------------------------------------------------------------------------------------------------ sizes and chunks

def estimate(mod, fns):
    """Estimated bytes of C per function (gen.estimate_sizes costs, on our function extents)."""
    w = mod.words
    cost = np.full(len(w), gen.EST_INT, dtype=np.int64)
    cost[(w & 0x0E000000) == 0x0E000000] = gen.EST_SIMD
    cost[((w & 0x0A000000) == 0x08000000) & (((w >> 26) & 1) == 1)] = gen.EST_SIMD_LDST
    has_br = np.zeros(len(w), dtype=np.int64)
    has_br[np.flatnonzero((w & 0xFFFFFC1F) == 0xD61F0000)] = 1
    cs = np.concatenate([[0], np.cumsum(cost)])
    bs = np.concatenate([[0], np.cumsum(has_br)])
    s = np.array([f[0] for f in fns]) >> 2
    e = np.array([f[1] for f in fns]) >> 2
    size = cs[e] - cs[s] + gen.EST_FN
    wb = (bs[e] - bs[s]) > 0
    size[wb] += (e[wb] - s[wb]) * gen.EST_BR_LABELS
    return size


def plt_stubs():
    """[(addr, import name or None)] for every PLT stub (module offsets)."""
    out = []
    for r in tsv(MAPS / 'functions_final.tsv'):
        if r['kind'] == 'plt_stub':
            n = r['notes']
            out.append((int(r['start'], 16), n[len('import:'):] if n.startswith('import:') else None))
    return out


def ident(name):
    return re.sub(r'[^A-Za-z0-9_]', '_', name)


def plt_text(stubs):
    lines = [header('PLT stubs'), '#include "cpu.h"', '',
             '// Each PLT stub calls imp_<name>. The default binding hands the call to the loader-resolved target of the import',
             '// (host override, sdk definition or main definition; host/loader.cpp rt_import_call). A strong imp_<name>(Cpu *c)',
             '// defined by the host replaces it (arguments in c->x[0..7], result in c->x[0]).', '']
    seen = {}
    for i, (a, n) in enumerate(stubs):
        key = n if n else f'plt_{a:x}'
        if key not in seen:
            seen[key] = f'imp_{ident(key)}'
            lines.append(f'__attribute__((weak)) void {seen[key]}(Cpu *c) {{ rt_import_call(c, {i}); }}')
    lines.append('')
    for a, n in stubs:
        key = n if n else f'plt_{a:x}'
        lines.append(f'void {fname(BASE + a)}(Cpu *c) {{ {seen[key]}(c); }}')
    lines.append('')
    lines.append('const char *const main_import_names[] = {' + ','.join(f'"{(n or "plt_%x" % a)}"' for a, n in stubs) + '};')
    lines.append('const uint64_t main_import_stub_addrs[] = {' + ','.join(hx(BASE + a) for a, _ in stubs) + '};')
    lines.append(f'const uint64_t main_import_count = {len(stubs)};')
    return '\n'.join(lines) + '\n', len(seen)


def table_text(addrs, mod='main'):
    lines = [header(f'{mod} function table'), '#include "cpu.h"\n\n']
    lines += [f'void {fname(a)}(Cpu *c);\n' for a in addrs]
    lines.append(f'\nconst uint64_t {mod}_fn_addrs[] = {{\n' + ',\n'.join(hx(a) for a in addrs))
    lines.append(f'\n}};\nconst GuestFn {mod}_fn_ptrs[] = {{\n' + ',\n'.join(fname(a) for a in addrs))
    lines.append(f'\n}};\nconst uint64_t {mod}_fn_count = {len(addrs)};\n')
    return ''.join(lines)


SETJMP_NAMES = ('setjmp', '_setjmp', '__setjmp', 'sigsetjmp', '__sigsetjmp')
# sdk functions replaced by host code (module offsets). The bodies are libunwind's register restore, musl's longjmp and
# setjmp, and nn::ro::detail::GetExceptionInfo (docs/runtime-boundary.md sections 2 and 3).
SDK_HOOKS = {0x513794: 'rt_hook_unw_jumpto', 0x51d584: 'rt_hook_longjmp', 0x51d5c4: 'rt_hook_setjmp',
             0x35cc40: 'rt_hook_get_exception_info'}


def sdk_plan():
    """Function extents of sdk by discovery (recomp/gen.py discover), plus PLT stubs {offset: GOT slot offset}.
    Returns (fns [(start, end)] module offsets, plt {offset: got offset}, discovery stats)."""
    mod = load_module(name='sdk')
    starts, stats = gen.discover(mod)
    s = {int(mod.lo + 4 * x) - mod.base for x in starts}
    secs = elf_sections(SDK_ELF)
    plt = {}
    plo, psz = secs['.plt']
    w = mod.words
    for off in range(plo, plo + psz - 12, 4):
        i = (off - mod.nso.text[0]) >> 2
        a, b, c3, d = (int(w[i + k]) for k in range(4))
        if (a & 0x9F00001F) == 0x90000010 and (b & 0xFFC003FF) == 0xF9400211 and d == 0xD61F0220:
            imm = ((((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3))
            imm = (imm - (1 << 21) if imm & (1 << 20) else imm) << 12
            plt[off] = ((off & ~0xFFF) + imm) + ((b >> 10) & 0xFFF) * 8
    stats = dict(stats, plt_stubs=len(plt))
    s |= set(plt)
    s |= set(SDK_HOOKS)
    s |= set(range(0x399c18, 0x399fa8, 8))   # SVC stub table (docs/runtime-boundary.md)
    s = sorted(x for x in s if 0 <= x < mod.nso.text[1])
    fns = [(a, s[i + 1] if i + 1 < len(s) else mod.nso.text[1]) for i, a in enumerate(s)]
    return fns, plt, stats


def peak_rss_mb():
    import ctypes
    from ctypes import wintypes

    class PMC(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                    ('WorkingSetSize', ctypes.c_size_t), ('a', ctypes.c_size_t), ('b', ctypes.c_size_t),
                    ('c', ctypes.c_size_t), ('d', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                    ('PeakPagefileUsage', ctypes.c_size_t)]
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    ctypes.windll.psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
    ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.windll.kernel32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
    return pmc.PeakWorkingSetSize / 2**20


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--module', choices=('main', 'sdk'), default='main')
    ap.add_argument('--out', type=Path, default=OUT)
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--chunk-fns', type=int, default=3000)
    ap.add_argument('--chunk-bytes', type=int, default=1500000)
    ap.add_argument('--big-fn', type=int, default=400000)
    ap.add_argument('--limit', type=int, default=0, help='emit only the first N functions (debug)')
    ap.add_argument('--jt-source', choices=('map', 'generator'), default='map')
    ap.add_argument('--no-resume', action='store_true', help='omit the exception/longjmp resume checks (overhead baseline)')
    args = ap.parse_args()
    lower_priority()
    sampler = MemSampler().__enter__()
    t0 = time.perf_counter()
    name = args.module
    base = MODULES[name][1]
    resume = not args.no_resume
    mod = load_module(use_map_jump_tables=args.jt_source == 'map', name=name)
    plt_got, hooks, setjmp_targets, stubs, disc_stats = {}, {}, set(), [], {}
    if name == 'main':
        fl = functions()
        all_starts = sorted(base + s for s, e, k, t in fl)                      # function, thunk, plt_stub, unresolved
        emit = sorted((base + s, base + e) for s, e, k, t in fl if k in ('function', 'thunk', 'unresolved'))
        stubs = plt_stubs()
        setjmp_targets = {base + a for a, n in stubs if n in SETJMP_NAMES}
    else:
        fns, plt, disc_stats = sdk_plan()
        all_starts = sorted(base + a for a, _ in fns)
        emit = [(base + a, base + e) for a, e in fns]
        plt_got = {base + a: base + g for a, g in plt.items()}
        hooks = {base + a: h for a, h in SDK_HOOKS.items()}
        slots = {r[0] for r in mod.nso.relocs if r[2] and mod.nso.syms[r[2]][0] in SETJMP_NAMES}
        setjmp_targets = {base + 0x51d5c4} | {base + a for a, g in plt.items() if g in slots}   # direct bl, and via the PLT
    if args.limit:
        emit = emit[:args.limit]
    est = estimate(mod, [(a - base, b - base) for a, b in emit])
    chunks = gen.partition(est, args.chunk_fns, args.chunk_bytes, args.big_fn)
    moddir = args.out / name
    moddir.mkdir(parents=True, exist_ok=True)
    tasks = []
    for n, (i0, i1) in enumerate(chunks):
        tasks.append((int(est[i0:i1].sum()), (n, str(moddir / f'{name}_{n:04d}.c'), emit[i0:i1])))
    t_prep = time.perf_counter() - t0
    print(f'{name}: {len(emit)} functions, {len(all_starts)} starts, {len(chunks)} chunks, resume={resume}, prep {t_prep:.1f}s',
          file=sys.stderr, flush=True)
    del mod

    tasks.sort(key=lambda t: -t[0])
    results, errors = [], []
    t_emit = time.perf_counter()
    ctx = mp.get_context('spawn')
    with ctx.Pool(args.jobs, initializer=_worker_init,
                  initargs=(name, all_starts, args.jt_source == 'map', resume, hooks, plt_got, sorted(setjmp_targets))) as pool:
        for i, r in enumerate(pool.imap_unordered(_emit_chunk, [t for _, t in tasks], chunksize=1)):
            results.append(r)
            if r['error']:
                errors.append(r['error'])
            if (i + 1) % 20 == 0 or i + 1 == len(tasks):
                print(f'emitted {i + 1}/{len(tasks)} chunks, {time.perf_counter() - t_emit:.0f}s', file=sys.stderr, flush=True)
    t_emit = time.perf_counter() - t_emit
    results.sort(key=lambda r: r['number'])

    # tables, PLT stubs, unresolved
    t1 = time.perf_counter()
    defined = {fname(a) for a, _ in emit} | {fname(base + a) for a, _ in stubs}
    refs = set()
    for r in results:
        refs |= set(r['refs'])
    undefined = sorted(refs - defined)
    table_addrs = sorted({a for a, _ in emit} | {base + a for a, _ in stubs})
    write_if_changed(moddir / f'{name}_table.c', table_text(table_addrs, name))
    nimp = 0
    if name == 'main':
        ptext, nimp = plt_text(stubs)
        write_if_changed(moddir / 'plt_stubs.c', ptext)
    ul = [header('unresolved callees'), '#include "cpu.h"', '']
    for f in undefined:
        a = int(f[2:], 16)
        ul.append(f'void {f}(Cpu *c) {{ rt_unresolved(c, {a:#x}ull, "guest address {a - base:#x} is not a function start of the lifted set"); }}')
    write_if_changed(moddir / 'unresolved.c', '\n'.join(ul) + '\n')
    # stale chunks
    keep = {r['file'] for r in results}
    for p in moddir.glob(f'{name}_[0-9][0-9][0-9][0-9].c'):
        if p.name not in keep:
            p.unlink()
    stats = Counter()
    unsup = Counter()
    for r in results:
        stats.update(r['stats'])
        unsup.update(r['unsupported'])
    t_total = time.perf_counter() - t0
    sampler.__exit__()
    manifest = {
        'module': name, 'resume_checks': resume, 'functions': len(emit), 'plt_stubs': len(stubs) or len(plt_got), 'distinct_imports': nimp,
        'table_entries': len(table_addrs), 'unresolved_callee_stubs': len(undefined), 'chunks': [r['file'] for r in results],
        'chunk_info': [{'file': r['file'], 'functions': r['functions'], 'insns': r['insns'], 'bytes': r['bytes'],
                        'seconds': round(r['seconds'], 2)} for r in results],
        'c_bytes': sum(r['bytes'] for r in results), 'instructions': sum(r['insns'] for r in results),
        'chunks_written': sum(r['written'] for r in results),
        'stats': {k: v for k, v in sorted(stats.items())}, 'unsupported': dict(unsup), 'discovery': disc_stats,
        'timings': {'prep_s': round(t_prep, 1), 'emit_wall_s': round(t_emit, 1),
                    'emit_worker_s': round(sum(r['seconds'] for r in results), 1), 'total_s': round(t_total, 1)},
        'jobs': args.jobs, 'system_peak_used_mb': sampler.peak_used_mb, 'parent_peak_rss_mb': round(peak_rss_mb()), 'worker_peak_rss_mb': round(max(r['peak_mb'] for r in results)),
        'jt_source': args.jt_source, 'chunk_bytes_target': args.chunk_bytes,
    }
    write_if_changed(moddir / 'manifest.json', json.dumps(manifest, indent=1) + '\n')
    print(json.dumps({k: v for k, v in manifest.items() if k not in ('chunks', 'chunk_info')}), file=sys.stderr)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
