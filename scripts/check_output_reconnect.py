#!/usr/bin/env python3
"""Opt-in macOS CLI reconnect acceptance; output opens paused, never plays.

Uses a temporary bundle; no global device settings, plugins or workflows change.
Device-loss detection is tested separately with injected snapshots in CTest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--stream", action="store_true")
    args = parser.parse_args()
    source, build = args.bundle.resolve(), args.build.resolve()

    def hashes():
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.is_file()}

    before = hashes()
    with tempfile.TemporaryDirectory(prefix="daw-output-reconnect-") as temporary:
        root = Path(temporary)/"bundle"
        shutil.copytree(source, root)
        command = [str(build/"daw_session_play")]
        if args.stream:
            command.append("--stream")
        command.append(str(root/"session.dawsession"))
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as child:
            try:
                # No newline: exercise nonblocking partial input, then resume it.
                child.stdin.write("discon"); child.stdin.flush(); time.sleep(.35)
                commands = ("nect\ndevices\nseek 1.25\ngain cello -9\nstatus\n"
                            "play\nreconnect 4294967295\nstatus\n"
                            "reconnect 0\nstatus\nreconnect\nstatus\nundo\nredo\n"
                            "save reconnected.dawsession\ndisconnect\nstop\nstatus\n"
                            "reconnect -1\nreconnect 0 extra\nstatus")  # Deliberate EOF without newline.
                stdout, stderr = child.communicate(commands, timeout=30)
            except BaseException:
                child.kill(); child.wait(); raise
        assert child.returncode == 0, stderr
        assert stdout.count("Paused 1.25s") == 4, stdout
        assert stdout.count("Paused 0s") == 2, stdout
        assert "Output reconnected, playback remains paused at 1.25s" in stdout, stdout
        assert "output running=1" in stdout and "output running=0" in stdout
        assert "callback errors=0" in stdout
        assert "use reconnect before play" in stderr, stderr
        assert "Reconnect failed; playback remains paused" in stderr, stderr
        assert "device ID must be an unsigned integer" in stderr, stderr
        assert "unexpected extra arguments" in stderr, stderr
        assert "mix revision=3, undo=1, redo=0" in stdout, stdout
        reference = subprocess.run([str(build/"daw_session_edit"), str(root/"session.dawsession"),
                                    str(root/"expected.dawsession"), "--gain", "cello", "-9"],
                                   capture_output=True, text=True, timeout=10)
        assert reference.returncode == 0, reference.stderr
        assert (root/"reconnected.dawsession").read_bytes() == (root/"expected.dawsession").read_bytes()
    assert before == hashes(), "source bundle changed"
    print(json.dumps({"result": "PASS", "mode": "stream" if args.stream else "resident",
                      "paused_reconnect_position_mix_history_save": True,
                      "failed_reconnect_retains_edits": True, "partial_input_and_eof": True,
                      "system_device_settings_changed": False}, indent=2))


if __name__ == "__main__":
    main()
