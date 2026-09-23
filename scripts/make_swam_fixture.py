#!/usr/bin/env python3
"""Create original monophonic cello/CC11 fixtures; never loads a plugin."""
import argparse
import pathlib
from make_piano_fixture import write_midi


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("new_directory", type=pathlib.Path)
    root = parser.parse_args().new_directory
    root.mkdir(exist_ok=False)
    notes, controls = [], [(0, 0xb0, 11, 45)]
    for index, pitch in enumerate([48, 55, 51, 50, 48, 46, 43, 48]):
        start = 960 + index * 1920
        notes.append((start, 1740, pitch, 84))
        for step, expression in enumerate([45, 58, 75, 90, 96, 88, 73, 50]):
            controls.append((start + step * 240, 0xb0, 11, expression))
    write_midi(root / "cello-phrase.mid", notes, controls, ((0, 84),))
    for name, expression in [("soft", 25), ("loud", 110)]:
        write_midi(root / (name + ".mid"), [(960, 3840, 48, 84)],
                   [(0, 0xb0, 11, expression)], ((0, 120),))
    write_midi(root / "missing-expression.mid", [(960, 3840, 48, 84)], (), ((0, 120),))
    print(root)


if __name__ == "__main__":
    main()
