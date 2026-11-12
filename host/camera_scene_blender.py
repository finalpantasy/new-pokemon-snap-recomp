# SPDX-License-Identifier: MIT
# Blender scene for host/render_camera_icon.py. Reads <out>/camera_dump.txt (mesh dump made locally from the owner's game data)
# and <out>/tex/*.png (decoded textures); writes <out>/camera_r1..r3.png. Contains no game data.
import bpy, sys, os, math, mathutils
out = os.path.abspath(sys.argv[sys.argv.index('--') + 1])
VIEWS = os.environ.get('CAM_VIEWS', 'r1,r2,r3').split(',')
RES = int(os.environ.get('CAM_RES', '1024')); SAMPLES = int(os.environ.get('CAM_SAMPLES', '48'))
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
objs = {}
cur = None
for l in open(os.path.join(out, 'camera_dump.txt')):
    p = l.split()
    if p[0] == 'o':
        cur = p[1] + p[2]; objs[cur] = {'v': [], 'n': [], 't': [], 'f': []}
    else:
        objs[cur][p[0]].append(tuple(float(x) for x in p[1:]) if p[0] != 'f' else tuple(int(x) for x in p[1:]))
MAT = {'Camera_meshShape0': ('CameraA00', 'CameraA00_emi'), 'Camera_meshShape1': ('CameraDisplayA00', 'CameraDisplayA00_emi'),
       'Camera_meshShape2': ('CameraDisplayBackA00', 'CameraDisplayBackA00_emi'),
       'CameraCase_meshShape0': ('CameraCaseA00', None), 'CameraLensCover_meshShape0': ('CameraLensCoverA00', None)}
HIDE = set(os.environ.get('CAM_HIDE', 'CameraCase_meshShape0').split(','))


def drop_strap(o, ymax=0.9):
    # the body mesh also carries the neck strap (connected components that reach above ymax); keep only the camera
    par = list(range(len(o['v'])))

    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]; a = par[a]
        return a
    for f in o['f']:
        par[find(f[0])] = find(f[1]); par[find(f[0])] = find(f[2])
    top = {}
    for i, v in enumerate(o['v']):
        r = find(i); top[r] = max(top.get(r, -9), v[1])
    o['f'] = [f for f in o['f'] if top[find(f[0])] <= ymax]
    used = sorted({i for f in o['f'] for i in f}); m = {a: b for b, a in enumerate(used)}
    o['v'] = [o['v'][i] for i in used]; o['t'] = [o['t'][i] for i in used]
    o['f'] = [tuple(m[i] for i in f) for f in o['f']]



def tex(name):
    suffix = '_emi' if name.endswith('_emi') else '_dif'
    base = name[:-4] if name.endswith('_emi') else name
    return bpy.data.images.load(os.path.join(out, 'tex', '0_' + base + suffix + '.png'))


