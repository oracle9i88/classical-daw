import { validateProject, beatToSeconds } from './core.mjs';

// Deliberately bounded CPU renderer. The browser edition ships these original
// additive voices, not AU plugins, sampled instruments or a network sound bank.
export const AUDIO_LIMITS = Object.freeze({
  seconds: 120,
  voiceFrames: 80_000_000,
  simultaneousVoices: 128,
  minimumSampleRate: 8000,
  maximumSampleRate: 96000,
});

const SUPPORTED_CONTROLS = new Set([7, 10, 11, 64]);
const DEFAULT_CONTROLS = { 7: 100, 10: 64, 11: 127, 64: 0 };
const TWO_PI = Math.PI * 2;
const VOICES = {
  piano: { attack: .006, release: .16, decay: 2.8, harmonics: [1, .32, .18, .09, .055, .025] },
  strings: { attack: .08, release: .25, decay: 0, harmonics: [1, .4, .24, .15, .10, .06, .035, .02] },
  sine: { attack: .01, release: .08, decay: 0, harmonics: [1] },
};

function upperBound(events, frame) {
  let lo = 0, hi = events.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (events[mid].frame <= frame) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

function controlValue(events, frame, fallback) {
  const index = upperBound(events, frame) - 1;
  return index < 0 ? fallback : events[index].value;
}

function controllerPan(value) {
  return (value - 64) / (value < 64 ? 64 : 63);
}

function coefficients(track, values, master) {
  const gain = master * 10 ** (track.gainDb / 20) * values[7] / 127 * values[11] / 127;
  const pan = Math.max(-1, Math.min(1, track.pan + controllerPan(values[10])));
  const angle = (pan + 1) * Math.PI / 4;
  return {
    left: pan === 1 ? 0 : Math.cos(angle) * gain,
    right: pan === -1 ? 0 : Math.sin(angle) * gain,
  };
}

function prepareControls(project, track, channel, rate, startFrame, endFrame, master) {
  const controls = { 7: [], 10: [], 11: [], 64: [] };
  // Stable sorting preserves the final source value for simultaneous messages.
  for (const control of track.controls) {
    if (control.channel !== channel || !SUPPORTED_CONTROLS.has(control.controller)) continue;
    controls[control.controller].push({
      frame: Math.round(beatToSeconds(project, control.beat) * rate), value: control.value,
    });
  }
  for (const controller of Object.keys(controls)) {
    const events = controls[controller];
    events.sort((a, b) => a.frame - b.frame);
    // A pedal-up followed by pedal-down at the same frame still releases notes
    // ALREADY held by the pedal. Retaining that ordered edge is essential;
    // notes whose NoteOff is at this frame instead see the final CC state.
    if (controller === '64') continue;
    const compact = [];
    for (const event of events) {
      if (compact.length && compact.at(-1).frame === event.frame) compact[compact.length - 1] = event;
      else compact.push(event);
    }
    controls[controller] = compact;
  }
  let nextRelease = Infinity;
  for (let i = controls[64].length - 1; i >= 0; --i) {
    const event = controls[64][i];
    if (event.value < 64) nextRelease = event.frame;
    event.nextRelease = nextRelease;
  }
  const values = { ...DEFAULT_CONTROLS };
  const changes = [];
  for (const controller of [7, 10, 11]) {
    values[controller] = controlValue(controls[controller], startFrame, values[controller]);
    for (const event of controls[controller]) {
      if (event.frame > startFrame && event.frame < endFrame) changes.push({ ...event, controller });
    }
  }
  changes.sort((a, b) => a.frame - b.frame);
  const spans = [];
  let frame = startFrame;
  for (let i = 0; i < changes.length;) {
    const boundary = changes[i].frame;
    spans.push({ frame, end: boundary, ...coefficients(track, values, master) });
    while (i < changes.length && changes[i].frame === boundary) {
      values[changes[i].controller] = changes[i].value;
      ++i;
    }
    frame = boundary;
  }
  spans.push({ frame, end: endFrame, ...coefficients(track, values, master) });
  return { controls, spans };
}

function pedalRelease(events, noteEnd, projectEnd) {
  const i = upperBound(events, noteEnd);
  if (i === 0 || events[i - 1].value < 64) return noteEnd;
  // End-of-project acts as a terminal pedal reset. The release tail may extend
  // beyond a selected window, but never creates an unbounded retained voice.
  return Math.min(projectEnd, events[i - 1].nextRelease);
}

function prepareVoices(project, rate, startFrame, endFrame) {
  const voices = [], overlapEvents = [];
  const master = 10 ** (project.masterGainDb / 20);
  const projectEnd = Math.round(beatToSeconds(project, project.lengthBeats) * rate);
  const anySolo = project.tracks.some(track => track.solo);
  let voiceFrames = 0;
  for (const track of project.tracks) {
    if (track.mute || (anySolo && !track.solo)) continue;
    const definition = VOICES[track.instrument];
    if (!definition) throw new Error(`不支持的浏览器音色：${track.instrument}`);
    const channels = new Map();
    for (const note of track.notes) {
      if (!channels.has(note.channel)) channels.set(note.channel, []);
      channels.get(note.channel).push(note);
    }
    for (const [channel, notes] of channels) {
      const lane = prepareControls(project, track, channel, rate, startFrame, endFrame, master);
      const ordered = notes.map(note => ({
        note, start: Math.round(beatToSeconds(project, note.start) * rate),
        nominalEnd: Math.round(beatToSeconds(project, note.start + note.duration) * rate),
      })).sort((a, b) => a.start - b.start);
      const nextKeyStart = new Map();
      // Equal-onset unisons retain their written voices. The next STRICTLY
      // later same-key attack releases previous voices, including pedal holds.
      for (let groupEnd = ordered.length; groupEnd > 0;) {
        let groupStart = groupEnd - 1;
        while (groupStart > 0 && ordered[groupStart - 1].start === ordered[groupEnd - 1].start) --groupStart;
        for (let i = groupStart; i < groupEnd; ++i) {
          const entry = ordered[i];
          entry.nextAttack = nextKeyStart.get(entry.note.midi) ?? Infinity;
        }
        for (let i = groupStart; i < groupEnd; ++i) nextKeyStart.set(ordered[i].note.midi, ordered[i].start);
        groupEnd = groupStart;
      }
      for (const { note, start, nominalEnd, nextAttack } of ordered) {
        if (note.velocity === 0 || start >= endFrame) continue;
        const gateEnd = Math.max(start + 1, Math.min(pedalRelease(lane.controls[64], nominalEnd, projectEnd), nextAttack));
        const releaseFrames = Math.max(1, Math.round(definition.release * rate));
        const from = Math.max(startFrame, start), to = Math.min(endFrame, gateEnd + releaseFrames);
        if (from >= to) continue;
        const frequency = 440 * 2 ** ((note.midi - 69) / 12);
        const partials = definition.harmonics.map((weight, i) => ({ weight, frequency: frequency * (i + 1) }))
          .filter(partial => partial.frequency < rate / 2);
        if (!partials.length) continue;
        voiceFrames += to - from;
        if (voiceFrames > AUDIO_LIMITS.voiceFrames) {
          throw new Error('当前范围的音符计算量超过 8000 万声部采样帧，请缩短试听范围或减少声部。');
        }
        overlapEvents.push({ frame: from, change: 1 }, { frame: to, change: -1 });
        voices.push({
          start, gateEnd, releaseFrames, from, to, definition, partials, spans: lane.spans,
          amplitude: .35 * note.velocity / 127 / partials.reduce((sum, partial) => sum + partial.weight, 0),
        });
      }
    }
  }
  overlapEvents.sort((a, b) => a.frame - b.frame || a.change - b.change);
  let simultaneous = 0, peakVoices = 0;
  for (const event of overlapEvents) {
    simultaneous += event.change;
    peakVoices = Math.max(peakVoices, simultaneous);
    if (simultaneous > AUDIO_LIMITS.simultaneousVoices) {
      throw new Error('当前范围同时发声超过 128 个音（含延音和尾音），请缩短范围或减少声部。');
    }
  }
  return { voices, voiceFrames, peakVoices };
}

export function renderAudio(project, options = {}, progressCallback = () => {}) {
  project = validateProject(project);
  const { startBeat = 0, endBeat = project.lengthBeats, sampleRate = 48000 } = options;
  if (!Number.isInteger(sampleRate) || sampleRate < AUDIO_LIMITS.minimumSampleRate || sampleRate > AUDIO_LIMITS.maximumSampleRate) {
    throw new Error('采样率须为 8000 至 96000 Hz 的整数。');
  }
  if (!Number.isFinite(startBeat) || !Number.isFinite(endBeat) || startBeat < 0 || endBeat > project.lengthBeats || endBeat <= startBeat) {
    throw new Error('试听范围须在工程内，且结束位置必须晚于开始位置。');
  }
  const startSeconds = beatToSeconds(project, startBeat), endSeconds = beatToSeconds(project, endBeat);
  if (endSeconds - startSeconds > AUDIO_LIMITS.seconds + 1e-9) throw new Error('单次试听或导出最多 120 秒，请缩短范围。');
  const startFrame = Math.round(startSeconds * sampleRate), endFrame = Math.round(endSeconds * sampleRate);
  const frames = endFrame - startFrame;
  if (!Number.isSafeInteger(startFrame) || !Number.isSafeInteger(endFrame) || frames < 1) throw new Error('所选范围不足一个音频采样帧。');
  // All duration/polyphony/work limits are checked before allocating audio.
  const { voices, voiceFrames, peakVoices } = prepareVoices(project, sampleRate, startFrame, endFrame);
  const left = new Float32Array(frames), right = new Float32Array(frames);
  let completed = 0, notified = 0;
  progressCallback(0);
  for (const voice of voices) {
    const { start, gateEnd, releaseFrames, definition, partials, amplitude, spans } = voice;
    let spanIndex = Math.max(0, upperBound(spans, voice.from) - 1);
    for (let frame = voice.from; frame < voice.to;) {
      const span = spans[spanIndex];
      const to = Math.min(voice.to, span.end);
      for (; frame < to; ++frame) {
        const seconds = (frame - start) / sampleRate;
        const attack = Math.min(1, seconds / definition.attack);
        let envelope = .5 - .5 * Math.cos(Math.PI * attack);
        if (definition.decay) envelope *= Math.exp(-seconds / definition.decay);
        if (frame >= gateEnd) envelope *= .5 + .5 * Math.cos(Math.PI * (frame - gateEnd) / releaseFrames);
        let sample = 0;
        for (const partial of partials) sample += Math.sin(TWO_PI * partial.frequency * seconds) * partial.weight;
        sample *= envelope * amplitude;
        const index = frame - startFrame;
        left[index] += sample * span.left;
        right[index] += sample * span.right;
      }
      ++spanIndex;
    }
    completed += voice.to - voice.from;
    const progress = voiceFrames ? completed / voiceFrames : 1;
    if (progress - notified >= .02) { progressCallback(Math.min(.99, progress)); notified = progress; }
  }
  let peak = 0, clippedSamples = 0;
  for (let i = 0; i < frames; ++i) {
    if (!Number.isFinite(left[i]) || !Number.isFinite(right[i])) throw new Error('渲染产生非有限音频值，已停止输出。');
    peak = Math.max(peak, Math.abs(left[i]), Math.abs(right[i]));
    if (Math.abs(left[i]) > 1) { ++clippedSamples; left[i] = Math.max(-1, Math.min(1, left[i])); }
    if (Math.abs(right[i]) > 1) { ++clippedSamples; right[i] = Math.max(-1, Math.min(1, right[i])); }
  }
  progressCallback(1);
  return { left, right, sampleRate, frames, peak, clippedSamples,
    voiceFrames, peakVoices, synthesizedVoices: voices.length,
    startSeconds: startFrame / sampleRate, endSeconds: endFrame / sampleRate };
}

export function encodeWav(rendered) {
  const { left, right, sampleRate, frames } = rendered;
  if (!(left instanceof Float32Array) || !(right instanceof Float32Array) ||
      !Number.isSafeInteger(frames) || frames < 1 || left.length !== frames || right.length !== frames ||
      !Number.isInteger(sampleRate) || sampleRate < AUDIO_LIMITS.minimumSampleRate || sampleRate > AUDIO_LIMITS.maximumSampleRate ||
      frames > sampleRate * AUDIO_LIMITS.seconds) throw new Error('无效的双声道音频缓冲。');
  const bytes = new Uint8Array(44 + frames * 4), view = new DataView(bytes.buffer);
  const text = (offset, value) => { for (let i = 0; i < value.length; ++i) bytes[offset + i] = value.charCodeAt(i); };
  text(0, 'RIFF'); view.setUint32(4, bytes.length - 8, true); text(8, 'WAVE'); text(12, 'fmt ');
  view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 2, true);
  view.setUint32(24, sampleRate, true); view.setUint32(28, sampleRate * 4, true);
  view.setUint16(32, 4, true); view.setUint16(34, 16, true); text(36, 'data'); view.setUint32(40, frames * 4, true);
  for (let i = 0; i < frames; ++i) {
    for (let channel = 0; channel < 2; ++channel) {
      const sample = channel ? right[i] : left[i];
      if (!Number.isFinite(sample) || sample < -1 || sample > 1) throw new Error('WAV 导出需要已完成削波检查的有限音频。');
      view.setInt16(44 + i * 4 + channel * 2, Math.round(sample * (sample < 0 ? 32768 : 32767)), true);
    }
  }
  return bytes;
}
