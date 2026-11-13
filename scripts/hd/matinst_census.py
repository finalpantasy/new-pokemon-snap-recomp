"""Read-only census of Pokemon material instances (matinstb), schemaless.
Usage: python -I scripts/hd/matinst_census.py [root] > analysis/materials/census.txt
Prints, per shader id, the number of materials, texture slot names and parameter names."""
import os, re, sys, struct, collections
root = sys.argv[1] if len(sys.argv) > 1 else 'extracted/drp-v262144/nx/Pack/Characters/master'
def strings(b):
    out = []
    i = 0
    while i + 5 < len(b):
        n = struct.unpack_from('<I', b, i)[0]
        if 3 <= n < 200 and i + 4 + n < len(b) and b[i + 4 + n] == 0:
            s = b[i + 4:i + 4 + n]
            if all(32 <= c < 127 for c in s):
                out.append((i, s.decode()))
                i = (i + 4 + n + 1 + 3) & ~3
                continue
        i += 4
    return out
shaders = collections.defaultdict(lambda: dict(n=0, params=collections.Counter(), slots=collections.Counter(), ex=[]))
for d in sorted(os.listdir(root)):
    if not re.fullmatch(r'CH_\d+\.drp', d):
        continue
    for f in sorted(os.listdir(os.path.join(root, d))):
        if not f.startswith('MI_CH_') or not f.endswith('.matinstb'):
            continue
        b = open(os.path.join(root, d, f), 'rb').read()
        ss = [s for _, s in strings(b)]
        sh = [s for s in ss if re.fullmatch(r'CharactersMaterials_CharacterShader_\d+', s)]
        key = sh[-1] if sh else '?'
        e = shaders[key]
        e['n'] += 1
        for s in ss:
            if s.endswith('.nutexb'):
                continue
            if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', s) and not s.startswith(('Characters', 'CharacterShader')):
                if s in ('baseColorTexture', 'paramsTexture', 'normalTexture', 'sampler_diffuse') or s.endswith('Texture') or s.startswith('sampler'):
                    e['slots'][s] += 1
                else:
                    e['params'][s] += 1
        if len(e['ex']) < 3:
            e['ex'].append(d + '/' + f)
for k, e in sorted(shaders.items(), key=lambda x: -x[1]['n']):
    print(k, 'materials', e['n'])
    print('  slots', dict(e['slots']))
    print('  params', dict(e['params']))
    print('  e.g.', e['ex'])
