# SPDX-License-Identifier: MIT
"""Read-only: which texture channels each EnvironmentMaterials_StageShader pixel shader really uses (same method as
material_channels.py, for the course shaders). Needs the translator harness (build/xlate/r2/h), the corpus and spirv-cross.
  python -E -P scripts/hd/env_channels.py <shader ids file (json list)> <out.json>
Output: {id: {sampler name: components used}}."""
import collections, hashlib, json, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts')); sys.path.insert(0, str(ROOT / 'scripts' / 'hd'))
import nushdb_parse as P
HARNESS = ROOT / 'build/xlate/r2/h/bin/Release/net10.0/ShaderHarness.exe'
CROSS = ROOT / 'tools/vulkan_sdk/Bin/spirv-cross.exe'
LIST = ROOT / 'build/xlate/corpus/list.tsv'
RES = ROOT / 'extracted/drp-v262144/nx/Pack/Resident/master/Resident.drp'
import re
src = open(ROOT / 'scripts/hd/material_channels.py').read()
ns = {}
exec(src[src.index('def analyse'):src.index('def main')], {'re': re, 'collections': collections}, ns)
analyse = ns['analyse']
rows = {}
for l in open(LIST):
    f = l.rstrip('\n').split('\t')
    rows[f[4]] = l
ids = [i.replace('EnvironmentMaterials_StageShader_', '') for i in json.load(open(sys.argv[1]))]
res = {}
with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    ok = [i for i in ids if 'StageShader_%sPS' % i in rows]
    (td / 'list.tsv').write_text(''.join(rows['StageShader_%sPS' % i] for i in ok))
    subprocess.run([str(HARNESS), '--nvn-batch', str(td / 'list.tsv'), str(ROOT / 'build/xlate/corpus/bins'), str(td / 'out'), 'x'], check=True, capture_output=True)
    for i in ok:
        h, st, co, cd, nm = rows['StageShader_%sPS' % i].rstrip('\n').split('\t')
        data = (ROOT / 'build/xlate/corpus/bins' / (h + '.bin')).read_bytes()
        key = hashlib.sha256(data + ('x|%s|%s' % (st, cd)).encode()).hexdigest()
        glsl = subprocess.run([str(CROSS), str(td / 'out' / (key + '.spv'))], capture_output=True, text=True).stdout
        pr = [p for p in P.parse_file(str(RES / ('EnvironmentMaterials_StageShader_%s.nushdb' % i))) if p['name'] == 'StageShader_%sPS' % i][0]
        refl = {s['binding_per_stage'][pr['blob']['nvn_stage']]: s['name'].rsplit('_', 1)[0] for s in pr['blob']['refl']['samplers']}
        a = analyse(glsl, refl)
        res[i] = {k: ''.join(sorted(v, key='xyzw'.index)) for k, v in a.items()}
json.dump(res, open(sys.argv[2], 'w'), indent=1)
print(len(res), 'of', len(ids), 'shaders')
