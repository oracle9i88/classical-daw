#!/usr/bin/env python3
"""Write an original short piano phrase and isolated probes; no dependencies.

This only creates MIDI fixtures. It never loads a plugin or launches a render.
Destination must not exist. Use the local MIDI hygiene scanner before rendering.
"""
import argparse
import pathlib
import struct


def vlq(value):
    result = [value & 127]
    value >>= 7
    while value:
        result.insert(0, 128 | (value & 127))
        value >>= 7
    return bytes(result)


def write_midi(path, notes, controls=(), tempos=((0, 80),)):
    events = [(0, -2, b"\xff\x58\x04\x04\x02\x18\x08")]
    for tick, bpm in tempos:
        events.append((tick, -1, b"\xff\x51\x03" + round(60_000_000 / bpm).to_bytes(3, "big")))
    for tick, status, data1, data2 in controls:
        events.append((tick, 1, bytes((status, data1)) if status & 0xf0 in (0xc0, 0xd0)
                       else bytes((status, data1, data2))))
    for start, duration, pitch, velocity in notes:
        events.append((start, 2, bytes((0x90, pitch, velocity))))
        events.append((start + duration, 0, bytes((0x80, pitch, 32))))
    track = bytearray()
    previous = 0
    for tick, _, message in sorted(events, key=lambda item: (item[0], item[1])):
        track += vlq(tick - previous) + message
        previous = tick
    track += b"\x00\xff\x2f\x00"
    path.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 960) + b"MTrk" + struct.pack(">I", len(track)) + track)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("new_directory", type=pathlib.Path)
    args = parser.parse_args()
    args.new_directory.mkdir(exist_ok=False)
    melody = [
        [72, 76, 79, 76], [74, 77, 81, 77], [71, 74, 79, 74], [72, 76, 79, 84],
        [81, 77, 74, 77], [79, 76, 72, 76], [74, 71, 67, 71], [72, 76, 79, 72],
    ]
    harmony = [(48, 55), (50, 57), (43, 55), (48, 55), (41, 53), (45, 52), (43, 55), (48, 55)]
    notes, controls = [], [(0, 0xc0, 0, 0)]
    for bar, pitches in enumerate(melody):
        tick = 480 + bar * 3840
        controls += [(tick, 0xb0, 64, 95), (tick + 3520, 0xb0, 64, 0)]
        for beat, pitch in enumerate(pitches):
            notes.append((tick + beat * 960, 820, pitch, [68, 61, 74, 57][beat] + (5 if bar == 3 else 0)))
        for pitch in harmony[bar]:
            notes.append((tick, 3250, pitch, 46))
    write_midi(args.new_directory / "piano-phrase.mid", notes, controls, ((0, 80), (15840, 88)))
    # Exact block-boundary probes: at 120 BPM/48k, tick 1024 is frame 25600.
    for name, velocity, pedal in [("soft", 25, False), ("loud", 100, False), ("pedal", 100, True)]:
        cc = [(1024, 0xb0, 64, 127), (4096, 0xb0, 64, 0)] if pedal else [(4096, 0xb0, 64, 0)]
        write_midi(args.new_directory / (name + ".mid"), [(1024, 1024, 60, velocity)], cc, ((0, 120),))
    print(args.new_directory)


if __name__ == "__main__":
    main()
