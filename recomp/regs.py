# Via TotK Recomp @ 7a37fc3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh); modified for the Snap prototype where noted.
"""Function-local general registers for recomp/gen.py.

The lifter reads and writes the guest's X0-X30 and SP in the Cpu (c->x[n], c->sp), so every guest instruction loads
and stores memory, and the C compiler cannot keep a register in a host register across a guest store: guest stores go
through pointers that may alias the Cpu. A generated function instead copies the LOCALS registers it uses most into
locals at its entry (xN, sp_), works on those, and syncs with the Cpu only where something else reads or writes them:
before a call, an SVC, a trap, a return or a tail exit it stores the registers it changed since the last sync (spill),
and after an SVC it loads them all again (reload). The other registers stay in the Cpu.

After a call it reloads only X0-X18 and X30: AAPCS64 has the callee preserve X19-X29 and SP, so their locals still
hold what the Cpu holds. What would break that does not occur in TotK: main and subsdk0 import neither longjmp nor
the C++ unwinder, sdk's longjmp, __cxa_throw and _Unwind_* have no direct callers, and the runtime writes no register
above X3 of a running thread's Cpu except X30, which rt_call restores (runtime/hostfn.c). Guest fibers each run on a host fiber of their own with their own Cpu (runtime/kernel.c),
and ExitThread leaves by longjmp without returning. gen.py --no-callee-saved-trust reloads those registers too.

Only plain assignments write registers in the lifted C (`c->x[n] = ...`, `c->sp = ...`), and no code takes a
register's address, so the rewrite is textual. A register counts as dirty from a write until the next spill on some
path: plan() runs that forward over the function's control flow, so a call spills only what changed since the
previous call or the entry. Spilling a register that is not dirty is harmless (the local equals the Cpu), so where a
case is unclear the plan errs on spilling more.
"""
import re
from collections import Counter

REG = re.compile(r'c->x\[(\d+)\]|c->sp\b')
WRITE = re.compile(r'c->(?:x\[(\d+)\]|(sp))\s*=(?!=)')
# Lifted instructions that hand the Cpu to the runtime: SVCs return, BRK and undefined words do not.
ESCAPE = re.compile(r'\brt_(?:svc|brk|udf)\(c\b')
SP = 31   # index of SP in the register sets
# Markers gen.py puts where the Cpu must be current, resolved by Plan.resolve(): spill before; reload all, or reload
# what a call may change, after.
SPILL, RELOAD, RELOAD_CALL = '\x00S', '\x00R', '\x00C'
CALLEE_SAVED = frozenset(range(19, 30)) | {SP}
# Locals per function: the most used registers. More cost more than they save, as clang then spills them to the stack
# and the syncs stay (300 hot TotK functions: fewest instructions and memory operands at 4-6).
LOCALS = 6
# Wider register plan (2026-11-23, worker cpufps; docs/cpu-fps.md): SNAP_GEN_REGS=<extra_kept>,<leaf_all>,<min>,<max>
# at generation, for experiments. Defaults are the measured choice.
import os as _os


def _knobs(var, default):
    """default's comma fields, the leading ones replaced by those of the environment variable var."""
    d, e = default.split(','), [x for x in (_os.environ.get(var) or '').split(',') if x != '']
    return e + d[len(e):]


_cfg = _knobs('SNAP_GEN_REGS', '0,0,2,0')
EXTRA_KEPT, LEAF_ALL, EXTRA_MIN, MAX_LOCALS = bool(int(_cfg[0])), bool(int(_cfg[1])), int(_cfg[2]), int(_cfg[3])
# V locals: SNAP_GEN_VREGS=<on>,<min>,<per call>,<max>: a function keeps every V register it names in locals
# (runtime/simd.h VB_) when it has at least <min> SIMD&FP instructions, at least <per call> of them per call, and names at
# most <max> V registers (more than the host's 16 vector registers spill to the stack and lose to the Cpu copy).
_vcfg = _knobs('SNAP_GEN_VREGS', '1,4,2,20')
V_LOCALS, V_MIN, V_PER_CALL, V_MAX = bool(int(_vcfg[0])), int(_vcfg[1]), float(_vcfg[2]), int(_vcfg[3])


