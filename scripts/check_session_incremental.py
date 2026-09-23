#!/usr/bin/env python3
"""Incremental rendering regression on a read-only captured piano/cello duet.

Default: no plugins or audio hardware. --render-changed opts into ONE real
incremental bounce: changed piano loads Pianoteq, unchanged cello uses its cache.
All mutations and output use temporary copies; no network or workflows.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from check_session_collection import hashes, require


def payload(path):
    data = path.read_bytes()
    require(data[:8] == b"DAWFRZ01", "bad cache signature")
    offset = 8
    for _ in range(2):
        size, = struct.unpack_from("<I", data, offset)
        offset += 4 + size
    return data[offset + 16:-4]  # exact float payload, excluding new identity/CRC


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--render-changed", action="store_true")
    args = parser.parse_args()
    build = args.build.resolve()
    original_hashes = hashes(args.bundle)
    checks = 0

    def run(tool, *arguments, failure=None):
        nonlocal checks
        result = subprocess.run([str(build / tool), *map(str, arguments)],
                                capture_output=True, text=True, timeout=180)
        require(result.returncode == (1 if failure else 0), result.stdout + result.stderr)
        if failure:
            require(failure in result.stderr, result.stderr)
        checks += 1
        return result

    with tempfile.TemporaryDirectory(prefix="daw-incremental-cli-") as folder:
        root = Path(folder)
        old, current = root / "previous", root / "current"
        shutil.copytree(args.bundle, old)
        shutil.copytree(args.bundle, current)
        old_entry, entry = old / "session.dawsession", current / "session.dawsession"
        score_path = current / "score.dawproj"
        original_score, original_session = score_path.read_text(), entry.read_text()
        require('route "piano"' in original_session and 'route "cello"' in original_session,
                "expected piano/cello fixture")

        def plan(reuse, render):
            result = run("daw_session_render", "--incremental-check", old_entry, entry)
            require(f"reuse={reuse} render={render}" in result.stdout, result.stdout + result.stderr)
            require("Streaming instrument:" not in result.stderr, "preflight loaded a plugin")
            return result

        def bounce(name, failure=None):
            output = root / name
            result = run("daw_session_render", "--incremental", old_entry, entry, output, failure=failure)
            if failure:
                require(not output.exists(), "failed incremental render left output")
            return output, result

        plan(2, 0)
        reference = root / "reference"
        run("daw_session_render", "--stream-frozen", old_entry, reference)
        unchanged, result = bounce("unchanged")
        require("Streaming instrument:" not in result.stderr, "unchanged bounce loaded instrument")
        require(hashes(unchanged) == hashes(reference), "unchanged incremental differs from strict frozen export")

        # Metadata changes require NEW cache identity, but not new samples.
        notation = original_score.replace("name 5069616e6f\n", "name 52656e616d6564\n", 1)
        require(notation != original_score, "fixture piano name missing")
        score_path.write_text(notation)
        plan(2, 0)
        renamed, result = bounce("notation")
        require("Streaming instrument:" not in result.stderr, "notation-only edit loaded instrument")
        for i in (1, 2):
            name = f"track-{i}.dawfreeze"
            require(payload(renamed / name) == payload(old / name), "notation edit changed captured audio")
            require((renamed / name).read_bytes() != (old / name).read_bytes(), "identity was not rebound")
        require(hashes(renamed)["mix.wav"] == original_hashes["mix.wav"], "notation edit changed mix")
        run("daw_session_collect", "--check", renamed / "session.dawsession")
        run("daw_session_play", "--stream-check", renamed / "session.dawsession")

        # Only first piano attack changes; current stale cache must NOT be used.
        changed_score = original_score.replace("pitch_alter 0\n", "pitch_alter 1\n", 1)
        require(changed_score != original_score, "fixture first note missing")
        score_path.write_text(changed_score)
        result = plan(1, 1)
        require('RENDER "piano"' in result.stderr and 'REUSE "cello"' in result.stderr, "wrong part invalidated")
        run("daw_session_render", "--stream-frozen", entry, root / "stale-strict", failure="source identity mismatch")
        require(not (root / "stale-strict").exists(), "strict frozen failure left output")
        if args.render_changed:
            changed, result = bounce("one-changed")
            require("Streaming instrument: piano" in result.stderr and "Streaming instrument: cello" not in result.stderr,
                    "mixed bounce instantiated wrong instrument")
            report = json.loads((changed / "report.json").read_text())
            require([s["audio_source"] for s in report["stems"]] == ["plugin", "frozen"], "report provenance wrong")
            require(report["stems"][0]["peak"] > 0, "changed piano silent")
            require(payload(changed / "track-2.dawfreeze") == payload(old / "track-2.dawfreeze"), "cello performance changed")
            require(hashes(changed)["track-2.wav"] == original_hashes["track-2.wav"], "cello stem changed")
            require(hashes(changed)["track-1.wav"] != original_hashes["track-1.wav"], "piano edit did not reach audio")
            run("daw_session_collect", "--check", changed / "session.dawsession")
            run("daw_session_play", "--stream-check", changed / "session.dawsession")
            rerun = root / "refrozen"
            run("daw_session_render", "--stream-frozen", changed / "session.dawsession", rerun)
            left, right = hashes(changed), hashes(rerun)
            for name in left.keys() - {"report.json"}:
                require(left[name] == right[name], "new frozen re-export changed " + name)

        score_path.write_text(original_score.replace("bpm 84\n", "bpm 85\n", 1))
        plan(0, 2)
        score_path.write_text(original_score)
        piano_state = current / "track-1.aupreset"
        original_state = piano_state.read_bytes()
        piano_state.write_bytes(original_state + b"different")
        result = plan(1, 1)
        require("instrument state changed" in result.stderr, "state change did not invalidate")
        piano_state.write_bytes(original_state)

        # Stable IDs, not route indices or filenames, select the previous cache.
        lines = original_session.splitlines()
        indices = [i for i, line in enumerate(lines) if line.startswith("route ")]
        require(len(indices) == 2, "expected two routes")
        lines[indices[0]], lines[indices[1]] = lines[indices[1]], lines[indices[0]]
        entry.write_text("\n".join(lines) + "\n")
        plan(2, 0)
        reordered, _ = bounce("reordered")
        require(payload(reordered / "track-1.dawfreeze") == payload(old / "track-2.dawfreeze"), "reordered cello misrouted")
        require(payload(reordered / "track-2.dawfreeze") == payload(old / "track-1.dawfreeze"), "reordered piano misrouted")
        entry.write_text(original_session)

        previous_cache = old / "track-2.dawfreeze"
        good_cache = previous_cache.read_bytes()
        corrupt = bytearray(good_cache); corrupt[-1] ^= 1
        previous_cache.write_bytes(corrupt)
        bounce("corrupt", failure="checksum mismatch")
        previous_cache.write_bytes(good_cache)
        previous_score = old / "score.dawproj"
        previous_score.write_text(notation)
        bounce("stale-previous", failure="source identity mismatch")
        previous_score.write_text(original_score)
        previous_cache.rename(old / "hidden-cache")
        bounce("missing-previous", failure="regular file")
        (old / "hidden-cache").rename(previous_cache)
        run("daw_session_render", "--incremental", old_entry, entry, unchanged, failure="must be new")
        require(hashes(unchanged) == hashes(reference), "existing output overwritten")
    require(hashes(args.bundle) == original_hashes, "original source changed")
    print(json.dumps({"result": "PASS", "cli_operations": checks,
                      "real_changed_piano_bounces": int(args.render_changed),
                      "cello_plugin_loads": 0, "audio_devices_opened": 0,
                      "unchanged_bundle_bit_exact": True, "notation_rebind_bit_exact_audio": True,
                      "state_tempo_note_invalidation": True, "source_hashes_unchanged": True}, indent=2))


if __name__ == "__main__":
    main()
