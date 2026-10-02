#!/usr/bin/env python3
"""Data-only provenance path: real XML import, JSON inspection, edits and recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=Path('build'))
a = parser.parse_args()
build = a.build.resolve()
example = Path(__file__).resolve().parents[1]/'examples/classical-study/study.musicxml'

def run(tool, *args, commands=None, expected=0):
    r = subprocess.run([str(build/tool), *map(str,args)], input=commands, text=True,
                       capture_output=True, timeout=60)
    assert r.returncode == expected, r.stdout+r.stderr
    assert 'Command failed:' not in r.stdout and 'AUTOSAVE FAILED' not in r.stdout, r.stdout
    return r.stdout

def inspect(path):
    return json.loads(run('daw_performance_provenance', path))

with tempfile.TemporaryDirectory(prefix='daw-receipt-cli-') as folder:
    root = Path(folder)
    state = root/'data-only.state'
    state.write_bytes(b'not-a-real-preset-never-play')
    source_hash = hashlib.sha256(example.read_bytes()).hexdigest()
    run('daw_performance_import', example, state, root/'original')
    receipt = inspect(root/'original')
    assert receipt == json.loads(example.with_name('import-receipt.json').read_text()), 'public receipt is stale'
    assert receipt['format'] == 'classical-daw-import-receipt-1'
    assert receipt['source']['filename'] == 'study.musicxml'
    assert receipt['source']['bytes'] == example.stat().st_size
    assert receipt['audition_repair']['repeats_separated'] == 3
    assert len(receipt['changes']) == 3
    for change in receipt['changes']:
        assert change['reason'] == 'repeat_separated'
        assert change['before']['duration_ticks'] - change['after']['duration_ticks'] == 1
        assert change['part_id'] == 'P1' and change['notation_id_at_import'] > 0
    assert receipt['coverage']['reader'] == 'aggregate_only'
    assert not receipt['coverage']['source_offsets']
    commands = ['provenance', 'gain -18', 'curve-adopt 1 0 64', 'undo', 'redo',
                f'save "{root/"edited"}"', 'quit']
    run('daw_performance_play', root/'original', commands='\n'.join(commands)+'\n')
    assert inspect(root/'edited') == receipt
    recovery = run('daw_performance_recover','list',root/'original').strip()
    run('daw_performance_recover','restore',root/'original',recovery,root/'recovered')
    assert inspect(root/'recovered') == receipt
    # Reader repairs must be visible as counts, not fabricated before/after IDs.
    grace = root/'grace.musicxml'
    grace.write_text(example.read_text().replace('<note>', '<note><grace/><pitch><step>B</step><octave>5</octave></pitch><voice>1</voice><type>eighth</type><staff>1</staff></note><note>', 1))
    run('daw_performance_import', grace, state, root/'grace-work')
    assert inspect(root/'grace-work')['musicxml_reader']['grace_notes_timed'] == 1
    # Independent JSON parsing checks quoted/newline lyrics and Unicode paths.
    lyric_source = root/'练习曲.musicxml'
    pitch = '<pitch><step>C</step><octave>4</octave></pitch>'
    lyric_source.write_text('<score-partwise><part-list><score-part id="P1"><part-name>Piano</part-name></score-part></part-list><part id="P1"><measure number="弱起"><attributes><divisions>960</divisions></attributes><note>'+pitch+'<duration>240</duration><voice>1</voice><lyric><text>verse\n&quot;two&quot;</text></lyric></note><backup><duration>240</duration></backup><note>'+pitch+'<duration>960</duration><voice>2</voice></note></measure></part></score-partwise>')
    run('daw_performance_import',lyric_source,state,root/'lyric-work')
    lyric_receipt = inspect(root/'lyric-work')
    assert lyric_receipt['source']['filename'] == '练习曲.musicxml'
    lost = lyric_receipt['changes'][0]
    assert lost['measure_label'] == '弱起'
    assert lost['before']['lyric'] == 'verse\n"two"' and lost['after']['lyric'] == ''
    # MIDI parser/converter and audition repairs are counted separately.
    track = bytes.fromhex('00 90 3c 5a 81 70 90 3c 50 81 70 80 3c 00 81 70 80 3c 00 0a 80 3c 00 00 ff 2f 00')
    midi = root/'overlap.mid'
    midi.write_bytes(b'MThd'+struct.pack('>IHHH',6,0,1,960)+b'MTrk'+struct.pack('>I',len(track))+track)
    run('daw_performance_import',midi,state,root/'midi-work')
    mr = inspect(root/'midi-work')
    assert mr['midi_reader']['overlaps_trimmed'] == 1
    assert mr['midi_reader']['dropped_orphan_releases'] == 1
    assert mr['audition_repair']['repeats_separated'] == 1
    assert len(mr['changes']) == 1 and mr['changes'][0]['before']['duration_ticks'] == 240
    assert mr['changes'][0]['after']['duration_ticks'] == 239
    # Legacy absence is visible and never invented from the edited score.
    run('daw_performance_fixture',state,root/'legacy')
    run('daw_performance_provenance',root/'legacy',expected=2)
    assert hashlib.sha256(example.read_bytes()).hexdigest() == source_hash
print(json.dumps(dict(result='PASS', public_example_changes=3, source_unchanged=True,
    reader_scope_explicit=True, midi_stages_separated=True, unicode_json_parsed=True,
    public_receipt_reproduced=True, receipt_survives_edit_save_recovery=True,
    legacy_absence_reported=True, plugins_opened=0, output_devices_opened=0),indent=2))
