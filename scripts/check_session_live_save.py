#!/usr/bin/env python3
"""Opt-in native CLI save/reopen check; opens the output device but never plays.

Usage: python3 scripts/check_session_live_save.py BUNDLE [BUILD_DIRECTORY]
Works on a temporary copy of the canonical piano/cello bundle.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    source = Path(sys.argv[1]).resolve()
    repo = Path(__file__).resolve().parents[1]
    build = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else repo / "build"

    def hashes():
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.is_file()}

    def run(name, *args, stdin=None):
        result = subprocess.run([str(build / name), *map(str, args)], input=stdin,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        return result

    before = hashes()
    with tempfile.TemporaryDirectory(prefix="daw-live-save-") as temp:
        root = Path(temp) / "bundle"
        shutil.copytree(source, root)
        session = root / "session.dawsession"
        commands = ("solo cello 1\ngain cello -2\nbalance cello -0.3\nmaster -1\n"
                    "gain missing 2\nmute piano 0.5\nmix\n"
                    "save live.dawsession\nsave live.dawsession\nstatus\nquit\n")
        live = run("daw_session_play", session, stdin=commands)
        assert "Saved accepted mix settings:" in live.stdout
        assert "Paused 0s" in live.stdout and "callback errors=0" in live.stdout
        assert "unknown part ID" in live.stderr and "mute/solo must be 0 or 1" in live.stderr
        assert "destination session already exists" in live.stderr
        run("daw_session_edit", session, root / "expected.dawsession", "--solo", "cello", 1,
            "--gain", "cello", -2, "--balance", "cello", -0.3, "--master", -1)
        assert (root / "live.dawsession").read_bytes() == (root / "expected.dawsession").read_bytes()
        reopened = json.loads(run("daw_session_play", "--check", root / "live.dawsession").stdout)
        assert reopened["rms"] > 0 and reopened["clipped_samples"] == 0 and not reopened["playing"]
        assert reopened["final_frame"] == reopened["frames"]
        assert not list(root.glob("*.saving")), "staging residue left behind"
    assert before == hashes(), "source bundle modified"
    print(json.dumps({"result": "PASS", "saved_settings_match_offline_editor": True,
                      "native_start_paused": True, "reopened": reopened}, indent=2))


if __name__ == "__main__":
    main()
