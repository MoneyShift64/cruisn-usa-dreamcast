#!/usr/bin/env python3
"""Build SOUND.BIN for the port: the ROM's sound data (ROM 0x5C40E0 up to the
end of the sequence file) with every multi-byte field of the bank ("SN64") and
the sequence file ("SSEQ") byte-swapped to little-endian, so the decompiled
engine can read it with plain loads. The port's dmaRomRead() serves ROM reads
in this range from the file, at the same offsets.

The samples between them (ROM 0x5D2BE0) are left as they are: VADPCM is a
byte stream, and the mixer reads raw 16-bit samples as big-endian itself.

    tools/port_sound.py baserom.z64 OUT/SOUND.BIN
"""
import struct, sys, os

BANK = 0x5C40E0
SAMPLES = 0x5D2BE0
SSEQ = 0x72AD60


def main():
    rom = open(sys.argv[1], 'rb').read()
    out = bytearray(rom)              # swapped in place, then cut out

    def be(fmt, a):
        return struct.unpack_from('>' + fmt, rom, a)

    def swap(a, fmt):
        """swap the fields at ROM address a; fmt is a struct format without byte order"""
        vals = struct.unpack_from('>' + fmt, rom, a)
        struct.pack_into('<' + fmt, out, a, *vals)
        return a + struct.calcsize('>' + fmt)

    # ---- SN64 bank ----
    assert rom[BANK:BANK + 4] == b'SN64'
    swap(BANK, 'IIII B3x III')
    data_size = be('I', BANK + 0x18)[0]
    info = BANK + 0x20
    _, ninst, _, nkm, _, nwave, _, n10, _, _ = be('i hh hh hh hh i', info)
    swap(info, 'i hhhhhhhh i')
    d0 = info + 0x18                  # loaded to an 8-aligned address; offsets below are from it
    a = d0
    for i in range(ninst):
        a = swap(a, 'BBh')
    a = d0 + ((a - d0 + 7) & ~7)
    for i in range(nkm):
        a = swap(a, '10B h HHH BB')
    a = d0 + ((a - d0 + 7) & ~7)
    waves = a
    raw = set()
    for i in range(nwave):
        if rom[a + 8] == 1:
            raw.add(i)
        a = swap(a, 'Ii BBh iii')
    a = d0 + ((a - d0 + 7) & ~7)
    for i in range(n10):
        a = swap(a, 'i')
    a = d0 + ((a - d0 + 7) & ~7)
    _, nraw, nadpcm, _ = be('hhhh', a)
    swap(a, 'hhhh')
    a += 8
    for i in range(nraw):
        a = swap(a, 'IIII')
    for i in range(nadpcm):
        a = swap(a, 'III 16h I')
    nbooks = 0
    for i in range(nwave):
        if i not in raw:
            swap(a + i * 0x108, 'ii 128h')
            nbooks = i + 1
    end = a + nbooks * 0x108
    assert end - d0 <= data_size, (hex(end - d0), hex(data_size))
    assert d0 + data_size <= SAMPLES

    # ---- SSEQ sequences ----
    assert rom[SSEQ:SSEQ + 4] == b'SSEQ'
    nseq = be('H', SSEQ + 0xE)[0]
    table_size = be('I', SSEQ + 0x18)[0]
    swap(SSEQ, 'III HH B3x iiI')
    t = SSEQ + 0x20
    data = t + table_size
    last = data
    for i in range(nseq):
        ntrk, packed, size, off, _ = be('HBxiiI', t + i * 0x10)
        swap(t + i * 0x10, 'HBxiiI')
        assert packed == 0
        p = data + off
        for k in range(ntrk):
            nlab, script = be('h i', p + 0xE)
            swap(p, 'BBhhBBBBhHhi')
            p += 0x14
            for j in range(nlab):
                p = swap(p, 'i')
            p += script
        assert p <= data + off + size, (i, hex(p), hex(data + off + size))
        last = max(last, data + off + size)

    # ---- the port's own tunes, added after the ROM's (sequence numbers nseq...) ----
    # Jingle Bells, for the snow cheat (tools/midi_to_sseq.py): sequence 0xAD
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from midi_to_sseq import build
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    extra = [build(os.path.join(root, 'port/music/jingle_bells.mid'))[0]]
    seqs = bytearray(out[SSEQ:last])          # header, table, data (little-endian now)
    hdr, table, sdata = seqs[:0x20], seqs[0x20:0x20 + table_size], seqs[0x20 + table_size:]
    while len(sdata) % 8:
        sdata += b'\0'
    for x in extra:
        table += struct.pack('<HBxiiI', extra_tracks(x), 0, len(x), len(sdata), 0)
        sdata += x
    struct.pack_into('<H', hdr, 0xE, nseq + len(extra))
    struct.pack_into('<i', hdr, 0x18, len(table))
    blob = bytes(out[BANK:SSEQ]) + bytes(hdr) + bytes(table) + bytes(sdata)
    open(sys.argv[2], 'wb').write(blob)
    print(f'SOUND.BIN: {len(blob)} bytes (ROM {BANK:#x}-{last:#x} and {len(extra)} tunes of the port), '
          f'{nwave} waves ({len(raw)} raw), {nseq + len(extra)} sequences')


def extra_tracks(x):
    """the number of tracks in a sequence made by midi_to_sseq.py"""
    n, p = 0, 0
    while p + 0x14 <= len(x) and x[p] == 1:
        nlab, size = struct.unpack_from('<hi', x, p + 0xE)
        p += 0x14 + 4 * nlab + size
        n += 1
    return n


if __name__ == '__main__':
    main()
