#!/usr/bin/env python3
# MoveEhr W1: our own animated test models (fixtures, not deliverables; no third-party model files). Pure Python, no dependencies.
# Writes binary glTF (.glb) into test_models/gen/anim/ (gitignored), then (unless --no-fbx) the FBX twins through headless
# Blender (Blender only converts our files; nothing of it is vendored).
#   tube.glb     a skinned tube (3-joint chain), clips Bend (linear), Twist (cubic spline), Wave (step)
#   morph.glb    a cube with 2 morph targets (Bulge, Squash), clips Pulse (linear), Steps (step), Smooth (cubic spline)
#   gears.glb    rigid node animation: a static base + 3 gears in a parent chain, clip Spin (linear rotation, translation bob)
#   combo.glb    one skinned tube that also carries a morph target, clip Wave drives both
#   joints1.glb  8 influences per vertex (JOINTS_0/1 + WEIGHTS_0/1) and a second uv set, clip Bend
#   static.glb   the gears scene with NO animation (the static path: must load byte-identical to LoadEhr; its FBX twin carries
#                Blender's default take of constant keys, the 'empty take' case)
# usage: tools/gen_anim_models.py [--no-fbx] [--dense]   (--dense: dense_31k.glb / dense_719k.glb, the pose-pass cost fixtures, W1 step 6)
import json, math, os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'test_models', 'gen', 'anim')
BLENDER = '/Applications/Blender.app/Contents/MacOS/Blender'

FLOAT, USHORT, UINT, UBYTE = 5126, 5123, 5125, 5121
ARRAY, ELEMENT = 34962, 34963


class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.j = {'asset': {'version': '2.0', 'generator': 'MoveEhr tools/gen_anim_models.py'}, 'scene': 0, 'scenes': [{'nodes': []}],
                  'nodes': [], 'meshes': [], 'buffers': [], 'bufferViews': [], 'accessors': [], 'materials': []}

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            v['target'] = target
        self.bin += data
        self.j['bufferViews'].append(v)
        return len(self.j['bufferViews']) - 1

    def acc(self, values, ctype, typ, target=None, minmax=False):
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[typ]
        fmt = {FLOAT: 'f', USHORT: 'H', UINT: 'I', UBYTE: 'B'}[ctype]
        flat = [x for v in values for x in (v if isinstance(v, (list, tuple)) else [v])]
        data = struct.pack('<%d%s' % (len(flat), fmt), *flat)
        a = {'bufferView': self.view(data, target), 'componentType': ctype, 'count': len(flat) // n, 'type': typ}
        if minmax:
            rows = [flat[i:i + n] for i in range(0, len(flat), n)]
            a['min'] = [min(r[k] for r in rows) for k in range(n)]
            a['max'] = [max(r[k] for r in rows) for k in range(n)]
        self.j['accessors'].append(a)
        return len(self.j['accessors']) - 1

    def material(self, name, rgba, metal=0.0, rough=0.5):
        self.j['materials'].append({'name': name, 'pbrMetallicRoughness': {'baseColorFactor': rgba, 'metallicFactor': metal, 'roughnessFactor': rough}})
        return len(self.j['materials']) - 1

    def node(self, **kw):
        self.j['nodes'].append(kw)
        return len(self.j['nodes']) - 1

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        for k in [k for k, v in self.j.items() if v == []]:
            del self.j[k]
        js = json.dumps(self.j, separators=(',', ':')).encode()
        while len(js) % 4:
            js += b' '
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, total))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


def quat_axis(axis, deg):
    a = math.radians(deg) / 2
    s = math.sin(a)
    l = math.sqrt(sum(c * c for c in axis))
    return [axis[0] / l * s, axis[1] / l * s, axis[2] / l * s, math.cos(a)]


