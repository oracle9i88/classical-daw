import test from 'node:test';
import assert from 'node:assert/strict';
import { createProject, validateProject, parseProject, projectJson, beatToSeconds, secondsToBeat, bars, LIMITS, clone } from '../core.mjs';
import { Editor, Recovery, RECOVERY_PREFIX, LEGACY_RECOVERY } from '../editor-state.mjs';

const store = () => { const values = new Map(); return { get length() { return values.size; }, key(i) { return [...values.keys()][i]; }, getItem(k) { return values.get(k) ?? null; }, setItem(k, v) { values.set(k, v); }, removeItem(k) { values.delete(k); } }; };
test('v2 exact canonical save/reopen: multi-part identities, low pitches and controls', () => {
  const p = createProject(true); p.tracks[0].controls = [{ beat: 0, channel: 0, controller: 64, value: 127 }, { beat: 1, channel: 0, controller: 64, value: 0 }, { beat: 1, channel: 0, controller: 64, value: 127 }];
  assert.equal(projectJson(parseProject(projectJson(p))), projectJson(p));
  assert.equal(new Set(p.tracks.flatMap(t => t.notes.map(n => n.id))).size, 72);
  assert(p.tracks.some(t => t.notes.some(n => n.midi < 60)));
});
test('legacy v1 migration keeps notes, meter, tempo, lyrics and allocation', () => {
  const raw = { format: 'classical-daw-web-project', version: 1, ppq: 960, tempo: 77, measures: 8, timeSignature: { numerator: 6, denominator: 8 }, notes: [{ id: 7, midi: 60, start: 1, duration: 1, velocity: 90, lyric: '词' }], nextId: 8, selectedId: 7 };
  const p = parseProject(JSON.stringify(raw)); assert.equal(p.lengthBeats, 24); assert.equal(p.tracks[0].notes[0].lyric, '词'); assert.equal(p.nextId, 8); assert.equal(p.tempoChanges[0].bpm, 77);
  assert.equal(projectJson(parseProject(projectJson(p))), projectJson(p));
});
test('tempo conversion and inverse span tempo points; meter changes create partial bars', () => {
  const p = createProject(); p.lengthBeats = 16; p.tempoChanges = [{ beat: 0, bpm: 120 }, { beat: 4, bpm: 60 }, { beat: 9, bpm: 180 }];
  p.meterChanges = [{ beat: 0, numerator: 4, denominator: 4 }, { beat: 6, numerator: 3, denominator: 4 }];
  assert.equal(beatToSeconds(p, 9), 7);
  for (let b = 0; b < 16; b += .125) assert(Math.abs(secondsToBeat(p, beatToSeconds(p, b)) - b) < 1e-12);
  assert.deepEqual(bars(p).map(b => [b.start, b.end]), [[0,4],[4,6],[6,9],[9,12],[12,15],[15,16]]);
});
test('one history orders note, mixer and curve changes; invalid edit is transactional', () => {
  const editor = new Editor(createProject(true)); const before = projectJson(editor.project);
  editor.edit(p => { p.tracks[0].notes[0].midi = 20; }); const note = projectJson(editor.project);
  editor.edit(p => { p.tracks[1].gainDb = -24; }); const mix = projectJson(editor.project);
  editor.edit(p => { p.tracks[0].controls.push({ beat: 7, channel: 0, controller: 11, value: 91 }); }); const curve = projectJson(editor.project), revision = editor.revision;
  assert.throws(() => editor.edit(p => { p.tracks[0].notes[0].duration = Infinity; })); assert.equal(editor.revision, revision); assert.equal(projectJson(editor.project), curve);
  editor.undo(); assert.equal(projectJson(editor.project), mix); editor.undo(); assert.equal(projectJson(editor.project), note); editor.undo(); assert.equal(projectJson(editor.project), before);
  editor.redo(); editor.redo(); editor.redo(); assert.equal(projectJson(editor.project), curve);
  editor.undo(); editor.edit(p => { p.title = 'branch'; }); assert.equal(editor.future.length, 0);
});
test('history is bounded and no-op changes do not consume revisions', () => {
  const editor = new Editor(createProject()); assert.equal(editor.edit(() => {}), false); assert.equal(editor.revision, 0);
  for (let i = 0; i < 100; i++) editor.edit(p => { p.title = String(i); }); assert.equal(editor.past.length, 32);
});
test('unresolved recovery survives new edits, separate tabs and storage failures', () => {
  const storage = store(), old = new Recovery(storage, 'old'); old.save(createProject(true));
  const raw = storage.getItem(RECOVERY_PREFIX + 'old'), fresh = new Recovery(storage, 'new');
  assert.equal(fresh.list().length, 1); const p = createProject(); p.title = 'new'; fresh.save(p);
  assert.equal(storage.getItem(RECOVERY_PREFIX + 'old'), raw); assert.equal(fresh.list()[0].project.title, createProject(true).title);
  const other = new Recovery(storage, 'other'); other.save(createProject()); assert.equal(other.list().length, 2);
  assert.throws(() => new Recovery({ setItem() { throw new Error('quota'); } }, 'quota').save(p), /quota/);
  assert.throws(() => fresh.remove('unrelated')); assert.equal(storage.getItem(RECOVERY_PREFIX + 'old'), raw);
});
test('bad recovery is offered for raw backup, not silently discarded', () => {
  const storage = store(); storage.setItem(LEGACY_RECOVERY, '{bad'); const r = new Recovery(storage, 'test'); assert(r.list()[0].error); assert.equal(storage.getItem(LEGACY_RECOVERY), '{bad');
});
test('bad, malicious or excessive project data is refused before installation', () => {
  const p = createProject(true);
  for (const mutate of [x => x.tracks[0].notes[0].id = x.tracks[1].notes[0].id, x => x.tracks[0].notes[0].midi = 128, x => x.tracks[0].notes[0].start = NaN, x => x.tracks[0].pan = 2, x => x.lengthBeats = 1, x => x.tracks[0].instrument = '__proto__', x => x.tempoChanges.push({ beat: 0, bpm: 90 }), x => x.nextId = 1, x => x.meterChanges[0].denominator = 3]) {
    const q = clone(p); mutate(q); assert.throws(() => validateProject(q));
  }
  assert.throws(() => parseProject(' '.repeat(LIMITS.fileBytes + 1)), /8 MiB/);
  assert.throws(() => parseProject('{"__proto__":{"polluted":true}}')); assert.equal({}.polluted, undefined);
});
test('the accepted editor state always fits its own UTF-8 save/reopen limit', () => {
  const editor = new Editor(createProject()), before = projectJson(editor.project);
  assert.throws(() => editor.edit(p => {
    for (let i = 0; i < 20000; i++) p.tracks[0].notes.push({ id: p.nextId++, midi: 60, start: 0, duration: 1, velocity: 90, channel: 0, lyric: '词'.repeat(256) });
  }), /8 MiB/);
  assert.equal(projectJson(editor.project), before); assert.equal(editor.revision, 0);
  editor.edit(p => { for (let i = 0; i < 20000; i++) p.tracks[0].notes.push({ id: p.nextId++, midi: i % 128, start: 0, duration: 1, velocity: 90, channel: 0, lyric: '' }); });
  assert.equal(parseProject(projectJson(editor.project)).tracks[0].notes.length, 20000);
});
