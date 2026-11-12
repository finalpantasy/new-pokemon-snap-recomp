"""Generate a deterministic, game-free v1 test cache using only Python stdlib.
The glTF exporter writes a lathed vessel and an alpha-cut calibration card.
"""
import json
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent / 'testdata' / 'cache'
EXT = ROOT.parent / 'external'  # mirrors build/hd/external next to build/hd/cache


def png(path, size, pixel):
    path.parent.mkdir(parents=True, exist_ok=True)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + bytes(c for x in range(size) for c in pixel(x / size, y / size)) for y in range(size))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def albedo(u, v, detailed=False, warm=False):
    line = (int(u * 16) + int(v * 12)) % 2
    wave = .5 + .5 * math.sin(u * math.tau * 8 + math.sin(v * math.tau * 4))
    base = (35, 136, 146) if not warm else (181, 106, 59)
    band = abs((v * 6) % 1 - .5) < .045
    if band: return (224, 194, 127, 255)
    factor = .74 + .2 * wave + .12 * line
    if detailed: factor += .09 * math.sin(u * math.tau * 90) * math.cos(v * math.tau * 70)
    return tuple(min(255, int(c * factor)) for c in base) + (255,)


def normal(u, v, detailed=False):
    amplitude = 18 if detailed else 10
    return (int(128 + amplitude * math.cos(u * math.tau * 24)), int(128 + amplitude * math.sin(v * math.tau * 20)), 251, 255)


def alpha(u, v, detailed=False):
    radius = math.hypot((u - .5) * 2, (v - .5) * 2)
    holes = abs(math.sin(u * math.tau * 3) * math.sin(v * math.tau * 3)) > .82
    return (230, 185, 106, 0 if radius > .93 or holes else 255)


def mesh(path, segments, rows, texture_uri=None):
    binary = bytearray()
    views, accessors, primitives = [], [], []
    def attribute(values, components, component_type, kind, bounds=False):
        while len(binary) % 4: binary.append(0)
        start = len(binary)
        code = 'f' if component_type == 5126 else 'I'
        flat = [n for row in values for n in row] if components > 1 else values
        binary.extend(struct.pack('<' + code * len(flat), *flat))
        views.append({'buffer': 0, 'byteOffset': start, 'byteLength': len(binary) - start})
        accessor = {'bufferView': len(views) - 1, 'componentType': component_type, 'count': len(values), 'type': kind}
        if bounds:
            accessor['min'] = [min(v[c] for v in values) for c in range(components)]
            accessor['max'] = [max(v[c] for v in values) for c in range(components)]
        accessors.append(accessor)
        return len(accessors) - 1
    def primitive(positions, normals, uv, indices, material):
        primitives.append({'attributes': {'POSITION': attribute(positions, 3, 5126, 'VEC3', True), 'NORMAL': attribute(normals, 3, 5126, 'VEC3'), 'TEXCOORD_0': attribute(uv, 2, 5126, 'VEC2')}, 'indices': attribute(indices, 1, 5125, 'SCALAR'), 'material': material})
    def radius(t): return .46 + .51 * math.sin(math.pi * t) ** 1.3 + .09 * math.cos(t * math.pi * 6)
    positions, normals, uv, indices = [], [], [], []
    for y in range(rows + 1):
        t = y / rows
        derivative = (radius(min(1, t + .0001)) - radius(max(0, t - .0001))) / (min(1, t + .0001) - max(0, t - .0001)) / 2.5
        for x in range(segments + 1):
            u = x / segments
            angle = u * math.tau
            r = radius(t)
            positions.append([r * math.cos(angle), (t - .5) * 2.5, r * math.sin(angle)])
            length = math.sqrt(1 + derivative * derivative)
            normals.append([math.cos(angle) / length, -derivative / length, math.sin(angle) / length])
            uv.append([u, 1 - t])
    for y in range(rows):
        for x in range(segments):
            a = y * (segments + 1) + x
            b = a + segments + 1
            indices.extend([a, b, a + 1, a + 1, b, b + 1])
    primitive(positions, normals, uv, indices, 0)
    primitive([[-.34,-.42,1.03],[.34,-.42,1.03],[.34,.42,1.03],[-.34,.42,1.03]], [[0,0,1]]*4, [[0,1],[1,1],[1,0],[0,0]], [0,1,2,0,2,3], 1)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {'asset': {'version': '2.0', 'generator': 'Asset Viewer procedural fixture exporter'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0, 'name': 'Procedural calibration vessel'}], 'meshes': [{'primitives': primitives}], 'materials': [{'name': 'ceramic'}, {'name': 'cutout', 'doubleSided': True}], 'buffers': [{'uri': path.with_suffix('.bin').name, 'byteLength': len(binary)}], 'bufferViews': views, 'accessors': accessors}
    if texture_uri:  # third-party style model: carries its own texture
        data['materials'] = [{'name': 'alt_body', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0}}, {'name': 'alt_card', 'doubleSided': True}]
        data['images'] = [{'uri': texture_uri}]
        data['textures'] = [{'source': 0}]
    path.write_text(json.dumps(data, indent=2), encoding='utf-8')
    path.with_suffix('.bin').write_bytes(binary)