def tube(rings=24, seg=24, height=2.0, radius=0.25):
    """A capped tube along +Y from 0 to height. Returns pos, nrm, uv, idx and the height of every vertex."""
    pos, nrm, uv, idx, hgt = [], [], [], [], []
    for r in range(rings + 1):
        y = height * r / rings
        for s in range(seg + 1):
            a = 2 * math.pi * s / seg
            pos.append([radius * math.cos(a), y, radius * math.sin(a)])
            nrm.append([math.cos(a), 0.0, math.sin(a)])
            uv.append([s / seg, 1.0 - r / rings])
            hgt.append(y)
    for r in range(rings):
        for s in range(seg):
            a = r * (seg + 1) + s
            b = a + seg + 1
            idx += [a, b, a + 1, a + 1, b, b + 1]
    for y, ny in ((0.0, -1.0), (height, 1.0)):
        c = len(pos)
        pos.append([0.0, y, 0.0]); nrm.append([0.0, ny, 0.0]); uv.append([0.5, 0.5]); hgt.append(y)
        for s in range(seg + 1):
            a = 2 * math.pi * s / seg
            pos.append([radius * math.cos(a), y, radius * math.sin(a)]); nrm.append([0.0, ny, 0.0])
            uv.append([0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)]); hgt.append(y)
        for s in range(seg):
            idx += [c, c + 2 + s, c + 1 + s] if ny > 0 else [c, c + 1 + s, c + 2 + s]
    return pos, nrm, uv, idx, hgt


def chain_weights(hgt, joint_y):
    """Smooth blend between the two nearest joints along the chain (4 slots, 2 used)."""
    J, W = [], []
    nj = len(joint_y)
    for y in hgt:
        k = 0
        while k + 1 < nj and y > joint_y[k + 1]:
            k += 1
        if k + 1 >= nj:
            J.append([k, 0, 0, 0]); W.append([1.0, 0.0, 0.0, 0.0]); continue
        t = max(0.0, min(1.0, (y - joint_y[k]) / (joint_y[k + 1] - joint_y[k])))
        t = t * t * (3 - 2 * t)
        J.append([k, k + 1, 0, 0]); W.append([1.0 - t, t, 0.0, 0.0])
    return J, W


def add_skinned_tube(g, name, mat, morph_targets=None, extra_influences=False, second_uv=False, rings=24, seg=24):
    pos, nrm, uv, idx, hgt = tube(rings, seg)
    joint_y = [0.0, 0.7, 1.4]
    root = g.node(name=name + '_root', translation=[0.0, -1.0, 0.0])
    j0 = g.node(name=name + '_j0', translation=[0.0, 0.0, 0.0])
    j1 = g.node(name=name + '_j1', translation=[0.0, 0.7, 0.0])
    j2 = g.node(name=name + '_j2', translation=[0.0, 0.7, 0.0])
    g.j['nodes'][root]['children'] = [j0]
    g.j['nodes'][j0]['children'] = [j1]
    g.j['nodes'][j1]['children'] = [j2]
    # the mesh sits at world y - 1 (positions shifted below), the joints at world y = joint_y - 1: inverse bind = translate +(1 - y)
    ibm = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0, 1.0 - y, 0.0, 1] for y in joint_y]
    attrs = {'POSITION': g.acc([[p[0], p[1] - 1.0, p[2]] for p in pos], FLOAT, 'VEC3', ARRAY, minmax=True),
             'NORMAL': g.acc(nrm, FLOAT, 'VEC3', ARRAY), 'TEXCOORD_0': g.acc(uv, FLOAT, 'VEC2', ARRAY)}
    J, W = chain_weights(hgt, joint_y)
    if extra_influences:
        # 8 influences: the real two (x 0.94) + six tiny ones (0.01 each) over all joints (sum 1): the loader keeps the top 4
        J8 = [[j[0], j[1], 0, 1, 2, 0, 1, 2] for j in J]
        W8 = [[w[0] * 0.94, w[1] * 0.94] + [0.01] * 6 for w in W]
        attrs['JOINTS_0'] = g.acc([j[:4] for j in J8], USHORT, 'VEC4', ARRAY)
        attrs['WEIGHTS_0'] = g.acc([w[:4] for w in W8], FLOAT, 'VEC4', ARRAY)
        attrs['JOINTS_1'] = g.acc([j[4:] for j in J8], USHORT, 'VEC4', ARRAY)
        attrs['WEIGHTS_1'] = g.acc([w[4:] for w in W8], FLOAT, 'VEC4', ARRAY)
    else:
        attrs['JOINTS_0'] = g.acc(J, UBYTE, 'VEC4', ARRAY)
        attrs['WEIGHTS_0'] = g.acc(W, FLOAT, 'VEC4', ARRAY)
    if second_uv:
        attrs['TEXCOORD_1'] = g.acc([[u[1], u[0]] for u in uv], FLOAT, 'VEC2', ARRAY)
    prim = {'attributes': attrs, 'indices': g.acc(idx, UINT, 'SCALAR', ELEMENT), 'material': mat}
    mesh = {'name': name, 'primitives': [prim]}
    if morph_targets:
        prim['targets'] = []
        for tname, fn in morph_targets:
            d = [fn(p, n, h) for p, n, h in zip(pos, nrm, hgt)]
            prim['targets'].append({'POSITION': g.acc(d, FLOAT, 'VEC3', None, minmax=True)})
        mesh['weights'] = [0.0] * len(morph_targets)
        mesh['extras'] = {'targetNames': [t[0] for t in morph_targets]}
    g.j['meshes'].append(mesh)
    g.j.setdefault('skins', []).append({'joints': [j0, j1, j2], 'inverseBindMatrices': g.acc(ibm, FLOAT, 'MAT4'), 'skeleton': root})
    meshnode = g.node(name=name + '_mesh', mesh=len(g.j['meshes']) - 1, skin=len(g.j['skins']) - 1)
    g.j['scenes'][0]['nodes'] += [root, meshnode]
    return {'root': root, 'j': [j0, j1, j2], 'mesh': meshnode}


