#!/usr/bin/env python3
"""reconcile_functions.py - authoritative function list for the AArch64 lifter (v262144 main).

Inputs (all local, read-only):
  extracted/v262144/exefs/main.uncompressed.nso      image bytes
  analysis/ghidra/<project>.fde.tsv                  .eh_frame FDE (start, length) list
  analysis/maps/functions.tsv, imports.tsv, relocations.tsv   Ghidra / image exports
Outputs (analysis/maps, deterministic: sorted, no timestamps):
  functions_final.tsv          one row per function and per classified gap, covering all of .text
  functions_final.md           counts, rules, coverage, risks
  functions_size_buckets.tsv   every SIZE_MISMATCH / FDE_NO_FUNCTION / FUNCTION_NO_FDE row with its cause bucket
  jump_tables.tsv              every `br xN` site classified; recovered jump tables with targets
  functions_interior_entries.tsv  call/address-taken targets that land inside a function (not at its start)

Rules (see functions_final.md): FDE range wins; non-FDE functions need positive evidence
(direct bl target, address-taken target, dynsym, init/fini array, thunk/PLT, tail-b from another function).
Run: python -I scripts/reconcile_functions.py [--root <dir>] [--project snap-v262144]
"""
import os, sys as _s
_s.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "pylib"))
import argparse, bisect, collections, hashlib, re, struct
import capstone

ap = argparse.ArgumentParser()
ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ap.add_argument("--project", default="snap-v262144")
ap.add_argument("--out", default=None)
args = ap.parse_args()
R = os.path.abspath(args.root)
OUT = args.out or os.path.join(R, "analysis", "maps")
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- image
nso = open(os.path.join(R, "extracted", "v262144", "exefs", "main.uncompressed.nso"), "rb").read()
SHA = hashlib.sha256(nso).hexdigest()
BUILD_ID = nso[0x40:0x54].hex().upper()
segs = []
for i in range(3):
    fo, mo, sz = struct.unpack_from("<3I", nso, 0x10 + 0x10 * i)
    segs.append((mo, nso[fo:fo + sz]))
text = segs[0][1]
TEXT_END = len(text)               # includes .plt (module offsets, base 0)
NW = TEXT_END // 4
W = struct.unpack_from("<%dI" % NW, text, 0)
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
H = lambda v: "0x%x" % v


def tsv(p):
    out = []
    for l in open(os.path.join(R, p), encoding="utf8"):
        if l.startswith("#") or not l.strip():
            continue
        out.append(l.rstrip("\n").split("\t"))
    return out


fde = sorted((int(a, 16), int(b, 16)) for a, b in tsv("analysis/ghidra/%s.fde.tsv" % args.project))
fdestart = dict(fde)
fde_starts = [a for a, _ in fde]
gf = {}
tags_all = {}
for r in tsv("analysis/maps/functions.tsv")[1:]:
    tags = r[8].split(",")
    tags_all[int(r[0], 16)] = tags
    if "ghidra" in tags:
        gf[int(r[0], 16)] = dict(size=int(r[2], 16), tags=tags, name=r[5])
imports = tsv("analysis/maps/imports.tsv")[1:]
plt = {}
for r in imports:
    if r[6]:
        plt[int(r[6], 16)] = r[1]
reloc_fn = set()       # RELATIVE reloc targets landing in .text
for r in tsv("analysis/maps/relocations.tsv")[1:]:
    if r[1] == "RELATIVE" and r[5] and r[6] == ".text":
        reloc_fn.add(int(r[5], 16))


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) else v

# ---------------------------------------------------------------- instruction scan (bit masks, fast)
bl_cnt = collections.Counter()
b_list = []                # (pc, target) unconditional b
adrp_regs = {}
adrp_add = set()           # addresses formed by adrp+add that land in .text
br_sites = []
blr_count = 0
for i, w in enumerate(W):
    pc = i * 4
    t = w >> 26
    if t == 0x25:
        bl_cnt[pc + sx(w & 0x3ffffff, 26) * 4] += 1
    elif t == 5:
        b_list.append((pc, pc + sx(w & 0x3ffffff, 26) * 4))
    elif (w & 0x9f000000) == 0x90000000:
        imm = sx(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21)
        adrp_regs[w & 31] = ((pc & ~0xfff) + (imm << 12), pc)
    elif (w & 0xff800000) == 0x91000000:
        rn = (w >> 5) & 31
        imm = (w >> 10) & 0xfff
        if (w >> 22) & 1:
            imm <<= 12
        if rn in adrp_regs and pc - adrp_regs[rn][1] <= 64:
            tt = adrp_regs[rn][0] + imm
            if 0 <= tt < TEXT_END:
                adrp_add.add(tt)
    elif (w & 0xfffffc1f) == 0xd61f0000:
        br_sites.append(pc)
    elif (w & 0xfffffc1f) == 0xd63f0000:
        blr_count += 1


def fde_of(a):
    i = bisect.bisect_right(fde_starts, a) - 1
    if i >= 0 and fde[i][0] <= a < fde[i][0] + fde[i][1]:
        return fde[i]
    return None


