#!/usr/bin/env python3
"""scope_threads.py - thread / fiber / sync inventory for v262144 main -> analysis/runtime/threads.md
Run with: python -I scripts/scope_threads.py --pkl <pkl from scope_scan.py> (scope_report.py must have run: it writes <pkl>.graph)
Argument values are recovered by a short forward simulation over up to 300 instructions before each call site
(mov/movz/movk/adrp/add/GOT-load/register copies); anything else is reported as 'var'. Read-only, static."""
import argparse, os, pickle, re, sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_scan import tsv, hx, ROOT, set_lowprio, Image, load_relocs, GOT, GOTPLT
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

OUT = os.path.join(ROOT, 'analysis', 'runtime')


def simulate(img, md, fs, site, rel, window=300):
    lo = max(fs, site - 4 * window)
    code = img.rd(lo, site - lo)
    R = {}
    for ins in md.disasm(code, lo):
        m, o = ins.mnemonic, ins.op_str
        ops = [x.strip() for x in o.split(',')]
        d = ops[0] if ops else ''
        # normalise w/x names
        def key(r):
            return r.replace('w', 'x') if re.fullmatch(r'w\d+', r) else r
        def val(x):
            x = x.strip()
            if x.startswith('#'):
                return int(x[1:], 0)
            if x in ('xzr', 'wzr'):
                return 0
            return R.get(key(x))
        dd = key(d)
        if m == 'mov' and len(ops) == 2:
            R[dd] = val(ops[1])
        elif m == 'movz':
            sh = 0
            mm = re.search(r'lsl #(\d+)', o)
            if mm:
                sh = int(mm.group(1))
            R[dd] = int(ops[1].split()[0][1:], 0) << sh
        elif m == 'movk':
            sh = 0
            mm = re.search(r'lsl #(\d+)', o)
            if mm:
                sh = int(mm.group(1))
            v = int(ops[1].split()[0][1:], 0)
            cur = R.get(dd)
            R[dd] = ((cur & ~(0xffff << sh)) | (v << sh)) if isinstance(cur, int) else None
        elif m == 'adrp':
            R[dd] = ('a', int(ops[1][1:], 0))
        elif m == 'add' and len(ops) == 3 and ops[2].startswith('#'):
            s = R.get(key(ops[1]))
            imm = int(ops[2][1:], 0)
            R[dd] = ('a', s[1] + imm) if isinstance(s, tuple) else (s + imm if isinstance(s, int) else None)
        elif m == 'ldr' and ops[0].startswith('x') and len(ops) >= 2:
            mm = re.match(r'\[(x\d+)(?:, #(0x[0-9a-f]+|\d+))?\]', ', '.join(ops[1:]))
            if mm:
                s = R.get(mm.group(1))
                imm = int(mm.group(2), 0) if mm.group(2) else 0
                if isinstance(s, tuple):
                    ea = s[1] + imm
                    rl = rel.get(ea)
                    if rl and rl[0] == 'RELATIVE':
                        R[dd] = ('a', rl[2])
                    else:
                        R[dd] = None
                else:
                    R[dd] = None
            else:
                R[dd] = None
        elif m in ('bl', 'blr'):
            for k in range(19):
                R['x%d' % k] = None
        elif m in ('str', 'stp', 'strb', 'strh', 'stur', 'cmp', 'tst', 'b', 'cbz', 'cbnz', 'ret', 'nop', 'prfm', 'stlr', 'stlxr') or m.startswith('b.'):
            pass
        else:
            if d.startswith(('x', 'w')):
                R[dd] = None
            if m in ('ldp', 'ldpsw') and len(ops) > 1 and ops[1].startswith(('x', 'w')):
                R[key(ops[1])] = None
    return R


def fmt(v):
    if v is None:
        return 'var'
    if isinstance(v, tuple):
        return '%#x' % v[1]
    return '%d' % v if v < 4096 else '%#x' % v


