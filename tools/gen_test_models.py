#!/usr/bin/env python3
"""LoadEhr test models, generated (ours, no licence question): writes test_models/gen/<name>/<name>.obj + .mtl.

usage: tools/gen_test_models.py          (idempotent: the same bytes every run; regress / parity / chain tests use them)

  knot     a (2,3) torus knot tube, gold metal            (smooth, ~19k triangles)
  mask     a domed face mask with two see-through eye holes and a nose, ivory  (the eye finder finds the holes)
  crystal  a cluster of seven hexagonal crystals, violet glassy (flat shaded)
  star     a twisted five-point star column, red            (flat shaded)

LoadEhr has no library: these are files a test points a Model slot at by path, like any other file.
"""
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'test_models', 'gen')


def norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / l, v[1] / l, v[2] / l)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


class Mesh:
    def __init__(self):
        self.v, self.n, self.f = [], [], []  # f: list of ((vi, ni), ...) 1-based

    def vert(self, p, n):
        self.v.append(p)
        self.n.append(norm(n))
        return len(self.v)

    def tri(self, a, b, c):
        self.f.append((a, b, c))

    def flat(self, ps):
        """a flat polygon (counter-clockwise seen from outside): its own vertices, one face normal"""
        n = norm(cross(sub(ps[1], ps[0]), sub(ps[2], ps[0])))
        ids = [self.vert(p, n) for p in ps]
        for i in range(1, len(ids) - 1):
            self.tri(ids[0], ids[i], ids[i + 1])

    def write(self, name, mtl):
        d = os.path.join(OUT, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name + '.mtl'), 'w') as m:
            m.write(f'# LoadEhr generated test model ({name})\nnewmtl {name}\n{mtl}\n')
        with open(os.path.join(d, name + '.obj'), 'w') as o:
            o.write(f'# LoadEhr generated test model ({name}), tools/gen_test_models.py\nmtllib {name}.mtl\no {name}\nusemtl {name}\n')
            for p in self.v:
                o.write('v %.6f %.6f %.6f\n' % p)
            for n in self.n:
                o.write('vn %.6f %.6f %.6f\n' % n)
            for a, b, c in self.f:
                o.write(f'f {a}//{a} {b}//{b} {c}//{c}\n')
        return len(self.f)


def knot():
    m = Mesh()
    P, Q, N, S, R, r = 2, 3, 420, 24, 1.0, 0.16

    def curve(t):
        k = R * (2 + math.cos(Q * t))
        return (k * math.cos(P * t), k * math.sin(P * t), R * -math.sin(Q * t))
    idx = []
    for i in range(N):
        t = 2 * math.pi * i / N
        c, c2 = curve(t), curve(t + 1e-3)
        T = norm(sub(c2, c))
        B = norm(cross(T, norm(c)))
        Nn = cross(B, T)
        row = []
        for j in range(S):
            a = 2 * math.pi * j / S
            d = tuple(Nn[k] * math.cos(a) + B[k] * math.sin(a) for k in range(3))
            row.append(m.vert(tuple(c[k] + r * d[k] for k in range(3)), d))
        idx.append(row)
    for i in range(N):
        for j in range(S):
            a, b = idx[i][j], idx[(i + 1) % N][j]
            c, d = idx[(i + 1) % N][(j + 1) % S], idx[i][(j + 1) % S]
            m.tri(a, b, c), m.tri(a, c, d)
    return m.write('knot', 'Kd 1.0 0.62 0.18\nKs 1.0 0.8 0.5\nNs 400\nPm 1.0\nPr 0.25')


