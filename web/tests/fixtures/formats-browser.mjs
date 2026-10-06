import { createProject, validateProject } from '../../core.mjs';
import { parseMusicXml, musicXml } from '../../formats.mjs';
const tests = [];
const test = (name, run) => tests.push({ name, run });
const ok = (v, message = 'assertion failed') => { if (!v) throw new Error(message); };
const equal = (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`actual ${JSON.stringify(a)} != expected ${JSON.stringify(b)}`); };
const rejects = (fn, match) => { try { fn(); } catch (e) { ok(match.test(e.message), e.message); return; } throw new Error('expected rejection'); };
const part = (id, measures) => `<part id="${id}">${measures}</part>`;
const measure = (number, body, implicit = false) => `<measure number="${number}"${implicit ? ' implicit="yes"' : ''}>${body}</measure>`;
const attrs = (divisions = 960) => `<attributes><divisions>${divisions}</divisions><time><beats>4</beats><beat-type>4</beat-type></time></attributes>`;
const n = (step, duration, body = '', octave = 4) => `<note><pitch><step>${step}</step><octave>${octave}</octave></pitch><duration>${duration}</duration>${body}</note>`;
const wrap = parts => `<?xml version="1.0"?><score-partwise version="4.0"><part-list>${parts.map(([id]) => `<score-part id="${id}"><part-name>${id}</part-name></score-part>`).join('')}</part-list>${parts.map(([id, measures]) => part(id, measures)).join('')}</score-partwise>`;
const musicalNotes = p => p.tracks.map(t => t.notes.map(({ id, ...note }) => note).sort((a, b) => a.start - b.start || a.midi - b.midi || a.duration - b.duration));

test('external DOCTYPE and comments accepted; entities/internal subsets rejected', () => {
  const source = wrap([['P1', measure('0', attrs() + n('C', 960), true)]]);
  const withDtd = source.replace('<score-partwise', '<!DOCTYPE score-partwise PUBLIC "-//Recordare//DTD MusicXML 4.0 Partwise//EN" "https://invalid.example/partwise.dtd"><!-- ordinary comment --><score-partwise');
  equal(parseMusicXml(withDtd).project.tracks[0].notes.length, 1);
  rejects(() => parseMusicXml(source.replace('<score-partwise', '<!DOCTYPE score-partwise [<!ENTITY a "bad">]><score-partwise')), /ENTITY|DTD/);
  rejects(() => parseMusicXml('<score-partwise>'), /语法/);
});

test('pickup/token measure labels, divisions changes, backup/forward and independent voices', () => {
  const source = wrap([['P1', measure('0', attrs(3) + n('C', 1) + '<forward><duration>2</duration></forward>', true)
    + measure('X1', '<attributes><divisions>6</divisions></attributes>' + n('D', 12, '<voice>1</voice>')
      + '<backup><duration>12</duration></backup>' + n('E', 6, '<voice>2</voice>') + '<forward><duration>6</duration></forward>')
    + measure('X2', '')]]);
  const p = parseMusicXml(source).project;
  equal(p.tracks[0].notes.map(n => [n.midi, n.start, n.duration]), [[60, 0, 1 / 3], [62, 1, 2], [64, 1, 1]]);
  equal(p.lengthBeats, 9); // pickup 1 + two normal bars, including final empty bar
});

test('fractional pickup sequence rounds absolute endpoints without cumulative drift', () => {
  const source = wrap([['P1', Array.from({ length: 7 }, (_, i) => measure(String(i), (i ? '' : attrs(7)) + n('C', 1), true)).join('')]]);
  const result = parseMusicXml(source);
  equal(result.project.lengthBeats, 1);
  equal(result.project.tracks[0].notes.at(-1).start + result.project.tracks[0].notes.at(-1).duration, 1);
  ok(result.warnings.some(w => w.includes('舍入')));
});

test('different-duration chord advances by its first note only', () => {
  const source = wrap([['P1', measure('1', attrs() + n('C', 960)
    + '<note><chord/><pitch><step>E</step><octave>4</octave></pitch><duration>1920</duration></note>' + n('G', 960))]]);
  equal(parseMusicXml(source).project.tracks[0].notes.map(n => [n.midi, n.start, n.duration]), [[60, 0, 1], [64, 0, 2], [67, 1, 1]]);
});

