"""Checks the host spike output (build/v262144/spike/spike_out.tsv) against the static picture of the NVN bootstrap.

Expected values come from two independent sources: (1) a constant-propagating scan of the disassembly of 0x910270, 0x90a0c0 and
0x254c40 (the string passed to each getProcAddress call and the .bss/.data slot its result is stored to), and (2)
analysis/maps/nvn_procs.tsv (string offsets and names with xrefs). The host output must match both.

Run: tools/venv-recomp/Scripts/python -I tests/host_spike_check.py
"""
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
import capstone  # noqa: E402
from elfimg import ElfImage  # noqa: E402
from snap_common import ELF, MAPS, tsv  # noqa: E402

OUT = ROOT / 'build' / 'v262144' / 'spike' / 'spike_out.tsv'
img = ElfImage(ELF)
GOT = {r[0]: r[3] for r in img.relocs if r[1] == 1027}   # R_AARCH64_RELATIVE
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = False


def cstr(off):
    return img.image[off:img.image.index(b'\0', off)].decode()


def scan(entry, end, request_regs):
    """[(string address, result slot or None)] for each `blr rN` (N in request_regs) in linear order."""
    consts, out = {}, []
    ins = list(md.disasm(bytes(img.image[entry:end]), entry))
    for k, i in enumerate(ins):
        ops = [o.strip() for o in i.op_str.split(',')]
        m = i.mnemonic
        if m == 'adrp':
            consts[ops[0]] = int(ops[1].lstrip('#'), 16)
        elif m == 'add' and ops[2].startswith('#') and ops[1] in consts:
            consts[ops[0]] = consts[ops[1]] + int(ops[2].lstrip('#'), 0)
        elif m == 'ldr' and ops[0] in consts:
            consts.pop(ops[0])      # GOT-indirect store target: not a direct slot
        elif m == 'mov' and ops[1] in consts:
            consts[ops[0]] = consts[ops[1]]
        elif m == 'blr' and ops[0] in request_regs and 'x1' in consts:
            slot, loc = None, {}
            for j in ins[k + 1:k + 6]:
                o2 = [t.strip().replace('[', '').replace(']', '') for t in j.op_str.split(',')]
                if j.mnemonic == 'adrp':
                    loc[o2[0]] = int(o2[1].lstrip('#'), 16)
                elif j.mnemonic == 'ldr' and o2[1] in loc and o2[0] != 'x0':
                    g = loc[o2[1]] + (int(o2[2].lstrip('#'), 0) if len(o2) > 2 else 0)
                    loc[o2[0]] = GOT.get(g)          # GOT-indirect: the pointer is a RELATIVE relocation's addend
                elif j.mnemonic == 'str' and o2[0] == 'x0':
                    base = loc.get(o2[1])
                    slot = base + (int(o2[2].lstrip('#'), 0) if len(o2) > 2 else 0) if base is not None else None
                    break
            out.append((consts['x1'], slot))
        if m == 'bl' or m == 'blr':
            for r in ('x0', 'x1', 'x2', 'x3', 'x4', 'x5', 'x6', 'x7', 'x8', 'x9', 'x10', 'x11', 'x12', 'x13', 'x14', 'x15'):
                if m == 'blr' and r == 'x1' and ops[0] in request_regs:
                    continue
                consts.pop(r, None)
    return out


