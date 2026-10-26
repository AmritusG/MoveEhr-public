#!/usr/bin/env python3
"""MoveEhr W5 (S5): our own test HDRIs (Radiance .hdr, flat RGBE scanlines, "-Y h +X w"), generated, no third-party image.
Writes test_models/gen/hdri/ (gitignored):
  sunset.hdr    2048 x 1024  warm sky gradient, a sun disc (radiance 60) low in front-left, dark ground with a horizon glow
  panels.hdr    1024 x 512   black room, three bright panels: red left (-x), green overhead, blue right (+x), white behind the camera
  checker.hdr   1024 x 512   a grey / white checker room (8 x 4 per turn) with a cyan floor: what a mirror-sharp reflection shows
and sphere.obj (a smooth UV sphere: the mirror ball the reflect / rough gates look at); plus the bad files for the Status gate: bad_text.hdr (not an HDR), bad_trunc.hdr (header + half the pixels), bad_layout.hdr (+Y layout).
Direction convention = the shader's: u = atan(x, -z) / 2pi + 0.5 (u 0.5 = straight ahead, -z), v = 0.5 - asin(y) / pi (row 0 = up).
"""
import math, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'test_models', 'gen', 'hdri')


def rgbe(r, g, b):
    m = max(r, g, b)
    if m < 1e-32:
        return b'\0\0\0\0'
    mant, ex = math.frexp(m)
    s = mant * 256.0 / m
    return bytes((int(r * s), int(g * s), int(b * s), ex + 128))


def write(name, w, h, fn, layout='-Y %d +X %d'):
    rows = []
    for y in range(h):
        v = (y + 0.5) / h
        el = (0.5 - v) * math.pi
        cy, sy = math.cos(el), math.sin(el)
        row = bytearray()
        for x in range(w):
            u = (x + 0.5) / w
            az = (u - 0.5) * 2 * math.pi
            d = (cy * math.sin(az), sy, -cy * math.cos(az))
            row += rgbe(*fn(d, u, v))
        rows.append(bytes(row))
    head = b'#?RADIANCE\n# MoveEhr W5 test HDRI (generated)\nFORMAT=32-bit_rle_rgbe\n\n' + (layout % (h, w)).encode() + b'\n'
    p = os.path.join(OUT, name)
    with open(p, 'wb') as f:
        f.write(head + b''.join(rows))
    return p


def sunset(d, u, v):
    x, y, z = d
    if y > 0:
        t = y ** 0.5
        c = [1.6 * (1 - t) + 0.25 * t, 0.7 * (1 - t) + 0.45 * t, 0.35 * (1 - t) + 1.1 * t]
    else:
        g = math.exp(y * 12)
        c = [0.05 + 0.6 * g, 0.04 + 0.3 * g, 0.03 + 0.12 * g]
    sun = (-0.55, 0.12, -0.83)
    n = math.sqrt(sum(s * s for s in sun))
    cosang = (x * sun[0] + y * sun[1] + z * sun[2]) / n
    if cosang > math.cos(math.radians(2.5)):
        c = [60.0, 42.0, 22.0]
    elif cosang > 0.9:
        k = (cosang - 0.9) / 0.1
        c = [c[0] + 3 * k ** 4, c[1] + 2 * k ** 4, c[2] + 0.8 * k ** 4]
    return c


def panels(d, u, v):
    x, y, z = d
    c = [0.02, 0.02, 0.025]
    if x < -0.8 and abs(y) < 0.45:
        c = [12.0, 0.6, 0.4]
    if y > 0.85:
        c = [0.5, 9.0, 0.8]
    if x > 0.8 and abs(y) < 0.45:
        c = [0.4, 1.0, 14.0]
    if z > 0.85 and abs(y) < 0.4:
        c = [6.0, 6.0, 6.0]
    return c


def checker(d, u, v):
    x, y, z = d
    if y < -0.35:
        return [0.05, 0.6, 0.65]
    k = (int(u * 8) + int(v * 4)) % 2
    return [2.0, 2.0, 2.0] if k else [0.15, 0.15, 0.15]


def sphere_obj(path, nu=96, nv=48):
    """a smooth UV sphere (radius 1, normals): a mirror ball shows which way the HDRI faces"""
    L = ['# MoveEhr W5 test sphere (generated)']
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu + 1):
            ph = 2 * math.pi * i / nu
            x, y, z = math.sin(th) * math.sin(ph), math.cos(th), math.sin(th) * math.cos(ph)
            L.append('v %.6f %.6f %.6f' % (x, y, z))
            L.append('vn %.6f %.6f %.6f' % (x, y, z))
            L.append('vt %.6f %.6f' % (i / nu, 1 - j / nv))
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i + 1
            b, c, d = a + nu + 1, a + nu + 2, a + 1
            L.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (a, a, a, b, b, b, c, c, c))
            L.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (a, a, a, c, c, c, d, d, d))
    open(path, 'w').write('\n'.join(L) + '\n')
    return path


def main():
    os.makedirs(OUT, exist_ok=True)
    print(sphere_obj(os.path.join(OUT, 'sphere.obj')))
    print(write('sunset.hdr', 2048, 1024, sunset))
    print(write('panels.hdr', 1024, 512, panels))
    print(write('checker.hdr', 1024, 512, checker))
    open(os.path.join(OUT, 'bad_text.hdr'), 'w').write('this is not an image\n')
    good = open(os.path.join(OUT, 'panels.hdr'), 'rb').read()
    open(os.path.join(OUT, 'bad_trunc.hdr'), 'wb').write(good[:len(good) // 2])
    print(write('bad_layout.hdr', 64, 32, checker, layout='+Y %d +X %d'))
    print('bad files: bad_text.hdr bad_trunc.hdr bad_layout.hdr')


if __name__ == '__main__':
    main()