def add_anim(g, name, channels):
    """channels: (node, path, interp, times, values); cubic: [in, value, out] per key."""
    samplers, chans = [], []
    for node, path, interp, times, values in channels:
        typ = {'rotation': 'VEC4', 'translation': 'VEC3', 'scale': 'VEC3', 'weights': 'SCALAR'}[path]
        samplers.append({'input': g.acc(times, FLOAT, 'SCALAR', None, minmax=True), 'output': g.acc(values, FLOAT, typ), 'interpolation': interp})
        chans.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path}})
    g.j.setdefault('animations', []).append({'name': name, 'samplers': samplers, 'channels': chans})


def cubic_keys(vals, tangents=None):
    """CUBICSPLINE output: [in-tangent, value, out-tangent] per key (zero tangents unless given)."""
    out = []
    for i, v in enumerate(vals):
        t = tangents[i] if tangents else ([0.0] * len(v) if isinstance(v, list) else 0.0)
        out += [t, v, t]
    return out


def gen_tube(path):
    g = Gltf()
    t = add_skinned_tube(g, 'Tube', g.material('Tube', [0.85, 0.35, 0.2, 1.0], 0.0, 0.4))
    j0, j1, j2 = t['j']
    times = [0.0, 0.5, 1.0, 1.5, 2.0]
    bend = [quat_axis([0, 0, 1], a) for a in (0, 35, 0, -35, 0)]
    add_anim(g, 'Bend', [(j1, 'rotation', 'LINEAR', times, bend), (j2, 'rotation', 'LINEAR', times, bend)])
    tw = [quat_axis([0, 1, 0], a) for a in (0, 60, 0, -60, 0)]
    # tangents on the angle's slope (d(sin(a/2))/dt etc. approximated per key) so the spline is not flat at every key
    tan = [[0.0, 0.5 * math.cos(math.radians(a) / 2) * math.radians(120) * 0.5, 0.0, -0.5 * math.sin(math.radians(a) / 2) * math.radians(120) * 0.5] for a in (0, 60, 0, -60, 0)]
    add_anim(g, 'Twist', [(j1, 'rotation', 'CUBICSPLINE', times, cubic_keys(tw, tan)), (j2, 'rotation', 'CUBICSPLINE', times, cubic_keys(tw, tan))])
    st = [[0.0, 0.7 + d, 0.0] for d in (0.0, 0.15, 0.0, -0.1)]
    add_anim(g, 'Wave', [(j2, 'translation', 'STEP', [0.0, 0.25, 0.5, 0.75], st),
                         (j1, 'rotation', 'STEP', [0.0, 0.5], [quat_axis([1, 0, 0], 20), quat_axis([1, 0, 0], -20)])])
    g.write(path)