def main():
    set_lowprio()
    ap = argparse.ArgumentParser()
    ap.add_argument('--pkl', required=True)
    a = ap.parse_args()
    R = pickle.load(open(a.pkl, 'rb'))
    G = pickle.load(open(a.pkl + '.graph', 'rb'))
    rev = G['rev']
    seen = G['seen']
    seen2 = G['seen2']
    ev = R['events']
    img = Image()
    rel = load_relocs()
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = False
    imports = {r['name']: r for r in tsv('imports.tsv')}
    funcs = R['funcs']
    import bisect
    starts = [f[0] for f in funcs]

    def func_of(x):
        i = bisect.bisect_right(starts, x) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= x < funcs[i][1] else None

    # site lists per import
    sites = defaultdict(list)  # name -> [(func, pc)]
    for f, e in ev.items():
        for pc, n, k in e['plt']:
            sites[n].append((f, pc))
        for pc, n, called in e['imp_ld']:
            if called:
                sites[n].append((f, pc))

    def dem(n):
        return imports[n]['demangled'] or n if n in imports else n

    def match(pat):
        return sorted(n for n in imports if re.search(pat, dem(n)) or re.search(pat, n))

    create = [n for n in imports if re.search(r'nn::os::CreateThread|^nnosCreateThread', dem(n)) or n == 'nnosCreateThread']
    out = []
    out.append('# Threads, fibers and sync primitives (v262144 main)\n')
    out.append('Static, read-only. Source: `scripts/scope_threads.py` (argument recovery by short forward simulation before each call site; `var` = not a constant in that window) on the call-site scan from `scripts/scope_scan.py`. Module offsets.\n')
    # ---- thread creation
    rows = []
    for n in create:
        for f, pc in sites[n]:
            r = simulate(img, md, f, pc, rel)
            ent = r.get('x1')
            rows.append((f, pc, n, r))
    rows.sort(key=lambda x: (x[0], x[1]))
    # SetThreadName strings by function
    nm_sites = defaultdict(list)
    for n in match(r'SetThreadName|nnosSetThreadNamePointer'):
        for f, pc in sites[n]:
            r = simulate(img, md, f, pc, rel)
            s = None
            x1 = r.get('x1')
            if isinstance(x1, tuple):
                s = img.cstr(x1[1])
            nm_sites[f].append((pc, s))
    out.append('## Thread creation sites (%d)\n' % len(rows))
    out.append('| site | in function | overload | entry | arg (x2) | stack ptr (x3) | stack size (x4) | priority | core (7-arg only) | name candidates in same function | function reachable (direct / tier 2) | callers of the function |\n|---|---|---|---|---|---|---|---|---|---|---|---|')
    summary_rows = []
    for f, pc, n, r in rows:
        seven = 'CreateThreadEiiE' in n or 'EmmiiE' in n or 'EPvmii' in n
        ov = '7-arg (core)' if re.search(r'Ellii$|Emii$|PvmiiE|iiE$', n) and 'EPvmi' in n and n.endswith('ii') else None
        dn = dem(n)
        sev = dn.count(',') == 6
        ent = r.get('x1')
        names = [s for p, s in nm_sites.get(f, []) if s]
        reach = ('direct' if f in seen else ('tier2' if f in seen2 else 'no'))
        callers = ', '.join('%#x' % c for c in rev.get(f, [])[:5]) or 'none (address-taken)'
        core = fmt(r.get('x6')) if sev else 'n/a (default core)'
        row = '| %#x | %#x | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            pc, f, '7-arg' if sev else ('C wrapper' if n == 'nnosCreateThread' else '6-arg'), fmt(ent), fmt(r.get('x2')), fmt(r.get('x3')), fmt(r.get('x4')),
            fmt(r.get('x5')), core, ', '.join('"%s"' % x for x in names[:3]) or '-', reach, callers)
        out.append(row)
        summary_rows.append((pc, f, ent, r, sev, names, reach))
    out.append('')
    # entry function hints
    out.append('### Entry functions\n')
    for pc, f, ent, r, sev, names, reach in summary_rows:
        if isinstance(ent, tuple):
            e0 = ent[1]
            e = ev.get(e0, {})
            calls = [dem(n) for _, n, k in e.get('plt', [])][:8]
            calls = [re.sub(r'\(.*', '', c) for c in calls]
            out.append('- site %#x -> entry %#x: imports called directly: %s; direct callees %d; indirect blr %d.' % (
                pc, e0, ', '.join(calls) or 'none', len(e.get('calls', [])), e.get('blr', 0)))
        else:
            out.append('- site %#x -> entry %s (not a constant: thread entry passed in by the caller, so creation is wrapped; see callers column).' % (pc, fmt(ent)))
    out.append('')
    # ---- start/wait/priority/core mask
    def listing(title, pat, argcols):
        ms = match(pat)
        tot = sum(len(sites[n]) for n in ms)
        out.append('## %s (%d sites)\n' % (title, tot))
        out.append('| site | function | import | args |\n|---|---|---|---|')
        for n in ms:
            for f, pc in sorted(sites[n]):
                r = simulate(img, md, f, pc, rel)
                out.append('| %#x | %#x | %s | %s |' % (pc, f, re.sub(r'\(.*', '', dem(n)).replace('nn::os::', ''), ', '.join('%s=%s' % (c, fmt(r.get(c))) for c in argcols)))
        out.append('')
    listing('SetThreadCoreMask', r'SetThreadCoreMask', ['x1', 'x2'])
    listing('ChangeThreadPriority / GetThreadPriority', r'ChangeThreadPriority|GetThreadCurrentPriority|GetThreadPriority', ['x1'])
    listing('Thread names (SetThreadName)', r'SetThreadName|nnosSetThreadNamePointer', ['x1'])
    # decode names as strings
    out2 = []
    for f, lst in sorted(nm_sites.items()):
        for pc, s in lst:
            out2.append('%#x in %#x: %s' % (pc, f, s or 'var'))
    out.append('Thread name strings recovered: ' + '; '.join(out2) + '\n')
    out.append('## Counts: start / wait / destroy / yield / sleep / core query\n')
    for pat, lab in ((r'nn::os::StartThread|nnosStartThread', 'StartThread'), (r'WaitThread|nnosWaitThread', 'WaitThread'), (r'DestroyThread|nnosDestroyThread', 'DestroyThread'),
                     (r'YieldThread', 'YieldThread'), (r'SleepThread|nnosSleepThread', 'SleepThread'), (r'GetCurrentCoreNumber', 'GetCurrentCoreNumber'),
                     (r'GetCurrentThread|nnosGetCurrentThread', 'GetCurrentThread'), (r'GetThreadId', 'GetThreadId')):
        ms = match(pat)
        out.append('- %s: %d sites (%d functions)' % (lab, sum(len(sites[n]) for n in ms), len({f for n in ms for f, _ in sites[n]})))
    out.append('')
    # ---- fibers
    out.append('## Fibers\n')
    for pat, lab in ((r'InitializeFiber', 'InitializeFiber'), (r'SwitchToFiber', 'SwitchToFiber'), (r'FinalizeFiber', 'FinalizeFiber'), (r'GetCurrentFiber', 'GetCurrentFiber')):
        ms = match(pat)
        out.append('- **%s**: %d sites: %s' % (lab, sum(len(sites[n]) for n in ms), ', '.join('%#x (in %#x)' % (pc, f) for n in ms for f, pc in sorted(sites[n]))))
    for n in match('InitializeFiber'):
        for f, pc in sites[n]:
            r = simulate(img, md, f, pc, rel)
            out.append('  - InitializeFiber at %#x: entry %s, arg %s, stack %s, size %s, flag %s. Signature (FiberType*, FiberType*(*)(void*), void* arg, void* stack, size_t size, int flags).' % (
                pc, fmt(r.get('x1')), fmt(r.get('x2')), fmt(r.get('x3')), fmt(r.get('x4')), fmt(r.get('x5'))))
    out.append('')
    # ---- sync inventory
    out.append('## Sync primitives and scheduler-facing calls\n')
    out.append('| primitive | imports | call sites | functions |\n|---|---|---|---|')
    groups = [
        ('nn::os Mutex', r'nn::os::(Initialize|Finalize|Lock|Unlock|TryLock)Mutex|nnos(Initialize|Finalize|Lock|Unlock)Mutex'),
        ('nn::os Event', r'nn::os::\w*(Initialize|Finalize|Signal|Clear|Wait|TimedWait)Event\(|nnos\w*Event$'),
        ('nn::os SystemEvent', r'SystemEvent'),
        ('nn::os TimerEvent', r'TimerEvent'),
        ('nn::os Semaphore', r'Semaphore'),
        ('nn::os ConditionVariable', r'ConditionVariable'),
        ('nn::os TLS', r'Tls'),
        ('nn::os Thread', r'nn::os::\w*Thread|nnos\w*Thread'),
        ('nn::os Fiber', r'Fiber'),
        ('pthread (libc)', r'^pthread_'),
        ('std::mutex / recursive_mutex', r'std::(recursive_)?mutex'),
        ('std::condition_variable', r'std::condition_variable'),
        ('std::__shared_mutex_base', r'__shared_mutex_base'),
        ('std::__shared_weak_count (atomic refcount)', r'__shared_weak_count'),
        ('__cxa_guard (static init locks)', r'__cxa_guard'),
        ('std::future / async', r'__assoc_sub_state|future_error'),
    ]
    for lab, pat in groups:
        ms = [n for n in imports if re.search(pat, n) or re.search(pat, dem(n))]
        if lab.startswith('nn::os Thread'):
            ms = [n for n in ms if not re.search(r'Fiber|SystemEvent', dem(n))]
        out.append('| %s | %d | %d | %d |' % (lab, len(ms), sum(len(sites[n]) for n in ms), len({f for n in ms for f, _ in sites[n]})))
    out.append('')
    out.append('Atomics are inline: the binary has no LSE instructions and 39,150 LL/SC exclusive words in 3,099 functions (AGENTS.md), so every `ldaxr/stlxr` loop is a possible spin or lock and needs the scheduler polling hook.\n')
    # ---- generic wrapper analysis (manual disassembly of 0x8dea70 and 0x8de680, encoded here so the report is reproducible)
    def callsites(target):
        res = []
        for f, e in ev.items():
            for pc, tf, k, t in e['calls']:
                if tf == target:
                    res.append((f, pc))
        return sorted(res)
    w = callsites(0x8dea70)
    fw = callsites(0x8de680)
    out.append('## Game thread wrapper 0x8dea70 (found by reading its disassembly)\n')
    out.append('Only 10 `CreateThread` call sites exist, but 0x8dea70 is a generic "start thread from a descriptor" function: it is the CreateThread site 0x8debc0 listed above (the one with `priority 1` in the table is only the last `mov w5` before the call; the priority is chosen in the switch below). It is called from %d sites in %d functions, including nnMain (site 0x4e496c). Behavior:\n' % (len(w), len({f for f, _ in w})))
    out.append('- Stack: 4 KB-aligned allocation of `desc[0x88]` bytes (via 0x991c30), thread object lives inside the owner object at +0x90, entry is the trampoline 0x8dee90 with the owner as argument (the trampoline calls the object through a vtable, 5 `blr`).')
    out.append('- Priority enum `desc[0x30]` -> Horizon priority: 0 = inherit current thread priority (GetCurrentThread + GetThreadPriority), 1 = 31 (default), 2 = 23, 3 = 16, 4 = 8, 5 = 1, 6 or more = 31 (jump table at 0xb285cd).')
    out.append('- Core mask `desc[0x38]`: -1 -> 7 (all three cores); 1..7 used as given; anything else 7. Ideal core: if the mask has exactly one bit, log2 of it, else -1 (dont-care). Then `SetThreadName` (desc name or a default string), `SetThreadCoreMask(thread, ideal, mask)`, `StartThread`. `desc[0x82]` flag stored at owner+0x45, `desc[0x81]` selects the default name.')
    out.append('- Callers (function: sites): ' + ', '.join('%#x: %d' % (f, n) for f, n in sorted(Counter(f for f, _ in w).items())) + '.')
    out.append('- Fiber init wrapper 0x8de680 (contains the only `InitializeFiber`, 0x8de6e0, flags 1, entry 0x8de7b0) is called from %s. The five `SwitchToFiber` sites sit in 0x548f10, 0x549570, 0x54c020, 0x54ddf0, 0x54e040: a fiber-based job scheduler (0x548000..0x54f000), whose worker threads use `SetThreadCoreMask(-3, 7)` (-3 = keep ideal core, mask 7) at 0x54be94, 0x54c4c0, 0x54c630 and 0x54ecd0.' % (', '.join('%#x (site %#x)' % (f, pc) for f, pc in fw) or 'no direct caller'))
    out.append('- Core masks seen as constants are always 7 (all three cores) with ideal core -1 (default wrapper) or -3 (no update); no thread in the static constants is pinned to one core, so the three-virtual-core model matters for `GetCurrentCoreNumber` users (2 sites) and for Wwise (`AK::` threads) which pass the mask from `AkThreadProperties`.')
    out.append('- Wwise (`AK::`) threads: names AK Suspended, AK::BankManager, AK::EventManager, AK::OpusDecodingThread, AK::IOThread, created through one `nn::os::CreateThread` each, with stack, size and priority taken from the Wwise thread-properties struct (not constants). Other native threads: `LibcurlResolver` (C wrapper `nnosCreateThread`, thread entry 0x2546c8), a timer thread (0x159288: stack 0x2000 static buffer, priority 16, waits on a timer event) and the thread started from 0x162dc0 (calls `ChangeThreadPriority` then `GetThreadId`; second site 0x162eb0 is the 7-arg overload with the core argument).\n')
    open(os.path.join(OUT, 'threads.md'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('written %d thread rows' % len(rows), file=sys.stderr)


if __name__ == '__main__':
    main()