def material(key):
    d, e = MAT[key]
    m = bpy.data.materials.new(key); m.use_nodes = True
    nt = m.node_tree; b = nt.nodes['Principled BSDF']
    t = nt.nodes.new('ShaderNodeTexImage'); t.image = tex(d); t.extension = 'REPEAT'
    hs = nt.nodes.new('ShaderNodeHueSaturation'); hs.inputs['Saturation'].default_value = float(os.environ.get('CAM_SAT', '1.25'))
    hs.inputs['Value'].default_value = float(os.environ.get('CAM_VAL', '0.9'))
    nt.links.new(t.outputs['Color'], hs.inputs['Color']); nt.links.new(hs.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = 0.3
    if 'Display' not in key:
        b.inputs['Coat Weight'].default_value = 0.55; b.inputs['Coat Roughness'].default_value = 0.04
    if e:
        te = nt.nodes.new('ShaderNodeTexImage'); te.image = tex(e); te.extension = 'REPEAT'
        nt.links.new(te.outputs['Color'], b.inputs['Emission Color']); b.inputs['Emission Strength'].default_value = float(os.environ.get('CAM_EMI', '1.5'))
    return m


made = {}
for k, o in objs.items():
    if k in HIDE:
        continue
    if k == 'Camera_meshShape0' and not os.environ.get('CAM_STRAP'):
        drop_strap(o)
    me = bpy.data.meshes.new(k)
    me.from_pydata(o['v'], [], [f for f in o['f']])
    me.update()
    uv = me.uv_layers.new(name='uv')
    for poly in me.polygons:
        for li, vi in zip(poly.loop_indices, poly.vertices):
            u, v = o['t'][vi]; uv.data[li].uv = (u, 1.0 - v)
    ob = bpy.data.objects.new(k, me); sc.collection.objects.link(ob)
    ob.data.materials.append(material(k)); made[k] = ob
    for p in me.polygons:
        p.use_smooth = True
bpy.context.view_layer.update()
rot = [float(x) for x in os.environ.get('CAM_ROT', '90,0,0').split(',')]
root = bpy.data.objects.new('root', None); sc.collection.objects.link(root)
for ob in made.values():
    ob.parent = root
bb = [mathutils.Vector(c) for ob in made.values() for c in ob.bound_box]
ctr = sum(bb, mathutils.Vector()) / len(bb)
for ob in made.values():
    ob.location -= ctr
root.rotation_euler = [math.radians(x) for x in rot]
bpy.context.view_layer.update()
rad = max((c - ctr).length for c in bb)
sc.render.engine = 'CYCLES'; sc.cycles.samples = SAMPLES; sc.cycles.use_denoising = False
sc.render.film_transparent = True
sc.view_settings.view_transform = 'Standard'; sc.view_settings.look = 'None'; sc.view_settings.exposure = float(os.environ.get('CAM_EXP', '-0.25'))
sc.render.resolution_x = sc.render.resolution_y = RES
sc.render.image_settings.file_format = 'PNG'; sc.render.image_settings.color_mode = 'RGBA'
w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
w.node_tree.nodes['Background'].inputs['Color'].default_value = (0.85, 0.9, 1.0, 1); w.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.45
for name, loc, en in (('key', (2, -3, 3), 300), ('fill', (-3, -2, 1), 110), ('rim', (-1.5, 3, 1.5), 330), ('rim2', (3, 2, 1), 230), ('glint', (-1.2, -3.2, 2.4), 260)):
    ld = bpy.data.lights.new(name, 'AREA'); ld.energy = en * rad * rad * 40; ld.size = (0.5 if name == 'glint' else 3) * rad
    lo = bpy.data.objects.new(name, ld); lo.location = tuple(x * rad * 3 for x in loc); sc.collection.objects.link(lo)
    lo.rotation_euler = (mathutils.Vector((0, 0, 0)) - lo.location).to_track_quat('-Z', 'Y').to_euler()
cam = bpy.data.cameras.new('c'); cam.type = 'ORTHO'; cam.clip_end = 1000
co = bpy.data.objects.new('c', cam); sc.collection.objects.link(co); sc.camera = co
AZ = float(os.environ.get('CAM_AZ', '38'))
EL = float(os.environ.get('CAM_EL', '18')); ROLL = float(os.environ.get('CAM_ROLL', '-6'))
VIEWDEF = {'r1': (AZ, EL, False, ROLL), 'r2': (AZ, EL, True, ROLL), 'r3': (0, 0, False, 0)}  # azimuth, elevation, cap, roll
cap = made.get('CameraLensCover_meshShape0')
SIZES = [int(x) for x in os.environ.get('CAM_SIZES', '').split(',') if x]
if SIZES:   # per-size mode: one render per icon size, tight framing, supersampled
    az, el, capon, roll = VIEWDEF['r1']
    if cap:
        cap.hide_render = True
    d = rad * 6
    co.location = (d * math.sin(math.radians(az)) * math.cos(math.radians(el)), -d * math.cos(math.radians(az)) * math.cos(math.radians(el)), d * math.sin(math.radians(el)))
    co.rotation_euler = (mathutils.Vector((0, 0, 0)) - co.location).to_track_quat('-Z', 'Y').to_euler()
    co.rotation_euler = (co.rotation_euler.to_quaternion() @ mathutils.Quaternion((0, 0, 1), math.radians(roll))).to_euler()
    bpy.context.view_layer.update()
    inv = co.matrix_world.inverted(); xs = []; ys = []
    for ob in made.values():
        if ob.hide_render:
            continue
        for vv in ob.data.vertices:
            q = inv @ (ob.matrix_world @ vv.co); xs.append(q.x); ys.append(q.y)
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    sub = os.path.join(out, 'r1p'); os.makedirs(sub, exist_ok=True)
    for n in SIZES:
        ss = 8 if n <= 64 else 4
        margin = (2.0 if n <= 40 else 2.5 + n / 40.0) / n          # room for the soft shadow, as a fraction per side
        sc.render.resolution_x = sc.render.resolution_y = n * ss
        sc.cycles.samples = 64 if n <= 64 else 96
        cam.ortho_scale = max(x1 - x0, y1 - y0) / (1 - 2 * margin)
        cam.shift_x = ((x0 + x1) / 2) / cam.ortho_scale; cam.shift_y = ((y0 + y1) / 2) / cam.ortho_scale
        sc.render.filepath = os.path.join(sub, f'render_{n}.png')
        bpy.ops.render.render(write_still=True)
    sys.exit(0)
for v in VIEWS:
    az, el, capon, roll = VIEWDEF.get(v, (0, 0, False, 0))
    if cap:
        cap.hide_render = not capon
    d = rad * 6
    co.location = (d * math.sin(math.radians(az)) * math.cos(math.radians(el)), -d * math.cos(math.radians(az)) * math.cos(math.radians(el)), d * math.sin(math.radians(el)))
    co.rotation_euler = (mathutils.Vector((0, 0, 0)) - co.location).to_track_quat('-Z', 'Y').to_euler()
    co.rotation_euler = (co.rotation_euler.to_quaternion() @ mathutils.Quaternion((0, 0, 1), math.radians(roll))).to_euler()
    cam.ortho_scale = rad * 2.2
    tag = os.environ.get('CAM_TAG')
    sc.render.filepath = os.path.join(out, f'camera_{v}.png' if tag is None else f'prev_{tag}_{v}.png')
    bpy.ops.render.render(write_still=True)
