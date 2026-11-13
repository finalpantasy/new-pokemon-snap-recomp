"""Instruction-form census of main and sdk .text: which AArch64 forms the game executes, with counts and a risk class.

A form is the capstone mnemonic plus the operand shape (register classes, arrangements, lane/immediate/shift markers;
register numbers and immediate values dropped). main is walked over the function/thunk ranges of
analysis/maps/functions_final.tsv; sdk over its whole .text. Output (module offsets only, no game bytes):
  analysis/recomp/insn_census.tsv   form, risk class, main count, sdk count, distinct words, score
Score = (main + sdk count) x risk weight; the fuzzer (fuzz.py) takes its form list from this file.

Usage: tools/venv-recomp/Scripts/python -I tests/insn/census.py
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import re
import struct
import sys
from collections import Counter, defaultdict

import capstone
import numpy as np

from common import ROOT
sys.path.insert(0, str(ROOT / 'recomp'))
from snap_common import ELF, SDK_ELF, elf_sections, functions, lower_priority  # noqa: E402

OUT = ROOT / 'analysis' / 'recomp' / 'insn_census.tsv'

RISK = [  # (class, weight, mnemonic regex); first match wins
    ('excl', 8, r'^(ldx|ldax|stx|stlx|clrex|cas|swp|ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|ldar|stlr|ldapr)'),
    ('sysreg', 8, r'^(mrs|msr|dmb|dsb|isb|sys|dc|ic|hint|yield|wfe|wfi|sev)'),
    ('fpcvt', 10, r'^(fcvt|scvtf|ucvtf|frint|fjcvt)'),
    ('fpest', 10, r'^(frecp|frsqrt|urecpe|ursqrte)'),
    ('fparith', 8, r'^(fmadd|fmsub|fnmadd|fnmsub|fmla|fmls|fsqrt|fdiv|fmulx|fmul|fnmul|fadd|fsub|fabd|faddp|fmax|fmin)'),
    ('fpcmp', 8, r'^(fcmp|fccmp|fcm|facg|facge|facgt|fcsel)'),
    ('fpmove', 3, r'^(fmov|fabs|fneg)'),
    ('neon_sat', 10, r'^(sq|uq|suqadd|usqadd|sqxtn|uqxtn|sqxtun)'),
    ('neon_narrow', 7, r'^(xtn|shrn|rshrn|addhn|raddhn|subhn|rsubhn)'),
    ('neon_widen', 6, r'^(sxtl|uxtl|sshll|ushll|saddl|uaddl|ssubl|usubl|smull|umull|smlal|umlal|smlsl|umlsl|saddw|uaddw|ssubw|usubw|saddlp|uaddlp|sadalp|uadalp|saddlv|uaddlv)'),
    ('neon_shift', 6, r'^(sshr|ushr|srshr|urshr|ssra|usra|srsra|ursra|shl|sli|sri|sshl|ushl|srshl|urshl)'),
    ('neon_lane', 5, r'^(ins|mov|dup|umov|smov|ext|zip|uzp|trn|tbl|tbx|rev|ld\dr?|st\d)$|^(ld[1-4]|st[1-4])'),
    ('neon_int', 4, r'^(add|sub|mul|mla|mls|abs|neg|cm|cnt|addp|addv|smax|smin|umax|umin|sabd|uabd|bsl|bit|bif|and|orr|orn|eor|bic|not|movi|mvni|shadd|uhadd|srhadd|urhadd|shsub|uhsub|smaxv|sminv|umaxv|uminv|smaxp|sminp|umaxp|uminp|cls|clz|rbit|mvn)'),
    ('flags', 6, r'^(adds|subs|cmp|cmn|tst|ands|bics|adcs|sbcs|ngcs|negs|ccmp|ccmn|adc|sbc|ngc)$'),
    ('intdiv', 6, r'^(sdiv|udiv|smulh|umulh|smaddl|umaddl|smsubl|umsubl|smull|umull|smnegl|umnegl|madd|msub|mneg|mul)$'),
    ('bitfield', 4, r'^(sbfx|ubfx|sbfiz|ubfiz|bfi|bfxil|bfc|sxtb|sxth|sxtw|uxtb|uxth|extr|asr|lsl|lsr|ror|cls|clz|rbit|rev|rev16|rev32)$'),
    ('crc', 6, r'^crc32'),
    ('ldst', 3, r'^(ldr|ldur|str|stur|ldp|stp|ldnp|stnp|ldpsw|ldrs|ldurs|prfm|prfum|ldtr|sttr|stg|stgp)'),
    ('branch', 2, r'^(b|bl|br|blr|ret|cbz|cbnz|tbz|tbnz|b\.\w+)$'),
    ('csel', 4, r'^(csel|csinc|csinv|csneg|cset|csetm|cinc|cinv|cneg)$'),
]
RISK = [(c, w, re.compile(p)) for c, w, p in RISK]


def risk(form):
    mn, ops = (form.split(' ', 1) + [''])[:2]
    simd = 'v.' in ops or re.search(r'(^|[ ,{\[])[bhsdq]($|[ ,}\]])', ops)
    for c, w, p in RISK:
        if c.startswith('neon') and not simd:
            continue
        if p.search(mn):
            return c, w
    if mn in ('add', 'sub', 'neg', 'mov', 'and', 'orr', 'eor', 'bic', 'orn', 'eon', 'mvn', 'cmp', 'cmn', 'udf', 'adr', 'adrp',
              'movk', 'movz', 'movn', 'nop'):
        return 'int', 2
    return ('simd_other' if simd else 'int'), (5 if simd else 2)


_SHAPES = [
    (re.compile(r'\bv\d+\.(\d*[bhsdq])\[\d+\]'), r'v.\1[]'),
    (re.compile(r'\bv\d+\.(\d+[bhsd])'), r'v.\1'),
    (re.compile(r'\bv\d+\.([bhsdq])'), r'v.\1'),
    (re.compile(r'\b([xwbhsdq])(\d+|zr)\b'), r'\1'),
    (re.compile(r'\bsp\b'), 'x'),
    (re.compile(r'#-?0x[0-9a-f]+|#-?\d+(\.\d+(e[+-]?\d+)?)?'), '#'),
    (re.compile(r'\b(lsl|lsr|asr|ror|msl) #'), r'\1#'),
]


def shape(op_str):
    s = op_str
    for p, r in _SHAPES:
        s = p.sub(r, s)
    s = re.sub(r'\{[^}]*\}', lambda m: '{' + str(m.group(0).count(',') + 1) + ':' + m.group(0).split(',')[0][1:] + '}', s)
    return s


def main():
    lower_priority()
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    from elfimg import ElfImage
    cnt = {'main': Counter(), 'sdk': Counter()}
    distinct = defaultdict(set)
    seen = {}
    for name, path in (('main', ELF), ('sdk', SDK_ELF)):
        img = ElfImage(str(path))
        if name == 'main':
            ranges = [(s, e) for s, e, k, _ in functions() if k in ('function', 'thunk')]
        else:
            a, n = elf_sections(path)['.text']
            ranges = [(a, a + n)]
        wc = Counter()
        for s, e in ranges:
            wc.update(np.frombuffer(bytes(img.image[s:e]), dtype='<u4').tolist())
        for w, n in wc.items():
            f = seen.get(w)
            if f is None:
                d = next(md.disasm(struct.pack('<I', w), 0), None)
                f = seen[w] = (d.mnemonic + ' ' + shape(d.op_str)).strip() if d else '?undef'
            cnt[name][f] += n
            distinct[f].add(w)
        print(f'{name}: {sum(wc.values())} words, {len(wc)} distinct', file=sys.stderr)
    rows = []
    for f in set(cnt['main']) | set(cnt['sdk']):
        c, wt = risk(f)
        m, s = cnt['main'][f], cnt['sdk'][f]
        rows.append((f, c, m, s, len(distinct[f]), (m + s) * wt))
    rows.sort(key=lambda r: -r[5])
    with open(OUT, 'w', newline='\n') as fh:
        fh.write('# insn_census.tsv: AArch64 forms in main (function ranges) + sdk (.text), tests/insn/census.py\n')
        fh.write('form\tclass\tmain\tsdk\tdistinct_words\tscore\n')
        for r in rows:
            fh.write('\t'.join(map(str, r)) + '\n')
    by = Counter()
    for r in rows:
        by[r[1]] += r[2] + r[3]
    print(f'{len(rows)} forms; per class: ' + ', '.join(f'{k} {v}' for k, v in by.most_common()), file=sys.stderr)


if __name__ == '__main__':
    main()
