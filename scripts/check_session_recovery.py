#!/usr/bin/env python3
"""Opt-in macOS crash/recovery check. Real output opens paused; no notes play.

Usage: python3 scripts/check_session_recovery.py BUNDLE [BUILD_DIRECTORY] [--stream]
Uses a temporary bundle copy, kills only its own child process after an
acknowledged checkpoint, restores to a new session and compares offline output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading


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
    with tempfile.TemporaryDirectory(prefix="daw-crash-recovery-") as temp:
        root = Path(temp) / "bundle"
        shutil.copytree(source, root)
        session = root / "session.dawsession"
        # Seed bundles may already contain recovery copies from manual use.
        previous = set(root.glob("session.dawsession.mix-recovery-*"))
        process = subprocess.Popen([str(build / "daw_session_play"), *(["--stream"] if options.stream else []), str(session)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1)
        lines = queue.Queue()
        def capture():
            for line in process.stdout:
                lines.put(line)
            lines.put(None)
        reader = threading.Thread(target=capture, daemon=True)
        reader.start()
        transcript = []
        def until_status(revision, saved):
            while True:
                line = lines.get(timeout=20)
                assert line is not None, "child exited before checkpoint: " + "".join(transcript)
                transcript.append(line)
                if f"mix revision={revision}, undo=1, redo=0" in line or f"mix revision={revision}, undo=1, redo=1" in line:
                    assert "Paused 0s" in line and f"recovery saved revision={saved}" in line
                    return
        def send(commands):
            process.stdin.write(commands)
            process.stdin.flush()
        try:
            send("solo cello 1\ngain cello -2\nbalance cello -0.3\nmaster -1\nundo\nredo\nstatus\n")
            until_status(6, 6)
            assert "NOT saved" not in "".join(transcript), "".join(transcript)
            candidates = set(root.glob("session.dawsession.mix-recovery-*")) - previous
            assert len(candidates) == 1
            directory = candidates.pop()
            # Interrupt a checkpoint staging area to exercise the UI failure path.
            staging = directory / ".saving"
            staging.mkdir()
            (staging / "checkpoint.tmp").write_text("partial")
            send("gain cello -4\nstatus\n")
            until_status(7, 6)
            assert "recovery NOT saved at revision=7" in "".join(transcript)
            assert (staging / "checkpoint.tmp").read_text() == "partial"
            (staging / "checkpoint.tmp").unlink()
            staging.rmdir()
            send("recovery\nstatus\n")
            until_status(7, 7)
            send("undo\nstatus\n")
            until_status(8, 8)
            process.kill()  # No graceful shutdown, save command, or destructor.
            process.wait(timeout=10)
            assert process.returncode < 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            process.stdin.close()
            reader.join(timeout=5)
            process.stdout.close()
        candidates = set(root.glob("session.dawsession.mix-recovery-*")) - previous
        assert len(candidates) == 1
        directory = candidates.pop()
        listing = run("daw_session_recover", "--list", session)
        assert "revision=8 valid" in listing.stdout
        recovered = root / "recovered.dawsession"
        run("daw_session_recover", session, directory, recovered)
        expected = root / "expected.dawsession"
        run("daw_session_edit", session, expected, "--solo", "cello", 1,
            "--gain", "cello", -2, "--balance", "cello", -0.3, "--master", -1)
        assert recovered.read_bytes() == expected.read_bytes()
        audible = json.loads(run("daw_session_play", "--check", recovered).stdout)
        reference = json.loads(run("daw_session_play", "--check", expected).stdout)
        assert audible == reference and audible["rms"] > 0 and audible["clipped_samples"] == 0
        opened = run("daw_session_play", session, stdin="status\nquit\n")
        assert "Recovery candidates found:" in opened.stdout
        assert "mix revision=0, undo=0, redo=0" in opened.stdout  # Never silently loads recovery.
        assert session.read_bytes() == (source / "session.dawsession").read_bytes()
        assert (directory / "latest.mixrecovery").exists(), "opening source destroyed prior recovery"
    assert hashes() == before, "canonical source bundle changed"
    print(json.dumps({"result": "PASS", "forced_process_exit": True,
                      "checkpoint_revision": 8, "failed_write_retry": True, "source_unchanged": True,
                      "recovered_settings_and_audio_match": True, "reopened": audible}, indent=2))


if __name__ == "__main__":
    main()