def cube_mesh(size=1.0, n=8):
    pos, nrm, uv, idx = [], [], [], []
    faces = [([1, 0, 0], [0, 1, 0], [0, 0, -1]), ([-1, 0, 0], [0, 1, 0], [0, 0, 1]), ([0, 1, 0], [0, 0, -1], [1, 0, 0]),
             ([0, -1, 0], [0, 0, 1], [1, 0, 0]), ([0, 0, 1], [0, 1, 0], [1, 0, 0]), ([0, 0, -1], [0, 1, 0], [-1, 0, 0])]
    h = size / 2
    for N, U, R in faces:
        base = len(pos)
        for i in range(n + 1):
            for k in range(n + 1):
                a, b = -1 + 2 * k / n, -1 + 2 * i / n
                pos.append([h * (N[c] + a * R[c] + b * U[c]) for c in range(3)]); nrm.append([float(x) for x in N]); uv.append([k / n, 1 - i / n])
        for i in range(n):
            for k in range(n):
                a = base + i * (n + 1) + k
                idx += [a, a + 1, a + n + 1, a + 1, a + n + 2, a + n + 1]
    return pos, nrm, uv, idx


def gen_morph(path):
    g = Gltf()
    m = g.material('Cube', [0.2, 0.55, 0.9, 1.0], 0.1, 0.35)
    pos, nrm, uv, idx = cube_mesh(1.4)
    bulge = [[p[c] * 0.45 * (1 - abs(p[1]) / 0.7) for c in range(3)] for p in pos]
    squash = [[p[0] * 0.3, -p[1] * 0.45, p[2] * 0.3] for p in pos]
    prim = {'attributes': {'POSITION': g.acc(pos, FLOAT, 'VEC3', ARRAY, minmax=True), 'NORMAL': g.acc(nrm, FLOAT, 'VEC3', ARRAY),
                           'TEXCOORD_0': g.acc(uv, FLOAT, 'VEC2', ARRAY)},
            'indices': g.acc(idx, UINT, 'SCALAR', ELEMENT), 'material': m,
            'targets': [{'POSITION': g.acc(bulge, FLOAT, 'VEC3', None, minmax=True)}, {'POSITION': g.acc(squash, FLOAT, 'VEC3', None, minmax=True)}]}
    g.j['meshes'].append({'name': 'Cube', 'primitives': [prim], 'weights': [0.0, 0.0], 'extras': {'targetNames': ['Bulge', 'Squash']}})
    cube = g.node(name='Cube', mesh=0)
    g.j['scenes'][0]['nodes'] = [cube]
    add_anim(g, 'Pulse', [(cube, 'weights', 'LINEAR', [0.0, 0.5, 1.0, 1.5, 2.0], [0, 0, 1, 0, 0, 1, 0, 0, 0, 0])])  # 2 weights per key
    add_anim(g, 'Steps', [(cube, 'weights', 'STEP', [0.0, 0.5, 1.0, 1.5], [0, 0, 1, 0, 0, 1, 1, 1])])
    add_anim(g, 'Smooth', [(cube, 'weights', 'CUBICSPLINE', [0.0, 1.0, 2.0],
                            [0, 0, 0, 0, 0, 0,  1.5, 0.5, 1, 0.5, -1.5, -0.5,  0, 0, 0, 0, 0, 0])])  # per key: in(2) value(2) out(2)
    g.write(path)


