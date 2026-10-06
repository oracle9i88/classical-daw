import test from 'node:test';
import assert from 'node:assert/strict';
import { Worker } from 'node:worker_threads';
import { createProject, beatToSeconds } from '../core.mjs';
import { renderAudio, encodeWav, AUDIO_LIMITS } from '../audio-engine.mjs';

function project(notes = [{ midi: 69, start: 0, duration: 2 }], instrument = 'sine') {
  const p = createProject();
  p.lengthBeats = 4; p.masterGainDb = 0; p.tempoChanges = [{ beat: 0, bpm: 120 }];
  Object.assign(p.tracks[0], { gainDb: 0, pan: 0, instrument,
    notes: notes.map((note, i) => ({ id: i + 1, velocity: 100, channel: 0, lyric: '', ...note })) });
  p.nextId = notes.length + 1;
  return p;
}
const render = (p, options = {}) => renderAudio(p, { sampleRate: 8000, ...options });
const silent = data => data.every(value => value === 0);
const energy = data => data.reduce((sum, value) => sum + value * value, 0);
const cc = (beat, controller, value, channel = 0) => ({ beat, channel, controller, value });

test('tempo changes determine quantized frames and note placement', () => {
  const p = project([{ midi: 69, start: 2, duration: .5 }]);
  p.tempoChanges.push({ beat: 2, bpm: 60 });
  const audio = render(p);
  assert.equal(audio.frames, 24000);
  assert.equal(audio.endSeconds, 3);
  assert.ok(silent(audio.left.subarray(0, 8001)));
  assert.ok(energy(audio.left.subarray(8040, 10000)) > 0);
  assert.ok(silent(audio.left.subarray(13000)));
  const short = render(p, { startBeat: 2.00001, endBeat: 2.01001 });
  assert.equal(short.frames, 80);
  assert.equal(short.startSeconds, 1);
  assert.equal(short.endSeconds, 1.01);
});

test('stereo pan endpoints and mute precedence preserve track separation', () => {
  const p = project();
  p.tracks[0].pan = -1;
  let audio = render(p);
  assert.ok(energy(audio.left) > 0); assert.ok(silent(audio.right));
  const other = structuredClone(p.tracks[0]);
  Object.assign(other, { id: 'right', name: '右声部', pan: 1 });
  other.notes[0].id = 2; other.notes[0].midi = 72; p.nextId = 3; p.tracks.push(other);
  audio = render(p);
  assert.ok(energy(audio.left) > 0 && energy(audio.right) > 0);
  other.solo = true;
  audio = render(p); assert.ok(silent(audio.left)); assert.ok(energy(audio.right) > 0);
  other.mute = true;
  audio = render(p); assert.ok(silent(audio.left) && silent(audio.right));
});

test('controller channel isolation, CC7/11 level and CC10 pan are audible', () => {
  const p = project([{ midi: 69, start: 0, duration: 3, channel: 1 }]);
  p.tracks[0].pan = -1;
  const baseline = render(p);
  p.tracks[0].controls = [cc(0, 11, 0, 0), cc(0, 7, 0, 0)];
  assert.deepEqual(render(p).left, baseline.left);
  p.tracks[0].controls = [cc(0, 7, 127, 1), cc(0, 11, 127, 1)];
  const full = render(p);
  p.tracks[0].controls = [cc(0, 7, 64, 1), cc(0, 11, 64, 1), cc(1, 11, 0, 1)];
  const quiet = render(p);
  assert.ok(Math.abs(quiet.left[3001] / full.left[3001] - (64 / 127) ** 2) < 1e-6);
  assert.ok(silent(quiet.left.subarray(4000)));
  p.tracks[0].pan = 0;
  p.tracks[0].controls = [cc(0, 10, 0, 1), cc(1, 10, 127, 1)];
  const pan = render(p);
  assert.ok(silent(pan.right.subarray(0, 4000)) && energy(pan.left.subarray(0, 4000)) > 0);
  assert.ok(silent(pan.left.subarray(4000)) && energy(pan.right.subarray(4000)) > 0);
});

