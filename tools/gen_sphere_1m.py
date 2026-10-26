#!/usr/bin/env python3
"""The 1M-vertex point-cloud test sphere (ours): writes test_models/gen/sphere_1m.glb (~48 MB, so it is generated, not committed).
A UV sphere, 1003 rings x 1000 segments (+ a seam column): ~1.0M distinct positions, as a GLB (positions, normals, uint32 indices).
usage: tools/gen_sphere_1m.py [out.glb]"""
import json, math, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'test_models', 'gen', 'sphere_1m.glb')

def make():
    # a UV sphere, 1003 rings x 1000 segments (+ a seam column): ~1.0M distinct positions, as a GLB (positions, normals, uint32 indices)
    rings, segs = 1003, 1000
    pos, idx = bytearray(), bytearray()
    nv = 0
    for i in range(rings):
        th = math.pi * (i + 0.5) / rings
        st, ct = math.sin(th), math.cos(th)
        for j in range(segs + 1):
            ph = 2 * math.pi * j / segs
            pos += struct.pack('<3f', st * math.cos(ph), ct, st * math.sin(ph))
            nv += 1
    for i in range(rings - 1):
        for j in range(segs):
            a, b = i * (segs + 1) + j, (i + 1) * (segs + 1) + j
            idx += struct.pack('<6I', a, b, a + 1, a + 1, b, b + 1)
    nrm = pos  # unit sphere: the normal is the position
    bins = bytes(pos) + bytes(nrm) + bytes(idx)
    gl = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
          'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1}, 'indices': 2}]}],
          'buffers': [{'byteLength': len(bins)}],
          'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': len(pos)}, {'buffer': 0, 'byteOffset': len(pos), 'byteLength': len(nrm)},
                          {'buffer': 0, 'byteOffset': 2 * len(pos), 'byteLength': len(idx)}],
          'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': nv, 'type': 'VEC3', 'min': [-1, -1, -1], 'max': [1, 1, 1]},
                        {'bufferView': 1, 'componentType': 5126, 'count': nv, 'type': 'VEC3'},
                        {'bufferView': 2, 'componentType': 5125, 'count': len(idx) // 4, 'type': 'SCALAR'}]}
    js = json.dumps(gl).encode()
    js += b' ' * ((4 - len(js) % 4) % 4)
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bins)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(bins), 0x004E4942) + bins
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'wb').write(out)
    print('make: %s (%d vertices, %d triangles, %.1f MB)' % (os.path.relpath(OUT, ROOT), nv, len(idx) // 12, len(out) / 1e6))
    return True


if __name__ == "__main__":
    make()
