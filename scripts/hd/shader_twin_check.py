# SPDX-License-Identifier: MIT
"""Read-only: compare a CharacterShader pixel shader with candidate twins (shaders that add `normalTexture`) at the translated
SPIR-V / GLSL level. Used to choose the twin for make_material_mod.py.

  python -I scripts/hd/shader_twin_check.py <original id> <candidate id> [<candidate id> ...] [--keep dir]

For each candidate it prints the difference in: uniform components read (ParameterList and the other constant buffers), GLSL
function-call histogram (texture, sqrt, inversesqrt, fma, ...), and float literals. A same-lighting twin shows the original
plus exactly one extra fetch (+1 texture), the z rebuild (+1 sqrt), two normalizes (+2 inversesqrt), ~21 fma and one extra
uniform component (cbuf 11 [13].x = sssNormalMapWeight); everything else equal. Candidates that differ elsewhere carry other
lighting options.
Needs the translator harness (build/xlate/r2/h, see analysis/shaders/translator.md), the corpus (build/xlate/corpus) and
tools/vulkan_sdk spirv-cross. Translates in a temp folder; nothing is written elsewhere.
"""
import collections, hashlib, os, re, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / 'build/xlate/r2/h/bin/Release/net10.0/ShaderHarness.exe'
CROSS = ROOT / 'tools/vulkan_sdk/Bin/spirv-cross.exe'
NAMES = ROOT / 'analysis/materials/names.tsv'        # name, program sha, key (scripts/hd/material_channels.py)
LIST = ROOT / 'build/xlate/corpus/list.tsv'          # program sha, stage, const off, code off, name
FN = r'\b(sqrt|inversesqrt|pow|exp2|log2|textureLod|texture|normalize|dot|cross|mix|clamp|fma|min|max|abs|floor|fract|isnan)\('


def corpus_row(sid):
    sha = None
    for l in open(NAMES):
        f = l.split()
        if f[0] == 'CharacterShader_%sPS' % sid:
            sha = f[1]
    for l in open(LIST):
        f = l.rstrip('\n').split('\t')
        if f[0] == sha and f[1] == '1':
            return l
    raise SystemExit('shader not in the corpus: ' + sid)


def glsl_of(ids, work):
    rows = [corpus_row(s) for s in ids]
    (work / 'list.tsv').write_text(''.join(rows))
    subprocess.run([str(HARNESS), '--nvn-batch', str(work / 'list.tsv'), str(ROOT / 'build/xlate/corpus/bins'), str(work / 'out'), 'x'],
                   check=True, capture_output=True)
    out = {}
    for sid, l in zip(ids, rows):
        h, st, co, cd, nm = l.rstrip('\n').split('\t')
        data = (ROOT / 'build/xlate/corpus/bins' / (h + '.bin')).read_bytes()
        key = hashlib.sha256(data + ('x|%s|%s' % (st, cd)).encode()).hexdigest()
        out[sid] = subprocess.run([str(CROSS), str(work / 'out' / (key + '.spv'))], capture_output=True, text=True).stdout
    return out


def feats(t):
    u = set()
    for c, i, sw in re.findall(r'fp_c(\d+)_\d*\._m0\[(\d+)\]\.?([xyzw]*)', t):
        for ch in (sw or 'xyzw'):
            u.add((c, i, ch))
    fx = collections.Counter(re.findall(FN, t))
    lits = collections.Counter(re.findall(r'(?<![\w.])-?\d+\.\d+(?:e[-+]?\d+)?(?![\w])', t))
    return u, fx, lits


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2:
        raise SystemExit(__doc__)
    with tempfile.TemporaryDirectory() as td:
        g = glsl_of(args, Path(td))
    ou, of, ol = feats(g[args[0]])
    print(args[0], 'uniform comps', len(ou), 'calls', dict(of))
    for c in args[1:]:
        u, f, l = feats(g[c])
        print(c, 'extra uniform comps', sorted(u - ou), 'missing', sorted(ou - u),
              '| call delta', {k: f[k] - of[k] for k in set(f) | set(of) if f[k] != of[k]},
              '| literals +%d -%d' % (sum((l - ol).values()), sum((ol - l).values())))


if __name__ == '__main__':
    main()
