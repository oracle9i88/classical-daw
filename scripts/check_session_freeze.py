#!/usr/bin/env python3
"""Exercise mix editing and frozen reuse in a temporary copy; never loads plugins.

Input: a completed two-part daw_session_fixture bounce with .dawfreeze files.
The supplied source bundle is read-only. No network or workflow is invoked.
"""
import argparse
import array
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import zlib


def require(ok, message):
    if not ok:
        raise ValueError(message)


def pcm(path):
    with wave.open(str(path), "rb") as wav:
        require((wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (48000, 2, 2), "bad PCM format")
        result = array.array("h", wav.readframes(wav.getnframes()))
    if sys.byteorder != "little":
        result.byteswap()
    return result


def frozen(path):
    data = path.read_bytes()
    require(data[:8] == b"DAWFRZ01", "bad freeze signature")
    require(zlib.crc32(data[:-4]) == struct.unpack_from("<I", data, len(data) - 4)[0], "independent CRC check failed")
    offset = 8
    for _ in range(2):  # length-prefixed source identity and preset name
        count, = struct.unpack_from("<I", data, offset)
        offset += 4 + count
    _, rate, channels, frames = struct.unpack_from("<IIII", data, offset)
    offset += 16
    require((rate, channels) == (48000, 2) and offset + frames * 8 + 4 == len(data), "freeze size/format")
    values = array.array("f", data[offset:-4])
    if sys.byteorder != "little":
        values.byteswap()
    require(all(math.isfinite(v) for v in values), "non-finite freeze")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--build", type=Path, default=Path("build"))
    args = parser.parse_args()
    build = args.build.resolve()
    editor, renderer = build / "daw_session_edit", build / "daw_session_render"
    report = json.loads((args.bundle / "report.json").read_text())
    require([s["part_id"] for s in report["stems"]] == ["piano", "cello"], "expected original duet route order")
    for stem in report["stems"]:
        frozen(args.bundle / stem["frozen"])
    checks = 0
    with tempfile.TemporaryDirectory(prefix="daw-frozen-cli-") as folder:
        root = Path(folder)
        source = root / "source"
        shutil.copytree(args.bundle, source)

        def run(command, failure=None):
            nonlocal checks
            result = subprocess.run([str(v) for v in command], capture_output=True, text=True)
            require(result.returncode == (1 if failure else 0), result.stdout + result.stderr)
            if failure:
                require(failure in result.stderr, f"wrong failure: {result.stderr}")
            checks += 1
            return result

        def edit(parent, output_name, *options, failure=None):
            destination = parent / output_name
            run([editor, parent / "session.dawsession", destination, *options], failure)
            if failure:
                require(not destination.exists(), "failed edit left a session")
            return destination

        def bounce(session, name, failure=None):
            destination = root / name
            result = run([renderer, "--frozen", session, destination], failure)
            require("Rendering " not in result.stderr, "frozen path loaded an instrument")
            if failure:
                require(not destination.exists(), "failed bounce left a partial bundle")
            return destination

        unchanged = bounce(source / "session.dawsession", "unchanged")
        for name in ("mix.wav", "track-1.wav", "track-2.wav", "track-1.dawfreeze", "track-2.dawfreeze"):
            require((source / name).read_bytes() == (unchanged / name).read_bytes(), f"unchanged reuse altered {name}")
        solo = edit(source, "solo.dawsession", "--solo", "piano", "1", "--mute", "cello", "1")
        solo_out = bounce(solo, "solo")
        require(not any(pcm(solo_out / "track-2.wav")), "muted cello audible")
        require((solo_out / "track-1.wav").read_bytes() == (source / "track-1.wav").read_bytes(), "solo changed piano")
        mix, piano = pcm(solo_out / "mix.wav"), pcm(solo_out / "track-1.wav")
        master = 10 ** (report["master_gain_db"] / 20)
        require(max(abs(m - round(p * master)) for m, p in zip(mix, piano)) <= 1, "solo master contains another track")
        restored = edit(solo_out, "restored.dawsession", "--solo", "piano", "0", "--mute", "cello", "0")
        restored_out = bounce(restored, "restored")
        require((restored_out / "mix.wav").read_bytes() == (source / "mix.wav").read_bytes(), "unmute failed to restore exact mix")
        all_muted = edit(source, "all-muted.dawsession", "--solo", "piano", "1", "--mute", "piano", "1")
        silence = bounce(all_muted, "silence")
        require(not any(pcm(silence / "mix.wav")), "muted solo should silence entire master")
        moved = edit(source, "moved.dawsession", "--gain", "piano", "-10", "--balance", "piano", "1")
        moved_out = bounce(moved, "moved")
        moved_pcm = pcm(moved_out / "track-1.wav")
        require(not any(moved_pcm[0::2]) and any(moved_pcm[1::2]), "right balance not applied")
        raw = frozen(source / "track-1.dawfreeze")
        gain = 10 ** (-10 / 20)
        require(max(abs(p - round(v * gain * 32767)) for p, v in zip(moved_pcm[1::2], raw[1::2])) <= 1,
                "gain not applied from pre-fader raw audio")
        show = run([editor, "--show", moved])
        require("gain=-10" in show.stdout and "balance=1" in show.stdout, "settings not persisted")
        edit(source, "bad-flag.dawsession", "--mute", "piano", "2", failure="0 or 1")
        edit(source, "bad-id.dawsession", "--solo", "absent", "1", failure="unknown part")
        edit(source, "bad-gain.dawsession", "--gain", "piano", "nan", failure="gain must be finite")
        edit(source, "bad-pan.dawsession", "--balance", "piano", "2", failure="stereo balance")
        sentinel = source / "existing.dawsession"
        sentinel.write_text("preserve")
        run([editor, source / "session.dawsession", sentinel], "output session must be new")
        require(sentinel.read_text() == "preserve", "existing edit target overwritten")
        run([editor, source / "session.dawsession", root / "outside.dawsession"], "must stay beside")
        for filename in ("score.dawproj", "track-1.aupreset"):
            path = source / filename
            original = path.read_bytes()
            try:
                changed = original.replace(b"bpm 84\n", b"bpm 85\n", 1) if filename == "score.dawproj" else original + b"\n"
                require(changed != original, "fixture mutation did not change input")
                path.write_bytes(changed)
                bounce(source / "session.dawsession", "stale-" + filename, "frozen audio is stale")
            finally:
                path.write_bytes(original)
        damaged = source / "track-2.dawfreeze"
        original = damaged.read_bytes()
        try:
            data = bytearray(original); data[-8] ^= 1; damaged.write_bytes(data)
            bounce(source / "session.dawsession", "corrupt", "checksum mismatch")
        finally:
            damaged.write_bytes(original)
        overload = edit(source, "overload.dawsession", "--gain", "piano", "1.5", "--gain", "cello", "0", "--master", "12")
        bounce(overload, "overload", "master exceeds PCM16 headroom")
    print(json.dumps({"result": "PASS", "cli_operations": checks, "plugin_loads": 0,
                      "unchanged_remix_bit_exact": True, "unmute_restores_bit_exact_mix": True,
                      "stale_corrupt_overload_cleanup": "PASS"}, indent=2))


if __name__ == "__main__":
    main()
