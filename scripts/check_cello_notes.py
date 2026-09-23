#!/usr/bin/env python3
"""Read-only per-note audio audit for monophonic, sustained cello test fixtures.

Optional developer dependencies: mido and numpy. Not a general musical-quality
metric: notes must be >= 1 s, non-overlapping and intentionally audible.
Checks sustained energy and strongest spectral-peak agreement with an integer
harmonic of the written pitch. Never generates audio or alters the MIDI.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import wave
import mido
import numpy as np


def audit_notes(midi_path, audio_path):
    active, notes, seconds = {}, [], 0.0
    for event in mido.MidiFile(midi_path):
        seconds += event.time
        if event.type == "note_on" and event.velocity:
            if active:
                raise ValueError("audit requires monophonic non-overlapping fixture notes")
            active[(event.channel, event.note)] = seconds
        elif event.type == "note_off" or (event.type == "note_on" and not event.velocity):
            start = active.pop((event.channel, event.note))
            if seconds - start < 1:
                raise ValueError("audit requires sustained notes of at least one second")
            notes.append((event.note, start, seconds))
    if active or not notes:
        raise ValueError("empty/incomplete MIDI fixture")
    with wave.open(str(audio_path), "rb") as wav:
        if wav.getsampwidth() != 2 or wav.getnchannels() != 2:
            raise ValueError("expected stereo PCM16 WAV")
        rate = wav.getframerate()
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").reshape(-1, 2).mean(axis=1) / 32768
    rows = []
    for key, start, end in notes:
        begin, finish = round((start + .25) * rate), round(min(start + 1.25, end - .1) * rate)
        if finish > len(audio):
            raise ValueError("audio ends before a scheduled note window")
        segment = audio[begin:finish]
        rms = float(np.sqrt(np.mean(segment * segment)))
        db = 20 * math.log10(max(rms, 1e-12))
        spectrum = abs(np.fft.rfft(segment * np.hanning(len(segment))))
        frequencies = np.fft.rfftfreq(len(segment), 1 / rate)
        spectrum[frequencies < 30] = 0
        peak = float(frequencies[int(np.argmax(spectrum))])
        expected = 440 * 2 ** ((key - 69) / 12)
        harmonic = round(peak / expected)
        cents = 1200 * math.log2(peak / (expected * harmonic)) if harmonic > 0 and peak > 0 else None
        valid = db > -55 and 1 <= harmonic <= 8 and cents is not None and abs(cents) < 35
        rows.append({"midi_note": key, "onset_seconds": round(start, 4), "duration_seconds": round(end-start, 4),
                     "sustain_rms_dbfs": round(db, 2), "strongest_hz": round(peak, 2),
                     "harmonic": harmonic, "cents_error": round(cents, 2) if cents is not None else None,
                     "passed": valid})
    passed = sum(row["passed"] for row in rows)
    return {"result": "PASS" if passed == len(rows) else "FAIL",
            "notes_passed": passed, "notes_total": len(rows), "notes": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("midi", type=Path)
    parser.add_argument("audio", type=Path)
    args = parser.parse_args()
    report = audit_notes(args.midi, args.audio)
    print(json.dumps(report, indent=2))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