test('cross-measure ties merge only within voice, staff, channel and pitch', () => {
  const source = wrap([['P1', measure('1', attrs() + n('C', 3840, '<tie type="start"/><voice>1</voice>'))
    + measure('2', n('C', 960, '<tie type="stop"/><voice>1</voice>') + '<backup><duration>960</duration></backup>' + n('C', 960, '<voice>2</voice>'))]]);
  const result = parseMusicXml(source);
  equal(result.project.tracks[0].notes.map(n => [n.start, n.duration]), [[0, 5], [4, 1]]);
  equal(result.warnings.length, 1);
  ok(result.warnings[0].includes('试听假设'));
  equal(result.project.tempoChanges, [{ beat: 0, bpm: 96 }]);
});

test('tempo/metronome precedence, later tempo, meter, transpose, pedal and multi-part channels', () => {
  const source = wrap([['P1', measure('1', attrs() + '<direction><direction-type><metronome><beat-unit>half</beat-unit><per-minute>60</per-minute></metronome></direction-type><sound tempo="100"/></direction>'
      + '<direction><direction-type><pedal type="start"/></direction-type><offset>240</offset></direction>' + n('C', 3840))
      + measure('2', '<attributes><time><beats>7</beats><beat-type>8</beat-type></time><transpose><chromatic>-2</chromatic></transpose></attributes>'
        + '<direction><direction-type><words>Slow</words></direction-type><sound tempo="60"/></direction>' + n('D', 960))],
    ['P2', measure('1', attrs() + n('C', 3840, '', 3)) + measure('2', '<attributes><time><beats>7</beats><beat-type>8</beat-type></time></attributes>' + n('D', 960, '', 3))]]);
  const { project, warnings } = parseMusicXml(source);
  equal(project.tempoChanges, [{ beat: 0, bpm: 100 }, { beat: 4, bpm: 60 }]);
  equal(project.meterChanges, [{ beat: 0, numerator: 4, denominator: 4 }, { beat: 4, numerator: 7, denominator: 8 }]);
  equal(project.tracks[0].controls, [{ beat: .25, channel: 0, controller: 64, value: 127 }]);
  equal(project.tracks[0].notes[1].midi, 60); equal(project.lengthBeats, 7.5);
  ok(warnings.some(w => w.includes('transpose') || w.includes('移调')));
});

test('staff-specific transposition rejects instead of changing the other staff pitch', () => {
  const source = wrap([['P1', measure('1', '<attributes><divisions>960</divisions><staves>2</staves><transpose number="2"><diatonic>-1</diatonic><chromatic>-2</chromatic></transpose></attributes>'
    + n('C', 960, '<voice>1</voice><staff>1</staff>') + '<backup><duration>960</duration></backup>'
    + n('D', 960, '<voice>2</voice><staff>2</staff>'))]]);
  rejects(() => parseMusicXml(source), /按谱表.*移调/);
});

test('unsupported grace/microtones/invalid cursor explicitly reject; repeats visibly warn', () => {
  rejects(() => parseMusicXml(wrap([['P1', measure('1', attrs() + '<note><grace/><pitch><step>C</step><octave>4</octave></pitch></note>')]])), /倚音/);
  rejects(() => parseMusicXml(wrap([['P1', measure('1', attrs() + '<note><pitch><step>C</step><alter>0.5</alter><octave>4</octave></pitch><duration>960</duration></note>')]])), /微分音/);
  rejects(() => parseMusicXml(wrap([['P1', measure('1', attrs() + '<backup><duration>960</duration></backup>')]])), /越过/);
  rejects(() => parseMusicXml(wrap([['P1', measure('1', attrs() + '<direction><direction-type><octave-shift type="down" size="8"/></direction-type></direction>' + n('C', 960))]])), /八度移位/);
  const p = parseMusicXml(wrap([['P1', measure('1', attrs() + n('C', 3840) + '<barline><repeat direction="backward"/></barline>')]]));
  ok(p.warnings.some(w => w.includes('反复')));
});

function richProject() {
  const p = createProject(); p.title = '长谱 & “分轨”'; p.lengthBeats = 73.5; p.masterGainDb = -4;
  p.tempoChanges = [{ beat: 0, bpm: 96 }, { beat: 1 / 3, bpm: 88 }, { beat: 8, bpm: 120 }];
  p.meterChanges = [{ beat: 0, numerator: 4, denominator: 4 }, { beat: 9, numerator: 7, denominator: 8 }];
  const note = (id, midi, start, duration, channel = 0) => ({ id, midi, start, duration, channel, velocity: 81, lyric: id === 1 ? '<词 & >' : '' });
  p.tracks[0].notes = [note(1, 60, 0, 1), note(2, 64, 0, 2), note(3, 67, 1, 1), note(4, 0, 3, 7, 5), note(5, 127, 70, 1 / 3, 15)];
  p.tracks[0].controls = [{ beat: .5, channel: 0, controller: 64, value: 100 }, { beat: 71, channel: 5, controller: 11, value: 99 }];
  p.tracks.push({ id: 'cello', name: 'Cello', instrument: 'strings', gainDb: -4, pan: .5, mute: true, solo: false,
    notes: [note(6, 36, 0, 14)], controls: [{ beat: 30, channel: 0, controller: 2, value: 75 }] });
  p.nextId = 7; return validateProject(p);
}