def gear(teeth=10, r=0.45, depth=0.18, tooth=0.08):
    pos, nrm, uv, idx = [], [], [], []
    n = teeth * 4
    ring = []
    for s in range(n):
        a = 2 * math.pi * s / n
        rr = r + (tooth if (s % 4) in (1, 2) else 0.0)
        ring.append((rr * math.cos(a), rr * math.sin(a)))
    for z, nz in ((depth / 2, 1.0), (-depth / 2, -1.0)):
        c = len(pos)
        pos.append([0.0, 0.0, z]); nrm.append([0, 0, nz]); uv.append([0.5, 0.5])
        for x, y in ring:
            pos.append([x, y, z]); nrm.append([0, 0, nz]); uv.append([0.5 + x, 0.5 - y])
        for s in range(n):
            a, b = c + 1 + s, c + 1 + (s + 1) % n
            idx += [c, a, b] if nz > 0 else [c, b, a]
    for s in range(n):
        (x0, y0), (x1, y1) = ring[s], ring[(s + 1) % n]
        l = math.hypot(x1 - x0, y1 - y0) or 1
        nn = [(y1 - y0) / l, -(x1 - x0) / l, 0.0]
        b = len(pos)
        for x, y, z in ((x0, y0, depth / 2), (x1, y1, depth / 2), (x0, y0, -depth / 2), (x1, y1, -depth / 2)):
            pos.append([x, y, z]); nrm.append(nn); uv.append([s / n, 0.0 if z > 0 else 1.0])
        idx += [b, b + 2, b + 1, b + 1, b + 2, b + 3]
    return pos, nrm, uv, idx


def gen_gears(path):
    g = Gltf()
    mats = [g.material('Brass', [0.85, 0.65, 0.3, 1.0], 1.0, 0.3), g.material('Steel', [0.7, 0.72, 0.75, 1.0], 1.0, 0.25),
            g.material('Base', [0.25, 0.25, 0.28, 1.0], 0.0, 0.7)]
    gp, gn, gu, gi = gear()
    acc = {'POSITION': g.acc(gp, FLOAT, 'VEC3', ARRAY, minmax=True), 'NORMAL': g.acc(gn, FLOAT, 'VEC3', ARRAY), 'TEXCOORD_0': g.acc(gu, FLOAT, 'VEC2', ARRAY)}
    gidx = g.acc(gi, UINT, 'SCALAR', ELEMENT)
    g.j['meshes'].append({'name': 'GearA', 'primitives': [{'attributes': acc, 'indices': gidx, 'material': mats[0]}]})
    g.j['meshes'].append({'name': 'GearB', 'primitives': [{'attributes': acc, 'indices': gidx, 'material': mats[1]}]})
    bp, bn, bu, bi = cube_mesh(1.0, 2)
    bp = [[p[0] * 2.4, p[1] * 0.15 - 0.85, p[2] * 0.8] for p in bp]
    g.j['meshes'].append({'name': 'Base', 'primitives': [{'attributes': {'POSITION': g.acc(bp, FLOAT, 'VEC3', ARRAY, minmax=True), 'NORMAL': g.acc(bn, FLOAT, 'VEC3', ARRAY),
                                                                         'TEXCOORD_0': g.acc(bu, FLOAT, 'VEC2', ARRAY)},
                                                          'indices': g.acc(bi, UINT, 'SCALAR', ELEMENT), 'material': mats[2]}]})
    base = g.node(name='Base', mesh=2)
    g1 = g.node(name='Gear1', mesh=0, translation=[-0.8, 0.0, 0.0])
    g2 = g.node(name='Gear2', mesh=1, translation=[0.98, 0.0, 0.0], scale=[0.9, 0.9, 0.9])
    g3 = g.node(name='Gear3', mesh=0, translation=[0.9, 0.35, 0.05], scale=[0.7, 0.7, 0.7])
    g.j['nodes'][g1]['children'] = [g2]
    g.j['nodes'][g2]['children'] = [g3]
    g.j['scenes'][0]['nodes'] = [base, g1]
    t = [0.0, 1.0, 2.0, 3.0, 4.0]
    spin = lambda k: [quat_axis([0, 0, 1], k * a) for a in (0, 90, 180, 270, 360)]
    add_anim(g, 'Spin', [(g1, 'rotation', 'LINEAR', t, spin(1)), (g2, 'rotation', 'LINEAR', t, spin(-1)), (g3, 'rotation', 'LINEAR', t, spin(1)),
                         (g1, 'translation', 'LINEAR', t, [[-0.8, 0.0, 0.0], [-0.8, 0.12, 0.0], [-0.8, 0.0, 0.0], [-0.8, -0.12, 0.0], [-0.8, 0.0, 0.0]])])
    g.write(path)


