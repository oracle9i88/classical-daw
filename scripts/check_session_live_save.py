#!/usr/bin/env python3
"""Opt-in native CLI save/reopen check; opens the output device but never plays.

Usage: python3 scripts/check_session_live_save.py BUNDLE [BUILD_DIRECTORY] [--stream]
Works on a temporary copy of the canonical piano/cello bundle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("build", type=Path, nargs="?", default=repo / "build")
    parser.add_argument("--stream", action="store_true")
    options = parser.parse_args()
    source, build = options.bundle.resolve(), options.build.resolve()

    def hashes():
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.is_file()}

    def run(name, *args, stdin=None):
        if options.stream and name == "daw_session_play":
            args = ("--stream-check", *args[1:]) if args[0] == "--check" else ("--stream", *args)
        result = subprocess.run([str(build / name), *map(str, args)], input=stdin,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        return result

    before = hashes()
    with tempfile.TemporaryDirectory(prefix="daw-live-save-") as temp:
        root = Path(temp) / "bundle"
        shutil.copytree(source, root)
        session = root / "session.dawsession"
        commands = ("undo\nsolo cello 1\ngain cello -2\nbalance cello -0.3\nmaster -1\n"
                    "gain missing 2\nmute piano 0.5\nmix\n"
                    "undo\nsave undone.dawsession\nredo\nsave live.dawsession\nsave live.dawsession\n"
                    "undo\nmaster -3\nredo\nundo\ngain cello -4\nredo\nsave branch.dawsession\nstatus\nquit\n")
        live = run("daw_session_play", session, stdin=commands)
        assert "Saved accepted mix settings:" in live.stdout
        assert "Paused 0s" in live.stdout and "callback errors=0" in live.stdout
        assert "unknown part ID" in live.stderr and "mute/solo must be 0 or 1" in live.stderr
        assert "destination session already exists" in live.stderr
        assert "no mix edit to undo" in live.stderr and "no mix edit to redo" in live.stderr
        assert "mix revision=10, undo=1, redo=0" in live.stdout
        run("daw_session_edit", session, root / "expected.dawsession", "--solo", "cello", 1,
            "--gain", "cello", -2, "--balance", "cello", -0.3, "--master", -1)
        assert (root / "live.dawsession").read_bytes() == (root / "expected.dawsession").read_bytes()
        run("daw_session_edit", session, root / "expected-undone.dawsession", "--solo", "cello", 1,
            "--gain", "cello", -2, "--balance", "cello", -0.3)
        assert (root / "undone.dawsession").read_bytes() == (root / "expected-undone.dawsession").read_bytes()
        run("daw_session_edit", session, root / "expected-branch.dawsession", "--solo", "cello", 1,
            "--gain", "cello", -4, "--balance", "cello", -0.3)
        assert (root / "branch.dawsession").read_bytes() == (root / "expected-branch.dawsession").read_bytes()
        reopened = json.loads(run("daw_session_play", "--check", root / "live.dawsession").stdout)
        assert reopened["rms"] > 0 and reopened["clipped_samples"] == 0 and not reopened["playing"]
        assert reopened["final_frame"] == reopened["frames"]
        undone = json.loads(run("daw_session_play", "--check", root / "undone.dawsession").stdout)
        branch = json.loads(run("daw_session_play", "--check", root / "branch.dawsession").stdout)
        assert abs(undone["rms"] / reopened["rms"] - 10 ** (-2 / 20)) < 1e-8
        assert abs(branch["rms"] / undone["rms"] - 10 ** (-2 / 20)) < 1e-8
        reopened_cli = run("daw_session_play", root / "undone.dawsession", stdin="status\nundo\nquit\n")
        assert "mix revision=0, undo=0, redo=0" in reopened_cli.stdout
        assert "no mix edit to undo" in reopened_cli.stderr
        assert not list(root.glob("*.saving")), "staging residue left behind"
    assert before == hashes(), "source bundle modified"
    print(json.dumps({"result": "PASS", "saved_settings_match_offline_editor": True,
                      "native_start_paused": True, "undo_redo_branch_save_reload": "PASS",
                      "reopened": reopened}, indent=2))


if __name__ == "__main__":
    main()
