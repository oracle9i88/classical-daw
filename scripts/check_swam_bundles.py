#!/usr/bin/env python3
"""Read-only SWAM Cello bounce/reload/expression checks; no plugin runs."""
import argparse
import array
import json
import math
import pathlib
import plistlib
import sys
import wave
import xml.etree.ElementTree as ET
from check_cello_notes import audit_notes


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(folder):
    report = json.loads((folder / "report.json").read_text())
    with wave.open(str(folder / "instrument.wav"), "rb") as audio:
        require((audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (48000, 2, 2), "WAV format")
        require(audio.getnframes() == report["frames"], "WAV/report frame count")
        values = array.array("h", audio.readframes(audio.getnframes()))
        if sys.byteorder != "little":
            values.byteswap()
    require(report["component"] == "aumu/Sce3/AuMo", "wrong instrument")
    require(report["clipped_samples"] == 0 and report["rms_before_clipping"] > .001, "clipped or silent audio")
    require(report["last_second_rms"] < .0001, "tail has not decayed")
    return report, values


def state(folder):
    value = plistlib.loads((folder / "instrument.aupreset").read_bytes())
    # SWAM 3.12.2's JUCE XML state changes only this metadata field on reload.
    # Parse, never modify the user's saved state. All other XML stays compared.
    raw = value.pop("jucePluginState")
    require(raw[:4] == b"VC2!", "unknown JUCE state encoding")
    xml = ET.fromstring(raw[8:].rstrip(b"\0"))
    for node in xml.iter("datetime"):
        node.attrib.pop("value", None)
    value["jucePluginState"] = ET.tostring(xml)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=pathlib.Path)
    root = parser.parse_args().directory
    first, a = load(root / "first")
    second, b = load(root / "reloaded")
    for name in ("first", "reloaded"):
        notes = audit_notes(root / name / "performance.mid", root / name / "instrument.wav")
        require(notes["result"] == "PASS" and notes["notes_total"] == 8,
                f"{name}: cello note-level audio audit failed: {notes['notes_passed']}/8")
    require(first["preset"] == second["preset"] == "Cello", "preset changed")
    require(first["frames"] == second["frames"], "reload changed duration")
    require(state(root / "first") == state(root / "reloaded"), "reload changed instrument state beyond datetime")
    require((root / "first/score.dawproj").read_bytes() == (root / "reloaded/score.dawproj").read_bytes(), "score changed")
    difference = math.sqrt(sum((x-y)**2 for x, y in zip(a, b)) / len(a)) / 32768
    require(difference < .0001, "reload audio differs beyond fixture tolerance")
    _, soft = load(root / "soft-render")
    _, loud = load(root / "loud-render")
    def sustain_rms(samples):
        require(max(abs(value) for value in samples[:47040]) == 0, "sound before note")
        window = samples[48000:192000]  # .5..2 seconds
        return math.sqrt(sum(value*value for value in window) / len(window)) / 32768
    ratio = sustain_rms(loud) / sustain_rms(soft)
    require(ratio > 2, "CC11 expression has no measured effect")
    print(json.dumps({"result": "PASS", "reload_difference_rms": difference,
                      "cc11_loud_soft_rms_ratio": ratio, "frames": first["frames"]}, indent=2))


if __name__ == "__main__":
    main()
