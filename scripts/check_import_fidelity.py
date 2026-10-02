#!/usr/bin/env python3
"""Exercise part selection and zero-time pedal adoption through the real CLI, no AU/device."""
import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    build = parser.parse_args().build.resolve()

    def run(tool, *args, commands=None):
        result = subprocess.run([str(build / tool), *map(str, args)], input=commands,
                                text=True, capture_output=True, timeout=60)
        if result.returncode or 'Command failed:' in result.stdout:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def meter(beats):
        return f'<attributes><divisions>960</divisions><time><beats>{beats}</beats><beat-type>4</beat-type></time></attributes>'

    def note(step, duration):
        return f'<note><pitch><step>{step}</step><octave>4</octave></pitch><duration>{duration}</duration></note>'

    with tempfile.TemporaryDirectory(prefix='daw-import-fidelity-') as folder:
        root = Path(folder)
        source = root / 'parts.xml'
        source.write_text(
            "<score-partwise><part-list><score-part id='P1'><part-name>First</part-name></score-part>"
            "<score-part id='P2'><part-name>Second</part-name></score-part></part-list>"
            "<part id='P1'><measure number='1'>" + meter(4) + note('C',3840) +
            "</measure><measure number='2'>" + note('D',3840) + "</measure></part>"
            "<part id='P2'><measure number='1'>" + meter(3) +
            "<direction><direction-type><pedal type='start'/></direction-type></direction>" + note('E',2880) +
            "<direction><direction-type><pedal type='stop'/></direction-type></direction>"
            "</measure><measure number='2'>" + meter(2) + note('F',1920) + "</measure></part></score-partwise>")
        state = root / 'data-only.aupreset'
        state.write_bytes(b'not-an-AU-state: data-only regression must never load a plugin')
        document = root / 'document'
        run('daw_performance_import', source, state, document, '--part', '2')
        project = (document / 'score.dawproj').read_text().splitlines()
        assert 'meter 3 4 24 8' in project, project
        assert 'meter_changes 1' in project, project
        edited, undone = root / 'edited', root / 'undone'
        run('daw_performance_play', document, commands=
            f'curve-adopt 1 0 64\nsave "{edited}"\nundo\nsave "{undone}"\nquit\n')
        for file in document.iterdir():
            assert file.read_bytes() == (undone / file.name).read_bytes(), f'undo changed {file.name}'
        assert (edited / 'score.dawproj').read_bytes() == (document / 'score.dawproj').read_bytes()
        print(run('daw_performance_probe', edited, root / 'probe', '--data-only').strip())
    print('PASS selected meter=3/4 with later change; pedal adoption/undo; exact document reopen; no AU/device')


if __name__ == '__main__':
    main()
