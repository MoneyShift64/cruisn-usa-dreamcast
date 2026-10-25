#!/usr/bin/env python3
"""The ENTHUSIAST cheat's car names (Choose Car's details): new name pictures in
the game's own lettering, and copies of the four details meshes that use them.
tools/port_assets.py adds them to ASSETS.BIN after the ROM's files, and
src/frontend/carselect.c loads the copies instead of the originals when the
cheat is on.

The details of a car (mesh files 0x2BFC, 0x2C01, 0x2C06, 0x2C0B: gCsDLs[11-14])
are seven quads, each a small CI8 picture through palette 0x2C1F (colour 1
white, 249 the dark shadow). The name is the one at y 45-53. Its letters are
7 pixels tall with a shadow 1 down and 1 right; they are cut from the game's
other pictures here (X and Y aren't in any of them: drawn in the same style).

    python3 tools/port_realnames.py          (shows the names as text)
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crlib

PALETTE = 0x2C1F
WHITE, SHADOW = 1, 249

# car (gCsCar) -> (details mesh, its name picture, the new name)
CARS = [(0x2BFC, 0x2C68, "'63 CORVETTE STING RAY"),     # '63 Muscle Car
        (0x2C01, 0x2C7F, "'40 FORD V8 DE LUXE"),        # La Bomba
        (0x2C06, 0x2C96, "'93 HYUNDAI HCD-II EPOCH"),   # Devastator VI
        (0x2C0B, 0x2CAD, "'91 FERRARI 512 TR")]         # Italia P69

# letters: (picture, first column, end column), from the game's own text
CUTS = {
    "'": (0x2C68, 0, 1), '6': (0x2C68, 2, 8), '3': (0x2C68, 9, 14), 'U': (0x2C68, 27, 33),
    'S': (0x2C68, 34, 39), 'C': (0x2C68, 40, 45), 'L': (0x2C68, 46, 50), 'E': (0x2C68, 51, 56),
    'A': (0x2C68, 66, 71), 'R': (0x2C68, 72, 77), 'O': (0x2C7F, 21, 26), 'V': (0x2C96, 13, 18),
    '9': (0x2CAD, 42, 48), 'H': (0x2C69, 0, 5), 'P': (0x2C69, 6, 11), '4': (0x2C69, 14, 19),
    '5': (0x2C69, 20, 25), 'I': (0x2C69, 43, 44), 'D': (0x2C69, 45, 51), '8': (0x2C69, 63, 69),
    '1': (0x2CAE, 22, 23), '2': (0x2CAE, 24, 29), 'T': (0x2CC4, 20, 25), 'N': (0x2CC4, 53, 59),
    '0': (0x2CC5, 27, 32), 'G': (0x2C80, 69, 75), 'F': (0x2C62, 48, 53), '-': (0x2C63, 9, 11),
}
DRAWN = {
    'X': ['#   #', '#   #', ' # # ', '  #  ', ' # # ', '#   #', '#   #'],
    'Y': ['#   #', '#   #', ' # # ', '  #  ', '  #  ', '  #  ', '  #  '],
}
SPACE = 3          # (with the gaps either side: 4 empty columns, as in the game's)


def picture(fid):
    """-> (width, height, 1 where white)"""
    d = crlib.file(fid)
    pal = crlib.file(struct.unpack('>I', d[4:8])[0])
    colours = struct.unpack('>256H', pal[8:8 + 512])
    w, h = d[0] * 4, d[1]
    return w, h, [colours[b] == 0xFFFF for b in d[8:8 + w * h]]


def glyph(ch):
    """-> 7 rows of '#'/' '"""
    if ch in DRAWN:
        return DRAWN[ch]
    fid, a, b = CUTS[ch]
    w, h, px = picture(fid)
    return [''.join('#' if px[y * w + x] else ' ' for x in range(a, b)) for y in range(7)]


def text_mask(s):
    rows = [''] * 7
    for i, ch in enumerate(s):
        if ch == ' ':
            rows = [r + ' ' * SPACE for r in rows]
            continue
        g = glyph(ch)
        rows = [r + (' ' if i else '') + g[y] for y, r in enumerate(rows)]
    return rows


def name_picture(s):
    """-> (width used, texture file bytes: u8 w/4, u8 h, s16 flags 1, u32 palette, CI8)"""
    m = text_mask(s)
    used = len(m[0]) + 1                         # (and the shadow's column)
    w = (used + 7) & ~7
    px = bytearray(w * 8)
    for y in range(7):
        for x, c in enumerate(m[y]):
            if c == '#':
                px[y * w + x] = WHITE
    for y in range(7):
        for x, c in enumerate(m[y]):
            if c == '#' and px[(y + 1) * w + x + 1] != WHITE:
                px[(y + 1) * w + x + 1] = SHADOW
    return used, struct.pack('>BBhI', w // 4, 8, 1, PALETTE) + bytes(px)


def name_mesh(mesh, oldtex, newtex, used):
    """the details mesh (ROM bytes, big-endian) with the name quad drawing the new
    picture, as wide as it is"""
    d = bytearray(crlib.file(mesh))
    lv, lu = d[0], d[1]
    nrec = struct.unpack('>h', d[2:4])[0]
    uvb = 8 + 6 * (lv + 1)
    p = uvb + 4 * (lu + 1)
    rec = 0
    done = False
    while rec < nrec:
        if not (p & 2):
            at = p
            value = struct.unpack('>I', d[p:p + 4])[0]
            n = d[p + 4]
        else:
            at = p + 2
            n = d[p]
            value = struct.unpack('>I', d[p + 2:p + 6])[0]
        p += 6
        if (value & 0xFFFF) == oldtex:
            struct.pack_into('>I', d, at, (value & 0xFFFF0000) | newtex)
            vs, us = set(), set()
            for f in range(n):
                vs |= set(d[p + 6 * f:p + 6 * f + 3])
                us |= set(d[p + 6 * f + 3:p + 6 * f + 6])
            for v in vs:                         # the right edge
                if struct.unpack_from('>h', d, 8 + 6 * v)[0] != 0:
                    struct.pack_into('>h', d, 8 + 6 * v, used)
            for u in us:
                if struct.unpack_from('>h', d, uvb + 4 * u)[0] != 0:
                    struct.pack_into('>h', d, uvb + 4 * u, used * 64)
            done = True
        p += 6 * n
        rec += n + 1
    assert done, hex(mesh)
    return bytes(d)


def files(first):
    """-> [(id, kind, ROM-format bytes)]: the four pictures from 'first', then the four meshes.
    carselect.c expects the meshes at first + 4 (REALNAME_MESHES)."""
    out, meshes = [], []
    for i, (mesh, oldtex, s) in enumerate(CARS):
        used, tex = name_picture(s)
        out.append((first + i, 4, tex))
        meshes.append((first + 4 + i, 2, name_mesh(mesh, oldtex, first + i, used)))
    return out + meshes


if __name__ == '__main__':
    for mesh, oldtex, s in CARS:
        print(s)
        for r in text_mask(s):
            print('|' + r + '|')
