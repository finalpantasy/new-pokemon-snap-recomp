"""Static census of the guest `sdk` module for the runtime-boundary decision (docs/runtime-boundary.md).

Read-only. Inputs: analysis/import/v262144-{main,sdk}.elf (from scripts/nso2elf.py). Outputs (no game bytes, only
counts, module offsets and symbol names) in analysis/runtime/boundary/.
Run: tools/venv-recomp/Scripts/python.exe -I scripts/sdk_boundary.py
"""
import bisect, collections, json, os, struct, subprocess
from elftools.elf.elffile import ELFFile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'analysis', 'runtime', 'boundary')
CXXFILT = os.path.join(ROOT, 'tools', 'llvm', 'bin', 'llvm-cxxfilt.exe')


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def uleb(b, p):
    r = s = 0
    while True:
        c = b[p]; p += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80:
            return r, p


def sleb(b, p):
    r = s = 0
    while True:
        c = b[p]; p += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80:
            if c & 0x40:
                r -= 1 << s
            return r, p


class Mod:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        self.f = ELFFile(open(path, 'rb'))
        self.segs = [(s['p_vaddr'], s['p_filesz'], s['p_offset']) for s in self.f.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.sec = {}
        for s in self.f.iter_sections():
            self.sec.setdefault(s.name, (s['sh_addr'], s['sh_size']))
        self.syms = []   # (name, value, size, type, defined)
        dynsym = self.f.get_section_by_name('.dynsym')
        for s in dynsym.iter_symbols():
            if s.name:
                self.syms.append((s.name, s['st_value'], s['st_size'], s['st_info']['type'], s['st_shndx'] != 'SHN_UNDEF'))
        self.reloc_sym = {}
        for rs in ('.rela.plt', '.rela.dyn'):
            sec = self.f.get_section_by_name(rs)
            if sec:
                for r in sec.iter_relocations():
                    if r['r_info_sym']:
                        self.reloc_sym[r['r_offset']] = dynsym.get_symbol(r['r_info_sym']).name

    def read(self, va, n):
        for v, sz, off in self.segs:
            if v <= va and va + n <= v + sz:
                return self.data[off + va - v: off + va - v + n]
        return b'\0' * n

    def plt_map(self):
        """PLT stub address -> symbol (adrp x16 / ldr x17,[x16,#off] / add / br x17)."""
        lo, size = self.sec['.plt']
        words = struct.unpack('<%dI' % (size // 4), self.read(lo, size))
        m = {}
        for i in range(len(words) - 1):
            w, w2 = words[i], words[i + 1]
            if (w & 0x9F00001F) == 0x90000010 and (w2 & 0xFFC003FF) == 0xF9400211:
                pc = lo + 4 * i
                imm = sext((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21) << 12
                got = (pc & ~0xFFF) + imm + ((w2 >> 10) & 0xFFF) * 8
                if got in self.reloc_sym:
                    m[pc] = self.reloc_sym[got]
        return m

    def enc(self, buf, pos, e, base):
        if e == 0xFF:
            return None, pos
        fmt, va = e & 0x0F, base + pos
        if fmt == 0x01:
            v, pos = uleb(buf, pos)
        elif fmt == 0x09:
            v, pos = sleb(buf, pos)
        elif fmt in (0x02, 0x0A):
            v = struct.unpack_from('<h' if fmt == 0x0A else '<H', buf, pos)[0]; pos += 2
        elif fmt in (0x03, 0x0B):
            v = struct.unpack_from('<i' if fmt == 0x0B else '<I', buf, pos)[0]; pos += 4
        elif fmt in (0x00, 0x04, 0x0C):
            v = struct.unpack_from('<q' if fmt == 0x0C else '<Q', buf, pos)[0]; pos += 8
        else:
            raise ValueError('pointer encoding %#x' % e)
        if (e & 0x70) == 0x10:
            v += va
        return v, pos

    def fdes(self):
        lo, size = self.sec['.eh_frame']
        buf = self.read(lo, size)
        cies, out, pos = {}, [], 0
        while pos + 4 <= len(buf):
            ln = struct.unpack_from('<I', buf, pos)[0]
            if ln == 0:
                break
            start, body = pos, pos + 4
            cid = struct.unpack_from('<I', buf, body)[0]
            if cid == 0:
                p = body + 5
                aug = buf[p:buf.index(b'\0', p)].decode(); p += len(aug) + 1
                _, p = uleb(buf, p); _, p = sleb(buf, p); _, p = uleb(buf, p)
                info = {'aug': aug, 'R': 0, 'L': 0xFF, 'P': None}
                if aug.startswith('z'):
                    _, p = uleb(buf, p)
                    for ch in aug[1:]:
                        if ch == 'R':
                            info['R'] = buf[p]; p += 1
                        elif ch == 'L':
                            info['L'] = buf[p]; p += 1
                        elif ch == 'P':
                            pe = buf[p]; p += 1
                            v, p = self.enc(buf, p, pe, lo)
                            if pe & 0x80:
                                v = self.reloc_sym.get(v, 'got%#x' % v)
                            info['P'] = v
                cies[start] = info
            else:
                cie = cies[body - cid]
                p = body + 4
                pc, p = self.enc(buf, p, cie['R'], lo)
                rng, p = self.enc(buf, p, cie['R'] & 0x0F, lo)
                lsda = None
                if cie['aug'].startswith('z'):
                    _, p = uleb(buf, p)
                    if 'L' in cie['aug'] and cie['L'] != 0xFF:
                        v, _ = self.enc(buf, p, cie['L'] & 0x0F, lo)
                        if v:
                            lsda, _ = self.enc(buf, p, cie['L'], lo)
                out.append((pc, rng, lsda, cie['P']))
            pos = body + ln
        return out

    def lsda(self, va, fstart):
        """LSDA call-site table: list of (start, len, landing_pad or 0, action)."""
        buf = self.read(va, 8192)
        p = 0
        lpe = buf[p]; p += 1
        lpstart = fstart
        if lpe != 0xFF:
            lpstart, p = self.enc(buf, p, lpe, va)
        tte = buf[p]; p += 1
        if tte != 0xFF:
            _, p = uleb(buf, p)
        cse = buf[p]; p += 1
        cslen, p = uleb(buf, p)
        end, sites = p + cslen, []
        while p < end:
            s, p = self.enc(buf, p, cse & 0x0F, va)
            l, p = self.enc(buf, p, cse & 0x0F, va)
            lp, p = self.enc(buf, p, cse & 0x0F, va)
            act, p = uleb(buf, p)
            sites.append((fstart + s, l, lpstart + lp if lp else 0, act))
        return sites


def demangle(names):
    r = subprocess.run([CXXFILT], input='\n'.join(names), capture_output=True, text=True)
    return dict(zip(names, r.stdout.split('\n')))


def classify(name, dem):
    d = dem or name
    if name.startswith(('_Unwind_', '__unw_')) or 'libunwind' in d:
        return 'unwind'
    if name.startswith(('__cxa_', '__gxx_', '__dynamic_cast')) or '__cxxabiv1' in d:
        return 'libc++abi'
    for pre in ('vtable for ', 'typeinfo for ', 'typeinfo name for ', 'VTT for ', 'construction vtable for ',
                'guard variable for ', 'non-virtual thunk to ', 'virtual thunk to '):
        if d.startswith(pre):
            d = d[len(pre):]
    if d.startswith(('std::', 'operator new', 'operator delete')):
        return 'libc++'
    if d.startswith('nn::'):
        return 'nn::' + d.split('::')[1].split('(')[0].split('<')[0]
    if d.lower().startswith('nvn'):
        return 'nvn'
    if d.startswith('nv::') or name.startswith('nv'):
        return 'nv'
    if name.startswith('nn') or d.startswith('nn'):
        return 'nn C API'
    if name.startswith('_Z'):
        return 'other C++ (%s)' % d.split('::')[0].split('(')[0][:24]
    return 'libc'


SERVICES = ['sm:', 'fsp-srv', 'hid', 'hid:sys', 'irs', 'appletOE', 'appletAE', 'audout:u', 'audren:u', 'audin:u',
            'audctl', 'auddev', 'hwopus', 'nvdrv', 'nvdrv:a', 'nvmemp', 'vi:u', 'vi:s', 'vi:m', 'set', 'set:sys',
            'time:u', 'time:a', 'time:s', 'acc:u0', 'acc:u1', 'pctl', 'pctl:a', 'prepo:u', 'prepo:a', 'nifm:u', 'bsd:u',
            'bsd:s', 'sfdnsres', 'ssl', 'friend:u', 'caps:su', 'caps:u', 'caps:a', 'ldr:ro', 'ro:1', 'lm', 'pl:u', 'csrng',
            'mm:u', 'apm', 'psm', 'nsd:u', 'nsd:a', 'aoc:u', 'bcat:u', 'erpt:c', 'fatal:u', 'ngct:u', 'pdm:qry',
            'btm:u', 'nfp:user', 'nfc:user', 'ectx:aw', 'grc:c', 'ldn:u', 'friend:a', 'olsc:u', 'hidbus', 'mii:u',
            'pgl', 'jit:u', 'dauth:0', 'fsp-pr', 'ns:am2', 'ns:ec', 'pm:shell', 'spl:', 'ovln:snd', 'nim:eca']


def main():
    os.makedirs(OUT, exist_ok=True)
    sdk = Mod(os.path.join(ROOT, 'analysis', 'import', 'v262144-sdk.elf'))
    mn = Mod(os.path.join(ROOT, 'analysis', 'import', 'v262144-main.elf'))
    rep = {}
    sdefs = [s for s in sdk.syms if s[4]]
    sund = [s for s in sdk.syms if not s[4]]
    dem = demangle(sorted({s[0] for s in sdk.syms} | {s[0] for s in mn.syms}))
    by_name = {s[0]: s for s in sdefs}
    cls = {s[0]: classify(s[0], dem.get(s[0])) for s in sdk.syms + mn.syms}
    tlo, tsz = sdk.sec['.text']
    rep['sdk'] = {'text_bytes': tsz, 'plt_bytes': sdk.sec['.plt'][1], 'data_bytes': sdk.sec['.data'][1],
                  'bss_bytes': sdk.sec['.bss'][1], 'defined_dynsyms': len(sdefs),
                  'defined_funcs': sum(1 for s in sdefs if s[3] == 'STT_FUNC'), 'undefined_dynsyms': len(sund),
                  'undefined_names': sorted(s[0] for s in sund)}
    c, cb = collections.Counter(), collections.Counter()
    for s in sdefs:
        if s[3] == 'STT_FUNC':
            c[cls[s[0]]] += 1; cb[cls[s[0]]] += s[2]
    rep['sdk']['exported_funcs_by_class'] = {k: [c[k], cb[k]] for k, _ in c.most_common()}

    sf, mf = sdk.fdes(), mn.fdes()
    rep['sdk']['fdes'] = len(sf)
    rep['sdk']['fdes_with_lsda'] = sum(1 for x in sf if x[2])
    rep['sdk']['personalities'] = dict(collections.Counter(str(x[3]) for x in sf if x[2]))

    lp_rows, n_lp, n_catch = [], 0, 0
    for pc, rng, lsda, pers in mf:
        if not lsda:
            continue
        sites = mn.lsda(lsda, pc)
        withlp = [s for s in sites if s[2]]
        lps = sorted({s[2] for s in withlp})
        if lps:
            n_lp += len(lps)
            catch = any(s[3] for s in withlp)
            n_catch += catch
            lp_rows.append((pc, rng, len(withlp), len(lps), int(catch)))
    rep['main_eh'] = {'fdes': len(mf), 'fdes_with_lsda': sum(1 for x in mf if x[2]),
                      'personalities': dict(collections.Counter(str(x[3]) for x in mf if x[2])),
                      'functions_with_landing_pads': len(lp_rows), 'landing_pads_unique': n_lp,
                      'callsites_with_landing_pad': sum(r[2] for r in lp_rows),
                      'functions_with_catch_or_filter_action': n_catch}
    with open(os.path.join(OUT, 'main_landing_pads.tsv'), 'w', newline='\n') as fo:
        fo.write('# main functions whose LSDA has landing pads (module offsets); has_action = some pad has a catch/filter action\n')
        fo.write('function\tsize\tcallsites_with_lp\tunique_pads\thas_action\n')
        for r in lp_rows:
            fo.write('%#x\t%#x\t%d\t%d\t%d\n' % r)

    mimps = [s for s in mn.syms if not s[4]]
    res, unresolved = collections.Counter(), []
    with open(os.path.join(OUT, 'main_imports_in_sdk.tsv'), 'w', newline='\n', encoding='utf-8') as fo:
        fo.write('# each undefined dynsym of main resolved by name against sdk exports (sdk module offsets)\n')
        fo.write('import\tdemangled\tclass\tsdk_offset\tsdk_size\tsdk_type\n')
        for s in mimps:
            d = by_name.get(s[0])
            if d:
                res[cls[s[0]]] += 1
                fo.write('%s\t%s\t%s\t%#x\t%d\t%s\n' % (s[0], dem.get(s[0], ''), cls[s[0]], d[1], d[2], d[3]))
            else:
                unresolved.append(s[0])
                fo.write('%s\t%s\t%s\t-\t-\t-\n' % (s[0], dem.get(s[0], ''), cls[s[0]]))
    rep['main_imports'] = {'total': len(mimps), 'resolved_in_sdk': sum(res.values()),
                           'by_class': dict(res.most_common()), 'unresolved': unresolved}

    pltm = sdk.plt_map()
    rep['sdk']['plt_stubs_decoded'] = len(pltm)
    words = struct.unpack('<%dI' % (tsz // 4), sdk.read(tlo, tsz))
    starts = set(x[0] for x in sf) | {s[1] for s in sdefs if s[3] == 'STT_FUNC' and tlo <= s[1] < tlo + tsz}
    for i, w in enumerate(words):
        if (w & 0xFC000000) == 0x94000000:
            t = tlo + 4 * i + sext(w & 0x3FFFFFF, 26) * 4
            if tlo <= t < tlo + tsz:
                starts.add(t)
    starts = sorted(starts)
    fname = {}
    for s in sorted(sdefs, key=lambda s: s[0]):
        if s[3] == 'STT_FUNC':
            fname.setdefault(s[1], s[0])

    def fn_of(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 else None

    edges, ext, fsvc = collections.defaultdict(set), collections.defaultdict(set), collections.defaultdict(list)
    for i, w in enumerate(words):
        a = tlo + 4 * i
        if (w & 0x7C000000) == 0x14000000:
            t = a + sext(w & 0x3FFFFFF, 26) * 4
            src = fn_of(a)
            if t in pltm:
                nm = pltm[t]
                if nm in by_name and by_name[nm][1]:
                    edges[src].add(by_name[nm][1])
                else:
                    ext[src].add(nm)
            elif tlo <= t < tlo + tsz:
                dst = fn_of(t)
                if dst != src or t == src:
                    edges[src].add(dst)
        elif (w & 0xFFE0001F) == 0xD4000001:
            fsvc[fn_of(a)].append((w >> 5) & 0xFFFF)
    svc_count, svc_funcs = collections.Counter(), collections.defaultdict(set)
    for f, l in fsvc.items():
        for imm in l:
            svc_count[imm] += 1
            svc_funcs[imm].add(fname.get(f, '%#x' % f))
    rep['sdk']['svc_sites'] = sum(svc_count.values())
    rep['sdk']['svc_distinct'] = len(svc_count)
    rep['sdk']['svc_by_imm'] = {('%#x' % k): v for k, v in sorted(svc_count.items())}
    rep['sdk']['functions_in_callgraph'] = len(starts)

    def closure(roots):
        seen, st = set(), list(roots)
        while st:
            f = st.pop()
            if f not in seen:
                seen.add(f); st.extend(edges.get(f, ()))
        return seen

    def summarize(seen):
        size, sv = 0, collections.Counter()
        for f in seen:
            i = bisect.bisect_left(starts, f)
            size += (starts[i + 1] if i + 1 < len(starts) else tlo + tsz) - f
            for imm in fsvc.get(f, ()):
                sv[imm] += 1
        kinds = collections.Counter(cls[fname[f]] if f in fname else 'internal (not exported)' for f in seen)
        exts = set()
        for f in seen:
            exts |= ext.get(f, set())
        return {'functions': len(seen), 'bytes': size, 'svcs': {('%#x' % k): v for k, v in sorted(sv.items())},
                'by_class': dict(kinds.most_common()), 'external_calls': sorted(exts)}

    def roots(classes=None):
        return [by_name[s[0]][1] for s in mimps if s[0] in by_name and by_name[s[0]][3] == 'STT_FUNC'
                and (classes is None or cls[s[0]] in classes)]
    rt_classes = ('libc', 'libc++', 'libc++abi', 'unwind')
    ca, cr = closure(roots()), closure(roots(rt_classes))
    rep['closure_all_imports'] = summarize(ca)
    rep['closure_runtime_imports'] = summarize(cr)
    rep['sdk_whole'] = summarize(set(starts))
    nn_reached = sorted({fname[f] for f in cr if f in fname and cls[fname[f]].startswith(('nn', 'nv'))})
    rep['closure_runtime_imports']['nn_entry_points'] = len(nn_reached)
    with open(os.path.join(OUT, 'runtime_closure_nn_calls.tsv'), 'w', newline='\n', encoding='utf-8') as fo:
        fo.write('# nn::/nv exported sdk functions reached by direct calls from the libc/libc++/libc++abi/unwind imports of main\n')
        fo.write('class\tfunction\n')
        for n in nn_reached:
            fo.write('%s\t%s\n' % (cls[n], dem.get(n, n)))
    # Per service imported by main: does its direct-call closure reach IPC (nn::sf / svc) or stay user-mode?
    per = {}
    ipc_svcs = {0x1F, 0x21, 0x22, 0x23, 0x43, 0x44, 0x72}
    for k in sorted({cls[s[0]] for s in mimps}):
        cl = closure(roots((k,)))
        sv = {imm for f in cl for imm in fsvc.get(f, ())}
        sf = sum(1 for f in cl if f in fname and cls[fname[f]] in ('nn::sf', 'nn::sm'))
        per[k] = {'imports': sum(1 for s in mimps if cls[s[0]] == k), 'closure_functions': len(cl),
                  'svcs': len(sv), 'ipc_svcs': len(sv & ipc_svcs), 'nn_sf_functions': sf}
    rep['per_service_closure'] = per
    with open(os.path.join(OUT, 'per_service_closure.tsv'), 'w', newline='\n') as fo:
        fo.write('# direct-call closure inside sdk of the imports of each class (lower bound: no indirect calls)\n')
        fo.write('class\timports\tclosure_functions\tdistinct_svcs\tipc_svcs\tnn_sf_or_sm_functions\n')
        for k, v in per.items():
            fo.write('%s\t%d\t%d\t%d\t%d\t%d\n' % (k, v['imports'], v['closure_functions'], v['svcs'], v['ipc_svcs'], v['nn_sf_functions']))

    # Frontier: expand the runtime closure only through non-nn functions; the first nn::/nv export reached on each path
    # is where a host override would sit if sdk's libc/libc++ are lifted and nn:: is HLE'd.
    def is_nn(f):
        return f in fname and cls[fname[f]].startswith(('nn', 'nv'))
    seen, st, frontier = set(), list(roots(rt_classes)), set()
    while st:
        f = st.pop()
        if f in seen:
            continue
        seen.add(f)
        if is_nn(f):
            frontier.add(f)
            continue
        st.extend(edges.get(f, ()))
    fsv = collections.Counter(imm for f in seen if not is_nn(f) for imm in fsvc.get(f, ()))
    main_nn = {s[0] for s in mimps if cls[s[0]].startswith(('nn', 'nv'))}
    fr_names = sorted(fname[f] for f in frontier)
    rep['runtime_frontier'] = {'lifted_functions': len(seen) - len(frontier),
                               'nn_frontier_functions': len(frontier),
                               'nn_frontier_not_imported_by_main': sum(1 for n in fr_names if n not in main_nn),
                               'by_class': dict(collections.Counter(cls[n] for n in fr_names).most_common()),
                               'svcs_without_nn': {('%#x' % k): v for k, v in sorted(fsv.items())}}
    with open(os.path.join(OUT, 'runtime_frontier_nn.tsv'), 'w', newline='\n', encoding='utf-8') as fo:
        fo.write('# first nn::/nv export reached from main\'s libc/libc++/libc++abi/unwind imports (direct calls only): the HLE override points if sdk runtime is lifted\n')
        fo.write('class\timported_by_main\tfunction\n')
        for n in fr_names:
            fo.write('%s\t%d\t%s\n' % (cls[n], int(n in main_nn), dem.get(n, n)))
    with open(os.path.join(OUT, 'sdk_svcs.tsv'), 'w', newline='\n', encoding='utf-8') as fo:
        fo.write('# svc #imm in sdk .text. in_all/in_runtime = reached by the direct-call closure of all main imports / of the libc+libc++ imports only\n')
        fo.write('svc\tsites\tin_all_closure\tin_runtime_closure\tfunctions (first 4)\n')
        for imm in sorted(svc_count):
            fs = sorted(svc_funcs[imm])
            k = '%#x' % imm
            fo.write('%s\t%d\t%d\t%d\t%s\n' % (k, svc_count[imm], int(k in rep['closure_all_imports']['svcs']),
                     int(k in rep['closure_runtime_imports']['svcs']),
                     '; '.join(dem.get(x, x) for x in fs[:4]) + (' ...' if len(fs) > 4 else '')))
    with open(os.path.join(OUT, 'sdk_exports_by_class.tsv'), 'w', newline='\n', encoding='utf-8') as fo:
        fo.write('class\texported_funcs\tbytes_st_size_sum\n')
        for k, v in rep['sdk']['exported_funcs_by_class'].items():
            fo.write('%s\t%d\t%d\n' % (k, v[0], v[1]))
    blob = b''.join(sdk.read(v, sz) for v, sz, _ in sdk.segs)
    rep['sdk']['ipc_service_name_strings'] = [s for s in SERVICES if (b'\0' + s.encode() + b'\0') in blob]
    with open(os.path.join(OUT, 'sdk_census.json'), 'w', newline='\n') as fo:
        json.dump(rep, fo, indent=1, sort_keys=True)
    print(json.dumps(rep, indent=1, sort_keys=True)[:7000])


if __name__ == '__main__':
    main()