test('pedal extends a gate and a same-key reattack releases the old voice', () => {
  const p = project([{ midi: 60, start: 0, duration: .2 }], 'piano');
  const dry = render(p);
  p.tracks[0].controls = [cc(0, 64, 127), cc(3, 64, 0)];
  const sustained = render(p);
  assert.ok(silent(dry.left.subarray(2400, 4000)));
  assert.ok(energy(sustained.left.subarray(2400, 4000)) > 0);
  p.tracks[0].notes.push({ id: 2, midi: 60, start: 1, duration: .2, velocity: 100, channel: 0, lyric: '' });
  p.nextId = 3;
  const reattack = render(p);
  const onlySecond = structuredClone(p); onlySecond.tracks[0].notes.shift();
  const reference = render(onlySecond);
  // By 0.7 seconds the old voice's 0.16-second release has finished.
  assert.deepEqual(reattack.left.subarray(5600), reference.left.subarray(5600));
  assert.ok(silent(reattack.left.subarray(13600)));
  p.tracks[0].controls = [cc(0, 64, 127, 1)];
  const wrongChannel = render(p);
  p.tracks[0].controls = [];
  assert.deepEqual(wrongChannel.left, render(p).left);
});

test('a selected window chases original phase, envelope, pedal and controllers byte-for-byte', () => {
  const p = project([{ midi: 48, start: 0, duration: .4 }, { midi: 67, start: .8, duration: 2 }], 'strings');
  p.tempoChanges.push({ beat: 2, bpm: 90 });
  p.tracks[0].controls = [cc(0, 64, 127), cc(.6, 11, 80), cc(1, 10, 110), cc(2.7, 64, 0), cc(2.9, 7, 60)];
  const full = render(p);
  const fromBeat = 1.111, toBeat = 3.217;
  const excerpt = render(p, { startBeat: fromBeat, endBeat: toBeat });
  const from = Math.round(beatToSeconds(p, fromBeat) * 8000);
  const to = Math.round(beatToSeconds(p, toBeat) * 8000);
  assert.deepEqual(excerpt.left, full.left.subarray(from, to));
  assert.deepEqual(excerpt.right, full.right.subarray(from, to));
});

test('same-frame repedalling releases old held notes but not keys still held or new NoteOffs', () => {
  const held = project([{ midi: 60, start: 0, duration: 1 }]);
  held.tracks[0].controls = [cc(0, 64, 127), cc(2, 64, 0), cc(2, 64, 127)];
  const released = render(held);
  assert.ok(energy(released.left.subarray(6400, 7900)) > 0, 'old pedal-held note should sound before the release');
  assert.ok(silent(released.left.subarray(8800)), 'repedalling must not recapture an already released old note');
  const stillPressed = structuredClone(held);
  stillPressed.tracks[0].notes[0].duration = 3;
  assert.ok(energy(render(stillPressed).left.subarray(9600, 11200)) > 0, 'CC64 up cannot release a physically held key');
  const sameFrameOff = structuredClone(held);
  sameFrameOff.tracks[0].notes[0].duration = 2;
  assert.ok(energy(render(sameFrameOff).left.subarray(9600, 11200)) > 0, 'CC-before-NoteOff order must use the final pedal-down state');
  const reversed = structuredClone(held);
  reversed.tracks[0].controls = [cc(0, 64, 127), cc(2, 64, 127), cc(2, 64, 0)];
  assert.ok(silent(render(reversed).left.subarray(8800)), 'final pedal-up must also release old holds');
});

test('fixed voices do not transpose notes above Nyquist and output has finite tails', () => {
  const high = render(project([{ midi: 127, start: 0, duration: 1 }], 'piano'));
  assert.ok(silent(high.left)); assert.equal(high.synthesizedVoices, 0);
  for (const instrument of ['sine', 'piano', 'strings']) {
    const audio = render(project([{ midi: 100, start: 0, duration: 1 }], instrument));
    assert.ok(audio.left.every(Number.isFinite));
    assert.equal(audio.left[0], 0);
    assert.ok(energy(audio.left) > 0);
    assert.ok(silent(audio.left.subarray(6400)));
  }
});

test('hard clipping is reported and no normalization changes the waveform', () => {
  const p = project(Array.from({ length: 32 }, () => ({ midi: 69, start: 0, duration: 1 })));
  p.tracks[0].gainDb = 12;
  const audio = render(p);
  assert.ok(audio.peak > 1); assert.ok(audio.clippedSamples > 0);
  assert.ok(audio.left.includes(1) && audio.left.includes(-1));
  assert.ok(audio.left.every(value => value >= -1 && value <= 1));
  const a = encodeWav(audio), b = encodeWav(render(p));
  assert.deepEqual(a, b);
  const values = [...new Int16Array(a.buffer.slice(44))];
  assert.ok(values.includes(32767) && values.includes(-32768));
});