def gen_combo(path):
    g = Gltf()
    t = add_skinned_tube(g, 'Combo', g.material('Combo', [0.4, 0.8, 0.35, 1.0], 0.0, 0.45),
                         morph_targets=[('Bulge', lambda p, n, h: [n[0] * 0.18 * math.sin(math.pi * h / 2.0), 0.0, n[2] * 0.18 * math.sin(math.pi * h / 2.0)])])
    j0, j1, j2 = t['j']
    times = [0.0, 0.5, 1.0, 1.5, 2.0]
    add_anim(g, 'Wave', [(j1, 'rotation', 'LINEAR', times, [quat_axis([1, 0, 0], a) for a in (0, 30, 0, -30, 0)]),
                         (t['mesh'], 'weights', 'LINEAR', times, [0.0, 1.0, 0.0, 1.0, 0.0])])
    g.write(path)


def gen_joints1(path):
    g = Gltf()
    t = add_skinned_tube(g, 'Eight', g.material('Eight', [0.8, 0.8, 0.3, 1.0], 0.0, 0.5), extra_influences=True, second_uv=True)
    add_anim(g, 'Bend', [(t['j'][1], 'rotation', 'LINEAR', [0.0, 1.0, 2.0], [quat_axis([0, 0, 1], a) for a in (0, 45, 0)])])
    g.write(path)


def gen_static(path):
    gen_gears(path)
    # rewrite without the animation: parse the GLB back, drop 'animations', write again
    with open(path, 'rb') as f:
        data = f.read()
    jl = struct.unpack('<I', data[12:16])[0]
    j = json.loads(data[20:20 + jl])
    j.pop('animations', None)
    binchunk = data[20 + jl:]
    js = json.dumps(j, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + len(binchunk)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A) + js + binchunk)


def gen_dense(path, rings, seg):
    g = Gltf()
    t = add_skinned_tube(g, 'Dense', g.material('Dense', [0.85, 0.35, 0.2, 1.0], 0.0, 0.4), rings=rings, seg=seg)
    times = [0.0, 0.5, 1.0, 1.5, 2.0]
    bend = [quat_axis([0, 0, 1], a) for a in (0, 35, 0, -35, 0)]
    add_anim(g, 'Bend', [(t['j'][1], 'rotation', 'LINEAR', times, bend), (t['j'][2], 'rotation', 'LINEAR', times, bend)])
    g.write(path)


FBX_SCRIPT = r'''
import bpy, sys
src, dst = sys.argv[sys.argv.index('--') + 1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
bpy.ops.export_scene.fbx(filepath=dst, use_selection=False, bake_anim=True, bake_anim_use_all_actions=True, bake_anim_use_nla_strips=False,
                         add_leaf_bones=False, axis_forward='-Z', axis_up='Y', object_types={'ARMATURE', 'MESH', 'EMPTY'})
'''


def main():
    os.makedirs(OUT, exist_ok=True)
    gens = [('tube', gen_tube), ('morph', gen_morph), ('gears', gen_gears), ('combo', gen_combo), ('joints1', gen_joints1), ('static', gen_static)]
    for name, fn in gens:
        p = os.path.join(OUT, name + '.glb')
        fn(p)
        print('wrote', p, os.path.getsize(p), 'bytes')
    if '--dense' in sys.argv:
        for name, r, sg in (('dense_31k', 175, 175), ('dense_719k', 847, 847)):
            p = os.path.join(OUT, name + '.glb')
            gen_dense(p, r, sg)
            print('wrote', p, os.path.getsize(p), 'bytes')
        return
    if '--no-fbx' in sys.argv:
        return
    if not os.path.exists(BLENDER):
        print('no Blender at', BLENDER, ': FBX twins skipped')
        return
    script = os.path.join(OUT, '_glb2fbx.py')
    with open(script, 'w') as f:
        f.write(FBX_SCRIPT)
    for name, _ in gens:
        src, dst = os.path.join(OUT, name + '.glb'), os.path.join(OUT, name + '.fbx')
        r = subprocess.run([BLENDER, '-b', '--factory-startup', '--python', script, '--', src, dst], capture_output=True, text=True)
        ok = os.path.exists(dst) and r.returncode == 0
        print('wrote' if ok else 'FAILED', dst, os.path.getsize(dst) if ok else r.stderr[-600:])


if __name__ == '__main__':
    main()
