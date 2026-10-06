import test from 'node:test';
import assert from 'node:assert/strict';
import { createProject, validateProject, PPQ } from '../core.mjs';
import { midiFile, parseMidiFile, musicXml, parseMusicXml } from '../formats.mjs';

const note = (id, midi, start, duration, channel = 0, lyric = '') => ({ id, midi, start, duration, channel, lyric, velocity: 81 });
const track = (id, notes = [], controls = []) => ({ id, name: id, instrument: 'piano', gainDb: -9, pan: 0, mute: false, solo: false, notes, controls });
function fixture() {
  const p = createProject(); p.title = '管弦乐 & étude'; p.lengthBeats = 256;
  p.tempoChanges = [{ beat: 0, bpm: 96 }, { beat: 7 / 3, bpm: 120 }, { beat: 100, bpm: 72 }];
  p.meterChanges = [{ beat: 0, numerator: 4, denominator: 4 }, { beat: 128, numerator: 7, denominator: 8 }];
  p.tracks = [track('Piano', [note(1, 0, 0, 1 / 3, 3, '字'), note(2, 127, 100, 4, 15)], [
    { beat: 0, channel: 3, controller: 64, value: 127 }, { beat: 3, channel: 3, controller: 64, value: 0 },
    { beat: 4, channel: 15, controller: 2, value: 45 }]),
  track('Cello', [note(3, 36, 99, 6, 3)], [{ beat: 1, channel: 3, controller: 11, value: 95 }])];
  p.nextId = 4; return validateProject(p);
}
const musicalNotes = p => p.tracks.map(t => t.notes.map(({ id, ...n }) => n));
const vlq = n => { const a = [n & 127]; while ((n = Math.floor(n / 128))) a.unshift((n & 127) | 128); return a; };
const chunk = data => [77, 84, 114, 107, 0, 0, (data.length >> 8) & 255, data.length & 255, ...data];
const rawMidi = (tracks, division = 960, format = tracks.length === 1 ? 0 : 1) => new Uint8Array([77, 84, 104, 100, 0, 0, 0, 6, 0, format, 0, tracks.length, division >> 8, division & 255, ...tracks.flatMap(chunk)]);
const eot = [0, 255, 47, 0];

test('MIDI preserves all parts, full pitch range, channels, long duration, maps, lyrics and CC', () => {
  const original = fixture(), { project, warnings } = parseMidiFile(midiFile(original));
  assert.equal(project.tracks.length, 2); assert.equal(project.lengthBeats, 256);
  assert.deepEqual(musicalNotes(project), musicalNotes(original));
  assert.deepEqual(project.tracks.map(t => t.controls), original.tracks.map(t => t.controls));
  assert.deepEqual(project.tempoChanges.map(e => e.beat), original.tempoChanges.map(e => e.beat));
  original.tempoChanges.forEach((event, i) => assert.equal(project.tempoChanges[i].bpm, 60000000 / Math.round(60000000 / event.bpm)));
  assert.deepEqual(project.meterChanges, original.meterChanges);
  assert.equal(project.title, original.title);
  assert.ok(warnings.some(w => w.includes('CC7')));
  assert.ok(warnings.some(w => w.includes('端口')));
});

test('export uses distinct MIDI ports instead of colliding independent part channels', () => {
  const bytes = midiFile(fixture()); const ports = [];
  for (let i = 0; i < bytes.length - 3; i++) if (bytes[i] === 255 && bytes[i + 1] === 33 && bytes[i + 2] === 1) ports.push(bytes[i + 3]);
  assert.deepEqual(ports, [0, 1]);
});

test('MIDI array views respect byte offsets', () => {
  const bytes = midiFile(fixture()), padded = new Uint8Array(bytes.length + 17); padded.set(bytes, 9);
  assert.equal(parseMidiFile(padded.subarray(9, 9 + bytes.length)).project.tracks.length, 2);
});

test('MIDI PPQ conversion rounds once to 960 with an explicit warning, no quarter-grid quantization', () => {
  const bytes = rawMidi([[1, 0x92, 45, 67, 1, 45, 0, ...eot]], 7);
  const { project, warnings } = parseMidiFile(bytes), n = project.tracks[0].notes[0];
  assert.equal(n.start, Math.round(960 / 7) / 960);
  assert.equal(n.duration, (Math.round(1920 / 7) - Math.round(960 / 7)) / 960);
  assert.equal(n.channel, 2); assert.ok(warnings.some(w => w.includes('舍入')));
});

