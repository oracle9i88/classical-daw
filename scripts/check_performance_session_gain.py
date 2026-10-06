#!/usr/bin/env python3
"""Portable session-gain import regression. Fake states are data-only; never play."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shlex
import subprocess
import tempfile


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def hashes(folder):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.iterdir() if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    build = parser.parse_args().build.resolve()

    def run(tool, *args, code=0):
        result = subprocess.run([str(build/tool), *map(str, args)],
                                text=True, capture_output=True, timeout=30)
        require(result.returncode == code, result.stdout + result.stderr)
        return result.stdout + result.stderr

    accepted, rejected = 0, 0
    with tempfile.TemporaryDirectory(prefix='daw-session-gain-') as folder:
        root = Path(folder)
        source = root/'source'
        run('daw_session_fixture', source)
        for i in (1, 2):
            (source/f'track-{i}.aupreset').write_bytes(f'fake-data-only-state-{i}'.encode())
        cases = [(-60, (-4.5, -6), True), (0, (-4.5, -6), True),
                 (6, (-4.5, -6), True), (6, (6, -60), True),
                 (12, (0, -12), True), (6, (-4.5, 6.00001), False),
                 (12, (-4.5, 12), False)]
        for index, (master, route_gains, fits) in enumerate(cases):
            # The later route is muted+solo. It still must retain its gain and
            # must reject overflow: unmuting later cannot change the mix.
            entry = source/'gain.dawsession'
            entry.write_text(
                'CLASSICAL_DAW_SESSION 3\nscore "score.dawproj"\n'
                f'master_gain_db {master}\nroutes 2\n'
                f'route "piano" "pianoteq" {route_gains[0]} -0.2 "" "track-1.aupreset" 0 0 "" 0\n'
                f'route "cello" "swam-cello" {route_gains[1]} 0.2 "" "track-2.aupreset" 1 1 "" -12345\nend\n')
            before = hashes(source)
            target = root/f'work-{index}'
            text = run('daw_performance_session_import', entry, target, code=0 if fits else 1)
            require(hashes(source) == before, 'source was modified')
            if not fits:
                rejected += 1
                require('FAIL stage=gain' in text and 'part "cello"' in text and
                        'limit of +12 dB' in text and 'Nothing saved.' in text, text)
                require(not target.exists(), 'failed import created a destination')
                continue
            accepted += 1
            lines = (target/'performances.dawperformance').read_text().splitlines()
            require(lines[0] == 'CLASSICAL_DAW_PERFORMANCE 4' and lines[1] == '2', lines[:2])
            routes = [shlex.split(line) for line in lines[2:4]]
            saved_master = float(lines[5].split()[2])
            require(saved_master == min(master, 0), 'master gain was not rebased correctly')
            for i, route in enumerate(routes):
                saved_gain = float(route[2])
                require(saved_gain == route_gains[i] + max(master, 0), 'route gain changed')
                expected = math.pow(10, (master + route_gains[i])/20)
                actual = math.pow(10, (saved_master + saved_gain)/20)
                require(math.isclose(actual, expected, rel_tol=1e-14), 'effective gain changed')
                require(actual <= 4, 'runtime headroom bound exceeded')
                require((target/f'route-{i+1}.aupreset').read_bytes() ==
                        (source/f'track-{i+1}.aupreset').read_bytes(), 'state changed')
            require(routes[1][4:] == ['1', '1', '-12345'], 'mute/solo/delay changed')
            require(('Positive master rebased:' in text) == (master > 0), text)
            saved = hashes(target)
            run('daw_performance_session_import', entry, target, code=1)
            require(hashes(target) == saved, 'existing destination was overwritten')
    print(json.dumps(dict(result='PASS', accepted=accepted, rejected_overflow=rejected,
                         effective_gains_preserved=True, muted_route_checked=True,
                         source_and_states_unchanged=True,
                         plugins_opened=0, output_devices_opened=0), indent=2))


if __name__ == '__main__':
    main()
