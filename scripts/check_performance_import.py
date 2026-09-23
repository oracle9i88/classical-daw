#!/usr/bin/env python3
"""Import a supplied original score, exercise reachable editor commands, no AU/device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def hashes(root):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir() if p.is_file()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('score', type=Path)
    ap.add_argument('piano_state', type=Path)
    ap.add_argument('--build', type=Path, default=Path('build'))
    args = ap.parse_args()
    source_hash = hashlib.sha256(args.score.read_bytes()).hexdigest()
    state_hash = hashlib.sha256(args.piano_state.read_bytes()).hexdigest()
    build = args.build.resolve()
    def run(tool, arguments, commands=None):
        r = subprocess.run([str(build/tool), *map(str, arguments)], input=commands, text=True, capture_output=True, timeout=60)
        require(r.returncode == 0, r.stdout+r.stderr)
        return r.stdout
    with tempfile.TemporaryDirectory(prefix='daw-original-import-') as temp:
        root = Path(temp)
        original = root/'original'
        import_output = run('daw_performance_import', [args.score, args.piano_state, original])
        original_hashes = hashes(original)
        # A failed overwrite must not touch the existing bundle.
        duplicate = subprocess.run([str(build/'daw_performance_import'), str(args.score), str(args.piano_state), str(original)], capture_output=True, text=True)
        require(duplicate.returncode != 0 and hashes(original) == original_hashes, 'overwrite changed original bundle')
        listing = run('daw_performance_play', [original], 'notes 0 1\nnotes 1 1\nquit\n')
        rows = re.findall(r'performed=(\d+) notation=[^\n]+', listing)
        require(rows, 'no contextual note rows')
        require(all(token in listing for token in ('measure=', 'staff=', 'voice=', 'pitch=', 'attack_seconds=')), listing)
        label = re.search(r'measure=("[^"]*")', listing)[1]
        selected = run('daw_performance_play', [original], f'notes-at {label} 0 1\nquit\n')
        require('shown=1' in selected, selected)
        # Use a very short valid lane, independent of the piece's duration.
        commands = ['curve-put 91 0 64 10 0 0 20 0.001 127', 'gain -13', 'curve 91 20 80', 'curves',
                    f'save "{root/"edited"}"', 'undo','undo','undo',f'save "{root/"undone"}"',
                    'redo','redo','redo',f'save "{root/"redone"}"',
                    'curve-put 92 0 64 1 0 0 2 0.001 127', # duplicate lane
                    'curve-put 91 0 64 1 0 0 2 0 127', # duplicate time
                    f'save "{root/"rejected"}"', 'curve-remove 91', 'undo', f'save "{root/"delete-undone"}"', 'quit']
        output = run('daw_performance_play', [original], '\n'.join(commands)+'\n')
        require(output.count('Command failed:') == 2, output)
        require('curve=91 channel=0 cc=64' in output and 'point=20 seconds=0.001 value=80' in output, output)
        require(hashes(root/'undone') == original_hashes, 'undo bytes changed')
        for name in ('redone','rejected','delete-undone'):
            require(hashes(root/name) == hashes(root/'edited'), name+' bytes changed')
        require(hashes(root/'edited')['score.dawproj'] == original_hashes['score.dawproj'], 'curve changed notation')
        probe = run('daw_performance_probe', [original, root/'probe', '--data-only'])
        require(hashes(original) == original_hashes, 'source document changed')
        result = dict(result='PASS', score_sha256=source_hash, import_output=import_output.strip(), probe_output=probe.strip(),
                      contextual_note_lookup=True, create_curve_edit_delete_undo_redo=True, byte_exact_roundtrip=True,
                      source_unchanged=True, plugins_opened=0, output_devices_opened=0)
    require(hashlib.sha256(args.score.read_bytes()).hexdigest() == source_hash, 'source score changed')
    require(hashlib.sha256(args.piano_state.read_bytes()).hexdigest() == state_hash, 'source state changed')
    print(json.dumps(result, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