V0 = 64   # plan ids V0 + n are the SIMD&FP registers Vn (vregs(), V locals below)


def name(r):
    return 'sp_' if r == SP else f'vl_[{r - V0}]' if r >= V0 else f'x{r}'


def cpu(r):
    return 'c->sp' if r == SP else f'c->v[{r - V0}]' if r >= V0 else f'c->x[{r}]'


def vregs(w):
    """(read or written, written, read, killed) as frozensets of plan ids V0 + n: the SIMD&FP registers the AArch64
    word w names, from its register fields; None when w is not a SIMD&FP instruction. The first three are
    over-approximations; killed (registers the word always writes whole without reading them) is an under-approximation.
    Data processing SIMD&FP (op0 = x111) writes at most Rd and reads at most Rd, Rn, Rm and Ra (3-source FP), or the
    table Rn..Rn+len (TBL, TBX). SIMD&FP loads and stores (op0 = x1x0, V = 1) access Rt, Rt2 (pairs) or Rt..Rt+3
    (structures), and only loads write them; their Rn and Rm are general registers."""
    op0 = (w >> 25) & 0xF
    if op0 & 7 == 7:
        d, n, a, m = w & 31, (w >> 5) & 31, (w >> 10) & 31, (w >> 16) & 31
        used = {d, n, m}
        reads, kill = set(used), False
        if (w >> 24) & 0x1F == 0x1F and not (w >> 29) & 5:           # FP data-processing, 3 source (FMADD and the like)
            used.add(a)
            reads, kill = {n, m, a}, True
        elif (w >> 24) & 0x5F == 0x1E and (w >> 21) & 1 and not (w >> 29) & 5:   # scalar FP
            op = (w >> 10) & 0x3F
            if op & 0x1F == 0x10:                              # 1-source
                reads, kill = {n}, True
            elif op & 3 in (2, 3):                             # 2-source, conditional select
                reads, kill = {n, m}, True
            elif op & 7 == 4:                                  # FMOV (scalar, immediate)
                reads, kill = set(), True
        elif (w >> 19) & 0x3FF == 0x1E0 and (w >> 10) & 1 and not (w >> 31) & 1:   # AdvSIMD modified immediate
            cmode = (w >> 12) & 0xF
            if not (cmode < 12 and cmode & 1):                 # MOVI, MVNI, FMOV (vector, immediate); not ORR, BIC
                reads, kill = set(), True
        if w & 0xBFE08C00 == 0x0E000000:                       # TBL / TBX: table Rn..Rn+len
            used |= {(n + k) & 31 for k in range(((w >> 13) & 3) + 1)}
            reads |= used
            kill = False

        def f(rs):
            return frozenset(V0 + r for r in rs)
        return f(used), f([d]), f(reads), f([d]) if kill else frozenset()
    if op0 & 5 == 4 and (w >> 26) & 1:
        t, t2 = w & 31, (w >> 10) & 31
        whole = True
        if (w >> 27) & 7 == 5:                                 # pairs
            used, load = {t, t2}, (w >> 22) & 1
        elif not (w >> 31) & 1 and (w >> 24) & 0x3E == 0x0C:   # LD1-4 / ST1-4, multiple and single structures
            used, load, whole = {(t + k) & 31 for k in range(4)}, (w >> 22) & 1, False
        elif (w >> 27) & 7 == 3 and not (w >> 24) & 3:          # LDR (literal)
            used, load = {t}, 1
        else:                                                  # LDR / STR (immediate, register), LDUR / STUR
            used, load = {t}, (w >> 22) & 1
        used = frozenset(V0 + r for r in used)
        if not load:
            return used, frozenset(), used, frozenset()
        if whole:
            return used, used, frozenset(), used
        return used, used, used, frozenset()
    return None


def refs(text):
    """Registers text reads or writes, once per occurrence."""
    return [SP if m.group(0) == 'c->sp' else int(m.group(1)) for m in REG.finditer(text)]


def written(text):
    return {SP if m.group(2) else int(m.group(1)) for m in WRITE.finditer(text)}


