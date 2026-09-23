#!/usr/bin/env python3
"""Session CLI preflight/failure checks. No AU is loaded; no network is used."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    build = parser.parse_args().build.resolve()
    renderer = build / "daw_session_render"
    count = 0
    with tempfile.TemporaryDirectory(prefix="daw-session-check-") as folder:
        root = Path(folder) / "fixture"
        subprocess.run([str(build / "daw_session_fixture"), str(root)], check=True)
        source = (root / "duet.dawsession").read_text()

        def check(text, expected, output=None):
            nonlocal count
            path = root / "case.dawsession"
            path.write_text(text)
            args = [str(renderer), "--check", str(path)] if output is None else [str(renderer), str(path), str(output)]
            result = subprocess.run(args, capture_output=True, text=True)
            if result.returncode != (0 if expected is None else 1):
                raise ValueError(f"unexpected CLI status: {result.stdout} {result.stderr}")
            if expected is not None and expected not in result.stderr:
                raise ValueError(f"missing expected error {expected}: {result.stderr}")
            if expected is None and "Plugins were not loaded or validated" not in result.stdout:
                raise ValueError("check mode did not report its validation boundary")
            count += 1

        check(source, None)
        check(source + "garbage", "trailing session content")
        check(source.replace('score "score.dawproj"', 'score "../score.dawproj"'), "sibling filename")
        check(source.replace('route "piano"', 'route "unknown"'), "unknown score part")
        missing = source.replace('"Cello" ""', '"" "missing.aupreset"')
        check(missing, "input must be a regular file")
        (root / "missing.aupreset").symlink_to(root / "score.dawproj")
        check(missing, "input must be a regular file")
        existing = root / "existing"
        existing.mkdir()
        (existing / "sentinel").write_text("preserve")
        check(source, "output directory must be new", existing)
        if (existing / "sentinel").read_text() != "preserve":
            raise ValueError("existing output was modified")
        dangling = root / "dangling"
        dangling.symlink_to(root / "absent")
        check(source, "output directory must be new", dangling)
        if not dangling.is_symlink():
            raise ValueError("existing symlink was modified")
    print(f"PASS {count} session CLI checks; no plugins loaded")


if __name__ == "__main__":
    main()