def main():
    folder = ROOT / 'contour_vessel'
    texture_defs = [('ceramic_color','albedo',albedo),('ceramic_normal','normal',normal),('cutout_color','albedo',alpha)]
    textures = []
    for name, role, pixel in texture_defs:
        variants = {}
        for suffix, size in [('orig',64),('x4_detail',256),('x4_warm',256)]:
            path = folder / 'textures' / f'{name}.{suffix}.png'
            if name == 'ceramic_color': callback = lambda u,v,s=suffix: albedo(u,v,s!='orig',s=='x4_warm')
            else: callback = lambda u,v,p=pixel,s=suffix: p(u,v,s!='orig')
            png(path, size, callback)
            if suffix != 'orig': variants[suffix] = path.relative_to(ROOT).as_posix()
        textures.append({'name':name,'role':role,'orig':f'contour_vessel/textures/{name}.orig.png','w':64,'h':64,'format':'RGBA8','variants':variants})
    mesh(folder/'model.gltf',64,40)
    mesh(folder/'model_lod1.gltf',16,12)
    alt = EXT / 'contour_vessel' / 'alt'
    png(alt / 'alt_body.png', 32, lambda u, v: albedo(v, u, False, True))
    mesh(alt / 'model.gltf', 12, 8, 'alt_body.png')
    (ROOT / 'alt_models.json').write_text(json.dumps({'version': 1, 'alt_models': [
        {'asset_id': 'Contour Vessel', 'id': 'ext_demo', 'label': 'Synthetic low-poly (external)', 'model': '../external/contour_vessel/alt/model.gltf'},
        {'asset_id': 'no_such_asset', 'id': 'orphan', 'label': 'Ignored', 'model': '../external/none.gltf'}]}, indent=2), encoding='utf-8')
    materials = [{'name':'ceramic','albedo':'ceramic_color','normal':'ceramic_normal','mask':None,'emissive':None},{'name':'cutout','albedo':'cutout_color','normal':None,'mask':None,'emissive':None}]
    asset = {'id':'contour_vessel','name':'Contour vessel','category':'prop','model':'contour_vessel/model.gltf','lods':['contour_vessel/model.gltf','contour_vessel/model_lod1.gltf'],'materials':materials,'textures':textures}
    (folder/'info.json').write_text(json.dumps({'lods':asset['lods'],'materials':materials},indent=2),encoding='utf-8')
    manifest = {'version':1,'variants':[{'id':'x4_detail','label':'Procedural detail ×4','scale':4},{'id':'x4_warm','label':'Warm study ×4','scale':4}],'assets':[asset,{'id':'alpha_card','name':'Alpha calibration','category':'ui','model':None,'lods':[],'materials':[],'textures':[textures[2]]}]}
    (ROOT/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(f'Generated synthetic cache: {ROOT}')


if __name__ == '__main__': main()