VANY = re.compile(r'c->v\[|\bV(?:REG|Q|W|S8|S16|S32|S64|SF32|SF64|HI64)\(|\b(?:fp|sl|sv)_\w+\(c\b|\bSV_FOP')
_VNUM = re.compile(r'c->v\[(\d+)\]|\bV(?:REG|Q)\(\w+, (?:\w+, )?(\d+)\)|\bVW\(\w+, \w+, (\d+),|\bV(?:S8|S16|S32|S64|SF32|SF64|HI64)\((\d+)\)')


def check_vsets(entry, texts, vsets):
    """Stop the generation when lifted C touches a V register that vregs() did not name for its instruction: a
    non-SIMD&FP instruction that names any, or an explicit register number outside the instruction's set."""
    for i, v in enumerate(vsets):
        t = texts[i]
        if v is None:
            if VANY.search(t):
                raise ValueError(f'{entry + 4 * i:#x}: V register access in a word vregs() does not decode')
            continue
        for m in _VNUM.finditer(t):
            r = int(next(g for g in m.groups() if g is not None))
            if V0 + r not in v[0]:
                raise ValueError(f'{entry + 4 * i:#x}: V{r} is not in the decoded set {sorted(x - V0 for x in v[0])}')


class Plan:
    def __init__(self, regs, spill, kept, vlive=None):
        self.regs = regs     # sorted registers the function keeps in locals (V registers last, as V0 + n)
        self.local = set(regs)
        self.spill = spill   # per instruction: registers to store at its sync points
        self.kept = kept     # registers a call leaves as they were (CALLEE_SAVED, or none; never a V register)
        self.vregs = [r for r in regs if r >= V0]
        # V liveness (plan()): vlive[i] = V registers that may be read after instruction i before a whole write, vlive[-1]
        # those live at the entry. A call reloads only those, the entry loads only those.
        self.vlive = vlive
        self._reload_call_v = {}
        # the same text for every instruction of the function, built once (resolve() ran these joins per instruction)
        self._reload = ''.join(f' {name(r)} = {cpu(r)};' for r in regs)
        self._reload_call = ''.join(f' {name(r)} = {cpu(r)};' for r in regs if r not in kept)
        self._spill_text = {}

    def decl(self):
        """Locals at the top of the function, loaded from the Cpu. With V locals, the SIMD&FP register macros of
        runtime/simd.h (VB_) name the local array vl_ until end() puts them back on the Cpu."""
        g = [r for r in self.regs if r < V0]
        out = 'uint64_t ' + ', '.join(f'{name(r)} = {cpu(r)}' for r in g) + ';' if g else ''
        if self.vregs:
            ent = [r for r in self.vregs if self.vlive is None or r in self.vlive[-1]]
            out += (' u128 vl_[32];' + ''.join(f' {name(r)} = {cpu(r)};' for r in ent) +
                    '\n#undef VB_\n#define VB_ vl_')
        return out

    def end(self):
        return '#undef VB_\n#define VB_ (c->v)' if self.vregs else ''

    def resolve(self, i, text):
        """text of instruction i with its registers on the locals and its sync markers expanded."""
        def local(m):
            r = SP if m.group(0) == 'c->sp' else int(m.group(1))
            return name(r) if r in self.local else m.group(0)

        if 'c->' in text:
            text = REG.sub(local, text)
            if self.vregs:
                text = text.replace('c->v[', 'VB_[')
        if '\x00' not in text:     # no sync marker (SPILL, RELOAD, RELOAD_CALL all start with NUL)
            return text
        key = tuple(sorted(self.spill[i]))
        spill = self._spill_text.get(key)
        if spill is None:
            spill = self._spill_text[key] = ''.join(f'{cpu(r)} = {name(r)}; ' for r in key)
        reload_call = self._reload_call
        if self.vlive is not None and RELOAD_CALL in text:
            live = self.vlive[i]
            reload_call = self._reload_call_v.get(live)
            if reload_call is None:
                reload_call = self._reload_call_v[live] = ''.join(
                    f' {name(r)} = {cpu(r)};' for r in self.regs if r not in self.kept and (r < V0 or r in live))
        return text.replace(SPILL, spill).replace(RELOAD, self._reload).replace(RELOAD_CALL, reload_call)


