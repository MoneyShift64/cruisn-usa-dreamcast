#!/usr/bin/env python3
"""Build-time asset converter for the port: the ROM's asset files, unpacked
and byte-swapped to little-endian field by field, in one pack the port's
dmaReadSync reads instead of the cartridge.

    python3 tools/port_assets.py OUT.BIN [report.txt]

Pack layout (read by the game's own assetLoad, src/lowmem.c, through the
port's dmaReadSync; all multi-byte values little-endian):
  0x0000            u32 number of files
  4 + 8 * id        entry: u8 kind << 4 (never packed), u8 size bits 16-19,
                    u16 size bits 0-15, u32 offset (24 bits) of the data from 0
  data              each file 8-byte aligned

How each file is swapped:
  kind 1, 2  mesh (src/meshcomp.c): header, s16 vertices and uvs, then 6-byte
             records whose group headers hold a u32 (at a 2-byte boundary when
             the record is; the game uses LOAD_U32_UNALIGNED for those)
  kind 3     palette: u32 x 2, then u16 colours
  kind 4     texture: u8 w/4, u8 h, s16 flags, u32 palette id, texels (u8 if
             flags & 1, else u16)
  kind 0     by what refers to it: track headers (TrackHeader -> ModelRef[],
             SectionInfo[]), section models (SectionModel -> ModelPart[],
             ModelItem[]), id lists (u16, 0xFFFF-terminated), and a few known
             files. Anything else is copied unchanged and listed in the report
             (the port warns when one of those is loaded).
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crlib

NFILES = struct.unpack('>I', crlib.rom()[crlib.TABLE:crlib.TABLE + 4])[0]

def sw16(b, o):
    b[o:o + 2] = b[o:o + 2][::-1]

def sw32(b, o):
    b[o:o + 4] = b[o:o + 4][::-1]

def sw16_all(b, start=0, end=None):
    end = len(b) if end is None else end
    for o in range(start, end - 1, 2):
        sw16(b, o)

def sw32_all(b, start=0, end=None):
    end = len(b) if end is None else end
    for o in range(start, end - 3, 4):
        sw32(b, o)

# ---------------------------------------------------------------- formats
def conv_mesh(b):
    lv, lu = b[0], b[1]
    nrec = struct.unpack('>h', bytes(b[2:4]))[0]
    for o in (2, 4, 6):
        sw16(b, o)
    sw16_all(b, 8, 8 + 6 * (lv + 1) + 4 * (lu + 1))
    p = 8 + 6 * (lv + 1) + 4 * (lu + 1)
    rec = 0
    while rec < nrec and p + 6 <= len(b):
        if not (p & 2):
            sw32(b, p)
            n = b[p + 4]
        else:
            n = b[p]
            sw32(b, p + 2)           # read with LOAD_U32_UNALIGNED (common.h)
        p += 6 + 6 * n
        rec += n + 1

def conv_palette(b):
    sw32(b, 0); sw32(b, 4)
    sw16_all(b, 8)

def conv_texture(b):
    flags = struct.unpack('>h', bytes(b[2:4]))[0]
    sw16(b, 2); sw32(b, 4)
    if not (flags & 1):
        sw16_all(b, 8)

def be32(d, o):
    return struct.unpack('>I', bytes(d[o:o + 4]))[0]

class Packer:
    def __init__(self):
        self.data = {}        # id -> bytearray (converted)
        self.how = {}         # id -> format name
        for i in range(NFILES):
            self.data[i] = bytearray(crlib.file(i))
        for i in range(NFILES):
            k = crlib.entry(i)['kind']
            if k in (1, 2):
                conv_mesh(self.data[i]); self.how[i] = 'mesh'
            elif k == 3:
                conv_palette(self.data[i]); self.how[i] = 'palette'
            elif k == 4:
                conv_texture(self.data[i]); self.how[i] = 'texture'

    def mark(self, i, how):
        if i in self.how:
            if self.how[i] != how:
                print(f'  file {i:#x}: used as {self.how[i]} and {how}')
            return False
        self.how[i] = how
        return True

    # TrackHeader: ModelRef *models (id), s32 n, SectionInfo *sections (id), s32 n, s32 unk
    def track_header(self, i):
        if not self.mark(i, 'TrackHeader'):
            return
        d = crlib.file(i)
        models, nm, secs, ns = (be32(d, 0), be32(d, 4), be32(d, 8), be32(d, 12))
        sw32_all(self.data[i])
        self.model_refs(models & 0xFFFF, nm)
        self.section_infos(secs & 0xFFFF, ns)

    def model_refs(self, i, n):          # ModelRef / ModelPart: u32 data, u32 flags, u32
        if not self.mark(i, 'ModelPart[]'):
            return
        d = crlib.file(i)
        sw32_all(self.data[i])
        for k in range(len(d) // 12):
            data, flags = be32(d, 12 * k), be32(d, 12 * k + 4)
            if flags & 1:
                self.section_model(data & 0xFFFF)

    def section_infos(self, i, n):       # u8, u8, u16, s32 x 4
        if not self.mark(i, 'SectionInfo[]'):
            return
        b = self.data[i]
        for k in range(len(b) // 20):
            sw16(b, 20 * k + 2)
            sw32_all(b, 20 * k + 4, 20 * k + 20)

    def section_model(self, i):          # SectionModel: parts id, n, items id, n, unk
        if not self.mark(i, 'SectionModel'):
            return
        d = crlib.file(i)
        parts, items = be32(d, 0) & 0xFFFF, be32(d, 8) & 0xFFFF
        nitems = be32(d, 12)
        sw32_all(self.data[i])
        self.model_refs(parts, None)
        if nitems:
            self.model_items(items)

    def model_items(self, i):            # u8, u8, u16, s32 x 4
        if not self.mark(i, 'ModelItem[]'):
            return
        b = self.data[i]
        for k in range(len(b) // 20):
            sw16(b, 20 * k + 2)
            sw32_all(b, 20 * k + 4, 20 * k + 20)

    def id_list(self, i):
        if not self.mark(i, 'id list'):
            return
        sw16_all(self.data[i])

    def u16_file(self, i, what):
        if self.mark(i, what):
            sw16_all(self.data[i])

    def looks_like_id_list(self, i):
        d = crlib.file(i)
        if len(d) < 4 or len(d) % 2:
            return False
        v = struct.unpack('>%dH' % (len(d) // 2), d)
        if 0xFFFF not in v:
            return False
        end = v.index(0xFFFF)
        return end > 0 and all(x < NFILES for x in v[:end]) and all(x in (0, 0xFFFF) for x in v[end:])

def main(out, report=None):
    P = Packer()
    # tracks: both player-count tables
    for players in (1, 2):
        for t in range(14):
            P.track_header(crlib.track_def(t, players)['header'])
    # id lists anywhere (assetLoadGroup)
    for i in range(NFILES):
        if crlib.entry(i)['kind'] == 0 and i not in P.how and P.looks_like_id_list(i):
            P.id_list(i)
    P.u16_file(0x546, 'sky image (u16)')
    P.u16_file(0x2BD, 'palette (u16 x 256)')
    for i in (0x2BE, 0x2BF, 0x2C0, 0x2E4, 0x2E5, 0x2E6):     # 8-bit font images (group 0x2BC)
        P.mark(i, 'u8 image')

    # the port's own files, after the ROM's: the ENTHUSIAST cheat's car names
    # (tools/port_realnames.py; src/frontend/carselect.c REALNAME_FILES)
    import port_realnames
    kinds = {i: crlib.entry(i)['kind'] for i in range(NFILES)}
    total = NFILES
    for i, kind, d in port_realnames.files(NFILES):
        b = bytearray(d)
        if kind == 4:
            conv_texture(b); P.how[i] = 'texture (port)'
        else:
            conv_mesh(b); P.how[i] = 'mesh (port)'
        P.data[i] = b
        kinds[i] = kind
        total = max(total, i + 1)

    # pack
    body = bytearray(4 + 8 * total)
    struct.pack_into('<I', body, 0, total)
    for i in range(total):
        body += b'\0' * (-len(body) % 8)
        off = len(body)
        d = P.data[i]
        size = len(d)
        assert size < 1 << 20 and off < 1 << 24
        struct.pack_into('<BBHI', body, 4 + 8 * i, kinds[i] << 4, (size >> 16) & 0xF, size & 0xFFFF, off)
        body += d
    open(out, 'wb').write(body)
    left = [i for i in range(NFILES) if i not in P.how]
    counts = {}
    for h in P.how.values():
        counts[h] = counts.get(h, 0) + 1
    print(f'{out}: {NFILES} files, {len(body) // 1024} KB; ' + ', '.join(f'{v} {k}' for k, v in sorted(counts.items())))
    print(f'  {len(left)} files copied unchanged (format unknown)')
    if report:
        with open(report, 'w') as f:
            for i in range(NFILES):
                f.write(f'{i:#06x} kind {crlib.entry(i)["kind"]} {len(P.data[i]):6d} {P.how.get(i, "UNCONVERTED")}\n')
    # list of unconverted ids for the port's warning
    return left

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
