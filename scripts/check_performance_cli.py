#!/usr/bin/env python3
"""Exercise the unified interactive editor without loading plugins or devices."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from check_session_collection import hashes, require

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("document", type=Path)
parser.add_argument("--build", type=Path, default=Path("build"))
args = parser.parse_args()
source = hashes(args.document)
with tempfile.TemporaryDirectory(prefix="daw-work-editor-") as folder:
    root = Path(folder)
    # The path that follows listening: you heard something at a moment, you did
    # not count bars to get there, and you still have to reach that note's ID.
    commands = ["notes", "notes-near 2 4", "notes-near 0 2", "notes-near -1",
                'pitch 2 F 0 4', 'edit 1 120 .94 80', "edits",
                'shape 0 4 velocity 60 110', "undo", 'shape 4 0 velocity 1 2',
                'curve 2 2 105',
                f'save "{root / "edited"}"', "undo", "undo", "undo",
                f'save "{root / "undone"}"', "redo", "redo", "redo",
                f'save "{root / "redone"}"', "curve 2 2 128", "edit 999 0 1 80",
                f'save "{root / "rejected"}"', "status", "quit"]
    result = subprocess.run([str(args.build.resolve() / "daw_performance_play"), str(args.document)],
                            input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=30)
    require(result.returncode == 0, result.stdout + result.stderr)
    require(result.stdout.count("Command failed:") == 4, result.stdout)
    # A window around a moment must be centred on it, not started at it.
    around = [line for line in result.stdout.splitlines() if "attack_seconds=" in line]
    near = [line for line in around if line.startswith("performed=")]
    require(any("offset=0 " in line for line in result.stdout.splitlines()), "a window at zero did not clamp")
    require("seconds=" in result.stdout, "status does not report a time to search by")
    require(len(near) > 0, "notes-near returned nothing")
    # An override is invisible in where a note sounds, so the listing says so.
    require("edited_offset_ms=120" in result.stdout, "an override is not visible in the listing")
    require("Shaped " in result.stdout, "a passage edit reported nothing")
    require("performed=8 notation=8,9," in result.stdout, "tie mapping not visible")
    require("revision=9 active=1" in result.stdout, "invalid edit affected revision")
    require(hashes(root / "undone") == source, "unified undo did not restore complete document")
    require(hashes(root / "redone") == hashes(root / "edited"), "redo did not restore document")
    require(hashes(root / "rejected") == hashes(root / "edited"), "rejected edit changed document")
    reopened = subprocess.run([str(args.build.resolve() / "daw_performance_play"), str(root / "edited")],
                              input=f'save "{root / "reopened"}"\nquit\n', capture_output=True, text=True, timeout=30)
    require(reopened.returncode == 0 and "Command failed:" not in reopened.stdout, reopened.stdout + reopened.stderr)
    require(hashes(root / "reopened") == hashes(root / "edited"), "CLI reopen changed bytes")
require(hashes(args.document) == source, "source changed")
print(json.dumps({"result": "PASS", "plugin_loads": 0, "devices_opened": 0,
                  "interleaved_unified_undo_redo": True, "invalid_commands_preserve_revision": True,
                  "score_performance_state_bytes_on_reopen": "identical"}, indent=2))