test('same-key overlaps pair LIFO and survive export including same-onset durations', () => {
  const p = createProject(); p.tracks[0].notes = [note(1, 60, 0, 4), note(2, 60, 1, 1), note(3, 60, 5, 1), note(4, 60, 5, 3)]; p.nextId = 5;
  const result = parseMidiFile(midiFile(p));
  assert.deepEqual(result.project.tracks[0].notes.map(n => [n.start, n.duration]), [[0, 4], [1, 1], [5, 3], [5, 1]]);
  assert.ok(result.warnings.some(w => w.includes('LIFO')));
});

test('unrepresentable crossing same-key MIDI overlaps reject instead of changing durations', () => {
  const p = createProject(); p.tracks[0].notes = [note(1, 60, 0, 2), note(2, 60, 1, 3)]; p.nextId = 3;
  assert.throws(() => midiFile(p), /交错重叠/);
  assert.match(musicXml(p), /score-partwise/);
});

test('same-tick tempo follows source event order with a warning', () => {
  const bytes = rawMidi([[0, 255, 81, 3, 9, 137, 104, 0, 255, 81, 3, 7, 161, 32, 0, 0x90, 60, 80, ...vlq(960), 0x80, 60, 0, ...eot]]);
  const { project, warnings } = parseMidiFile(bytes);
  assert.equal(project.tempoChanges[0].bpm, 120); assert.ok(warnings.some(w => w.includes('最后一个')));
});

test('same-tick controller order is stable', () => {
  const p = createProject(); p.tracks[0].controls = [127, 0, 85].map(value => ({ beat: 2, channel: 0, controller: 64, value }));
  assert.deepEqual(parseMidiFile(midiFile(p)).project.tracks[0].controls.map(c => c.value), [127, 0, 85]);
});

// Deliberately independent of parseMidiFile: inspect the exported wire order.
function readWireChannelEvents(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), events = [];
  let pos = 14;
  while (pos < bytes.length) {
    const end = pos + 8 + view.getUint32(pos + 4); pos += 8;
    let tick = 0;
    const readVlq = () => { let v = 0, b; do { b = bytes[pos++]; v = v * 128 + (b & 127); } while (b & 128); return v; };
    while (pos < end) {
      tick += readVlq(); const status = bytes[pos++];
      if (status === 255) { ++pos; const size = readVlq(); pos += size; }
      else {
        assert.ok(status >= 128 && status < 240, 'exported event should have explicit channel status');
        const a = bytes[pos++], b = [192, 208].includes(status & 240) ? undefined : bytes[pos++];
        events.push({ tick, status, a, b });
      }
    }
    assert.equal(pos, end);
  }
  return events;
}

test('MIDI wire order is stable CC, then NoteOff, then NoteOn at a shared tick', () => {
  const p = createProject(); p.nextId = 3;
  p.tracks[0].notes = [note(1, 60, 0, 1), note(2, 62, 1, 1)];
  p.tracks[0].controls = [127, 0, 127].map(value => ({ beat: 1, channel: 0, controller: 64, value }));
  const atRelease = readWireChannelEvents(midiFile(p)).filter(e => e.tick === 960);
  assert.deepEqual(atRelease.map(e => [e.status, e.a, e.b]), [
    [0xb0, 64, 127], [0xb0, 64, 0], [0xb0, 64, 127], [0x80, 60, 0], [0x90, 62, 81],
  ]);
});

test('import reports same-channel same-tick CC64/NoteOff normalization in either source order', () => {
  for (const [first, second] of [
    [[0x80, 60, 0], [0xb0, 64, 127]], [[0xb0, 64, 127], [0x80, 60, 0]],
  ]) {
    const bytes = rawMidi([[0, 0x90, 60, 81, ...vlq(960), ...first, 0, ...second, ...eot]]);
    assert.ok(parseMidiFile(bytes).warnings.some(w => w.includes('CC64') && w.includes('归一化')));
  }
  const otherChannel = rawMidi([[0, 0x90, 60, 81, ...vlq(960), 0x80, 60, 0, 0, 0xb1, 64, 127, ...eot]]);
  assert.ok(!parseMidiFile(otherChannel).warnings.some(w => w.includes('归一化')));
});

