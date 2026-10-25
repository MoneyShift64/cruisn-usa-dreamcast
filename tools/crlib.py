#!/usr/bin/env python3
"""Shared readers for Cruis'n USA data (used by the preview and the Dreamcast
asset converter). Everything here follows the decompiled C:
  asset table / packing   src/lowmem.c
  mesh files, textures    src/meshcomp.c (modelCompile, texStateEmit)
  track layout            src/track.c (sectionsPlace), src/race/scenery.c (drawListAdd, sceneryDrawObj)
"""
import os, struct, math

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = 0x80470

_rom = None
def rom():
    global _rom
    if _rom is None:
        _rom = open(os.path.join(ROOT, 'baserom.z64'), 'rb').read()
    return _rom

# ---------------------------------------------------------------- assets
def entry(fid):
    e = rom()[TABLE + 4 + 8 * fid: TABLE + 12 + 8 * fid]
    w4 = struct.unpack('>I', e[4:8])[0]
    return dict(packed=e[0] >> 7, kind=(e[0] >> 4) & 7, method=e[0] & 0xF,
                size=((e[1] & 0xF) << 16) + struct.unpack('>H', e[2:4])[0],
                psize=((w4 >> 24) | ((e[1] >> 4) << 8)) << 8, offset=w4 & 0xFFFFFF)

def _rle(data, width, size):
    fmt = {1: 'B', 2: 'H', 4: 'I'}[width]
    top = 1 << (width * 8 - 1)
    units = struct.unpack('>%d%s' % (len(data) // width, fmt), data[:len(data) // width * width])
    out, i, n = [], 0, size // width
    while len(out) < n and i < len(units):
        c = units[i]; i += 1
        if c & top:
            out += [units[i]] * ((c & ~top) + 3); i += 1
        else:
            out += units[i:i + c + 1]; i += c + 1
    return struct.pack('>%d%s' % (n, fmt), *out[:n])

_cache = {}
def file(fid):
    fid &= 0xFFFF
    if fid not in _cache:
        e = entry(fid)
        start = TABLE + e['offset']
        if e['packed']:
            d = _rle(rom()[start:start + e['psize']], {2: 1, 3: 2, 4: 4}[e['method']], e['size'])
        else:
            d = rom()[start:start + e['size']]
        _cache[fid] = d
    return _cache[fid]

def main_word(addr, fmt='>I'):
    """a word of the main segment's data, by RAM address"""
    off = addr - 0x800FF000
    return struct.unpack(fmt, rom()[off:off + struct.calcsize(fmt)])[0]

# ---------------------------------------------------------------- textures
def rgba5551(c):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 6) & 31) * 255 // 31, ((c >> 1) & 31) * 255 // 31, 255 if c & 1 else 0)

OVERRIDES = os.path.join(ROOT, 'overrides')

def texture_override(fid):
    """overrides/XXXX.tex replaces texture file XXXX (for testing changes without
    touching the ROM; delete the file to go back). Format: u16 width, u16 height,
    then width*height big-endian RGBA5551 texels. Any size: the UVs are rescaled.
    -> (width, height, texels) or None"""
    p = os.path.join(OVERRIDES, '%04X.tex' % fid)
    if not os.path.exists(p):
        return None
    d = open(p, 'rb').read()
    w, h = struct.unpack('>HH', d[:4])
    return w, h, list(struct.unpack('>%dH' % (w * h), d[4:4 + 2 * w * h]))

def texture(fid):
    """-> (width, height, list of rgba5551 u16 texels, row-major). 8-bit colour
    index through a 256-colour palette, or 16-bit RGBA5551."""
    d = file(fid)
    w, h = d[0] * 4, d[1]
    flags = struct.unpack('>h', d[2:4])[0]
    if flags & 1:
        pal = file(struct.unpack('>I', d[4:8])[0])
        colours = struct.unpack('>256H', pal[8:8 + 512])
        texels = [colours[b] for b in d[8:8 + w * h]]
    else:
        texels = list(struct.unpack('>%dH' % (w * h), d[8:8 + 2 * w * h]))
    return w, h, texels

# ---------------------------------------------------------------- meshes
class Group:
    """one texture state and its triangles"""
    def __init__(self, flags, tex=None, color=None):
        self.flags = flags          # 4 untextured, 1 combiner, 2/8 S wrap/mirror, 0x20/0x10 T wrap/mirror
        self.tex = tex              # texture file id
        self.color = color          # RGBA (untextured)
        self.tris = []              # [(vertex index, (u, v)) x 3]

def mesh(fid, relocated=False):
    """-> (vertices [(x, y, z)], groups). uv in texels = raw / 64 (texture scale 0.5, 10.5 fixed)"""
    d = file(fid)
    lv, lu = d[0], d[1]
    nrec = struct.unpack('>h', d[2:4])[0]
    verts = [struct.unpack('>hhh', d[8 + 6 * i: 14 + 6 * i]) for i in range(lv + 1)]
    uvb = 8 + 6 * (lv + 1)
    uvs = [struct.unpack('>hh', d[uvb + 4 * i: uvb + 4 * i + 4]) for i in range(lu + 1)]
    p = uvb + 4 * (lu + 1)
    groups = []
    rec = 0
    while rec < nrec:
        if not (p & 2):
            value = struct.unpack('>I', d[p:p + 4])[0]; n = d[p + 4]; flags = d[p + 5]
        else:
            n = d[p]; flags = d[p + 1]; value = struct.unpack('>I', d[p + 2:p + 6])[0]
        p += 6
        g = Group(flags, color=value) if flags & 4 else Group(flags, tex=value & 0xFFFF)
        for f in range(n):
            vi = d[p:p + 3]; ui = d[p + 3:p + 6]
            g.tris.append([(vi[k], (uvs[ui[k]][0] / 64.0, uvs[ui[k]][1] / 64.0)) for k in range(3)])
            p += 6
        groups.append(g)
        rec += n + 1
    return verts, groups

