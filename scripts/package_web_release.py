#!/usr/bin/env python3
"""Package only committed public web sources, without running CI or uploading.

Usage: python3 scripts/package_web_release.py --output out/web-release
Serve the resulting directory or upload its contents to any static HTTPS host.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FILES = (
    "web/index.html", "web/styles.css", "web/app.js", "web/core.mjs",
    "web/editor-state.mjs", "web/formats.mjs", "web/audio-engine.mjs",
    "web/render-worker.mjs", "web/icon.svg", "web/README.md", "LICENSE",
)
REPOSITORY = "https://github.com/oracle9i88/classical-daw"


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    destination = args.output.resolve()
    archive = destination.with_suffix(".zip")
    if destination == archive or destination.exists() or archive.exists():
        parser.error("output directory or .zip already exists; choose a new name")
    revision = git("rev-parse", "HEAD").decode().strip()
    sources = {}
    for name in FILES:
        committed = git("show", f"{revision}:{name}")
        if (ROOT / name).read_bytes() != committed:
            parser.error(f"commit {name} before packaging; working source differs from HEAD")
        sources[name] = committed

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".web-release-", dir=destination.parent))
    archive_temporary = temporary / "release.zip"
    site = temporary / "site"
    site.mkdir()
    try:
        hashes = {}
        for name, data in sources.items():
            target = pathlib.Path(name).name
            if target == "index.html":
                data = data.replace(b"feature/web-public-alpha-20261007", revision.encode())
            if target == "README.md":
                data = data.replace(b"(../LICENSE)", b"(LICENSE)")
            (site / target).write_bytes(data)
            hashes[target] = digest(data)
        manifest = {
            "format": "classical-daw-web-release-1",
            "source_commit": revision,
            "source_url": f"{REPOSITORY}/tree/{revision}",
            "source_archive_url": f"{REPOSITORY}/archive/{revision}.zip",
            "license": "AGPL-3.0-or-later",
            "source_sha256": {name: digest(data) for name, data in sources.items()},
            "published_sha256": hashes,
            "transformations": ["pin HTML source/license links to source_commit",
                                "make packaged README license link local"],
            "private_assets_included": False,
        }
        (site / "release.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        with zipfile.ZipFile(archive_temporary, "w", compression=zipfile.ZIP_DEFLATED) as package:
            for item in sorted(site.iterdir()):
                info = zipfile.ZipInfo(item.name, (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                package.writestr(info, item.read_bytes())
        # Neither target is ever overwritten. Private scores, tests, plugins,
        # dependencies, Git metadata and local recordings are not on the allowlist.
        site.rename(destination)
        archive_temporary.rename(archive)
        print(json.dumps({"directory": str(destination), "archive": str(archive),
                          "source_commit": revision, "files": len(hashes) + 1,
                          "archive_sha256": digest(archive.read_bytes()),
                          "archive_bytes": archive.stat().st_size}, indent=2))
    finally:
        shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