def pick(texts, uses, n_locals=LOCALS, kept=CALLEE_SAVED):
    """The registers a function keeps in locals: its n_locals most used ones, plus (EXTRA_KEPT) every register a call
    leaves as it was (kept: X19-X29, SP) that it uses at least EXTRA_MIN times, plus (LEAF_ALL) in a function without
    a call or SVC every register it uses at least EXTRA_MIN times.
    A kept register costs a load at the entry and a store at a sync point after a write, but never a reload after a
    call, while in the Cpu it is loaded again after every guest store (guest stores may alias the Cpu)."""
    ranked = [r for r, _ in sorted(uses.items(), key=lambda kv: (-kv[1], kv[0]))]
    regs = set(ranked[:n_locals])
    if EXTRA_KEPT:
        regs |= {r for r in ranked if r in kept and uses[r] >= EXTRA_MIN}
    if LEAF_ALL and not any(RELOAD_CALL in t or RELOAD in t for t in texts):
        regs |= {r for r in ranked if uses[r] >= EXTRA_MIN}
    if MAX_LOCALS and len(regs) > MAX_LOCALS:
        regs = set([r for r in ranked if r in regs][:MAX_LOCALS])
    return regs


def plan(texts, succ, clean, n_locals=LOCALS, kept=CALLEE_SAVED, vsets=None):
    """texts[i]: instruction i's C with markers; succ[i]: successor indices inside the function; clean: instructions
    after which every local equals the Cpu (a call or an SVC that spills and reloads on every path through it);
    n_locals: how many registers to keep in locals; kept: registers a call does not reload; vsets: None, or per
    instruction None or vregs() of its word, when the function keeps every V register it names in locals."""
    n = len(texts)
    uses = Counter(r for t in texts for r in refs(t))
    regs = pick(texts, uses, n_locals, kept)
    vw = [frozenset()] * n
    if vsets:
        for i, v in enumerate(vsets):
            if v:
                regs |= v[0]
                vw[i] = v[1]
    writes = [(written(t) & regs) | vw[i] for i, t in enumerate(texts)]
    dirty = [set() for _ in range(n)]
    seen = [False] * n
    work = [0] if n else []
    if n:
        seen[0] = True
    while work:
        i = work.pop()
        out = set() if i in clean else dirty[i] | writes[i]
        for s in succ[i]:
            if not seen[s] or not out <= dirty[s]:
                seen[s] = True
                dirty[s] |= out
                work.append(s)
    every = set().union(*writes) if writes else set()
    for i in range(n):
        if not seen[i]:   # not reached from the entry in the plan's flow: anything may be dirty
            dirty[i] = every
    # A sync point stores what was dirty on entry to its instruction and what the instruction wrote before it
    # (c->x[30] of a call): all the instruction's writes, which is at most a little more.
    vlive = vliveness(texts, succ, vsets, frozenset(r for r in regs if r >= V0), seen) if vsets else None
    return Plan(sorted(regs), [dirty[i] | writes[i] if SPILL in texts[i] else set() for i in range(n)], kept, vlive)


def vliveness(texts, succ, vsets, vall, seen):
    """Per instruction the V registers live after it (may be read before a whole write), and last the ones live at the
    entry. A sync point (SPILL: call, SVC, return, tail exit, trap, hook) reads them all, as does anything outside the
    plan's flow. An instruction that is not a SIMD&FP word names no V register (check_vsets)."""
    n = len(texts)
    gen, kill = [], []
    for i in range(n):
        v = vsets[i] if i < len(vsets) else None
        if SPILL in texts[i] or not seen[i]:
            gen.append(vall)
            kill.append(frozenset())
        elif v:
            gen.append(v[2] & vall)
            kill.append(v[3])
        else:
            gen.append(frozenset())
            kill.append(frozenset())
    pred = [[] for _ in range(n)]
    for i in range(n):
        for t in succ[i]:
            pred[t].append(i)
    live_in = list(gen)
    live_out = [frozenset()] * n
    work = list(range(n))
    on = [True] * n
    while work:
        i = work.pop()
        on[i] = False
        out = frozenset().union(*(live_in[t] for t in succ[i])) if succ[i] else frozenset()
        live_out[i] = out
        inn = gen[i] | (out - kill[i])
        if inn != live_in[i]:
            live_in[i] = inn
            for q in pred[i]:
                if not on[q]:
                    on[q] = True
                    work.append(q)
    return live_out + [live_in[0] if n else frozenset()]