test('missing MIDI maps use standard 120 BPM and 4/4 without adding notes', () => {
  const { project: p, warnings } = parseMidiFile(rawMidi([[...vlq(960), 255, 47, 0]]));
  assert.deepEqual(p.tempoChanges, [{ beat: 0, bpm: 120 }]);
  assert.ok(warnings.some(w => w.includes('MIDI 默认 120 BPM')));
  assert.deepEqual(p.meterChanges, [{ beat: 0, numerator: 4, denominator: 4 }]);
  assert.equal(p.tracks[0].notes.length, 0); assert.equal(p.lengthBeats, 1);
});

test('MIDI uses 120 before the first nonzero-tick tempo change', () => {
  const { project, warnings } = parseMidiFile(rawMidi([[...vlq(960), 255, 81, 3, 9, 137, 104, ...eot]]));
  assert.deepEqual(project.tempoChanges, [{ beat: 0, bpm: 120 }, { beat: 1, bpm: 96 }]);
  assert.ok(warnings.some(w => w.includes('MIDI 默认 120 BPM')));
});

test('unsupported program, pitch bend and SysEx are visibly reported', () => {
  const { warnings } = parseMidiFile(rawMidi([[0, 0xc0, 42, 0, 0xe0, 0, 64, 0, 0xf0, 2, 1, 0xf7, ...eot]]));
  assert.ok(warnings.some(w => w.includes('Program Change'))); assert.ok(warnings.some(w => w.includes('SysEx')));
});

test('MIDI malformed note pairs and unsafe binary structures fail atomically', () => {
  const cases = [
    [[0, 0x80, 60, 0, ...eot], /NoteOff/],
    [[0, 0x90, 60, 80, ...eot], /NoteOn/],
    [[0, 0x90, 60, 80, 0, 60, 0, ...eot], /无正时值/],
    [[0, 60, 80, ...eot], /running status/],
    [[0x81, 0x81, 0x81, 0x81, 0, 0x90, 60, 80], /VLQ/],
    [[0, 0x90, 200, 80, ...eot], /数据字节/],
    [[0, 255, 47, 0, 0], /轨道结尾/],
    [[0, 255, 81, 4, 1], /截断/],
    [[0, 0xf0, 99], /截断/],
    [[0, 0xf1, 0], /系统状态/],
    [[0, 255, 33, 1, 0, 0, 255, 33, 1, 1, ...eot], /切换端口/],
  ];
  for (const [data, match] of cases) assert.throws(() => parseMidiFile(rawMidi([data])), match);
  assert.throws(() => parseMidiFile(rawMidi([eot], 0x8001)), /SMPTE/);
  assert.throws(() => parseMidiFile(rawMidi([eot], 960, 2)), /Type/);
  assert.throws(() => parseMidiFile(midiFile(fixture()).slice(0, -1)), /截断/);
});

test('all 16 parts plus conductor round trip without clipping', () => {
  const p = createProject(); p.tracks = Array.from({ length: 16 }, (_, i) => track(`P${i}`, [note(i + 1, i * 8, 0, 1, i)])); p.nextId = 17;
  assert.equal(parseMidiFile(midiFile(p)).project.tracks.length, 16);
});

test('twenty thousand notes export without argument-spread or size crashes', () => {
  const p = createProject(); p.lengthBeats = 4096; p.nextId = 20001;
  p.tracks[0].notes = Array.from({ length: 20000 }, (_, i) => note(i + 1, i % 128, Math.floor(i / 128), .5, Math.floor(i / 1024) % 16));
  const restored = parseMidiFile(midiFile(p)).project;
  assert.equal(restored.tracks[0].notes.length, 20000); assert.equal(restored.lengthBeats, 4096);
});

test('XML import does not pretend Node has a DOM implementation', () => {
  if (typeof DOMParser === 'undefined') assert.throws(() => parseMusicXml('<score-partwise/>'), /DOMParser/);
});
