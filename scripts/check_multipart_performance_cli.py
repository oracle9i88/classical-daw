#!/usr/bin/env python3
"""Check saved-session migration and multi-part editing without plugins/devices.

The source is read-only. Frozen sessions require explicit --unfreeze; their
audio caches stay in the source. IDs are discovered from the imported document.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def hashes(folder):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.iterdir() if p.is_file()}


def quote(text):
    return '"' + str(text).replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--build', type=Path, default=Path('build'))
    args = parser.parse_args()
    build = args.build.resolve()
    source = args.session.resolve()
    original = hashes(source.parent)
    rows = [shlex.split(line) for line in source.read_text().splitlines()]
    routes = [r for r in rows if r and r[0] == 'route']
    require(len(routes) >= 2, 'test needs at least two routed parts')
    frozen = any(len(r) > 9 and r[9] for r in routes)

    def run(tool, *arguments, commands=None, code=0):
        result = subprocess.run([str(build/tool), *map(str, arguments)],
                                input=commands, text=True, capture_output=True, timeout=60)
        require(result.returncode == code, result.stdout + result.stderr)
        return result.stdout + result.stderr

    with tempfile.TemporaryDirectory(prefix='daw-multipart-cli-') as folder:
        root = Path(folder)
        document = root/'document'
        if frozen:
            failure = run('daw_performance_session_import', source, document, code=1)
            require('--unfreeze explicitly' in failure and not document.exists(), failure)
        imported = run('daw_performance_session_import', source, document,
                       *(['--unfreeze'] if frozen else []))
        require('plugins_opened=0 output_devices_opened=0 score_repairs=0' in imported, imported)
        baseline = hashes(document)
        require(set(baseline) == {'score.dawproj', 'performances.dawperformance'} |
                {f'route-{i+1}.aupreset' for i in range(len(routes))}, 'missing/extra document files')
        for i, route in enumerate(routes):
            require((document/f'route-{i+1}.aupreset').read_bytes() ==
                    (source.parent/route[6]).read_bytes(), 'saved instrument state changed')
        part0, part1 = routes[0][1], routes[1][1]
        listing = run('daw_performance_play', document,
                      commands=f'notes-part {quote(part0)} 0 1\nnotes-part {quote(part1)} 0 1\nquit\n')
        ids = re.findall(r'performed=(\d+) notation=', listing)
        require(len(ids) == 2 and ids[0] != ids[1], listing)
        attacks = [float(x) for x in re.findall(r'attack_seconds=([\d.eE+-]+)', listing)]
        require(len(attacks) == 2, listing)
        gain = float(routes[0][3])
        new_gain = gain-1 if gain > -60 else gain+1
        commands = [
            f'edit {ids[0]} 8 .9 80',
            f'shape-part {quote(part1)} {max(0,attacks[1]-.1)} {attacks[1]+1} velocity 60 90',
            f'curve-step-part {quote(part1)} 1 0 11 1 0 100 2 .5 90',
            f'route {quote(part0)} {new_gain} 0 0 0',
            f'save {quote(root/"edited")}',
            'undo', 'undo', 'undo', 'undo', f'save {quote(root/"undone")}',
            'redo', 'redo', 'redo', 'redo', f'save {quote(root/"redone")}',
            'curve-step 2 0 11 1 0 80',
            'curve-step-part nonexistent-part 2 0 11 1 0 80',
            f'route {quote(part0)} 13 0 0 0',
            f'save {quote(root/"rejected")}', 'curves', 'routes', 'status', 'quit']
        text = run('daw_performance_play', document, commands='\n'.join(commands)+'\n')
        require(text.count('Command failed:') == 3, text)
        require('revision=12 active=0' in text and 'AUTOSAVE FAILED' not in text, text)
        require(hashes(root/'undone') == baseline, 'undo did not restore all bytes')
        edited = hashes(root/'edited')
        require(edited == hashes(root/'redone') == hashes(root/'rejected'),
                'redo/rejection changed document unexpectedly')
        require(edited['score.dawproj'] == baseline['score.dawproj'], 'performance edits changed notation')
        for name in baseline:
            if name.endswith('.aupreset'):
                require(edited[name] == baseline[name], 'mix edit changed AU state')
        reopened = run('daw_performance_play', root/'edited',
                       commands=f'curves\nroutes\nsave {quote(root/"reopened")}\nquit\n')
        require('cc=11 mode=step part='+quote(part1) in reopened, reopened)
        require(hashes(root/'reopened') == edited, 'reopen changed bytes')
        candidates = run('daw_performance_recover', 'list', document).splitlines()
        require(len(candidates) == 1, 'expected one edited CLI recovery journal: '+str(candidates))
        run('daw_performance_recover', 'restore', document, candidates[0], root/'restored')
        require(hashes(root/'restored') == edited, 'multi-route recovery changed bytes')
        # Existing destinations are not overwritten.
        run('daw_performance_session_import', source, document,
            *(['--unfreeze'] if frozen else []), code=1)
        require(hashes(document) == baseline, 'existing destination changed')
    require(hashes(source.parent) == original, 'source bundle changed')
    print(json.dumps(dict(result='PASS', parts=len(routes), dynamic_performed_ids=True,
                         explicit_unfreeze=frozen, interleaved_undo_redo=True,
                         rejected_edits_unchanged=True, exact_reopen_and_recovery=True,
                         plugins_opened=0, output_devices_opened=0), indent=2))


if __name__ == '__main__':
    main()
