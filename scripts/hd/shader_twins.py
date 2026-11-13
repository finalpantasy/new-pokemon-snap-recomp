"""Read-only: for CharacterShader pixel shaders without a normalTexture sampler, find 'twins' that have the same samplers
plus normalTexture and the same ParameterList member names (so a material could be re-pointed to the twin).
Usage: python -I scripts/hd/shader_twins.py <id> [<id> ...]"""
import glob, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import nushdb_parse as P
R = 'extracted/drp-v262144/nx/Pack/Resident/master/Resident.drp/'
def info(f):
    for p in P.parse_file(f):
        if p['name'].endswith('PS'):
            rf = p['blob']['refl']
            smp = frozenset(s['name'].rsplit('_', 1)[0] for s in rf['samplers'])
            par = frozenset(u['name'] for u in rf['uniforms'] if rf['ubos'][u['block']]['name'] == 'ParameterList')
            return smp, par, p['blob']['insn']
db = {}
for f in glob.glob(R + 'CharactersMaterials_CharacterShader_*.nushdb'):
    db[os.path.basename(f)[:-7].rsplit('_', 1)[1]] = info(f)
for a in sys.argv[1:]:
    smp, par, n = db[a]
    print(a, 'samplers', sorted(smp), 'params', len(par), 'insn', n)
    tw = [(k, v[2]) for k, v in db.items() if v[0] == smp | {'normalTexture'} and v[1] == par]
    tw2 = [(k, v[2], sorted(v[1] ^ par)) for k, v in db.items() if v[0] == smp | {'normalTexture'} and v[1] != par and len(v[1] ^ par) <= 3]
    print('  exact twins with normalTexture:', tw[:8], len(tw))
    print('  near twins (param diff<=3):', tw2[:6], len(tw2))
