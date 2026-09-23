#!/usr/bin/env python3
"""Verify real frozen duet relocation without plugins, audio hardware or network.

The supplied bundle is read-only. All edits, collections and bounces use a
temporary directory. Requires macOS native playback/render tools.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def require(ok, message):
    if not ok:
        raise ValueError(message)


def hashes(folder):
    result = {}
    for path in folder.rglob("*"):
        if path.is_file():
            digest = hashlib.sha256()
            with path.open("rb") as handle:
                for chunk in iter(lambda: handle.read(65536), b""):
                    digest.update(chunk)
            result[str(path.relative_to(folder))] = digest.hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--build", type=Path, default=Path("build"))
    args = parser.parse_args()
    build = args.build.resolve()
    source_hashes = hashes(args.bundle)
    checks = 0

    def run(tool, *arguments, failure=None):
        nonlocal checks
        result = subprocess.run([str(build / tool), *map(str, arguments)],
                                capture_output=True, text=True, timeout=120)
        require(result.returncode == (1 if failure else 0), result.stdout + result.stderr)
        if failure:
            require(failure in result.stderr, "unexpected error: " + result.stderr)
        checks += 1
        return result.stdout

    with tempfile.TemporaryDirectory(prefix="daw-collection-cli-") as folder:
        root = Path(folder)
        source = root / "original"
        shutil.copytree(args.bundle, source)
        edited = source / "saved cello solo.dawsession"
        run("daw_session_edit", source / "session.dawsession", edited,
            "--gain", "cello", "-9", "--balance", "cello", "-.35",
            "--solo", "cello", "1", "--mute", "piano", "1", "--master", "-5")
        reference_mix = run("daw_session_edit", "--show", edited)
        reference_play = json.loads(run("daw_session_play", "--stream-check", edited))
        require(reference_play["tracks"] == 2 and reference_play["rms"] > 0,
                "expected audible two-track frozen duet")
        run("daw_session_render", "--stream-frozen", edited, root / "before")
        before = hashes(root / "before")

        collected = root / "collected"
        run("daw_session_collect", edited, collected)
        expected = {"score.dawproj", "session.dawsession", "track-1.aupreset",
                    "track-2.aupreset", "track-1.dawfreeze", "track-2.dawfreeze"}
        copied = hashes(collected)
        require(set(copied) == expected, "collection includes unreferenced exports or misses dependencies")
        for name in expected - {"session.dawsession"}:
            require(copied[name] == source_hashes[name], "dependency bytes changed: " + name)
        require(not Path(str(collected) + ".collecting").exists(), "staging leaked")

        # Neither old directory remains at its original path.
        source.rename(root / "original hidden")
        moved = root / "移动后的二重奏"
        collected.rename(moved)
        entry = moved / "session.dawsession"
        run("daw_session_collect", "--check", entry)
        require(run("daw_session_edit", "--show", entry) == reference_mix, "mix targets changed")
        playback = json.loads(run("daw_session_play", "--stream-check", entry))
        require(playback == reference_play, "offline playback changed after relocation")
        run("daw_session_render", "--stream-frozen", entry, root / "after")
        require(hashes(root / "after") == before, "relocated export differs byte-for-byte")
        run("daw_session_collect", entry, moved, failure="already exists")
        require(hashes(moved) == copied, "existing target modified")
    require(hashes(args.bundle) == source_hashes, "original bundle modified")
    print(json.dumps({"result": "PASS", "cli_operations": checks, "plugin_loads": 0,
                      "audio_devices_opened": 0, "relocated_playback": playback,
                      "relocated_export_bundle_bit_exact": True,
                      "saved_mix_preserved": True, "source_hashes_unchanged": True}, indent=2))


if __name__ == "__main__":
    main()