# ---------------------------------------------------------------- tracks
def track_def(track, players=1):
    a = 0x80131F70 + (players - 1) * 14 * 0x10 + track * 0x10
    return dict(header=main_word(a, '>h'), first=main_word(a + 4, '>i'),
                drawDist=main_word(a + 8, '>f'), fadeOn=main_word(a + 12, '>i'))

def _sin(a):
    return math.sin(a * math.pi / 32768.0)

def sections(track):
    """placed sections as in sectionsPlace: [{xf, y, sxz, sy, model}] (route units)"""
    h = file(track_def(track)['header'])
    models_id, nmodels, secs_id, nsecs = struct.unpack('>iiii', h[0:16])
    secs = file(secs_id)
    ss = main_word(0x80131E94 + 4 * track)
    def hstep(i):
        a = main_word(0x80131A3C + 4 * track) + 4 * i
        return main_word(a, '>h'), main_word(a + 2, '>h')
    def sstep(i):
        a = ss + 6 * i
        return main_word(a, '>h'), main_word(a + 2, '>h'), main_word(a + 4, '>h')
    hi = si = 0
    angle = 0
    x = z = y = 0.0
    out = []
    for i in range(nsecs):
        u0, model, flags = struct.unpack('>BBH', secs[20 * i: 20 * i + 4])
        dx, dy, dz, turn = struct.unpack('>iiii', secs[20 * i + 4: 20 * i + 20])
        if i >= hstep(hi + 1)[0]:
            hi += 1
        if i >= sstep(si + 1)[0]:
            si += 1
        a = -angle
        c = math.cos(a * math.pi / 32768.0); s = _sin(a)
        xf = [c, s, x, -s, c, z]
        _, sxz, sy = sstep(si)
        out.append(dict(xf=xf, y=y, sxz=sxz * 1e-5 + 0.2, sy=sy * 1e-5 + 0.2, model=model, flags=flags))
        lx, ly, lz = dx * 0.2, dy * 0.2, dz * 0.2
        if lx != 0 or lz != 0:
            ly += 0.2 * hstep(hi)[1]
        y += ly
        x, z = xf[0] * lx + xf[1] * lz + xf[2], xf[3] * lx + xf[4] * lz + xf[5]
        angle += turn
    return out, models_id

def group(fid):
    d = file(fid)
    parts_id, nparts, items_id, nitems = struct.unpack('>iiii', d[0:16])
    pd = file(parts_id)
    parts = [struct.unpack('>III', pd[12 * k: 12 * k + 12])[:2] for k in range(nparts)]
    idd = file(items_id) if nitems else b''
    items = []
    for k in range(nitems):
        df, part, fl = struct.unpack('>BBH', idd[20 * k: 20 * k + 4])
        ix, iy, iz, ang = struct.unpack('>iiii', idd[20 * k + 4: 20 * k + 20])
        items.append(dict(drawFlags=df, part=part, flags=fl, x=ix / 65536.0, y=iy / 65536.0, z=iz / 65536.0, angle=ang))
    return [dict(data=p[0] & 0xFFFF, isGroup=p[1] & 1) for p in parts], items

def track_objects(track):
    """every drawn mesh instance of a track: (mesh file id, 3x3 matrix rows, translation)"""
    secs, models_id = sections(track)
    md = file(models_id)
    out = []
    for si, sec in enumerate(secs):
        mdata, mflags = struct.unpack('>II', md[12 * sec['model']: 12 * sec['model'] + 8])
        if not (mflags & 1):
            continue
        parts, items = group(mdata & 0xFFFF)
        for it in items:
            if it['drawFlags'] & 1:                       # animated objects: drawn by their own code
                continue
            part = parts[it['part']]
            if part['isGroup']:
                continue
            # obj->xf = section xf * rotate(item angle, item x, z)   (drawListAdd)
            c = math.cos(it['angle'] * math.pi / 32768.0); s = _sin(it['angle'])
            b = [c, s, it['x'], -s, c, it['z']]
            a = sec['xf']
            xf = [a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4], a[0] * b[2] + a[1] * b[5] + a[2],
                  a[3] * b[0] + a[4] * b[3], a[3] * b[1] + a[4] * b[4], a[3] * b[2] + a[4] * b[5] + a[5]]
            out.append(dict(mesh=part['data'], xf=xf, y=sec['y'] + it['y'], sxz=sec['sxz'], sy=sec['sy'],
                            kind=it['flags'] & 0xF00, section=si))
    return out

def place(obj, v):
    """mesh vertex -> route units (sceneryDrawObj's matrix)"""
    xf, s = obj['xf'], obj['sxz']
    return (s * (xf[0] * v[0] + xf[1] * v[2]) + xf[2], obj['sy'] * v[1] + obj['y'], s * (xf[3] * v[0] + xf[4] * v[2]) + xf[5])