def mask():
    """a dome front (z = depth), a back 0.06 behind, walls round the outline and the holes; cells of a 200 x 240 grid"""
    m = Mesh()
    G, H = 200, 240
    x0, x1, y0, y1 = -1.0, 1.0, -1.2, 1.2

    def inside(x, y):
        if x * x + (y / 1.2) ** 2 >= 0.97:
            return False
        for ex in (-0.38, 0.38):  # the eyes: see-through
            if ((x - ex) / 0.19) ** 2 + ((y - 0.22) / 0.11) ** 2 < 1.0:
                return False
        if (x / 0.28) ** 2 + ((y + 0.62) / 0.06) ** 2 < 1.0:  # the mouth slit
            return False
        return True

    def z(x, y):
        h = max(0.0, 1.0 - x * x - (y / 1.2) ** 2)
        return 0.55 * math.sqrt(h) + 0.16 * math.exp(-(x * x / 0.012 + (y + 0.08) ** 2 / 0.05))

    def grad(x, y):
        e = 1e-3
        return ((z(x + e, y) - z(x - e, y)) / (2 * e), (z(x, y + e) - z(x, y - e)) / (2 * e))
    X = [x0 + (x1 - x0) * i / G for i in range(G + 1)]
    Y = [y0 + (y1 - y0) * j / H for j in range(H + 1)]
    cell = [[inside(0.5 * (X[i] + X[i + 1]), 0.5 * (Y[j] + Y[j + 1])) for j in range(H)] for i in range(G)]
    front, back = {}, {}

    def fv(i, j):
        if (i, j) not in front:
            x, y = X[i], Y[j]
            gx, gy = grad(x, y)
            front[(i, j)] = m.vert((x, y, z(x, y)), (-gx, -gy, 1.0))
        return front[(i, j)]

    def bv(i, j):
        if (i, j) not in back:
            x, y = X[i], Y[j]
            gx, gy = grad(x, y)
            back[(i, j)] = m.vert((x, y, z(x, y) - 0.06), (gx, gy, -1.0))
        return back[(i, j)]
    for i in range(G):
        for j in range(H):
            if not cell[i][j]:
                continue
            a, b, c, d = fv(i, j), fv(i + 1, j), fv(i + 1, j + 1), fv(i, j + 1)
            m.tri(a, b, c), m.tri(a, c, d)
            a, b, c, d = bv(i, j), bv(i + 1, j), bv(i + 1, j + 1), bv(i, j + 1)
            m.tri(a, c, b), m.tri(a, d, c)
    # walls: every edge between a solid cell and an empty one
    for i in range(G):
        for j in range(H):
            if not cell[i][j]:
                continue
            for di, dj, e0, e1 in ((-1, 0, (i, j + 1), (i, j)), (1, 0, (i + 1, j), (i + 1, j + 1)),
                                   (0, -1, (i, j), (i + 1, j)), (0, 1, (i + 1, j + 1), (i, j + 1))):
                ni, nj = i + di, j + dj
                if 0 <= ni < G and 0 <= nj < H and cell[ni][nj]:
                    continue
                p0 = (X[e0[0]], Y[e0[1]])
                p1 = (X[e1[0]], Y[e1[1]])
                m.flat([(p0[0], p0[1], z(*p0) - 0.06), (p1[0], p1[1], z(*p1) - 0.06), (p1[0], p1[1], z(*p1)), (p0[0], p0[1], z(*p0))])
    return m.write('mask', 'Kd 0.92 0.88 0.80\nKs 0.3 0.3 0.3\nNs 60\nPm 0.0\nPr 0.45')


def crystal():
    m = Mesh()
    rods = [((0, 0, 0), (0, 1, 0), 1.7, 0.30), ((0.1, 0, 0.05), (0.55, 0.8, 0.1), 1.2, 0.22), ((-0.1, 0, 0), (-0.6, 0.75, 0.2), 1.25, 0.24),
            ((0, 0, 0.1), (0.1, 0.7, 0.7), 1.0, 0.2), ((0, 0, -0.1), (-0.2, 0.75, -0.6), 1.05, 0.21), ((0.1, 0, -0.1), (0.75, 0.45, -0.45), 0.85, 0.17),
            ((-0.1, 0, 0.1), (-0.75, 0.4, 0.5), 0.8, 0.16)]
    for base, axis, length, rad in rods:
        A = norm(axis)
        U = norm(cross(A, (0, 0, 1) if abs(A[2]) < 0.9 else (1, 0, 0)))
        V = cross(A, U)

        def ring(h, rr):
            return [tuple(base[k] + A[k] * h + rr * (U[k] * math.cos(2 * math.pi * s / 6) + V[k] * math.sin(2 * math.pi * s / 6)) for k in range(3)) for s in range(6)]
        lo, hi = ring(-0.2, rad), ring(length, rad)
        tip = tuple(base[k] + A[k] * (length + rad * 1.6) for k in range(3))
        bot = tuple(base[k] - A[k] * (0.2 + rad * 0.5) for k in range(3))
        for s in range(6):
            t = (s + 1) % 6
            m.flat([lo[s], lo[t], hi[t], hi[s]])
            m.flat([hi[s], hi[t], tip])
            m.flat([lo[t], lo[s], bot])
    return m.write('crystal', 'Kd 0.62 0.38 1.0\nKs 0.9 0.9 1.0\nNs 300\nPm 0.0\nPr 0.08')


def star():
    m = Mesh()
    L, n = 48, 10

    def outline(level):
        y = -1.3 + 2.6 * level / L
        tw = 1.4 * (level / L - 0.5)
        sc = 1.0 - 0.25 * abs(level / L - 0.5)
        return [(sc * (0.9 if s % 2 == 0 else 0.4) * math.cos(2 * math.pi * s / n + tw), y, sc * (0.9 if s % 2 == 0 else 0.4) * math.sin(2 * math.pi * s / n + tw)) for s in range(n)]
    rings = [outline(l) for l in range(L + 1)]
    for l in range(L):
        a, b = rings[l], rings[l + 1]
        for s in range(n):
            t = (s + 1) % n
            m.flat([a[t], a[s], b[s]])
            m.flat([a[t], b[s], b[t]])
    top, bot = rings[L], rings[0]
    ct, cb = (0, top[0][1], 0), (0, bot[0][1], 0)
    for s in range(n):
        t = (s + 1) % n
        m.flat([ct, top[t], top[s]])
        m.flat([cb, bot[s], bot[t]])
    return m.write('star', 'Kd 0.95 0.12 0.10\nKs 0.6 0.6 0.6\nNs 120\nPm 0.2\nPr 0.35')


if __name__ == '__main__':
    for f in (knot, mask, crystal, star):
        print(f.__name__, f(), 'triangles ->', os.path.join(OUT, f.__name__))
