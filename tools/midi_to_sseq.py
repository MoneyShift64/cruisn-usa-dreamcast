#!/usr/bin/env python3
"""Turn a MIDI file into a Cruis'n USA music sequence (the SSEQ format the sound
engine plays), for tools/port_sound.py to add to SOUND.BIN as a new tune.

A sequence is a list of tracks, each a 0x14-byte header (group, priority,
instrument, bend, volume, pan, -, volume mask, ticks per beat, tempo, labels,
script size), its labels (offsets of jump targets in the script) and the script:
delta time (MIDI variable length) then a command - 0x07 instrument (s16),
0x0C volume, 0x11 note on (key, velocity), 0x12 note off (key), 0x23 nothing
(the loop's start), 0x20 jump (label s16), 0x22 end. Command arguments are
little-endian; the header is written little-endian too (as port_sound.py swaps
the ROM's).

Each game instrument gets its own track (the drums map note by note onto
several instruments), so each track only changes instrument once. Every track
ends at the same tick and jumps back to its start: the tune loops.

    build(path, parts, drums, drumgain) -> bytes of the sequence, number of tracks
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from midiparse import parse

# The snow cheat's tune, port/music/jingle_bells.mid (the port's own arrangement of
# Jingle Bells, which is public domain): melody (MIDI channel 0) on instrument 17,
# bass (channel 1) on instrument 0, and the drums
SNOW_PARTS = {0: (17, 0, 0.9), 1: (0, 0, 1.0)}
SNOW_DRUMS = {35: (9, 38), 36: (9, 38), 38: (15, 42), 37: (15, 48), 42: (8, 42), 46: (8, 46),
              49: (5, 57), 54: (19, 54), 41: (18, 45), 45: (18, 49), 48: (18, 53)}


def delta(v):
    b = [v & 0x7F]
    v >>= 7
    while v:
        b.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(b))


def build(path, parts=SNOW_PARTS, drums=SNOW_DRUMS, drumgain=0.8, min_drum=12):
    fmt, div, tracks = parse(path)
    tempo = [e[2] for _, ev in tracks for e in ev if e[1] == 'tempo']
    bpm = round(60000000 / tempo[0]) if tempo else 120
    # notes per game instrument: (start, end, key, velocity)
    notes = {}
    for _, ev in tracks:
        on = {}
        for e in ev:
            if e[1] == 'on':
                on[(e[2], e[3])] = (e[0], e[4])
            elif e[1] == 'off' and (e[2], e[3]) in on:
                t0, vel = on.pop((e[2], e[3]))
                ch, key, t1 = e[2], e[3], e[0]
                if ch == 9:
                    if key not in drums:
                        continue
                    inst, k = drums[key]
                    g = drumgain
                    t1 = max(t1, t0 + min_drum)
                elif ch in parts:
                    inst, shift, g = parts[ch]
                    k = key + shift
                else:
                    continue
                v = max(1, min(127, round(vel * g)))
                notes.setdefault(inst, []).append((t0, t1, k, v))
    end = max(n[1] for ns in notes.values() for n in ns)
    bar = div * 4
    end = (end + bar - 1) // bar * bar
    out = b''
    order = sorted(notes, key=lambda i: (i not in [p[0] for p in parts.values()], i))
    for inst in order:
        ev = []                          # (tick, 0 = off first, key, velocity)
        for t0, t1, k, v in notes[inst]:
            ev.append((t0, 1, k, v))
            ev.append((min(t1, end), 0, k, 0))
        ev.sort()
        s = bytearray()
        s += delta(0) + bytes([0x07]) + struct.pack('<h', inst)
        s += delta(0) + bytes([0x0C, 127])
        s += delta(0)
        label = len(s)
        s += bytes([0x23])
        now = 0
        held = {}
        for t, kind, k, v in ev:
            if kind == 1:
                if held.get(k):          # the same key again before its end: end it first
                    s += delta(t - now) + bytes([0x12, k]); now = t
                    held[k] = 0
                s += delta(t - now) + bytes([0x11, k, v]); now = t
                held[k] = 1
            elif held.get(k):
                s += delta(t - now) + bytes([0x12, k]); now = t
                held[k] = 0
        for k, h in held.items():
            if h:
                s += delta(end - now) + bytes([0x12, k]); now = end
        s += delta(end - now) + bytes([0x20]) + struct.pack('<h', 0)
        s += delta(0) + bytes([0x22])
        while len(s) % 4:
            s += b'\0'
        hdr = struct.pack('<BBhhBBBBhHhi', 1, 0, inst, 0, 127, 64, 0, 0, div, bpm, 1, len(s))
        out += hdr + struct.pack('<i', label) + bytes(s)
    while len(out) % 8:
        out += b'\0'
    return out, len(order), bpm, end


if __name__ == '__main__':
    blob, n, bpm, end = build(sys.argv[1])
    print(f'{len(blob)} bytes, {n} tracks, {bpm} bpm, {end} ticks')
