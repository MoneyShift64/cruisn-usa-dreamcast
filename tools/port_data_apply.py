#!/usr/bin/env python3
"""Rebuild the port's data segment images (seg_<name>.bin) from the ROM and
the recipes tools/gen_data.py writes (which byte ranges to swap), so the
generated build files carry no ROM data.

    python3 tools/port_data_apply.py GENDIR
"""
import os, sys, glob, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crlib

def main(gen):
    rom = crlib.rom()
    for p in sorted(glob.glob(os.path.join(gen, 'seg_*.recipe'))):
        lines = open(p).read().split('\n')
        name, romoff, dlen, size = lines[0].split()
        romoff, dlen, size = int(romoff, 16), int(dlen, 16), int(size, 16)
        img = bytearray(rom[romoff:romoff + dlen]) if dlen else bytearray()
        img += bytearray(size - len(img))
        for ln in lines[1:]:
            if ln:
                o, n = ln.split()
                o = int(o, 16)
                if n == 'D':                 # a double, stored as a float (PORT_F64_IS_FLOAT)
                    v = struct.unpack('>d', bytes(img[o:o + 8]))[0]
                    img[o:o + 8] = struct.pack('<f', v) + b'\0\0\0\0'
                else:
                    n = int(n)
                    img[o:o + n] = img[o:o + n][::-1]
        open(os.path.join(gen, f'seg_{name}.bin'), 'wb').write(img)
        print(f'{gen}/seg_{name}.bin: {size} bytes')

if __name__ == '__main__':
    main(sys.argv[1])
