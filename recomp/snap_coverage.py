"""Instruction coverage of the TotK-derived lifter over every function in functions_final.tsv.
Writes analysis/recomp/insn_coverage.md (and insn_coverage.json next to it).
Run: tools/venv-recomp/Scripts/python -I recomp/snap_coverage.py"""
import json
import os
import re
import struct
import sys
import time
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import capstone
from snap_common import BASE, ROOT, functions, load_module, lower_priority
from lifter import Unsupported

MD = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
MD.detail = False

LSE = re.compile(r'^(cas|casp|swp|ld(add|clr|eor|set|smax|smin|umax|umin)|st(add|clr|eor|set|smax|smin|umax|umin))'
                 r'(a|l|al)?(b|h)?$')
EXCL = re.compile(r'^(ld|st)(a|l)?x(r|p)(b|h)?$')       # ldxr, ldaxr, stxr, stlxr, ldxp, ... (a/l before x)
EXCL2 = re.compile(r'^(ldxr|ldaxr|stxr|stlxr|ldxp|ldaxp|stxp|stlxp|ldxrb|ldaxrb|stxrb|stlxrb|ldxrh|ldaxrh|stxrh|stlxrh|clrex)$')
ACQREL = re.compile(r'^(ldar|ldarb|ldarh|stlr|stlrb|stlrh|ldapr|ldaprb|ldaprh|ldlar|stllr)$')
FP_ROUND = re.compile(r'^(frint[nmapzxi]|fcvt[nmapz][su]|fcvtxn|fcvtxn2)$')
CRYPTO = re.compile(r'^(aes|sha\d|sm3|sm4|pmull)')
CRC = re.compile(r'^crc32')
REG = [(re.compile(r'\bv\d+\.(\d*)([bhsdq])\b'), lambda m: f'V.{m.group(1)}{m.group(2).upper()}'),
       (re.compile(r'\bv\d+\.([bhsd])\[\d+\]'), lambda m: f'V.{m.group(1).upper()}[i]'),
       (re.compile(r'\bv\d+\b'), lambda m: 'V'),
       (re.compile(r'\b[xX]\d+\b'), lambda m: 'X'), (re.compile(r'\b[wW]\d+\b'), lambda m: 'W'),
       (re.compile(r'\b[bhsdq]\d+\b'), lambda m: m.group(0)[0].upper()),
       (re.compile(r'\b(xzr|sp)\b'), lambda m: 'X'), (re.compile(r'\bwzr\b'), lambda m: 'W'),
       (re.compile(r'#-?(0x[0-9a-f]+|\d+(\.\d+)?)'), lambda m: '#i')]


def form(mn, ops):
    s = ops
    for rx, f in REG:
        s = rx.sub(f, s)
    return f'{mn} {s}'.strip()


def category(mn, ops):
    c = []
    if mn in ('mrs', 'msr'):
        reg = ops.split(',')[-1 if mn == 'mrs' else 0].strip()
        c.append(f'sysreg:{mn} {re.sub(r"^x\d+,? ?", "", reg)}')
    if EXCL2.match(mn):
        c.append('exclusive_llsc')
    if LSE.match(mn):
        c.append('atomic_lse')
    if ACQREL.match(mn):
        c.append('acquire_release')
    if FP_ROUND.match(mn):
        c.append('fp_explicit_rounding')
    if CRYPTO.match(mn):
        c.append('crypto')
    if CRC.match(mn):
        c.append('crc32')
    if mn in ('svc', 'brk', 'hlt', 'udf', 'hvc', 'smc'):
        c.append(f'exception:{mn}')
    if mn in ('dc', 'ic', 'tlbi', 'at', 'sys', 'sysl'):
        c.append(f'cache_sys:{mn}')
    if mn in ('dmb', 'dsb', 'isb', 'sb', 'csdb'):
        c.append(f'barrier:{mn}')
    if mn.startswith('prf'):
        c.append('prefetch')
    return c


def is_neon(ops):
    return bool(re.search(r'\bv\d+\b', ops))


