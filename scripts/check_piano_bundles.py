#!/usr/bin/env python3
"""Read-only checks of local Pianoteq integration fixtures. Never renders/retries.

Expected subdirectories: verified, verified-reload, verified-soft,
verified-loud, verified-pedal. See docs/pianoteq.md for explicit render commands.
"""
import argparse
import array
import json
import math
import pathlib
import plistlib
import sys
import wave


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(directory):
    report = json.loads((directory / "report.json").read_text())
    with wave.open(str(directory / "piano.wav"), "rb") as audio:
        require((audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (48000, 2, 2),
                "expected stereo 48 kHz PCM16")
        require(audio.getnframes() == report["frames"], "report/WAV length mismatch")
        samples = array.array("h", audio.readframes(audio.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
    require(report["clipped_samples"] == 0, "fixture clips")
    require(report["rms_before_clipping"] > 1e-5, "silent render")
    require(report["last_second_rms"] < 0.0001, "fixture tail has not decayed")
    return report, samples


def rms(samples, start, end):
    window = samples[round(start * 48000) * 2:round(end * 48000) * 2]
    require(len(window) > 0, "empty audio window")
    return math.sqrt(sum(value * value for value in window) / len(window)) / 32768


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=pathlib.Path)
    root = parser.parse_args().directory
    first, a = load(root / "verified")
    second, b = load(root / "verified-reload")
    require(first["frames"] == second["frames"], "reload changed duration")
    require(first["preset"] == second["preset"], "reload changed preset name")
    require((root / "verified/score.dawproj").read_bytes() ==
            (root / "verified-reload/score.dawproj").read_bytes(), "reload changed score")
    require(plistlib.loads((root / "verified/piano.aupreset").read_bytes()) ==
            plistlib.loads((root / "verified-reload/piano.aupreset").read_bytes()), "reload changed AU state")
    require(first["sent_messages"] == second["sent_messages"], "reload changed message count")
    require(first["skipped_bank_program_messages"] == second["skipped_bank_program_messages"] == 1,
            "fixed-preset bank/program protection failed")
    delta = abs(first["rms_before_clipping"] / second["rms_before_clipping"] - 1)
    require(delta < .005, "reload changed overall level by more than 0.5%")
    difference = math.sqrt(sum((x-y)**2 for x, y in zip(a, b)) / len(a)) / 32768
    # Check observed repeatability, not a bit-exact guarantee for third-party DSP.
    require(difference < 0.0001, "reload audio differs beyond fixture tolerance")
    _, soft = load(root / "verified-soft")
    _, loud = load(root / "verified-loud")
    _, pedal = load(root / "verified-pedal")
    for samples in (soft, loud, pedal):
        require(rms(samples, 0, .52) == 0, "note sounded before its scheduled block")
        require(rms(samples, .55, 1) > .001, "note missing after scheduled onset")
    velocity_ratio = rms(loud, .55, 1) / rms(soft, .55, 1)
    pedal_ratio = rms(pedal, 1.3, 1.9) / rms(loud, 1.3, 1.9)
    require(velocity_ratio > 2, "attack velocity has no measured effect")
    require(pedal_ratio > 3, "sustain does not hold the released note")
    print(json.dumps({"result": "PASS", "reload_rms_relative_difference": delta,
                      "reload_difference_rms": difference, "loud_soft_rms_ratio": velocity_ratio,
                      "pedal_dry_release_rms_ratio": pedal_ratio}, indent=2))


if __name__ == "__main__":
    main()
