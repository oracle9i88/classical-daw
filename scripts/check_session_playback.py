#!/usr/bin/env python3
"""Plugin/device-free acceptance against the actual saved piano/cello duet.

Copies sources to a temporary directory, leaves supplied bundle unchanged.
Usage: python3 scripts/check_session_playback.py BUNDLE [BUILD_DIRECTORY]
"""
import array
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave


def main():
    source = Path(sys.argv[1]).resolve()
    repo = Path(__file__).resolve().parents[1]
    build = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else repo / "build"
    calls = 0

    def run(name, *args, fail=None):
        nonlocal calls
        calls += 1
        result = subprocess.run([str(build / name), *map(str, args)], capture_output=True, text=True, timeout=60)
        if fail:
            assert result.returncode != 0 and fail in result.stderr, result
        else:
            assert result.returncode == 0, result.stderr
        return result.stdout

    def hashes():
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.is_file()}

    before = hashes()
    with tempfile.TemporaryDirectory(prefix="daw-session-play-") as tmp:
        root = Path(tmp) / "bundle"
        shutil.copytree(source, root)
        session = root / "session.dawsession"

        def check(path):
            return json.loads(run("daw_session_play", "--check", path))

        initial = check(session)
        assert initial["tracks"] == 2 and initial["frames"] == initial["final_frame"]
        assert not initial["playing"] and initial["clipped_samples"] == 0
        # Independent quantized WAV evidence from the original offline renderer.
        with wave.open(str(root / "mix.wav"), "rb") as wav:
            assert wav.getframerate() == 48000 and wav.getnchannels() == 2 and wav.getsampwidth() == 2
            assert wav.getnframes() == initial["frames"]
            pcm = array.array("h", wav.readframes(wav.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
        peak = max(abs(v) for v in pcm) / 32767
        rms = math.sqrt(sum((v / 32767) ** 2 for v in pcm) / (len(pcm) + 480))
        assert abs(initial["peak"] - peak) < 2 / 32767
        assert abs(initial["rms"] - rms) < 2 / 32767

        lowered = root / "lowered.dawsession"
        # This checker expects the canonical duet's -3 dB master.
        assert "master_gain_db -3\n" in session.read_text()
        run("daw_session_edit", session, lowered, "--master", -9)
        low = check(lowered)
        assert abs(low["peak"] / initial["peak"] - 10 ** (-6 / 20)) < 1e-7
        assert abs(low["rms"] / initial["rms"] - 10 ** (-6 / 20)) < 1e-7

        silent = root / "silent.dawsession"
        run("daw_session_edit", session, silent, "--mute", "piano", 1, "--mute", "cello", 1)
        muted = check(silent)
        assert muted["peak"] == muted["rms"] == 0 and muted["final_frame"] == initial["frames"]
        solo = root / "solo.dawsession"
        run("daw_session_edit", session, solo, "--solo", "cello", 1)
        solo_result = check(solo)
        assert solo_result["rms"] > 0 and solo_result["rms"] != initial["rms"]

        score = root / "score.dawproj"
        original = score.read_bytes()
        assert b"velocity 63\n" in original
        score.write_bytes(original.replace(b"velocity 63\n", b"velocity 62\n", 1))
        run("daw_session_play", "--check", session, fail="stale")
        score.write_bytes(original)
        cache = root / "track-2.dawfreeze"
        original = cache.read_bytes()
        cache.write_bytes(original[:-3])
        run("daw_session_play", "--check", session, fail="truncated")
        cache.write_bytes(original)
        cache.rename(root / "cache-backup")
        cache.symlink_to(root / "cache-backup")
        run("daw_session_play", "--check", session, fail="symlink")
        cache.unlink()
        (root / "cache-backup").rename(cache)
        # End-to-end overload detection; the check must fail, not silently bless a clamp.
        hot = root / "hot.dawsession"
        run("daw_session_edit", session, hot, "--gain", "piano", 12, "--gain", "cello", 12, "--master", 12)
        result = subprocess.run([str(build / "daw_session_play"), "--check", str(hot)], capture_output=True, text=True, timeout=60)
        calls += 1
        assert result.returncode == 1 and json.loads(result.stdout)["clipped_samples"] > 0
        assert hashes() == before, "source bundle changed"
    print(json.dumps({"result": "PASS", "cli_calls": calls, "initial": initial,
                      "plugins_loaded": False, "audio_device_opened": False}, indent=2))


if __name__ == "__main__":
    main()
