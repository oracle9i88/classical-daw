#!/usr/bin/env python3
"""Read-only comparison of the C++ importer against mido on local MIDI files.

Optional developer dependency: mido. Source music is never copied into the
repository or modified. Compare normalized absolute timing, pitch, velocity,
channel, every track, tempo positions, and the rounding diagnostics.
"""
import argparse
import json
import math
import subprocess
from fractions import Fraction
from pathlib import Path

try:
    import mido
except ImportError:
    raise SystemExit("This optional comparison needs mido; run it with a Python environment containing mido.") from None


def compare(inspector, source):
    original = mido.MidiFile(source)
    if original.type not in (0, 1) or original.ticks_per_beat <= 0:
        raise ValueError("reference comparison requires a Type 0/1 PPQ file")
    ppq = original.ticks_per_beat
    normalize = lambda tick: (2 * tick * 960 + ppq) // (2 * ppq)
    result = subprocess.run([str(inspector), str(source), "--notes"],
                            check=True, text=True, capture_output=True)
    actual = json.loads(result.stdout)
    assert actual["engine_ppq"] == 960 and actual["source_ppq"] == ppq
    assert len(actual["tracks"]) == len(original.tracks)
    tempo = {0: 120.0}
    rounded_boundaries = rounded_tempos = channel_events = rounded_events = 0
    max_error = Fraction(0)
    note_count = 0
    for index, track in enumerate(original.tracks):
        absolute = 0
        active = {}
        expected = []
        expected_events = []
        for msg in track:
            absolute += msg.time
            if msg.type == "set_tempo":
                rounded_tempos += (absolute * 960) % ppq != 0
                tempo[normalize(absolute)] = 60000000.0 / msg.tempo
            elif msg.type == "note_on" and msg.velocity:
                key = (msg.channel, msg.note)
                # Do not validate an ambiguous same-pitch overlap by copying
                # the C++ reader's pairing policy into this reference tool.
                if key in active:
                    raise ValueError(f"ambiguous same-pitch overlap in track {index}")
                active[key] = (absolute, msg.velocity)
            elif msg.type == "note_off" or msg.type == "note_on":
                start, velocity = active.pop((msg.channel, msg.note))
                for boundary in (start, absolute):
                    rounded_boundaries += (boundary * 960) % ppq != 0
                    max_error = max(max_error, abs(Fraction(boundary * 960, ppq) - normalize(boundary)))
                expected.append([normalize(start), normalize(absolute), msg.note, velocity, msg.channel])
            elif not msg.is_meta and msg.type != "sysex":
                channel_events += 1
                data = msg.bytes()
                expected_events.append([normalize(absolute), data[0], data[1], data[2] if len(data) == 3 else 0])
                rounded_events += (absolute * 960) % ppq != 0
        assert not active, f"unclosed notes in reference track {index}"
        assert sorted(expected) == sorted(actual["tracks"][index]["notes"]), f"note mismatch in track {index}"
        assert expected_events == actual["tracks"][index]["channel_events"], f"channel event mismatch in track {index}"
        note_count += len(expected)
    assert actual["note_count"] == note_count
    assert actual["rounded_note_boundaries"] == rounded_boundaries
    assert actual["rounded_tempo_events"] == rounded_tempos
    assert actual["ignored_channel_events"] == 0
    assert actual["preserved_channel_events"] == channel_events
    assert actual["rounded_channel_events"] == rounded_events
    assert len(actual["tempo_changes"]) == len(tempo)
    for (tick, bpm), (expected_tick, expected_bpm) in zip(actual["tempo_changes"], sorted(tempo.items())):
        assert tick == expected_tick and math.isclose(bpm, expected_bpm, rel_tol=1e-12)
    assert max_error <= Fraction(1, 2)
    return {"source": str(source), "source_ppq": ppq, "tracks": len(original.tracks),
            "notes_checked": note_count, "tempo_positions_checked": len(tempo),
            "rounded_note_boundaries": rounded_boundaries, "max_error_engine_ticks": float(max_error),
            "preserved_channel_events": channel_events,
            "ignored_time_signature_events": actual["ignored_time_signature_events"], "passed": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspector", type=Path, default=Path("build/daw_midi_inspect"))
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.files:
        print(json.dumps(compare(args.inspector.resolve(), path), ensure_ascii=False))


if __name__ == "__main__":
    main()
