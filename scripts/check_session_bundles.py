#!/usr/bin/env python3
"""Read-only checks for the original daw_session_fixture duet; never runs plugins."""
import argparse
import array
import json
import math
from pathlib import Path
import sys
import wave
from check_cello_notes import audit_notes


def require(condition, message):
    if not condition:
        raise ValueError(message)


def audio(path, frames):
    with wave.open(str(path), "rb") as wav:
        require((wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getnframes())
                == (48000, 2, 2, frames), f"wrong WAV format/length: {path}")
        samples = array.array("h", wav.readframes(frames))
    if sys.byteorder != "little":
        samples.byteswap()
    require(len(samples) == frames * 2, "truncated PCM")
    require(0 < max(map(abs, samples)) < 32767, "silent or full-scale audio")
    return samples


def bundle(folder):
    report = json.loads((folder / "report.json").read_text())
    require(report["format"] in ("classical-daw-session-bounce-1", "classical-daw-session-bounce-2"), "report format")
    require(report["frames"] == 1517594 and report["end_tick"] == 34560,
            "tempo map or terminal silent measure changed")
    require(report["sample_rate"] == 48000 and report["channels"] == 2
            and report["tail_seconds"] == 5, "session audio format")
    require(report["clipped_samples"] == 0 and report["master_gain_db"] == -3,
            "master configuration or headroom changed")
    require(report["stem_tap"] in ("post-track-fader, pre-master", "post-track-fader-and-mute-solo, pre-master"), "unknown stem tap")
    expected = [("piano", "pianoteq", -4.5, -.2), ("cello", "swam-cello", -6, .2)]
    require([(s["part_id"], s["instrument"], s["gain_db"], s["balance"])
             for s in report["stems"]] == expected, "routing or faders changed")
    stems = [audio(folder / s["audio"], report["frames"]) for s in report["stems"]]
    mix = audio(folder / "mix.wav", report["frames"])
    gain = 10 ** (report["master_gain_db"] / 20)
    # The stems are independently quantized. Account for their rounding plus
    # the final WAV rounding, rather than demanding bit equality with float sum.
    error = max(abs(round(sum(values) * gain) - master)
                for values, master in zip(zip(*stems), mix))
    bound = math.ceil(.5 * len(stems) * gain + .5 + 1e-3)
    require(error <= bound, f"stems cannot reconstruct mix: {error} > {bound} LSB")
    return report, stems + [mix], error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    root = parser.parse_args().directory
    first, before, error_a = bundle(root / "first")
    second, after, error_b = bundle(root / "reloaded")
    for name, report in (("first", first), ("reloaded", second)):
        cello = next(stem for stem in report["stems"] if stem["part_id"] == "cello")
        notes = audit_notes(root / name / cello["midi"], root / name / cello["audio"])
        require(notes["result"] == "PASS" and notes["notes_total"] == 8,
                f"{name}: cello note-level audio audit failed: {notes['notes_passed']}/8")
    for name in ("score.dawproj", "session.dawsession"):
        require((root / "first" / name).read_bytes() == (root / "reloaded" / name).read_bytes(),
                f"reload changed {name}")
    require((root / "score.dawproj").read_bytes() == (root / "first/score.dawproj").read_bytes(),
            "bounce changed source score")
    for a, b in zip(first["stems"], second["stems"]):
        for field in ("part_id", "instrument", "preset", "component_version", "gain_db",
                      "balance", "sent_messages", "skipped_bank_program_messages"):
            require(a[field] == b[field], f"reload changed {field}")
        for field in ("state", "midi"):
            require((root / "first" / a[field]).read_bytes() ==
                    (root / "reloaded" / b[field]).read_bytes(), f"reload changed {field}")
        require((root / (a["part_id"] + ".mid")).read_bytes() ==
                (root / "first" / a["midi"]).read_bytes(), "part MIDI changed or leaked")
    differences = [math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)) / 32768
                   for a, b in zip(before, after)]
    require(differences[0] < .0001, "piano reload audio exceeds fixture tolerance")
    # SWAM's first factory-selected session and reopened session do not null in
    # this fixture, even with byte-identical saved state and MIDI. Do not call
    # envelope similarity deterministic rendering: retain/report the PCM error.
    # Check 100 ms energy windows, total energy, and onset separately to catch
    # lost expression, large gain drift, or shifted playback.
    def envelope(samples):
        return [math.sqrt(sum(v * v for v in samples[i:i + 9600]) /
                          len(samples[i:i + 9600]))
                for i in range(0, len(samples), 9600)]
    envelopes = []
    onset_deltas = []
    for a, b in zip(before, after):
        ea, eb = envelope(a), envelope(b)
        error = math.sqrt(sum((x - y) ** 2 for x, y in zip(ea, eb)) /
                          sum(x * x for x in ea))
        envelopes.append(error)
        onset_deltas.append(abs(next(i // 2 for i, v in enumerate(a) if v) -
                                next(i // 2 for i, v in enumerate(b) if v)))
        rms_ratio = math.sqrt(sum(v * v for v in b) / sum(v * v for v in a))
        require(.95 <= rms_ratio <= 1.05, "reload total level drift exceeds 5%")
        require(error <= .05, "reload 100 ms envelope error exceeds 5%")
        require(onset_deltas[-1] <= 48, "reload onset drift exceeds 1 ms")
    print(json.dumps({"result": "PASS", "frames": first["frames"],
                      "mix_reconstruction_max_lsb": [error_a, error_b],
                      "reload_difference_rms_piano_cello_mix": differences,
                      "reload_envelope_nrmse_piano_cello_mix": envelopes,
                      "reload_onset_delta_frames": onset_deltas,
                      "sample_identical_reload": [a == b for a, b in zip(before, after)],
                      "scope": "routing/state preservation and envelope checks; not deterministic AU audio"}, indent=2))


if __name__ == "__main__":
    main()