test('PCM16 WAV has independent standard header and signed quantization', () => {
  const audio = { sampleRate: 8000, frames: 5, left: new Float32Array([-1, -.5, 0, .5, 1]), right: new Float32Array(5) };
  const bytes = encodeWav(audio), view = new DataView(bytes.buffer);
  assert.equal(new TextDecoder().decode(bytes.subarray(0, 4)), 'RIFF');
  assert.equal(new TextDecoder().decode(bytes.subarray(8, 12)), 'WAVE');
  assert.equal(view.getUint16(20, true), 1); assert.equal(view.getUint16(22, true), 2);
  assert.equal(view.getUint32(24, true), 8000); assert.equal(view.getUint32(28, true), 32000);
  assert.equal(view.getUint32(40, true), 20); assert.equal(bytes.length, 64);
  assert.deepEqual(Array.from({ length: 5 }, (_, i) => view.getInt16(44 + i * 4, true)), [-32768, -16384, 0, 16384, 32767]);
  audio.left[0] = NaN;
  assert.throws(() => encodeWav(audio), /有限音频/);
});

test('duration, work, polyphony and invalid frame limits fail explicitly', () => {
  const long = project(); long.lengthBeats = 241;
  assert.throws(() => render(long), /120 秒/);
  const busy = project(Array.from({ length: 15 }, () => ({ midi: 60, start: 0, duration: 240 })));
  busy.lengthBeats = 240;
  assert.throws(() => renderAudio(busy), /8000 万/);
  const dense = project(Array.from({ length: 129 }, () => ({ midi: 60, start: 0, duration: .1 })));
  assert.throws(() => render(dense), /128/);
  assert.throws(() => render(project(), { sampleRate: NaN }), /采样率/);
  assert.throws(() => render(project(), { startBeat: 1, endBeat: 1.00000001 }), /采样帧/);
  assert.throws(() => render(project(), { startBeat: 4, endBeat: 3 }), /范围/);
  assert.equal(AUDIO_LIMITS.voiceFrames, 80_000_000);
});

test('rendering is deterministic, reports bounded progress and ignores unsupported CC', () => {
  const p = project([{ midi: 60, start: 0, duration: .2 }, { midi: 64, start: .3, duration: 1 }], 'piano');
  const progress = [];
  const a = renderAudio(p, { sampleRate: 8000 }, value => progress.push(value));
  assert.equal(progress[0], 0); assert.equal(progress.at(-1), 1);
  assert.ok(progress.every((value, i) => Number.isFinite(value) && value >= 0 && value <= 1 && (!i || value >= progress[i - 1])));
  assert.ok(a.voiceFrames > 0 && a.peakVoices >= 1);
  p.tracks[0].controls.push(cc(0, 74, 1));
  const b = render(p);
  assert.deepEqual(a.left, b.left); assert.deepEqual(a.right, b.right);
});

test('worker publishes progress, transferable audio results and clear errors', async () => {
  const url = new URL('../render-worker.mjs', import.meta.url).href;
  const worker = new Worker(`const { parentPort } = require('node:worker_threads');
    globalThis.postMessage = (data, transfer) => parentPort.postMessage(data, transfer);
    import(${JSON.stringify(url)}).then(() => parentPort.on('message', data => globalThis.onmessage({ data })));`, { eval: true });
  try {
    const messages = [];
    const run = data => new Promise((resolve, reject) => {
      const onMessage = message => {
        messages.push(message);
        if (message.type !== 'progress') { worker.off('message', onMessage); resolve(message); }
      };
      worker.once('error', reject); worker.on('message', onMessage); worker.postMessage(data);
    });
    const result = await run({ project: project(), range: { startBeat: .5, endBeat: 1.5 }, sampleRate: 8000 });
    assert.equal(result.type, 'result'); assert.equal(result.frames, 4000);
    assert.equal(result.left.length, 4000); assert.equal(result.right.length, 4000);
    assert.ok(messages.some(message => message.type === 'progress'));
    const failed = await run({ project: null });
    assert.equal(failed.type, 'error'); assert.match(failed.message, /工程/);
  } finally { await worker.terminate(); }
});