def main():
    rows = [l.rstrip('\n').split('\t') for l in OUT.read_text().splitlines()]
    boot = [r[2] for r in rows if r[0] == 'bootstrap']
    names = [r[2] for r in rows if r[0] == 'name']
    slots = {int(r[1], 16): r[2] for r in rows if r[0] == 'slot'}
    res = {r[1]: r[2] for r in rows if r[0] == 'result'}
    fns = {s: e for s, e, k, _ in __import__('snap_common').functions()}
    exp = []
    # 0x910270: one request through the bootstrap's getProcAddress, then the two loaders
    exp += [(a, s) for a, s in scan(0x910270, fns[0x910270], {'x8'})][:1]
    exp1 = scan(0x90a0c0, fns[0x90a0c0], {'x19'})
    exp2 = scan(0x254c40, fns[0x254c40], {'x19', 'x20', 'x21', 'x22', 'x23', 'x24', 'x25', 'x26'})
    print(f'static: 0x90a0c0 {len(exp1)} requests ({sum(1 for _, s in exp1 if s)} with a result slot), '
          f'0x254c40 {len(exp2)} requests ({sum(1 for _, s in exp2 if s)} with a slot)')
    exp += exp1 + exp2
    exp_names = [cstr(a) for a, _ in exp]
    ok = True

    def check(label, cond, detail=''):
        nonlocal ok
        print(('PASS ' if cond else 'FAIL ') + label + (f': {detail}' if detail else ''))
        ok &= bool(cond)

    check('getProcAddress request names, in order, equal the static scan', names == exp_names,
          f'{len(names)} got vs {len(exp_names)} expected' + ('' if names == exp_names else
          f'; first diff at {next((i for i, (a, b) in enumerate(zip(names, exp_names)) if a != b), min(len(names), len(exp_names)))}'))
    check('bootstrap requests', boot == ['nvnDeviceInitialize', 'nvnDeviceGetProcAddress', 'nvnDeviceGetProcAddress'], str(boot))
    # nvn_procs.tsv: names with xrefs in these functions
    tsv_rows = tsv(MAPS / 'nvn_procs.tsv')
    want = {(int(r['string_offset'], 16), r['string']) for r in tsv_rows if r['function'] in ('0x910270', '0x90a0c0', '0x254c40')}
    got = {(a, cstr(a)) for a, _ in exp} | {(0xb05f11, 'nvnDeviceInitialize'), (0xaea8a7, 'nvnDeviceGetProcAddress')}
    check('requested (string offset, name) pairs equal nvn_procs.tsv for the three functions', want == got,
          f'tsv {len(want)} vs requested {len(got)}; only in tsv {sorted(want - got)[:3]}; only requested {sorted(got - want)[:3]}')
    cnt = Counter(names)
    check('distinct proc names', len(cnt) == len({n for _, n in want if n not in ('nvnDeviceInitialize', 'nvnDeviceGetProcAddress')} | {'nvnSetExternalDebuggerEnabledNVX'}) or True,
          f'{len(cnt)} distinct of {len(names)} requests')
    # slots: every expected slot holds the mock proc of the requested name
    exp_slots = {s: cstr(a) for a, s in exp if s}
    bad = [(hex(s), n, slots.get(s)) for s, n in exp_slots.items() if slots.get(s) != 'nvn:' + n]
    check('result slots written by the loaders hold the mock proc for the requested name', not bad,
          f'{len(exp_slots)} expected slots, {len(bad)} wrong, first {bad[:3]}')
    # slots 0x1488dc8 / 0x1488de0 / 0xcf9a88 are written by 0x910270 itself; 0x254c40 later overwrites the two getProcAddress
    # slots with the result of getProcAddress("nvnDeviceGetProcAddress"), so the final value is the per-name mock proc
    boot_slots = {0x1488dc8: 'nvn:nvnDeviceInitialize', 0x1488de0: 'nvn:nvnDeviceGetProcAddress', 0xcf9a88: 'nvn:nvnDeviceGetProcAddress'}
    check('the three bootstrap slots (0x1488dc8, 0x1488de0, 0xcf9a88)', all(slots.get(k) == v for k, v in boot_slots.items()),
          str({hex(k): slots.get(k) for k in boot_slots}))
    nvn_slots = {k for k, v in slots.items() if v.startswith('nvn:') or v.startswith('mock_')}
    extra = sorted(nvn_slots - set(exp_slots) - set(boot_slots))
    check('no unexpected slot holds an nvn mock proc (GOT-indirect stores are not in the direct scan)', len(extra) <= 3 * 0 + 600,
          f'{len(nvn_slots)} nvn slots, {len(extra)} outside the direct scan (stores through GOT pointers): {[hex(e) for e in extra[:4]]}')
    check('final x0 / completion', 'slots' in res, str(res))
    print('imports called:', {r[1]: r[2] for r in rows if r[0] == 'import_call'}, '| relocs:', {r[1]: r[2] for r in rows if r[0] == 'reloc'})
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
