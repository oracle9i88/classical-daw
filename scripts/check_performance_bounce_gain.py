#!/usr/bin/env python3
"""Opt-in local AU check: saved gain changes the performance bounce, no output device."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('document', type=Path)
p.add_argument('--build', type=Path, default=Path('build'))
a = p.parse_args()
build = a.build.resolve()

def run(tool, *args, commands=None):
    r = subprocess.run([str(build/tool), *map(str,args)], input=commands,
                       capture_output=True, text=True, timeout=120)
    if r.returncode or 'Command failed:' in r.stdout or 'AUTOSAVE FAILED' in r.stdout:
        raise RuntimeError(r.stdout+r.stderr)
    return r.stdout

with tempfile.TemporaryDirectory(prefix='daw-bounce-gain-') as folder:
    root = Path(folder)
    # Editing is data-only; copy first so recovery folders stay inside this test.
    import shutil
    shutil.copytree(a.document, root/'source')
    run('daw_performance_play', root/'source', commands=f'gain -12\nsave "{root/"a"}"\ngain -18\nsave "{root/"b"}"\nquit\n')
    reports = []
    for name, gain in [('a', -12), ('b', -18)]:
        run('daw_performance_render', root/name, root/(name+'-bounce'))
        r = json.loads((root/(name+'-bounce')/'report.json').read_text())
        assert r['output_gain_db'] == gain and r['rms'] > 1e-5 and r['clipped_samples'] == 0, r
        reports.append(r)
    ratio = reports[1]['rms']/reports[0]['rms']
    expected = 10**(-6/20)
    assert abs(ratio/expected-1) < .02, ratio
    print(json.dumps(dict(result='PASS', output_devices_opened=0, gains_db=[-12,-18],
                          measured_rms_ratio=ratio, expected_ratio=expected,
                          relative_tolerance=.02), indent=2))