test('MusicXML long multi-part round trip retains note timing, CC, mix, maps and trailing bars', () => {
  const original = richProject(), { project, warnings } = parseMusicXml(musicXml(original));
  equal(musicalNotes(project), musicalNotes(original));
  equal(project.tempoChanges, original.tempoChanges); equal(project.meterChanges, original.meterChanges);
  equal(project.lengthBeats, original.lengthBeats); equal(project.masterGainDb, original.masterGainDb);
  equal(project.tracks.map(t => [t.name, t.instrument, t.gainDb, t.pan, t.mute, t.solo, t.controls]), original.tracks.map(t => [t.name, t.instrument, t.gainDb, t.pan, t.mute, t.solo, t.controls]));
  ok(warnings.some(w => w.includes('扩展'))); ok(warnings.some(w => w.includes('CC7')));
});

test('a terminal meter event survives without extending the final bar', () => {
  const p = createProject(); p.lengthBeats = 4;
  p.meterChanges.push({ beat: 4, numerator: 8, denominator: 4 });
  p.tempoChanges.push({ beat: 4, bpm: 122 });
  const restored = parseMusicXml(musicXml(p)).project;
  equal(restored.lengthBeats, 4); equal(restored.meterChanges, p.meterChanges); equal(restored.tempoChanges, p.tempoChanges);
});

test('exported XML is valid under an independent note cursor, including unequal chords and ties', () => {
  const p = richProject(), xml = new DOMParser().parseFromString(musicXml(p), 'application/xml');
  const exported = [];
  // Independent oracle: ignore our importer and follow MusicXML duration/backup.
  for (const part of xml.querySelectorAll('score-partwise > part')) {
    let absolute = 0; const notes = [];
    for (const measure of part.querySelectorAll(':scope > measure')) {
      let cursor = 0, extent = 0;
      for (const e of measure.children) {
        if (e.tagName === 'backup') cursor -= Number(e.querySelector('duration').textContent);
        if (e.tagName === 'note') {
          ok(!e.querySelector('chord'), 'independent lanes should need no chord cursor exceptions');
          const duration = Number(e.querySelector('duration').textContent), pitch = e.querySelector('pitch');
          if (pitch) {
            const step = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[pitch.querySelector('step').textContent];
            const midi = 12 * (Number(pitch.querySelector('octave').textContent) + 1) + step + Number(pitch.querySelector('alter')?.textContent || 0);
            notes.push({ midi, start: (absolute + cursor) / 960, duration: duration / 960, voice: e.querySelector('voice').textContent });
          }
          cursor += duration; extent = Math.max(extent, cursor);
        }
        ok(cursor >= 0, 'backup crossed bar start');
      }
      equal(cursor, extent); absolute += extent;
    }
    equal(absolute / 960, p.lengthBeats); exported.push(notes);
  }
  equal(exported[0].filter(n => [60, 64, 67].includes(n.midi)).map(n => [n.midi, n.start, n.duration]).sort((a, b) => a[0] - b[0]), [[60, 0, 1], [64, 0, 2], [67, 1, 1]]);
  equal(exported[0].filter(n => n.midi === 0).reduce((sum, n) => sum + n.duration, 0), 7);
});

export function runFormatBrowserTests() {
  const results = []; let passed = 0;
  for (const { name, run } of tests) {
    try { run(); passed++; results.push(`PASS ${name}`); }
    catch (e) { results.push(`FAIL ${name}\n${e.stack || e.message}`); }
  }
  return { passed, failed: tests.length - passed, total: tests.length, results };
}
const report = runFormatBrowserTests();
globalThis.formatsTestResults = report;
const element = document.getElementById('result');
if (element) { element.textContent = `${report.failed ? 'FAIL' : 'PASS'} ${report.passed}/${report.total}\n\n${report.results.join('\n')}`; document.title = `${report.failed ? 'FAIL' : 'PASS'} MusicXML regression`; }
