#!/usr/bin/env python3
"""Compare all channel messages through C++ SMF and native-project round-trips.

Requires optional developer dependency mido. Original sources are read-only.
Tempo/meter, SysEx and meta messages are excluded from this performance-event
comparison; the Score model does not yet preserve their complete semantics.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

try:
    import mido
except ImportError:
    raise SystemExit("Run this optional comparison in a Python environment containing mido.") from None


def channel_tracks(path):
    midi = mido.MidiFile(path)
    if midi.type not in (0, 1) or midi.ticks_per_beat <= 0:
        raise ValueError("Only Type 0/1 PPQ sources are supported")
    tracks = []
    for track in midi.tracks:
        tick = 0
        events = []
        for msg in track:
            tick += msg.time
            if msg.is_meta or msg.type == "sysex":
                continue
            data = msg.bytes()
            # Note-on zero and note-off zero have the same release semantics.
            if msg.type == "note_on" and msg.velocity == 0:
                data[0] = 0x80 | msg.channel
            normalized = (2 * tick * 960 + midi.ticks_per_beat) // (2 * midi.ticks_per_beat)
            events.append([normalized, *data])
        if events:  # Score drops metadata-only conductor tracks.
            tracks.append(events)
    return tracks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, default=Path("build/daw_midi_roundtrip"))
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args()
    for source in args.files:
        before = channel_tracks(source)
        with tempfile.TemporaryDirectory(prefix="daw-midi-performance-") as temp:
            output = Path(temp) / "output"
            run = subprocess.run([str(args.tool.resolve()), str(source), str(output)],
                                 capture_output=True, text=True)
            if run.returncode:
                raise RuntimeError(f"{source}: native round-trip failed: {run.stderr.strip()}")
            for kind in ("direct", "project"):
                after = channel_tracks(output / f"{kind}.mid")
                if before != after:
                    raise AssertionError(f"{source}: {kind} round-trip changed channel messages/order")
        print(json.dumps({"source": str(source), "tracks_checked": len(before),
                          "channel_messages_checked": sum(map(len, before)),
                          "direct_and_project_passed": True,
                          "excluded": "tempo/meter/meta/SysEx"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