def words(a, b):
    return W[a // 4:b // 4]

# ---------------------------------------------------------------- function set
funcs = {}     # start -> dict(end, kind, ssrc, esrc, conf, notes, tags)
for a, l in fde:
    funcs[a] = dict(end=a + l, kind="function", ssrc="fde", esrc="fde", conf="high", notes=[], tags=set())
plt_lo = min(plt)
plt_hi = max(plt) + 16
for a in sorted(plt):
    funcs[a] = dict(end=a + 16, kind="plt_stub", ssrc="plt+JUMP_SLOT", esrc="plt_stub16", conf="high",
                    notes=["import:" + plt[a]], tags={"thunk"})

fde_no_fn = [a for a, _ in fde if a not in gf]
for a in fde_no_fn:
    funcs[a]["notes"].append("ghidra_missed_function")


def evidence(a):
    ev = []
    if a in bl_cnt: ev.append("bl")
    if a in reloc_fn: ev.append("reloc_ptr")
    if a in adrp_add: ev.append("adrp_add_ptr")
    tg = tags_all.get(a, [])
    for t in ("dynsym", "init_array", "fini_array", "thunk"):
        if t in tg: ev.append(t)
    return ev


def gend(a):
    return a + max(gf[a]["size"], 4)

ghidra_external = sorted(a for a in gf if a >= TEXT_END)     # Ghidra external-import thunks (outside .text)
ghidra_only_all = sorted(a for a in gf if a not in fdestart and a not in plt and a < TEXT_END)
inside_fde = [a for a in ghidra_only_all if fde_of(a)]
ghidra_only = [a for a in ghidra_only_all if not fde_of(a)]
rejected = {}   # addr -> reason
cand = []
for a in ghidra_only:
    if all(w in (0, 0xd503201f) for w in words(a, gend(a))):
        rejected[a] = "nop_or_zero_padding_function"
    else:
        cand.append(a)
candset = set(cand)
tailb_ev = set()
for pc, t in b_list:
    if t in candset and not (t <= pc < gend(t)):
        tailb_ev.add(t)


def falls_through(pc):
    w = W[pc // 4]
    if w >> 26 == 5: return False
    if (w & 0xfffffc1f) in (0xd65f0000, 0xd61f0000): return False
    if w == 0 or w == 0xe7ffdefe: return False
    if w >> 26 == 0x25: return False   # bl: assume may be noreturn -> not a fragment
    return True

unresolved = []
for a in cand:
    e = gend(a)
    ev = evidence(a)
    strong = list(ev)
    if a in tailb_ev: ev.append("tail_b")
    if ev:
        funcs[a] = dict(end=e, kind="thunk" if "thunk" in ev else "function", ssrc="+".join(ev), esrc="ghidra_body",
                        conf="high" if strong else "medium", notes=[], tags={"nofde"})
    else:
        funcs[a] = dict(end=e, kind="unresolved", ssrc="ghidra_only_no_evidence", esrc="ghidra_body", conf="low",
                        notes=[], tags={"nofde"})
        unresolved.append(a)


def clip_overlaps():
    out = []
    st_ = sorted(funcs)
    for s_, n_ in zip(st_, st_[1:]):
        if funcs[s_]["end"] > n_:
            out.append((s_, n_))
            funcs[s_]["end"] = n_
            funcs[s_]["notes"].append("clipped_at_next_start")
    return out

def trim_pad(a, e):
    while e - 4 > a and W[(e - 4) // 4] in (0, 0xd503201f):
        e -= 4
    return e

# ---------------------------------------------------------------- gaps
def classify_gap(a, b):
    ws = words(a, b)
    if a == 0: return "header", "MOD0 header, not code"
    if all(w == 0 for w in ws): return "gap_padding", "zero_udf0"
    if all(w in (0, 0xd503201f) for w in ws): return "gap_padding", "nop_or_zero"
    ins = list(md.disasm(text[a:b], a))
    if len(ins) * 4 == b - a and not any(i.mnemonic == "udf" for i in ins):
        return "gap_unknown_code", "decodes_as_code_no_function"
    return "gap_data", "undecodable_words"


def compute_gaps():
    st_ = sorted(funcs)
    g = []
    cur = 0
    for s_ in st_:
        if s_ > cur: g.append((cur, s_))
        cur = max(cur, funcs[s_]["end"])
    if cur < TEXT_END: g.append((cur, TEXT_END))
    return {x: classify_gap(*x) for x in g}

overlaps = clip_overlaps()
gapinfo = compute_gaps()
st = sorted(funcs)

def containing(a):
    i = bisect.bisect_right(st, a) - 1
    if i >= 0 and funcs[st[i]]["end"] > a:
        return st[i]
    return None

# evidence-only candidates: bl / pointer targets Ghidra has no function for, sitting in a non-padding gap
evid_only = {}
for tset, name in ((bl_cnt.keys(), "bl"), (reloc_fn, "reloc_ptr"), (adrp_add, "adrp_add_ptr")):
    for t in tset:
        if 0 < t < plt_lo and t % 4 == 0 and containing(t) is None:
            evid_only.setdefault(t, set()).add(name)
padding_targets = []
gaps_sorted = sorted(gapinfo)
for t, ev in sorted(evid_only.items()):
    j = bisect.bisect_right(gaps_sorted, (t, 1 << 60)) - 1
    if j < 0 or not (gaps_sorted[j][0] <= t < gaps_sorted[j][1]): continue
    a_, b_ = gaps_sorted[j]
    if gapinfo[(a_, b_)][0] not in ("gap_unknown_code", "gap_data"):
        padding_targets.append(t); continue
    # end = next evidence-only start in the same gap, else the gap end
    nxt = [x for x in evid_only if t < x < b_]
    e = min(nxt) if nxt else b_
    funcs[t] = dict(end=trim_pad(t, e), kind="function", ssrc="+".join(sorted(ev)), esrc="next_start_or_gap_end", conf="medium",
                    notes=["evidence_only_not_in_ghidra"], tags={"nofde"})
overlaps += clip_overlaps()
st = sorted(funcs)

# fragments: no-evidence Ghidra-only entries and orphan code gaps that belong to the predecessor
def branches_into(s_, lo, hi):
    for i in md.disasm(text[s_:funcs[s_]["end"]], s_):
        if i.mnemonic != "bl" and (i.mnemonic.startswith("b") or i.mnemonic in ("cbz", "cbnz", "tbz", "tbnz")):
            m = re.search(r"#(0x[0-9a-f]+)$", i.op_str)
            if m and lo <= int(m.group(1), 16) < hi:
                return True
    return False

absorbed = 0
for a in sorted(unresolved):
    prev = max((s_ for s_ in funcs if s_ < a and funcs[s_]["kind"] != "plt_stub"), default=None)
    if prev is not None and funcs[prev]["end"] == a and (falls_through(a - 4) or branches_into(prev, a, funcs[a]["end"])):
        funcs[prev]["end"] = funcs[a]["end"]
        funcs[prev]["esrc"] += "+merged_fragment"
        funcs[prev]["notes"].append("absorbed_ghidra_split_at_" + H(a))
        rejected[a] = "fragment_merged_into_predecessor"
        del funcs[a]
        unresolved.remove(a)
st = sorted(funcs)
gapinfo = compute_gaps()
orphan_gap_c = collections.Counter()
for (a, b), (k, n) in sorted(gapinfo.items()):
    if k != "gap_unknown_code": continue
    st = sorted(funcs)
    i = bisect.bisect_left(st, a) - 1
    prev = st[i] if i >= 0 else None
    if prev is not None and funcs[prev]["end"] == a and funcs[prev]["kind"] != "plt_stub" and (falls_through(a - 4) is True or branches_into(prev, a, b)):
        funcs[prev]["end"] = trim_pad(prev, b)
        funcs[prev]["esrc"] += "+merged_orphan_code"
        funcs[prev]["notes"].append("absorbed_orphan_code_at_" + H(a))
        orphan_gap_c["merged"] += 1
    else:
        funcs[a] = dict(end=trim_pad(a, b), kind="unresolved", ssrc="code_in_gap_no_evidence", esrc="gap_end", conf="low",
                        notes=["decodes_as_code_no_call_or_pointer_evidence"], tags={"nofde"})
        unresolved.append(a)
        orphan_gap_c["unresolved"] += 1
st = sorted(funcs)
gapinfo = compute_gaps()
overlaps += clip_overlaps()

# ---------------------------------------------------------------- noreturn fixpoint
NR_NAME = re.compile(r"__throw|^__cxa_throw$|^abort$|AbortImpl|DiagAbort|^_ZSt9terminatev$|^__cxa_rethrow$|^exit$|"
                     r"^_Unwind_Resume$|^longjmp$|^__assert_fail$|^__cxa_call_unexpected$|^__cxa_pure_virtual$|"
                     r"rethrow_exception|^_exit$|^__stack_chk_fail$")
noret = {a for a, n in plt.items() if NR_NAME.search(n)}
bymap = collections.defaultdict(list)
for pc, t in b_list:
    i = bisect.bisect_right(st, pc) - 1
    if i >= 0:
        s = st[i]
        if pc < funcs[s]["end"] and not (s <= t < funcs[s]["end"]):
            bymap[s].append(t)
finfo = {}
for s in st:
    if funcs[s]["kind"] == "plt_stub": continue
    ws = words(s, funcs[s]["end"])
    finfo[s] = (any((w & 0xfffffc1f) == 0xd65f0000 for w in ws), any((w & 0xfffffc1f) == 0xd61f0000 for w in ws), bymap.get(s, []))
changed = True
while changed:
    changed = False
    for s, (hr, hb, tails) in finfo.items():
        if s in noret or hr or hb: continue
        if funcs[s]["kind"] == "unresolved": continue
        if all(t in noret for t in tails):
            noret.add(s); changed = True
for s in noret:
    if s in funcs: funcs[s]["tags"].add("noreturn")
fn_noret = sorted(s for s in noret if s in funcs and funcs[s]["kind"] != "plt_stub")

# ---------------------------------------------------------------- jump tables
def dec(pc):
    return next(md.disasm(text[pc:pc + 4], pc), None)


def seg_read(addr, n):
    for mo, data in segs:
        if mo <= addr and addr + n <= mo + len(data):
            return data[addr - mo:addr - mo + n]
    return None

TERM = (0xd65f0000, 0xd61f0000)
NODEF = {"cbz", "cbnz", "tbz", "tbnz", "cmp", "cmn", "tst", "str", "stp", "strb", "strh", "stur", "sturb", "ccmp", "ccmn", "blr", "br", "ret"}


def writes(i, r):
    """does instruction i define register name r (x/w alias)?"""
    if i.mnemonic in NODEF or i.mnemonic.startswith("b"):
        return False
    o = [x.strip() for x in i.op_str.split(",")]
    return bool(o) and o[0][1:] == r[1:] and o[0][0] in "xw"

# Tables whose base register is set on a path the window walk cannot follow (phi of two adrp+add, or a register
# kept live across calls). Table address checked by hand against the TotK generator's path-sensitive resolver
# (2026-11-11, analysis/recomp/jump_table_resolution.md): same 17 and 86 distinct targets from the table bytes
# and the guarding cmp (cmp w9,#0x13 and cmp w8,#0x9b).
# Bound read by hand where the guard is `cmp w1,#9 ; cbz x2,L ; ... L: b.hi default` (flags survive the cbz).
MANUAL_COUNT = {0x23d7a4: 10}
MANUAL_TABLE = {0x2222c0: 0xb213a5, 0x9dc1a0: 0xb29682}


def analyze_br(s):
    if plt_lo <= s < plt_hi:
        return "plt_stub", {}
    rn = dec(s).op_str.strip()
    win = []
    p = s - 4
    while p >= 0 and len(win) < 28:
        w = W[p // 4]
        if (w & 0xfffffc1f) in TERM or w >> 26 == 5 or w == 0:
            break
        i = dec(p)
        if i is None: break
        win.append(i)
        p -= 4
    mdef = None
    for k, i in enumerate(win):
        if writes(i, rn):
            mdef = i
            win = win[k + 1:]          # only definitions before the add
            break
    if mdef is None or mdef.mnemonic in ("ldr", "mov", "ldp"):
        return ("indirect_tail_vcall" if mdef is not None else "indirect_other"), {}
    if mdef.mnemonic != "add":
        return "indirect_other", {}
    ops = [o.strip() for o in mdef.op_str.split(",")]
    if len(ops) < 3 or ops[2].startswith("#"):
        return "indirect_other", {}
    ra, rb = ops[1], ops[2]
    shift = 0
    ext = None
    if len(ops) > 3:
        m = re.match(r"(lsl|sxtw|uxtw)\s*(#(\w+))?", ops[3])
        if m:
            ext = m.group(1)
            shift = int(m.group(3), 0) if m.group(3) else 0

    def find_def(r):
        for i in win:
            if writes(i, r):
                return i
        return None
    da, db = find_def(ra), find_def(rb)
    ld = base_ins = None
    for d_, other in ((db, da), (da, db)):
        if d_ is not None and d_.mnemonic in ("ldrb", "ldrh", "ldr", "ldrsb", "ldrsh", "ldrsw"):
            ld, base_ins = d_, other
            break
    if ld is None:
        return "indirect_other", {}
    esz = {"ldrb": 1, "ldrsb": 1, "ldrh": 2, "ldrsh": 2, "ldr": 4, "ldrsw": 4}[ld.mnemonic]
    signed = ld.mnemonic in ("ldrsb", "ldrsh", "ldrsw")
    m = re.match(r"\[(\w+),\s*(\w+)", ld.op_str.split(",", 1)[1].strip())
    if not m:
        return "indirect_other", {}
    rt = m.group(1)
    add_i = adrp_i = None
    for i in win:
        o = [x.strip() for x in i.op_str.split(",")]
        if o and o[0] == rt and i.mnemonic == "add" and len(o) > 2 and o[2].startswith("#") and add_i is None:
            add_i = i
        if o and o[0] == rt and i.mnemonic == "adrp":
            adrp_i = i
            break
    if adrp_i is None:
        # table register set far above the window: walk back to its defining adrp(+add) in the same function
        fd0 = fde_of(s)
        lo0 = fd0[0] if fd0 else max(0, s - 0x2000)
        p2 = s - 4
        add_i = None
        while p2 >= lo0 and p2 > s - 0x4000:
            i = dec(p2)
            if i is not None and writes(i, rt):
                if i.mnemonic == "add" and add_i is None and i.op_str.split(",")[1].strip() == rt and i.op_str.split(",")[2].strip().startswith("#"):
                    add_i = i
                elif i.mnemonic == "adrp":
                    adrp_i = i
                    break
                else:
                    break
            p2 -= 4
    if adrp_i is None and s not in MANUAL_TABLE:
        return "jump_table_unrecovered", {"why": "no_adrp_for_table"}
    if adrp_i is None:
        tbl = MANUAL_TABLE[s]
    else:
        tbl = int(adrp_i.op_str.split("#")[1], 16) + (int(add_i.op_str.split("#")[-1], 0) if add_i else 0)
    base = None
    if base_ins is not None and base_ins.mnemonic == "adr":
        base = int(base_ins.op_str.split("#")[1], 16)
    elif base_ins is None or base_ins.op_str.split(",")[0].strip()[1:] == rt[1:]:
        base = tbl
    if base is None:
        return "jump_table_unrecovered", {"why": "base_unknown", "tbl": tbl}
    count = bsrc = None
    p = s - 4
    cnt = 0
    last_cc = None
    while p >= 0 and cnt < 40:
        i = dec(p); cnt += 1
        if i is None: break
        if i.mnemonic in ("b.hi", "b.hs", "b.cs", "b.ls", "b.lo", "b.cc") and last_cc is None:
            last_cc = i
        elif i.mnemonic == "ccmp" and "#" in i.op_str and last_cc is not None and last_cc.mnemonic in ("b.hi", "b.ls"):
            # `cmp a,#0 ; ccmp x,#K,#nzcv,ne ; b.hi default`: with nzcv C=1,Z=0 the failed-condition path also
            # takes `hi`, so the bound is the ccmp immediate (0x3f1014 used to read the `cmp a,#0` as the bound)
            ops_cc = [o.strip() for o in i.op_str.split(",")]
            nz = int(ops_cc[2].lstrip("#"), 0)
            if (nz & 2) and not (nz & 4):
                count = int(ops_cc[1].lstrip("#"), 0) + 1
                bsrc = "ccmp_bound"
            break
        elif i.mnemonic == "cmp" and "#" in i.op_str and last_cc is not None:
            k = int(i.op_str.split("#")[1], 0)
            count = k + 1 if last_cc.mnemonic in ("b.hi", "b.ls") else k
            bsrc = "cmp_bound"
            break
        if (W[p // 4] & 0xfffffc1f) in TERM: break
        p -= 4
    fd = fde_of(s)
    flo, fhi = (fd[0], fd[0] + fd[1]) if fd else (0, plt_lo)
    if not count and s in MANUAL_COUNT:
        count, bsrc = MANUAL_COUNT[s], "cmp_bound_manual"
    if not count:
        # No guard in the straight-line window: the dispatch block (from the table adrp) is entered only through a
        # `cmp rN,#K ; b.ls/b.hi` of the function. Accept exactly one such conditional edge into the block.
        q = adrp_i.address if adrp_i is not None else s
        bs = q
        while bs > flo and not (W[bs // 4 - 1] >> 26 in (5, 0x25) or (W[bs // 4 - 1] & 0xfffffc1f) in TERM
                                or (dec(bs - 4) is not None and dec(bs - 4).mnemonic.startswith(("b.", "cb", "tb")))):
            bs -= 4
        edges = []
        for a in range(flo, fhi, 4):
            i = dec(a)
            if i is None or not (i.mnemonic.startswith(("b.", "cb", "tb"))):
                continue
            if i.mnemonic.startswith("b.") and ("#" in i.op_str) and bs <= int(i.op_str.split("#")[1], 16) <= s:
                edges.append((a, i, True))
        pj = dec(bs - 4)
        if pj is not None and pj.mnemonic in ("b.hi", "b.hs", "b.ls", "b.lo"):
            edges.append((bs - 4, pj, False))   # fallthrough edge
        if len(edges) == 1:
            a, i, taken = edges[0]
            # nearest cmp before the branch (up to 3 instructions back, no flag writer between)
            for back in (4, 8, 12):
                c = dec(a - back)
                if c is not None and c.mnemonic == "cmp" and "#" in c.op_str:
                    k = int(c.op_str.split("#")[1], 0)
                    ok = ((i.mnemonic == "b.ls" and taken) or (i.mnemonic == "b.hi" and not taken))
                    ok2 = ((i.mnemonic == "b.lo" and taken) or (i.mnemonic == "b.hs" and not taken))
                    if ok or ok2:
                        count = k + 1 if ok else k
                        bsrc = "cmp_bound_pred"
                    break
                if c is not None and c.mnemonic in ("subs", "adds", "tst", "ccmp", "fcmp"):
                    break
    targets = []
    for k in range(count if count else 512):
        raw = seg_read(tbl + k * esz, esz)
        if raw is None: break
        v = int.from_bytes(raw, "little", signed=signed)
        tg = base + (v << shift)
        if not count and not (flo <= tg < fhi and tg % 4 == 0):
            break
        targets.append(tg)
    valid = len(targets) > 0 and all(flo <= t < fhi and t % 4 == 0 for t in targets)
    info = dict(tbl=tbl, base=base, esz=esz, shift=shift, signed=signed, n=len(targets), targets=targets)
    if count and valid:
        info.update(bsrc=bsrc, conf="high")
        return "jump_table", info
    if not count and valid and len(targets) >= 2:
        info.update(bsrc="scan_until_invalid", conf="low" if len(targets) >= 512 else "medium")
        return "jump_table", info
    info["why"] = "targets_invalid_or_no_bound"
    return "jump_table_unrecovered", info

br_class = collections.Counter()
jt_infos = {}
for s in br_sites:
    c, info = analyze_br(s)
    br_class[c] += 1
    jt_infos[s] = (c, info)
with open(os.path.join(OUT, "jump_tables.tsv"), "w", encoding="utf8", newline="\n") as f:
    f.write("# file: jump_tables.tsv\n# every `br xN` site outside the PLT (%d PLT-stub sites excluded). class: jump_table (recovered; every target validated 4-aligned inside the containing FDE), jump_table_unrecovered, indirect_tail_vcall (target loaded from memory/register move), indirect_other\n" % br_class["plt_stub"])
    f.write("# build_id: %s\n# input_sha256: %s\n" % (BUILD_ID, SHA))
    f.write("br_site\tfunction\tclass\ttable\tbase\tentry_bytes\tshift\tsigned\tcount\tbound\tconfidence\ttargets\n")
    for s in sorted(jt_infos):
        c, info = jt_infos[s]
        if c == "plt_stub": continue
        fn = containing(s)
        fns = H(fn) if fn is not None else ""
        if c == "jump_table":
            f.write("\t".join([H(s), fns, c, H(info["tbl"]), H(info["base"]), str(info["esz"]), str(info["shift"]),
                               str(int(info["signed"])), str(info["n"]), info["bsrc"], info["conf"], ";".join(H(t) for t in info["targets"])]) + "\n")
        else:
            f.write("\t".join([H(s), fns, c, H(info["tbl"]) if "tbl" in info else "", "", "", "", "", "", "", info.get("why", ""), ""]) + "\n")
n_jt = br_class["jump_table"]
jt_targets = sum(i["n"] for c, i in jt_infos.values() if c == "jump_table")

# ---------------------------------------------------------------- mismatch buckets
def bucket_extra(a, L, g):
    if g > L:
        if W[(a + L) // 4] == 0 and (W[(a + L - 4) // 4] >> 26) == 0x25:
            return "ghidra_longer_trailing_udf_after_final_bl"
        return "ghidra_longer_other"
    if L == 4 and W[a // 4] == 0xe7ffdefe:
        return "trap_stub_0xe7ffdefe_ghidra_size1"
    e0 = a + g
    ws = words(e0, a + L)
    if all(w in (0, 0xd503201f) for w in ws):
        return "extra_is_padding"
    ins = list(md.disasm(text[e0:a + L], e0))
    calls = [int(i.op_str[1:], 16) for i in ins if i.mnemonic == "bl"]
    pn = [plt.get(t, "") for t in calls]
    if any(n == "_Unwind_Resume" for n in pn):
        return "landing_pads_cleanup_unwind_resume"
    if any(n in ("__cxa_end_catch", "__cxa_begin_catch") for n in pn):
        return "landing_pads_catch"
    if any(x in gf and x not in fdestart for x in range(e0 + 4, a + L, 4)):
        return "fde_spans_ghidra_function"
    if ins and all(i.mnemonic in ("bl", "udf") for i in ins) and all(t in noret for t in calls):
        return "noreturn_call_tail_only"
    if ins and ins[0].mnemonic == "bl" and calls and calls[0] in noret and ins[-1].mnemonic != "ret":
        return "noreturn_call_then_landing_pad_code"
    if ins and ins[-1].mnemonic == "bl" and calls and calls[-1] in noret:
        return "landing_pad_other_ends_noreturn_call"
    for i in md.disasm(text[a:e0], a):
        if i.mnemonic != "bl" and (i.mnemonic.startswith("b") or i.mnemonic in ("cbz", "cbnz", "tbz", "tbnz")):
            m = re.search(r"#(0x[0-9a-f]+)$", i.op_str)
            if m and e0 <= int(m.group(1), 16) < a + L:
                return "extra_branch_target_cold_or_split_block"
    return "extra_unreached_code_other"

bucket_rows = []
for a, L in fde:
    g = gf.get(a)
    if g is None:
        bucket_rows.append(("FDE_NO_FUNCTION", a, L, 0, "ghidra_missed_function"))
    elif g["size"] != L:
        bucket_rows.append(("SIZE_MISMATCH", a, L, g["size"], bucket_extra(a, L, g["size"])))
for a in sorted(set(gf) - set(fdestart)):
    if a in plt: continue
    if a >= TEXT_END:
        bucket_rows.append(("FUNCTION_NO_FDE", a, 0, gf[a]["size"], "external_import_thunk_outside_text")); continue
    if a in rejected:
        reason = rejected[a]
    elif fde_of(a):
        reason = "ghidra_split_inside_fde_cold_or_shrinkwrap"
    elif a in funcs:
        reason = "accepted_%s_%s_evidence_%s" % ("thunk" if funcs[a]["kind"] == "thunk" else "function", funcs[a]["conf"], funcs[a]["ssrc"].replace("+", "_"))
    else:
        reason = "ghidra_only_dropped"
    bucket_rows.append(("FUNCTION_NO_FDE", a, 0, gf[a]["size"], reason))
bucket_rows.sort(key=lambda r: (r[1], r[0]))
with open(os.path.join(OUT, "functions_size_buckets.tsv"), "w", encoding="utf8", newline="\n") as f:
    f.write("# file: functions_size_buckets.tsv\n# disagreement rows (see functions_disagreements.tsv) with the cause bucket assigned by reconcile_functions.py (thunks that are PLT stubs excluded)\n")
    f.write("# build_id: %s\n# input_sha256: %s\n" % (BUILD_ID, SHA))
    f.write("kind\toffset\tfde_len\tghidra_size\tbucket\n")
    for k, a, L, g, b in bucket_rows:
        f.write("%s\t%s\t%s\t%s\t%s\n" % (k, H(a), H(L) if L else "", H(g) if g else "", b))
bcount = collections.Counter((k, b) for k, a, L, g, b in bucket_rows)

# ---------------------------------------------------------------- interior entries
interior = []
for t in sorted(set(bl_cnt) | reloc_fn | adrp_add):
    if not (0 < t < plt_lo) or t in funcs: continue
    c = containing(t)
    if c is not None and funcs[c]["kind"] != "plt_stub":
        ev = []
        if t in bl_cnt: ev.append("bl_x%d" % bl_cnt[t])
        if t in reloc_fn: ev.append("reloc_ptr")
        if t in adrp_add: ev.append("adrp_add_ptr")
        interior.append((t, c, ev))
with open(os.path.join(OUT, "functions_interior_entries.tsv"), "w", encoding="utf8", newline="\n") as f:
    f.write("# file: functions_interior_entries.tsv\n# call (bl) / address-taken targets that land strictly inside another function's range: possible extra entry points (lifter must support them or prove they are data/aliases)\n")
    f.write("# build_id: %s\n# input_sha256: %s\n" % (BUILD_ID, SHA))
    f.write("target\tinside_function\toffset_in_function\tevidence\n")
    for t, c, ev in interior:
        f.write("%s\t%s\t%s\t%s\n" % (H(t), H(c), H(t - c), ",".join(ev)))

# ---------------------------------------------------------------- final table
with open(os.path.join(OUT, "functions_final.tsv"), "w", encoding="utf8", newline="\n") as f:
    f.write("# file: functions_final.tsv\n# authoritative function list + classified gaps covering all of .text [0, 0x%x) (module offsets; .plt = 0x%x-0x%x). end is exclusive.\n" % (TEXT_END, plt_lo, plt_hi))
    f.write("# kind: function | thunk | plt_stub | unresolved | gap_padding | gap_data | gap_unknown_code | header. start_source/end_source = data that decided each boundary. confidence high|medium|low\n")
    f.write("# build_id: %s\n# input_sha256: %s\n" % (BUILD_ID, SHA))
    f.write("start\tend\tkind\tstart_source\tend_source\tconfidence\ttags\tnotes\n")
    items = []
    for s in funcs:
        x = funcs[s]
        items.append((s, x["end"], x["kind"], x["ssrc"], x["esrc"], x["conf"], ",".join(sorted(x["tags"])), ";".join(x["notes"])))
    for (a, b), (k, n) in gapinfo.items():
        items.append((a, b, k, "gap", "gap", "high" if k in ("gap_padding", "header") else "medium", "", n))
    items.sort(key=lambda r: (r[0], r[1]))
    for r in items:
        f.write("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" % (H(r[0]), H(r[1]), r[2], r[3], r[4], r[5], r[6], r[7]))

# ---------------------------------------------------------------- stats / md
kc = collections.Counter(funcs[s]["kind"] for s in funcs)
fde_funcs = sum(1 for s in funcs if funcs[s]["ssrc"] == "fde")
nofde_funcs = [s for s in funcs if "nofde" in funcs[s]["tags"]]
conf_c = collections.Counter(funcs[s]["conf"] for s in nofde_funcs)
src_c = collections.Counter(funcs[s]["ssrc"] for s in nofde_funcs)
gk = collections.Counter(); gb = collections.Counter()
for (a, b), (k, n) in gapinfo.items():
    gk[k] += 1; gb[k] += b - a
func_bytes = sum(funcs[s]["end"] - s for s in funcs)
cov_func = 100.0 * func_bytes / TEXT_END
cov_cls = 100.0 * (func_bytes + gb["gap_padding"] + gb["header"]) / TEXT_END
n_final = len(funcs)
unres = sorted(unresolved)
big = sorted(((b - a), a, b, k) for (a, b), (k, n) in gapinfo.items() if k != "gap_padding" and k != "header")
rej_c = collections.Counter(rejected.values())
noret_nonplt = len(fn_noret)
Z = []
Z.append("# functions_final - authoritative function list (v262144 main)\n")
Z.append("Build ID `%s`, SHA-256 `%s`. Module offsets (base 0); generated by `scripts/reconcile_functions.py` from the NSO, the FDE list and the Ghidra exports (deterministic, no timestamps).\n" % (BUILD_ID, SHA))
Z.append("## Result\n")
Z.append("- Final functions: **%d** (%d without PLT stubs). FDE-defined: %d (%d of those not found by Ghidra). Non-FDE: %d. PLT stubs: %d." % (n_final, n_final - kc["plt_stub"], fde_funcs, len(fde_no_fn), len(nofde_funcs), kc["plt_stub"]))
Z.append("- Kinds: " + ", ".join("%s %d" % (k, v) for k, v in sorted(kc.items())))
Z.append("- Non-FDE functions by confidence: " + ", ".join("%s %d" % (k, v) for k, v in sorted(conf_c.items())) + "; by evidence: " + ", ".join("%s %d" % (k, v) for k, v in sorted(src_c.items(), key=lambda x: (-x[1], x[0]))) + ".")
Z.append("- Unresolved (no positive evidence, not mergeable): **%d** %s" % (len(unres), ", ".join(H(a) for a in unres[:20])))
Z.append("- Ghidra functions rejected or absorbed: %d (%s); plus %d Ghidra functions that start strictly inside an FDE range (FDE wins) and %d Ghidra external-import thunks outside .text (ignored)." % (len(rejected), ", ".join("%s %d" % (k, v) for k, v in sorted(rej_c.items())) or "none", len(inside_fde), len(ghidra_external)))
Z.append("- Overlaps: %d (FDE ranges never overlap each other; non-FDE bodies are clipped at the next start)." % len(overlaps))
Z.append("- Coverage of `.text` [0, 0x%x) (%d bytes): functions %.3f%%; functions + classified padding + header %.3f%%." % (TEXT_END, TEXT_END, cov_func, cov_cls))
Z.append("- Gaps: " + ", ".join("%s %d (%d bytes)" % (k, gk[k], gb[k]) for k in sorted(gk)) + ".")
Z.append("")
Z.append("## Rules\n")
Z.append("1. An FDE `(start, len)` is a function with exactly that range (high confidence). The %d FDE starts Ghidra missed are real code (disassembled: prologue ... `ret` or tail `b`, e.g. 0x272530 refcount release, 0x2aeda0 null-checked tail jump, 0x605be0 state dispatch) and are kept; do not use Ghidra's list for them." % len(fde_no_fn))
Z.append("2. PLT stubs: every JUMP_SLOT stub, 16 bytes each (`adrp/ldr/add/br`), 0x%x-0x%x; they are thunks to imports." % (plt_lo, plt_hi))
Z.append("3. Non-FDE Ghidra functions are accepted only with positive evidence: direct `bl` target, RELATIVE-reloc function pointer, `adrp+add` code pointer, dynsym, init/fini array, thunk tag (high), or only a tail `b` from another function (medium). Range = Ghidra body end, clipped at the next start; trailing padding is excluded.")
Z.append("4. A Ghidra-only function with no evidence whose predecessor falls through into it is a shrink-wrapped split of the predecessor and is merged (predecessor's end extended). Anything else is `unresolved` and listed.")
Z.append("5. A Ghidra-only function made only of nop/zero words is rejected as padding. A Ghidra function that starts inside an FDE range is dropped (cold/split artifact).")
Z.append("6. Gaps are classified header (0x0-0x30, MOD0), padding (zero words = `udf #0`, or nop), data (undecodable), unknown_code (decodes, no owner). Evidence-only entries in non-padding gaps (bl/pointer target Ghidra lacks) would be added as medium: %d added." % sum(1 for s in funcs if "evidence_only_not_in_ghidra" in funcs[s]["notes"]))
Z.append("")
Z.append("## Disagreement buckets (exhaustive; per-row cause in functions_size_buckets.tsv)\n")
Z.append("| kind | bucket | count |\n|---|---|---|")
for (k, b), n in sorted(bcount.items(), key=lambda x: (x[0][0], -x[1], x[0][1])):
    Z.append("| %s | %s | %d |" % (k, b, n))
Z.append("")
Z.append("- `landing_pads_*`: the FDE covers exception cleanup/catch blocks reachable only through the LSDA; Ghidra never follows them (largest cause).")
Z.append("- `noreturn_call_*`, `landing_pad_other_ends_noreturn_call`: the FDE continues after the last `ret` with blocks ending in a call to a non-returning function (internal terminate helper such as 0x25bb48, or an import like `__cxa_throw`).")
Z.append("- `trap_stub_0xe7ffdefe_ghidra_size1`: 4-byte trap stubs (deleted/pure-virtual style, word 0xe7ffdefe) that Ghidra sizes as 1 byte.")
Z.append("- `ghidra_longer_trailing_udf_after_final_bl`: Ghidra's body includes the zero `udf` word after a final noreturn `bl`; the FDE excludes it.")
Z.append("- `accepted_*` / `fallthrough_*` / `nop_or_zero_padding_function` / `ghidra_split_inside_fde_*`: FUNCTION_NO_FDE outcomes (see Rules 3-5).")
Z.append("")
Z.append("## Remaining risks for the lifter\n")
Z.append("- `br xN` sites: %d total = %d PLT stubs + %d recovered jump tables (%d targets total) + %d jump-table-like sites unrecovered + %d indirect tail calls through loaded pointers + %d other. `blr` sites: %d (runtime-resolved: vtables, function pointers)." % (len(br_sites), br_class["plt_stub"], br_class["jump_table"], jt_targets, br_class["jump_table_unrecovered"], br_class["indirect_tail_vcall"], br_class["indirect_other"], blr_count))
Z.append("  Recovered tables are validated: every target is 4-aligned and inside the containing FDE; bound comes from the guarding `cmp` + `b.hi/b.hs` (`scan_until_invalid` ones have no guarding compare found: medium, or low if the 512-entry scan cap was hit). Unrecovered and `indirect_other` sites are in jump_tables.tsv and need manual or dynamic handling.")
Z.append("- Noreturn functions (fixpoint: no `ret`/`br`, tail calls only to noreturn; seeds are noreturn imports `__cxa_throw`, `_Unwind_Resume`, `abort`, `terminate`, `AbortImpl`, `__throw_*`): **%d** functions tagged `noreturn` in functions_final.tsv (heuristic, not dynamically verified). Code after `bl <noreturn>` must not fall through." % noret_nonplt)
Z.append("- Interior entry points: %d call/pointer targets land strictly inside a function (functions_interior_entries.tsv): outlined tails or aliases; the lifter needs a policy (split the function or support multiple entries)." % len(interior))
Z.append("- Landing pads are reachable only via LSDA/unwind tables; a lifter that supports C++ exceptions must read `.gcc_except_table` and treat them as entries.")
Z.append("- Region 0x21d45c-0x254c40 (about 227 KB, middle-ware code with no unwind tables) is covered only by Ghidra boundaries plus evidence (%d functions); its function ends are Ghidra body ends, not unwind data." % sum(1 for s in nofde_funcs if 0x21d45c <= s < 0x254c40))
Z.append("- Data in code: any `gap_data`/`gap_unknown_code` rows are listed in functions_final.tsv; the 0xe7ffdefe trap stubs are single-word functions.")
Z.append("- Indirect call targets (vtables, function pointers) are not resolved here; see classes.tsv.")
if big:
    Z.append("")
    Z.append("## Non-padding gaps (largest first, up to 15)\n")
    for ln, a, b, k in sorted(big, reverse=True)[:15]:
        Z.append("- %s-%s %s (%d bytes)" % (H(a), H(b), k, ln))
open(os.path.join(OUT, "functions_final.md"), "w", encoding="utf8", newline="\n").write("\n".join(Z) + "\n")
print("functions", n_final, dict(kc), "unresolved", len(unres), "cov %.3f/%.3f" % (cov_func, cov_cls))
print("br", dict(br_class), "jt_targets", jt_targets, "noreturn", noret_nonplt, "interior", len(interior))
print("gaps", dict(gk), dict(gb))
for k, v in sorted(bcount.items()): print(k, v)