def main():
    lower_priority()
    t0 = time.time()
    mod = load_module()
    fns = [(s, e, k) for s, e, k, _ in functions() if k in ('function', 'thunk')]
    mn_count, mn_fns = Counter(), defaultdict(set)
    form_count, form_fns = Counter(), defaultdict(set)
    cat_count, cat_fns = Counter(), defaultdict(set)
    unsup_count, unsup_fns, unsup_ex = Counter(), defaultdict(set), {}
    undecodable = Counter()
    undec_fns = set()
    total = 0
    neon_fns, fp_fns = set(), set()
    fpcr_writes = []
    for fi, (s, e, k) in enumerate(fns):
        for off in range(s, e, 4):
            a = BASE + off
            w = mod.word(a)
            total += 1
            d = next(MD.disasm(struct.pack('<I', w), a), None)
            if d is None:
                mn, ops = '.word', ''
                undecodable[f'{w:08x}'] += 1
                undec_fns.add(s)
            else:
                mn, ops = d.mnemonic, d.op_str
            mn_count[mn] += 1
            mn_fns[mn].add(s)
            fm = form(mn, ops)
            form_count[fm] += 1
            form_fns[fm].add(s)
            if is_neon(ops):
                neon_fns.add(s)
            for c in category(mn, ops):
                cat_count[c] += 1
                cat_fns[c].add(s)
                if c.startswith('sysreg:msr') and 'fpcr' in c:
                    fpcr_writes.append(off)
            ins = mod.lift(a)
            if ins.kind == 'trap' and ins.code.startswith('rt_udf') and '/*' in ins.code:
                msg = ins.code[ins.code.index('/*') + 2:ins.code.rindex('*/')].strip()
                msg = re.sub(r'\b[0-9a-f]{8}\b', '<w>', msg)
                key = f'{msg} [{fm}]' if mn != '.word' else msg
                unsup_count[key] += 1
                unsup_fns[key].add(s)
                unsup_ex.setdefault(key, (off, w, f'{mn} {ops}'))
        if fi % 5000 == 0:
            print(f'{fi}/{len(fns)} functions, {time.time() - t0:.0f}s', flush=True)

    # group unsupported by message root (strip the form tail)
    root = Counter()
    root_fns = defaultdict(set)
    for key, n in unsup_count.items():
        r = key.split(' [')[0]
        root[r] += n
        root_fns[r] |= unsup_fns[key]
    all_unsup_fns = set().union(*unsup_fns.values()) if unsup_fns else set()

    res = {'functions': len(fns), 'instructions': total, 'mnemonics': len(mn_count), 'forms': len(form_count),
           'unsupported_words': sum(unsup_count.values()), 'unsupported_functions': len(all_unsup_fns),
           'seconds': round(time.time() - t0, 1)}
    out = ROOT / 'analysis' / 'recomp'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'insn_coverage.json').write_text(json.dumps({
        'summary': res, 'mnemonics': dict(mn_count.most_common()),
        'categories': {c: [n, len(cat_fns[c])] for c, n in cat_count.most_common()},
        'unsupported': {k: [n, len(unsup_fns[k]), f'{unsup_ex[k][0]:#x} {unsup_ex[k][1]:08x} {unsup_ex[k][2]}']
                        for k, n in unsup_count.most_common()}}, indent=1))

    L = ['# Instruction coverage: lifter (TotK/LAS) over functions_final.tsv\n',
         'Inputs: `analysis/maps/functions_final.tsv` (kind function + thunk; PLT stubs and padding excluded) and '
         '`analysis/import/v262144-main.elf`. Disassembly: Capstone; support: `recomp/lifter.py` (`Unsupported` becomes an '
         '`rt_udf` trap, as `Module.lift` does). Re-run: `tools/venv-recomp/Scripts/python -I recomp/snap_coverage.py`.\n',
         f'- Functions scanned: **{res["functions"]:,}**; instruction words: **{total:,}**; distinct mnemonics: {len(mn_count)}; '
         f'distinct operand forms: {len(form_count):,}',
         f'- Words the lifter rejects: **{res["unsupported_words"]:,}** in **{res["unsupported_functions"]:,}** functions '
         f'({100 * res["unsupported_functions"] / max(1, len(fns)):.2f}% of functions)',
         f'- Functions using AdvSIMD (any v-register operand): {len(neon_fns):,}',
         f'- Capstone cannot decode: {sum(undecodable.values()):,} words in {len(undec_fns)} functions, by word: {dict(undecodable.most_common(5))} (0xfedeffe7 is the compiler UDF trap; the lifter emits `rt_udf` for it, a deliberate trap, not a gap)',
         f'- Scan time {res["seconds"]}s\n',
         '## Unsupported forms (lifter rejects), by message\n',
         '| message | words | functions |', '|---|---:|---:|']
    for r, n in root.most_common(40):
        L.append(f'| {r} | {n:,} | {len(root_fns[r]):,} |')
    L += ['\n### Unsupported, by operand form (top 50): form, words, functions, first example (offset word asm)\n']
    for key, n in unsup_count.most_common(50):
        o, w, asm = unsup_ex[key]
        L.append(f'- `{key}`: {n:,} words, {len(unsup_fns[key]):,} functions; e.g. `{o:#x}` `{w:08x}` `{asm}`')
    L += ['\n## Special categories (system registers, atomics, exclusives, rounding)\n', '| category | words | functions |',
          '|---|---:|---:|']
    for c, n in sorted(cat_count.items(), key=lambda x: (x[0].split(':')[0], -x[1])):
        L.append(f'| {c} | {n:,} | {len(cat_fns[c]):,} |')
    lse = sum(n for c, n in cat_count.items() if c == 'atomic_lse')
    llsc = cat_count.get('exclusive_llsc', 0)
    L.append(f'\nLSE atomics: {lse:,} words in {len(cat_fns.get("atomic_lse", ()))} functions; LL/SC exclusives: {llsc:,} words in '
             f'{len(cat_fns.get("exclusive_llsc", ()))} functions.')
    L.append(f'`msr fpcr` sites in function bodies: {len(fpcr_writes)} (FPCR writes switch rounding/FZ/DN; the portable FP lowering keeps '
             f'FPCR in `Cpu.fpcr`). Explicit-rounding conversions (frint*/fcvt[nmapz]*): '
             f'{sum(n for c, n in cat_count.items() if c == "fp_explicit_rounding"):,} words.')
    L += ['\n## Mnemonic histogram (top 60)\n', '| mnemonic | words | functions |', '|---|---:|---:|']
    for m, n in mn_count.most_common(60):
        L.append(f'| {m} | {n:,} | {len(mn_fns[m]):,} |')
    L += ['\n## Rare mnemonics (<= 5 words, up to 60)\n']
    rare = [(m, n) for m, n in sorted(mn_count.items(), key=lambda x: x[1]) if n <= 5][:60]
    L.append(', '.join(f'{m}({n})' for m, n in rare))
    L += ['\n## Operand forms (top 40 of all forms)\n', '| form | words | functions |', '|---|---:|---:|']
    for fm, n in form_count.most_common(40):
        L.append(f'| `{fm}` | {n:,} | {len(form_fns[fm]):,} |')
    (out / 'insn_coverage.md').write_text('\n'.join(L) + '\n', encoding='utf-8')
    print('\n'.join(L[:14]))


if __name__ == '__main__':
    main()
