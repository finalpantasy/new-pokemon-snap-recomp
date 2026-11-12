# SPDX-License-Identifier: MIT
"""Small CPU textured renderer for cache glTF models (bind pose, front view from +Z), used for before/after proofs.
Local data only: reads build/hd/cache, writes wherever the caller says."""
import json
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / 'build' / 'hd' / 'cache'


class Model:
    def __init__(self, asset, lod='model.gltf'):
        self.base = CACHE / asset
        g = json.loads((self.base / lod).read_text())
        buf = (self.base / g['buffers'][0]['uri']).read_bytes()
        self.g = g
        self.prims = []
        for m in g['meshes']:
            p = m['primitives'][0]
            a = p['attributes']
            P = self._acc(g, buf, a['POSITION'])
            I = self._acc(g, buf, p['indices'])[:, 0].astype(np.int64).reshape(-1, 3)
            UV = self._acc(g, buf, a['TEXCOORD_0']) if 'TEXCOORD_0' in a else None
            mat = g['materials'][p['material']] if 'material' in p else None
            tex = None
            if mat and UV is not None and 'baseColorTexture' in mat['pbrMetallicRoughness']:
                tex = g['images'][mat['pbrMetallicRoughness']['baseColorTexture']['index']]['name']
            self.prims.append(dict(P=P, I=I, UV=UV, tex=tex, blend=bool(mat and mat.get('alphaMode') == 'BLEND')))
        allp = np.concatenate([p['P'] for p in self.prims])
        self.lo, self.hi = allp.min(0), allp.max(0)

    @staticmethod
    def _acc(g, buf, i):
        a = g['accessors'][i]
        bv = g['bufferViews'][a['bufferView']]
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        dt = {5126: '<f4', 5125: '<u4', 5123: '<u2'}[a['componentType']]
        return np.frombuffer(buf, dt, a['count'] * n, bv['byteOffset']).reshape(a['count'], n)


def load_tex(model, name, variant):
    d = model.base / 'textures'
    f = d / ('%s.%s.png' % (name, variant))
    if not f.exists():
        f = d / (name + '.orig.png')
    return np.asarray(Image.open(f).convert('RGBA')).astype(np.float32) / 255


def sample(tex, u, v):
    h, w = tex.shape[:2]
    x = np.mod(u, 1) * w - 0.5
    y = np.mod(v, 1) * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    def g(xx, yy):
        return tex[yy % h, xx % w]
    return (g(x0, y0) * (1 - fx) * (1 - fy) + g(x0 + 1, y0) * fx * (1 - fy) + g(x0, y0 + 1) * (1 - fx) * fy + g(x0 + 1, y0 + 1) * fx * fy)


def render(model, variant, center, scale, size=1024, bg=(0.16, 0.16, 0.18), skip=()):
    """orthographic view from +Z; center=(x,y) world, scale = world units across the image."""
    W = H = size
    img = np.tile(np.array(bg, np.float32), (H, W, 1))
    zb = np.full((H, W), -1e9, np.float32)
    texes = {}
    zsz = float(model.hi[2] - model.lo[2])
    for pr in model.prims:
        if pr['tex'] is None or pr['tex'] in skip:
            continue
        if pr['tex'] not in texes:
            texes[pr['tex']] = load_tex(model, pr['tex'], variant)
    order = sorted(model.prims, key=lambda p: p['blend'])
    for pr in order:
        if pr['tex'] is None:
            continue
        tex = texes[pr['tex']]
        P, I, UV = pr['P'], pr['I'], pr['UV']
        X = (P[:, 0] - center[0]) / scale * W + W / 2
        Y = H / 2 - (P[:, 1] - center[1]) / scale * H
        Z = P[:, 2]
        bias = 0.004 * zsz if pr['blend'] else 0.0
        for t in I:
            x, y, z = X[t], Y[t], Z[t]
            x0, x1 = int(max(np.floor(x.min()), 0)), int(min(np.ceil(x.max()), W - 1))
            y0, y1 = int(max(np.floor(y.min()), 0)), int(min(np.ceil(y.max()), H - 1))
            if x1 < x0 or y1 < y0:
                continue
            d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
            if abs(d) < 1e-9:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            l0 = ((y[1] - y[2]) * (xs - x[2]) + (x[2] - x[1]) * (ys - y[2])) / d
            l1 = ((y[2] - y[0]) * (xs - x[2]) + (x[0] - x[2]) * (ys - y[2])) / d
            l2 = 1 - l0 - l1
            mk = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not mk.any():
                continue
            zz = l0 * z[0] + l1 * z[1] + l2 * z[2] + bias
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            ok = mk & (zz > sub)
            if not ok.any():
                continue
            u = l0 * UV[t[0], 0] + l1 * UV[t[1], 0] + l2 * UV[t[2], 0]
            v = l0 * UV[t[0], 1] + l1 * UV[t[1], 1] + l2 * UV[t[2], 1]
            c = sample(tex, u[ok], v[ok])
            if pr['blend'] or tex[..., 3].min() < 1:
                keep = c[:, 3] > 0.5
                idx = np.where(ok)
                sel = (idx[0][keep], idx[1][keep])
            else:
                sel = np.where(ok)
                keep = np.ones(len(c), bool)
            img[y0:y1 + 1, x0:x1 + 1][sel] = c[keep, :3]
            sub[sel] = zz[sel]
    return (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)
