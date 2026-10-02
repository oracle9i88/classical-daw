#!/usr/bin/env python3
"""Kill the data-only editor after an acknowledged autosave; recover exact bytes.
The fake state below is ONLY used for data tests. Never send play to this fixture.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=Path('build'))
args = parser.parse_args()
build = args.build.resolve()

def hashes(path):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in path.iterdir() if p.is_file()}

def run(tool, *arguments, commands=None):
    result = subprocess.run([str(build/tool), *map(str, arguments)], input=commands,
                            text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    return result.stdout

with tempfile.TemporaryDirectory(prefix='daw-recovery-cli-') as folder:
    root = Path(folder)
    (root/'data-only.state').write_bytes(b'not-a-plugin-state')
    source = root/'source'
    run('daw_performance_fixture', root/'data-only.state', source)
    before = hashes(source)
    process = subprocess.Popen([str(build/'daw_performance_play'), str(source)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output = b''
    try:
        commands = ['curve-step 1 0 64 1 0 127 2 .5 0 3 .5 127', 'gain -18',
                    'seek 1.25', 'status', 'range 1 2', 'repeat on', 'status',
                    'seek -1', 'range 3 2', 'status', f'save "{root/"expected"}"']
        process.stdin.write(('\n'.join(commands)+'\n').encode())
        process.stdin.flush()
        deadline = time.monotonic()+30
        marker = ('Saved '+str(root/'expected')+'\n').encode()
        while marker not in output and time.monotonic()<deadline:
            for key, _ in selector.select(.2):
                block = os.read(key.fileobj.fileno(), 65536)
                if not block:
                    raise RuntimeError(output.decode())
                output += block
        text = output.decode()
        assert marker in output, text
        assert 'AUTOSAVE FAILED' not in text and 'Autosaved revision=2 ' in text, text
        assert text.count('Command failed:') == 2, text
        assert 'frame=60000 range_end_frame=0 repeat=0' in text, text
        assert text.count('frame=48000 range_end_frame=96000 repeat=1') == 2, text
        process.kill()  # No quit, normal destruction or final save.
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        selector.close()
        process.stdin.close()
        process.stdout.close()
    candidates = run('daw_performance_recover', 'list', source).splitlines()
    assert len(candidates) == 1, candidates
    run('daw_performance_recover', 'restore', source, candidates[0], root/'restored')
    assert hashes(root/'restored') == hashes(root/'expected'), 'recovered bytes differ'
    assert hashes(source) == before, 'original changed'
    reopened = run('daw_performance_play', root/'restored', commands='curves\nstatus\nquit\n')
    assert 'mode=step' in reopened, reopened
    assert 'point=2 seconds=0.5 value=0' in reopened and 'point=3 seconds=0.5 value=127' in reopened
print(json.dumps(dict(result='PASS', killed_after_autosave=True, exact_recovered_bytes=True,
                      stopped_range_commands=True, plugins_opened=0, output_devices_opened=0), indent=2))
