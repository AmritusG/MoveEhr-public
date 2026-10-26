#!/usr/bin/env python3
# W21: writes the generated test emblem (ours, not Amrit's) as SVG and PDF from ONE geometry, then the PNG from the PDF (sips).
# Emblem: a gold ring (even-odd: outer + inner circle) and a red five-point star with a round hole in its centre.
import math, os, subprocess, sys
OUT = sys.argv[1] if len(sys.argv) > 1 else 'test_logos'
S = 512.0
K = 0.5522847498

def circle(cx, cy, r):  # 4 cubics, y down (SVG space); list of ('M',x,y) / ('C',...) / ('Z',)
    pts = [(cx + r, cy), (cx, cy + r), (cx - r, cy), (cx, cy - r)]
    out = [('M',) + pts[0]]
    for i in range(4):
        a, b = pts[i], pts[(i + 1) % 4]
        # tangent directions for a circle traversed through +90 deg steps
        ta = (-(a[1] - cy), a[0] - cx); tb = ((b[1] - cy), -(b[0] - cx))
        out.append(('C', a[0] + K * ta[0], a[1] + K * ta[1], b[0] + K * tb[0], b[1] + K * tb[1], b[0], b[1]))
    out.append(('Z',))
    return out

def star(cx, cy, R, r):
    out = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rr = R if i % 2 == 0 else r
        out.append(('M' if i == 0 else 'L', cx + rr * math.cos(a), cy + rr * math.sin(a)))
    out.append(('Z',))
    return out

shapes = [  # (colour rgb 0..1, path)
    ((0.83, 0.63, 0.09), circle(256, 256, 240) + circle(256, 256, 196)),
    ((0.70, 0.13, 0.13), star(256, 268, 168, 68) + circle(256, 262, 30)),
]

def svg_d(path):
    s = []
    for c in path:
        if c[0] == 'Z': s.append('Z')
        else: s.append(c[0] + ' ' + ' '.join('%.2f' % v for v in c[1:]))
    return ' '.join(s)

os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, 'test_emblem.svg'), 'w') as f:
    f.write('<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">\n')
    for col, p in shapes:
        f.write('  <path fill="#%02x%02x%02x" fill-rule="evenodd" d="%s"/>\n' % (tuple(round(c * 255) for c in col) + (svg_d(p),)))
    f.write('</svg>\n')

# PDF: same paths, y flipped (PDF y up), even-odd fill f*
ops = []
for col, p in shapes:
    ops.append('%.3f %.3f %.3f rg' % col)
    for c in p:
        if c[0] == 'M': ops.append('%.2f %.2f m' % (c[1], S - c[2]))
        elif c[0] == 'L': ops.append('%.2f %.2f l' % (c[1], S - c[2]))
        elif c[0] == 'C': ops.append('%.2f %.2f %.2f %.2f %.2f %.2f c' % (c[1], S - c[2], c[3], S - c[4], c[5], S - c[6]))
        else: ops.append('h')
    ops.append('f*')
content = ('\n'.join(ops) + '\n').encode()
objs = [b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 512 512] /Contents 4 0 R /Resources << >> >>',
        b'<< /Length %d >>\nstream\n' % len(content) + content + b'endstream']
pdf = bytearray(b'%PDF-1.4\n')
offs = []
for i, o in enumerate(objs):
    offs.append(len(pdf))
    pdf += b'%d 0 obj\n' % (i + 1) + o + b'\nendobj\n'
x = len(pdf)
pdf += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
for o in offs: pdf += b'%010d 00000 n \n' % o
pdf += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, x)
open(os.path.join(OUT, 'test_emblem.pdf'), 'wb').write(pdf)
subprocess.run(['sips', '-s', 'format', 'png', '-Z', '1024', os.path.join(OUT, 'test_emblem.pdf'), '--out', os.path.join(OUT, 'test_emblem.png')], check=True, stdout=subprocess.DEVNULL)
print('wrote', OUT)
