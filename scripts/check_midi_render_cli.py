#!/usr/bin/env python3
"""Run the 14 MIDI render CLI checks using only the Python standard library.

Uses one tiny generated SMF in a temporary directory. Checks rejected rates,
malformed input, existing/dangling output paths, and successful default/min/max
rates with matching WAV/JSON metadata. The input MIDI must remain unchanged.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import wave


def verify(renderer, directory):
    # CC1, middle-C attack, note-off at tick 960 with release velocity 64, EOT.
    track = bytes.fromhex("00 b0 01 20 00 90 3c 64 87 40 80 3c 40 00 ff 2f 00")
    source = directory / "input.mid"
    source.write_bytes(bytes.fromhex("4d546864000000060000000103c0")
                       + b"MTrk" + len(track).to_bytes(4, "big") + track)
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()

    def run(input_path, output_path, rate=None):
        command = [str(renderer), str(input_path), str(output_path)]
        if rate is not None:
            command.append(rate)
        return subprocess.run(command, capture_output=True, text=True)

    checks = 0
    for rate in ("0", "7999", "192001", "8000.0", "8000x", "", "-8000", "99999999999999999999999"):
        output = directory / f"invalid-rate-{checks}"
        result = run(source, output, rate)
        assert result.returncode != 0 and not output.exists(), (rate, result.stdout, result.stderr)
        checks += 1

    malformed = directory / "malformed.mid"
    malformed.write_bytes(b"not MIDI")
    output = directory / "malformed-output"
    result = run(malformed, output)
    assert result.returncode != 0 and not output.exists(), result.stderr
    checks += 1

    existing = directory / "existing"
    existing.mkdir()
    sentinel = existing / "diagnostic.wav"
    sentinel.write_bytes(b"existing-user-data")
    result = run(source, existing)
    assert result.returncode != 0 and sentinel.read_bytes() == b"existing-user-data", result.stderr
    assert list(existing.iterdir()) == [sentinel], "existing output directory changed"
    checks += 1

    link = directory / "dangling-output"
    missing_target = directory / "missing-link-target"
    link.symlink_to(missing_target)
    result = run(source, link)
    assert result.returncode != 0 and link.is_symlink() and not missing_target.exists(), result.stderr
    checks += 1

    for rate in (None, "8000", "192000"):
        output = directory / ("valid-" + (rate or "default"))
        result = run(source, output, rate)
        assert result.returncode == 0, result.stderr
        report = json.loads((output / "report.json").read_text())
        expected_rate = int(rate or 48000)
        assert report["sample_rate"] == expected_rate and report["renderer"] == "sine_diagnostic"
        assert report["import_diagnostics"]["preserved_channel_events"] == 1
        assert set(report["render_diagnostics"]) == {
            "interpreted_channel_events", "unsupported_channel_events", "voices_released_at_end",
            "ignored_release_velocities", "clipped_samples",
        }
        with wave.open(str(output / "diagnostic.wav"), "rb") as audio:
            assert audio.getframerate() == expected_rate
            assert audio.getnframes() == report["frames"] and audio.getnchannels() == report["channels"]
        assert "not an orchestral sampler" in result.stderr
        assert "unsupported channel events:" in result.stderr
        checks += 1

    assert hashlib.sha256(source.read_bytes()).hexdigest() == original_hash, "input MIDI changed"
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, default=Path("build/daw_midi_render"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="classical-daw-midi-render-cli-") as temporary:
        checks = verify(args.renderer.resolve(), Path(temporary))
    print(f"PASS: {checks} MIDI render CLI checks; source SHA-256 unchanged")


if __name__ == "__main__":
    main()
