"""Read-only: which texture channels each CharacterShader pixel shader really uses.
Inputs (all local, from the translator): a dir of <key>.frag (spirv-cross GLSL of the translated SPIR-V) and names.tsv
(shader name, program sha, key). Reflection comes from the nushdb (sampler name per binding).
Usage: python -I scripts/hd/material_channels.py <dir_with_frag_files> <names.tsv> [--shader <id>]
Prints per sampler name: shaders sampling it and how often each component (x y z w) is consumed downstream."""
import os, re, sys, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import nushdb_parse as P
R = 'extracted/drp-v262144/nx/Pack/Resident/master/Resident.drp/CharactersMaterials_CharacterShader_%s.nushdb'

def analyse(glsl, refl):
    """-> {sampler_name: set(components used)}; sampler_name via handle index (hex h -> binding (h-8)/2)."""
    out = collections.defaultdict(set)
    uses = collections.Counter(re.findall(r'\b_\d+\b', glsl))
    for m in re.finditer(r'(?:vec4|vec3|vec2|float) (_\d+) = (?:texture|textureLod|textureGrad)\(fp_t_tcb_([0-9A-F]+),[^\n]*?\)(?:\.([xyzw]+))?;', glsl):
        v, h, sw = m.group(1), int(m.group(2), 16), m.group(3)
        name = refl.get((h - 8) // 2, 'tcb_%X' % h)
        if sw is None:
            continue  # shadow compare sampler (scalar result)
        defline = m.group(0)
        if defline.startswith('float'):
            if uses[v] > 1: out[name].add(sw)
            continue
        full = sw if sw else 'xyzw'  # component i of the fetched value = source channel full[i]
        def src(ch):
            return full['xyzw'.index(ch)] if len(full) > 'xyzw'.index(ch) else ch
        for c in re.finditer(r'float (_\d+) = %s\.([xyzw]);' % v, glsl):
            if uses[c.group(1)] > 1: out[name].add(src(c.group(2)))
        for c in re.finditer(r'vec[23] (_\d+) = %s\.([xyzw]+);' % v, glsl):
            if uses[c.group(1)] > 1: out[name].update(src(ch) for ch in c.group(2))
    return out

def main():
    d, names = sys.argv[1], sys.argv[2]
    only = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--shader' else None
    rows = [l.split() for l in open(names)]
    seen = {}
    agg = collections.defaultdict(collections.Counter); nsh = collections.Counter()
    for name, h, key in rows:
        sid = name.replace('CharacterShader_', '').replace('PS', '')
        if only and sid != only: continue
        f = os.path.join(d, key + '.frag')
        if not os.path.exists(f): continue
        if h in seen: res = seen[h]
        else:
            pr = [p for p in P.parse_file(R % sid) if p['name'] == name][0]
            refl = {s['binding_per_stage'][pr['blob']['nvn_stage']]: s['name'].rsplit('_', 1)[0] for s in pr['blob']['refl']['samplers']}
            res = seen[h] = analyse(open(f).read(), refl)
        if only:
            print(sid, {k: ''.join(sorted(v, key='xyzw'.index)) for k, v in res.items()})
        for k, v in res.items():
            nsh[k] += 1
            for c in v: agg[k][c] += 1
    for k in sorted(nsh):
        print('%-26s shaders=%4d  components used: %s' % (k, nsh[k], dict(agg[k])))
main()
