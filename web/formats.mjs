// Dependency-free interchange. Imported timing uses quarter-note beats at PPQ.
// These adapters preserve the supported playback data, not engraved page layout.
import { validateProject, PPQ, LIMITS } from './core.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8');
const warn = (warnings, message) => { if (!warnings.includes(message)) warnings.push(message); };
const fail = (message) => { throw new Error(message); };
const int = (value, low, high, label) => {
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < low || number > high) fail(`${label} 无效`);
  return number;
};
const q = (beats, warnings) => {
  if (!Number.isFinite(beats) || beats < 0 || beats > LIMITS.beats) fail('时间超出网页工程范围');
  const rounded = Math.round(beats * PPQ) / PPQ;
  if (Math.abs(rounded - beats) > 1e-9) warn(warnings, `原始时间已舍入到 1/${PPQ} 四分音符；没有按卷帘网格量化。`);
  return rounded;
};
const initialProject = (title) => ({
  format: 'classical-daw-web-project', version: 2, ppq: PPQ, title,
  lengthBeats: 0, tempoChanges: [], meterChanges: [], tracks: [], nextId: 1,
});
const newTrack = (id, name) => ({ id, name, instrument: 'piano', gainDb: -9, pan: 0,
  mute: false, solo: false, notes: [], controls: [] });
function normalizeMap(events, fallback, warnings, label) {
  const result = new Map();
  for (const event of events) {
    const previous = result.get(event.beat);
    if (previous && JSON.stringify(previous) !== JSON.stringify(event)) {
      warn(warnings, `${label}在同一拍出现不同值，按源文件顺序采用最后一个。`);
    }
    result.set(event.beat, event);
  }
  if (!result.has(0)) result.set(0, fallback);
  return [...result.values()].sort((a, b) => a.beat - b.beat);
}
function finish(project, warnings) {
  if (!project.tracks.length) fail('文件没有可用的声部');
  project.tempoChanges = normalizeMap(project.tempoChanges, { beat: 0, bpm: 96 }, warnings, '速度');
  project.meterChanges = normalizeMap(project.meterChanges,
    { beat: 0, numerator: 4, denominator: 4 }, warnings, '拍号');
  project.lengthBeats = q(Math.max(project.lengthBeats, 1 / PPQ,
    ...project.tempoChanges.map(e => e.beat), ...project.meterChanges.map(e => e.beat)), warnings);
  let notes = 0, controls = 0;
  for (const track of project.tracks) {
    notes += track.notes.length; controls += track.controls.length;
    if (notes > LIMITS.notes || controls > LIMITS.controls) fail('音符或控制器数量超过网页工程上限');
    track.notes.sort((a, b) => a.start - b.start || a.id - b.id);
    track.controls.sort((a, b) => a.beat - b.beat); // stable: retain same-tick source order
    if (track.controls.some(c => ![7, 10, 11, 64].includes(c.controller))) {
      warn(warnings, '所有 MIDI CC 已保存；网页试听目前只解释 CC7、CC10、CC11、CC64，其他 CC 仅保留供 MIDI 导出。');
    }
  }
  return { project: validateProject(project), warnings };
}

export function parseMidiFile(input) {
  const bytes = input instanceof Uint8Array ? input : input instanceof ArrayBuffer ? new Uint8Array(input) : null;
  if (!bytes || bytes.length < 14 || bytes.length > LIMITS.fileBytes) fail('MIDI 文件为空、过大或类型无效');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u16 = at => view.getUint16(at);
  const u32 = at => view.getUint32(at);
  const tag = at => String.fromCharCode(...bytes.subarray(at, at + 4));
  if (tag(0) !== 'MThd' || u32(4) !== 6) fail('MIDI 文件头无效');
  const format = u16(8), count = u16(10), division = u16(12);
  if (![0, 1].includes(format) || !count || (format === 0 && count !== 1)) fail('只支持 MIDI Type 0/1');
  if (!division || division & 0x8000) fail('MIDI SMPTE 时间格式不受支持；请导出 PPQ MIDI');
  // Permit an extra conductor track, but never silently discard musical tracks.
  if (count > LIMITS.tracks + 1) fail(`MIDI 超过 ${LIMITS.tracks} 个声部及一个指挥轨的上限`);
  const project = initialProject('Imported MIDI'), warnings = [];
  let offset = 14, totalNotes = 0, totalControls = 0;
  const tempoEvents = [], meterEvents = [];
  for (let trackIndex = 0; trackIndex < count; ++trackIndex) {
    if (offset + 8 > bytes.length || tag(offset) !== 'MTrk') fail('缺少 MIDI MTrk 数据块');
    const end = offset + 8 + u32(offset + 4);
    offset += 8;
    if (end > bytes.length) fail('MIDI 轨道数据被截断');
    let cursor = offset, tick = 0, running = 0, ended = false, hasChannel = false, port = null;
    const track = newTrack(`midi-${trackIndex + 1}`, `Track ${trackIndex + 1}`);
    const active = new Map(), lyrics = new Map();
    const pedalTicks = new Map(), releaseTicks = new Map();
    const recordPedalReleaseOrder = (beat, channel, isPedal) => {
      const tick960 = Math.round(beat * PPQ), bit = 1 << channel;
      const own = isPedal ? pedalTicks : releaseTicks, other = isPedal ? releaseTicks : pedalTicks;
      own.set(tick960, (own.get(tick960) || 0) | bit);
      if ((other.get(tick960) || 0) & bit) warn(warnings, '同拍 CC64 与 NoteOff 已按工程规则归一化：CC 按源序先执行，再 NoteOff，再 NoteOn；原 MIDI 的放键/踏板顺序可能改变。');
    };
    const read = () => { if (cursor >= end) fail('MIDI 事件被截断'); return bytes[cursor++]; };
    const vlq = () => {
      let value = 0;
      for (let i = 0; i < 4; ++i) { const b = read(); value = value * 128 + (b & 127); if (!(b & 128)) return value; }
      return fail('MIDI VLQ 超过四字节');
    };
    while (cursor < end) {
      tick += vlq();
      if (!Number.isSafeInteger(tick) || tick / division > LIMITS.beats) fail('MIDI 时长超过网页工程上限');
      const beat = q(tick / division, warnings);
      let status = read();
      if (status < 0x80) {
        if (!running) fail('MIDI running status 缺少先前通道状态');
        --cursor; status = running;
      }
      if (status === 0xff) {
        running = 0;
        const type = read(), size = vlq();
        if (cursor + size > end) fail('MIDI 元事件被截断');
        const data = bytes.subarray(cursor, cursor + size);
        cursor += size;
        if (type === 0x2f) {
          if (size || cursor !== end) fail('MIDI End of Track 必须为空且位于轨道结尾');
          ended = true; project.lengthBeats = Math.max(project.lengthBeats, beat); break;
        }
        if (type === 0x51) {
          if (size !== 3) fail('MIDI tempo 元事件长度无效');
          const micros = data[0] * 65536 + data[1] * 256 + data[2];
          const bpm = 60000000 / micros;
          if (!micros || bpm < 20 - 1e-5 || bpm > 400 + 1e-5) fail('MIDI 速度超出 20–400 BPM');
          tempoEvents.push({ beat, bpm: Math.min(400, Math.max(20, bpm)) });
        } else if (type === 0x58) {
          if (size !== 4 || !data[0] || data[0] > 32 || data[1] > 5) fail('MIDI 拍号不受支持');
          meterEvents.push({ beat, numerator: data[0], denominator: 2 ** data[1] });
          if (data[2] !== 24 || data[3] !== 8) warn(warnings, 'MIDI 节拍器点击与记谱细分元数据不在网页工程中保存。');
        } else if (type === 0x21) {
          if (size !== 1 || data[0] > 127 || port !== null && port !== data[0]) fail('同一 MIDI 轨道切换端口暂不支持');
          port = data[0];
          warn(warnings, 'MIDI 端口按轨道独立导入；导出也为每个声部分配独立端口，请在外部 DAW 检查音源路由。');
        } else if (type === 3) track.name = decoder.decode(data) || track.name;
        else if (type === 5) {
          const list = lyrics.get(tick) || []; list.push(decoder.decode(data)); lyrics.set(tick, list);
        } else if (![1, 2, 4].includes(type)) {
          warn(warnings, `MIDI 元事件 0x${type.toString(16)} 未在网页工程中保存。`);
        } else if (type !== 3) warn(warnings, 'MIDI 文字、版权和乐器名称元数据仅作说明，未逐事件保存。');
      } else if (status === 0xf0 || status === 0xf7) {
        running = 0;
        const size = vlq();
        if (cursor + size > end) fail('MIDI SysEx 被截断');
        cursor += size; warn(warnings, 'MIDI SysEx 未保存或执行；音色配置可能与原文件不同。');
      } else if (status >= 0x80 && status < 0xf0) {
        running = status; hasChannel = true;
        const type = status & 0xf0, channel = status & 15;
        const a = read(), b = type === 0xc0 || type === 0xd0 ? 0 : read();
        if (a > 127 || b > 127) fail('MIDI 通道事件数据字节无效');
        const key = `${channel}/${a}`;
        if (type === 0x90 && b) {
          if (++totalNotes > LIMITS.notes) fail('MIDI 音符数量超过网页工程上限');
          const stack = active.get(key) || [];
          if (stack.length) warn(warnings, '检测到同通道同音高重叠；NoteOff 按 LIFO 配对，保留每个起音。');
          const lyricList = lyrics.get(tick) || [];
          const note = { id: project.nextId++, midi: a, start: beat, duration: 0,
            velocity: b, channel, lyric: lyricList.shift() || '' };
          stack.push(note); active.set(key, stack); track.notes.push(note);
        } else if (type === 0x80 || (type === 0x90 && !b)) {
          const stack = active.get(key);
          if (!stack?.length) fail('MIDI NoteOff 没有可配对的 NoteOn');
          const note = stack.pop(); note.duration = q(beat - note.start, warnings);
          if (note.duration <= 0) fail('MIDI 音符在 960 PPQ 下无正时值，无法无损导入');
          recordPedalReleaseOrder(beat, channel, false);
          if (type === 0x80 && b) warn(warnings, 'MIDI 松键力度未在网页工程中保存。');
        } else if (type === 0xb0) {
          if (++totalControls > LIMITS.controls) fail('MIDI 控制器数量超过网页工程上限');
          track.controls.push({ beat, channel, controller: a, value: b });
          if (a === 64) recordPedalReleaseOrder(beat, channel, true);
        } else {
          warn(warnings, 'MIDI Program Change、弯音和触后事件未保存；网页试听使用所选内置音色。');
          if (type === 0xc0 && a >= 40 && a <= 51) track.instrument = 'strings';
        }
      } else fail(`不支持的 MIDI 系统状态 0x${status.toString(16)}`);
    }
    if (!ended) fail('MIDI 轨道缺少 End of Track');
    if ([...active.values()].some(stack => stack.length)) fail('MIDI 轨道结尾仍有未配对的 NoteOn');
    if ([...lyrics.values()].some(list => list.length)) warn(warnings, '没有同拍音符可关联的 MIDI 歌词未保存。');
    if (hasChannel || trackIndex > 0 || count === 1) project.tracks.push(track);
    else {
      if (track.name !== `Track ${trackIndex + 1}`) project.title = track.name;
      warn(warnings, '首个无通道事件的 MIDI 轨道作为指挥轨读取，速度与拍号已合并至工程时间图。');
    }
    offset = end;
  }
  if (offset !== bytes.length) fail('MIDI 末尾存在未解释的数据块');
  if (project.tracks.length > LIMITS.tracks) fail('MIDI 声部数量超过网页工程上限');
  if (!tempoEvents.some(e => e.beat === 0)) {
    tempoEvents.unshift({ beat: 0, bpm: 120 });
    warn(warnings, '文件起点未声明速度，采用 MIDI 默认 120 BPM；后续速度事件仍按原位置保留。');
  }
  project.tempoChanges = tempoEvents; project.meterChanges = meterEvents;
  return finish(project, warnings);
}

const vlqBytes = (value) => {
  int(value, 0, 0x0fffffff, 'MIDI delta');
  const result = [value & 127];
  while ((value = Math.floor(value / 128))) result.unshift((value & 127) | 128);
  return result;
};
const be16 = n => [(n >> 8) & 255, n & 255];
const be32 = n => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
const meta = (type, text) => { const data = encoder.encode(text); return [255, type, ...vlqBytes(data.length), ...data]; };
function midiTrack(events, lastTick) {
  events.sort((a, b) => a.tick - b.tick || a.order - b.order);
  const bytes = []; let cursor = 0;
  for (const event of events) {
    bytes.push(...vlqBytes(event.tick - cursor), ...event.bytes); cursor = event.tick;
  }
  bytes.push(...vlqBytes(lastTick - cursor), 255, 47, 0);
  return [77, 84, 114, 107, ...be32(bytes.length), ...bytes];
}
export function midiFile(input) {
  const project = validateProject(input), lastTick = Math.round(project.lengthBeats * PPQ);
  const conductor = [{ tick: 0, order: 0, bytes: meta(3, project.title) }];
  for (const e of project.tempoChanges) {
    const micros = Math.round(60000000 / e.bpm);
    conductor.push({ tick: Math.round(e.beat * PPQ), order: 1,
      bytes: [255, 81, 3, (micros >> 16) & 255, (micros >> 8) & 255, micros & 255] });
  }
  for (const e of project.meterChanges) conductor.push({ tick: Math.round(e.beat * PPQ), order: 2,
    bytes: [255, 88, 4, e.numerator, Math.log2(e.denominator), 24, 8] });
  const chunks = [midiTrack(conductor, lastTick)];
  for (const [trackIndex, track] of project.tracks.entries()) {
    // SMF track boundaries alone do not isolate MIDI channels. A distinct port
    // prevents CC/note collisions between independent web instruments.
    const byKey = new Map();
    for (const note of [...track.notes].sort((a, b) => a.start - b.start || b.duration - a.duration)) {
      const key = `${note.channel}/${note.midi}`, stack = byKey.get(key) || [];
      while (stack.length && stack.at(-1) <= note.start) stack.pop();
      const end = note.start + note.duration;
      if (stack.length && end > stack.at(-1) + 1e-9) fail('同通道同音高存在交错重叠，MIDI 无音符 ID，无法按 LIFO 保真；请保存 JSON 或 MusicXML');
      stack.push(end); byKey.set(key, stack);
    }
    const events = [{ tick: 0, order: -10, bytes: meta(3, track.name) },
      { tick: 0, order: -9, bytes: [255, 0x21, 1, trackIndex] }];
    // Shared playback contract: stable source-ordered CC, then NoteOff, then NoteOn.
    for (const c of track.controls) events.push({ tick: Math.round(c.beat * PPQ), order: -2,
      bytes: [0xb0 | c.channel, c.controller, c.value] });
    for (const note of [...track.notes].sort((a, b) => a.start - b.start || b.duration - a.duration)) {
      const start = Math.round(note.start * PPQ), end = Math.round((note.start + note.duration) * PPQ);
      if (end <= start) fail('音符时值小于 MIDI 960 PPQ 的一个 tick');
      if (note.lyric) events.push({ tick: start, order: 1, bytes: meta(5, note.lyric) });
      events.push({ tick: start, order: 2, bytes: [0x90 | note.channel, note.midi, note.velocity] });
      events.push({ tick: end, order: -1, bytes: [0x80 | note.channel, note.midi, 0] });
    }
    chunks.push(midiTrack(events, lastTick));
  }
  const size = 14 + chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  if (size > LIMITS.fileBytes) fail('导出的 MIDI 超出文件大小上限');
  const output = new Uint8Array(size); output.set([77, 84, 104, 100, 0, 0, 0, 6, 0, 1, ...be16(chunks.length), ...be16(PPQ)]);
  let at = 14; for (const chunk of chunks) { output.set(chunk, at); at += chunk.length; }
  return output;
}

const children = (element, name) => Array.from(element?.children || []).filter(e => e.localName === name);
const child = (element, name) => children(element, name)[0] || null;
const content = (element, name, fallback = '') => child(element, name)?.textContent.trim() ?? fallback;
const descendants = (element, name) => Array.from(element.getElementsByTagName('*')).filter(e => e.localName === name);
const xmlEscape = text => String(text).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[c]));
const EXTENSION = 'classical-daw-web-playback-v2';
function numberElement(element, name, fallback) {
  const node = child(element, name);
  if (!node) { if (fallback !== undefined) return fallback; fail(`MusicXML 缺少 ${name}`); }
  if (!node.textContent.trim()) fail(`MusicXML ${name} 为空`);
  const value = Number(node.textContent.trim());
  if (!Number.isFinite(value)) fail(`MusicXML ${name} 不是数字`);
  return value;
}
function readTime(time) {
  const beats = children(time, 'beats'), types = children(time, 'beat-type');
  if (beats.length !== 1 || types.length !== 1) fail('复合或无拍号 MusicXML 暂不支持；请导出一个明确的拍号');
  const numerator = beats[0].textContent.trim().split('+').reduce((sum, v) => sum + int(v, 1, 32, '拍号分子'), 0);
  const denominator = int(types[0].textContent.trim(), 1, 32, '拍号分母');
  if (numerator > 32 || ![1, 2, 4, 8, 16, 32].includes(denominator)) fail('MusicXML 拍号超出网页范围');
  return { numerator, denominator };
}
function readTempo(direction, warnings) {
  const sound = child(direction, 'sound');
  let bpm;
  const metronome = descendants(direction, 'metronome')[0];
  if (metronome) {
    const unit = content(metronome, 'beat-unit');
    const value = { maxima: 32, long: 16, breve: 8, whole: 4, half: 2, quarter: 1,
      eighth: .5, '16th': .25, '32nd': .125, '64th': .0625 }[unit];
    if (!value || !child(metronome, 'per-minute')) warn(warnings, '复杂节拍等式速度标记未解释。');
    else bpm = numberElement(metronome, 'per-minute') * value * (2 - 2 ** -children(metronome, 'beat-unit-dot').length);
  }
  if (sound?.hasAttribute('tempo')) {
    const next = Number(sound.getAttribute('tempo'));
    if (bpm !== undefined && Math.abs(bpm - next) > 1e-7) warn(warnings, '同一 direction 的 metronome 与 sound 速度不同，采用显式 sound tempo。');
    bpm = next;
  }
  if (bpm !== undefined && (!Number.isFinite(bpm) || bpm < 20 || bpm > 400)) fail('MusicXML 速度超出 20–400 BPM');
  return bpm;
}
export function parseMusicXml(text) {
  if (typeof text !== 'string' || !text.length || encoder.encode(text).length > LIMITS.fileBytes) fail('MusicXML 文件为空或超出大小上限');
  if (typeof DOMParser === 'undefined') fail('当前环境没有 DOMParser；请在浏览器中导入 MusicXML');
  if (/<!ENTITY\b/i.test(text) || /<!DOCTYPE[^>]*\[/i.test(text)) fail('MusicXML 不允许 ENTITY 或内部 DTD 子集');
  // Ignore external declarations without resolving, fetching or executing them.
  const safeText = text.replace(/<!DOCTYPE\s+[^>]*>/gi, '');
  const xml = new DOMParser().parseFromString(safeText, 'application/xml');
  if (descendants(xml, 'parsererror').length) fail('MusicXML 语法无效');
  const root = xml.documentElement;
  if (root?.localName !== 'score-partwise') fail('只支持 score-partwise MusicXML；压缩 .mxl 请先解压');
  const warnings = [], project = initialProject(content(child(root, 'work'), 'work-title', content(root, 'movement-title', 'Imported MusicXML')));
  const definitions = new Map();
  for (const definition of children(child(root, 'part-list'), 'score-part')) {
    const id = definition.getAttribute('id');
    if (!id || definitions.has(id)) fail('MusicXML part-list 声部 ID 缺失或重复');
    const instruments = new Map();
    for (const midi of children(definition, 'midi-instrument')) {
      const channel = int(numberElement(midi, 'midi-channel', 1), 1, 16, 'MIDI 通道') - 1;
      const program = int(numberElement(midi, 'midi-program', 1), 1, 128, 'MIDI program') - 1;
      instruments.set(midi.getAttribute('id'), { channel, program });
    }
    definitions.set(id, { name: content(definition, 'part-name', id), instruments });
  }
  const parts = children(root, 'part');
  if (!parts.length || parts.length > LIMITS.tracks) fail(`MusicXML 必须有 1–${LIMITS.tracks} 个声部`);
  if (descendants(root, 'repeat').length || descendants(root, 'ending').length || descendants(root, 'segno').length || descendants(root, 'coda').length || descendants(root, 'sound').some(e => ['dacapo', 'dalsegno', 'tocoda', 'fine'].some(a => e.hasAttribute(a)))) {
    warn(warnings, '反复、房子和跳转尚未展开；此次按谱面书写顺序演奏一次。');
  }
  if (descendants(root, 'ornaments').length || descendants(root, 'articulations').length || descendants(root, 'wedge').length || descendants(root, 'slur').length) {
    warn(warnings, '装饰音型、奏法、连奏线和渐强弱记号不自动生成演奏；原始谱面布局未保存。');
  }
  if (['fermata', 'arpeggiate', 'technical', 'damp', 'damp-all'].some(name => descendants(root, name).length)) {
    warn(warnings, '延长记号、琶音及特殊演奏技法未展开，按写出的音符时值播放。');
  }
  if (descendants(root, 'octave-shift').length) fail('检测到 MusicXML 八度移位线；网页暂不解释 octave-shift，请先转换为实际音高再导入');
  if (descendants(root, 'scordatura').length) fail('检测到非常规定弦 scordatura，无法保证网页发声音高');
  if (descendants(root, 'key').length) warn(warnings, '音高按每个 note 的 pitch/alter 读取；调号拼写和谱面排版不在网页工程中保存。');
  const seen = new Set(); let totalNotes = 0, totalControls = 0;
  for (const part of parts) {
    const id = part.getAttribute('id'), definition = definitions.get(id);
    if (!id || seen.has(id) || !definition) fail('MusicXML part ID 重复或不在 part-list 中');
    seen.add(id);
    const track = newTrack(id, definition.name), defaultInstrument = definition.instruments.values().next().value || { channel: 0, program: 0 };
    if (defaultInstrument.program >= 40 && defaultInstrument.program <= 51) track.instrument = 'strings';
    let divisions = 0, measureStart = 0, meter = { numerator: 4, denominator: 4 }, transpose = 0, velocity = 90;
    const ties = new Map(), measures = children(part, 'measure');
    if (!measures.length) fail(`声部 ${id} 没有小节`);
    for (const measure of measures) {
      let cursor = 0, extent = 0, lastOnset = null, measureMeter = meter;
      let fullMeasureRest = false;
      for (const element of Array.from(measure.children)) {
        if (element.localName === 'attributes') {
          if (child(element, 'divisions')) divisions = numberElement(element, 'divisions');
          if (!Number.isFinite(divisions) || divisions < 0) fail('MusicXML divisions 必须为正数');
          const times = children(element, 'time');
          if (times.length > 1 && new Set(times.map(t => JSON.stringify(readTime(t)))).size > 1) fail('同一声部不同谱表的多重拍号暂不支持');
          if (times.length) {
            meter = readTime(times[0]);
            if (Math.abs(cursor) < 1e-8) measureMeter = meter;
            project.meterChanges.push({ beat: q(measureStart + cursor, warnings), ...meter });
          }
          const transpositions = children(element, 'transpose');
          if (transpositions.some(t => t.hasAttribute('number'))) fail('MusicXML 按谱表指定的移调（transpose number）暂不支持；未将该移调误用于整个声部');
          if (transpositions.length > 1) fail('同一声部多个移调设置暂不支持');
          if (transpositions.length) {
            const t = transpositions[0];
            if (child(t, 'double')) fail('MusicXML double 移调暂不支持');
            transpose = int(numberElement(t, 'chromatic', 0), -127, 127, '移调半音') + 12 * int(numberElement(t, 'octave-change', 0), -10, 10, '移调八度');
            if (transpose) warn(warnings, 'MusicXML 移调已转换为实际发声音高；原始移调记谱不保留。');
          }
        } else if (element.localName === 'backup' || element.localName === 'forward') {
          if (!(divisions > 0)) fail('MusicXML 时值之前缺少 divisions');
          const duration = numberElement(element, 'duration') / divisions;
          if (!(duration > 0)) fail('MusicXML backup/forward 时值必须为正');
          cursor += element.localName === 'backup' ? -duration : duration;
          if (cursor < -1e-8) fail('MusicXML backup 越过小节起点');
          cursor = Math.max(0, cursor); extent = Math.max(extent, cursor); lastOnset = null;
        } else if (element.localName === 'direction' || element.localName === 'sound') {
          const direction = element;
          const offset = numberElement(direction, 'offset', 0);
          if (offset && !(divisions > 0)) fail('MusicXML offset 之前缺少 divisions');
          const at = q(measureStart + cursor + (offset ? offset / divisions : 0), warnings);
          const sound = element.localName === 'sound' ? element : child(element, 'sound');
          const bpm = element.localName === 'sound' ? (sound.hasAttribute('tempo') ? Number(sound.getAttribute('tempo')) : undefined) : readTempo(direction, warnings);
          if (bpm !== undefined) {
            if (!Number.isFinite(bpm) || bpm < 20 || bpm > 400) fail('MusicXML 速度超出 20–400 BPM');
            project.tempoChanges.push({ beat: at, bpm });
          }
          if (sound?.hasAttribute('dynamics')) {
            const percent = Number(sound.getAttribute('dynamics'));
            if (!Number.isFinite(percent) || percent < 0 || percent > 127 / .9) fail('MusicXML dynamics 无效');
            velocity = Math.max(1, Math.min(127, Math.round(percent * .9)));
          } else {
            const dynamics = descendants(direction, 'dynamics')[0];
            const value = { pppp: 16, ppp: 24, pp: 36, p: 48, mp: 60, mf: 76, f: 92, ff: 108, fff: 120, ffff: 127 }[dynamics?.children[0]?.localName];
            if (value) { velocity = value; warn(warnings, '未指定 sound dynamics 的力度符号采用近似 MIDI 力度。'); }
          }
          for (const pedal of descendants(direction, 'pedal')) {
            const type = pedal.getAttribute('type');
            const add = value => {
              if (++totalControls > LIMITS.controls) fail('MusicXML 控制器数量超出上限');
              track.controls.push({ beat: at, channel: defaultInstrument.channel, controller: 64, value });
            };
            if (type === 'start' || type === 'resume') add(127);
            else if (type === 'stop' || type === 'discontinue') add(0);
            else if (type === 'change') { add(0); add(127); }
            else if (type !== 'continue') warn(warnings, '未知踏板方向未解释。');
          }
          if (sound?.hasAttribute('damper-pedal')) {
            const raw = sound.getAttribute('damper-pedal'), value = raw === 'yes' ? 127 : raw === 'no' ? 0 : Math.round(Number(raw) * 1.27);
            int(value, 0, 127, '踏板值');
            if (++totalControls > LIMITS.controls) fail('MusicXML 控制器数量超出上限');
            track.controls.push({ beat: at, channel: defaultInstrument.channel, controller: 64, value });
          }
          project.lengthBeats = Math.max(project.lengthBeats, at);
        } else if (element.localName === 'note') {
          if (child(element, 'grace')) fail('检测到倚音（grace）：网页尚未定义借时演奏，未导入；请保留原谱并先在记谱软件展开为有时值音符');
          if (child(element, 'cue')) warn(warnings, '提示音 cue 按写出的时值播放。');
          if (element.getAttribute('print-object') === 'no') warn(warnings, '隐藏音符仍按其 pitch/duration 播放，请核对导出软件的辅助声部。');
          if (child(element, 'unpitched')) fail('无固定音高打击乐暂不支持；未丢弃声部');
          const rest = child(element, 'rest'), chord = child(element, 'chord');
          const isFullRest = rest?.getAttribute('measure') === 'yes';
          let duration;
          if (!child(element, 'duration') && isFullRest) duration = meter.numerator * 4 / meter.denominator;
          else {
            if (!(divisions > 0)) fail('MusicXML note 之前缺少 divisions');
            duration = numberElement(element, 'duration') / divisions;
          }
          if (!(duration > 0)) fail('MusicXML 音符时值必须为正');
          if (chord && lastOnset === null) fail('MusicXML chord 没有同组的第一个音符');
          const onset = chord ? lastOnset : cursor;
          extent = Math.max(extent, onset + duration);
          fullMeasureRest ||= isFullRest;
          if (!rest) {
            const pitch = child(element, 'pitch');
            if (!pitch) fail('MusicXML 音符缺少 pitch');
            const pitchClass = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[content(pitch, 'step')];
            if (pitchClass === undefined) fail('MusicXML 音名无效');
            const alter = numberElement(pitch, 'alter', 0);
            if (!Number.isInteger(alter)) fail('微分音 pitch alter 无法用网页 MIDI 半音音高保真表示');
            const midi = int((int(numberElement(pitch, 'octave'), -1, 9, '八度') + 1) * 12 + pitchClass + alter + transpose, 0, 127, '音高');
            const start = q(measureStart + onset, warnings), stop = q(measureStart + onset + duration, warnings);
            if (stop <= start) fail('MusicXML 音符在 960 PPQ 下无正时值');
            const instrumentId = child(element, 'instrument')?.getAttribute('id');
            if (instrumentId && !definition.instruments.has(instrumentId)) fail('音符引用未定义的 midi-instrument');
            const channel = instrumentId ? definition.instruments.get(instrumentId).channel : defaultInstrument.channel;
            const lyrics = children(element, 'lyric');
            if (lyrics.length > 1) warn(warnings, '同一音符多段歌词仅保存第一段；请保留原始 MusicXML。');
            const lyric = lyrics[0] ? children(lyrics[0], 'text').map(e => e.textContent).join(' ') : '';
            const ownDynamics = element.hasAttribute('dynamics') ? Number(element.getAttribute('dynamics')) * .9 : velocity;
            if (!Number.isFinite(ownDynamics) || ownDynamics < 0 || ownDynamics > 127 + 1e-8) fail('MusicXML note dynamics 无效');
            const key = `${content(element, 'voice', '1')}/${content(element, 'staff', '1')}/${channel}/${midi}`;
            const types = new Set([...children(element, 'tie'), ...descendants(element, 'tied')].map(t => t.getAttribute('type')));
            const previous = ties.get(key);
            let note;
            if (types.has('stop') && previous && Math.abs(previous.start + previous.duration - start) < 1 / (2 * PPQ)) {
              note = previous; note.duration = q(stop - note.start, warnings);
              if (!note.lyric && lyric) note.lyric = lyric;
            } else {
              if (types.has('stop')) warn(warnings, '未对齐或缺少起点的延音片段保留为独立起音；请核对原谱。');
              if (++totalNotes > LIMITS.notes) fail('MusicXML 音符数量超出上限');
              note = { id: project.nextId++, midi, start, duration: q(stop - start, warnings), velocity: Math.max(1, Math.round(ownDynamics)), channel, lyric };
              track.notes.push(note);
            }
            if (types.has('start')) {
              if (previous && !types.has('stop')) warn(warnings, '重叠的同声部延音起点不能唯一配对；保留实际片段并使用最后一个起点。');
              ties.set(key, note);
            } else ties.delete(key);
          }
          if (!chord) { lastOnset = onset; cursor += duration; }
        } else if (['harmony', 'figured-bass', 'listening'].includes(element.localName)) {
          warn(warnings, '和弦名称、数字低音与 listening 元数据不在网页工程中保存。');
        }
      }
      const nominal = measureMeter.numerator * 4 / measureMeter.denominator;
      const implicit = measure.getAttribute('implicit') === 'yes';
      // An empty/full-rest normal bar carries its nominal duration. An implicit
      // pickup uses its actual extent; arbitrary printed measure labels are IDs.
      const span = implicit && extent > 0 && !fullMeasureRest ? extent : Math.max(nominal, extent);
      measureStart += span; // Keep raw cumulative time; round endpoints, not each bar.
      q(measureStart, warnings); // Validate bounds and expose sub-tick normalization.
      project.lengthBeats = Math.max(project.lengthBeats, measureStart);
    }
    if (ties.size) warn(warnings, '乐谱末尾有未闭合延音起点，按已写出的音符时值演奏。');
    project.tracks.push(track);
  }
  const fields = descendants(child(root, 'identification') || root, 'miscellaneous-field').filter(e => e.getAttribute('name') === EXTENSION);
  if (fields.length > 1) fail('网页播放扩展重复');
  if (fields.length) {
    let extension; try { extension = JSON.parse(fields[0].textContent); } catch { fail('网页播放扩展 JSON 无效'); }
    if (!extension || extension.version !== 1 || !Array.isArray(extension.tracks) || extension.tracks.length !== project.tracks.length) fail('网页播放扩展与声部数量不符');
    for (const track of project.tracks) {
      const entries = extension.tracks.filter(t => t.id === track.id);
      if (entries.length !== 1 || !Array.isArray(entries[0].controls)) fail('网页播放扩展与声部 ID 不符');
      const e = entries[0];
      track.instrument = e.instrument; track.gainDb = e.gainDb; track.pan = e.pan; track.mute = e.mute; track.solo = e.solo;
      track.controls = e.controls;
    }
    project.masterGainDb = extension.masterGainDb ?? -6;
    warn(warnings, '已读取本网页的 CC 与混音扩展；第三方 MusicXML 软件可能忽略这些扩展。');
  }
  if (!project.tempoChanges.some(e => e.beat === 0)) warn(warnings, 'MusicXML 起点未声明速度，试听暂按 96 BPM；这是试听假设，请按乐谱核对。');
  return finish(project, warnings);
}

const pitchXml = midi => {
  const spellings = [['C', 0], ['C', 1], ['D', 0], ['D', 1], ['E', 0], ['F', 0], ['F', 1], ['G', 0], ['G', 1], ['A', 0], ['A', 1], ['B', 0]];
  const [step, alter] = spellings[midi % 12];
  return `<pitch><step>${step}</step>${alter ? `<alter>${alter}</alter>` : ''}<octave>${Math.floor(midi / 12) - 1}</octave></pitch>`;
};
export function musicXml(input) {
  const project = validateProject(input), end = Math.round(project.lengthBeats * PPQ);
  const meters = project.meterChanges.map(e => ({ ...e, tick: Math.round(e.beat * PPQ) }));
  const bars = []; let start = 0, meterIndex = 0;
  while (start < end) {
    while (meterIndex + 1 < meters.length && meters[meterIndex + 1].tick <= start) ++meterIndex;
    const meter = meters[meterIndex], nominal = meter.numerator * 4 * PPQ / meter.denominator;
    const next = Math.min(end, start + nominal, meters[meterIndex + 1]?.tick ?? Infinity);
    if (next <= start) fail('拍号图无法划分有效小节');
    bars.push({ start, end: next, meter, implicit: next - start !== nominal }); start = next;
  }
  // Only playback fields absent from standard MusicXML are stored in this named
  // extension. Notes/timing are always parsed from the visible MusicXML score.
  const extension = { version: 1, masterGainDb: project.masterGainDb, tracks: project.tracks.map((t, i) => ({ id: `P${i + 1}`, instrument: t.instrument,
    gainDb: t.gainDb, pan: t.pan, mute: t.mute, solo: t.solo, controls: t.controls })) };
  const partList = project.tracks.map((track, i) => {
    const channels = [...new Set(track.notes.map(n => n.channel).concat(track.controls.map(c => c.channel), [0]))].sort((a, b) => a - b);
    return `<score-part id="P${i + 1}"><part-name>${xmlEscape(track.name)}</part-name>${channels.map(c => `<score-instrument id="P${i + 1}C${c}"><instrument-name>${xmlEscape(track.name)}</instrument-name></score-instrument>`).join('')}${channels.map(c => `<midi-instrument id="P${i + 1}C${c}"><midi-channel>${c + 1}</midi-channel><midi-program>${track.instrument === 'strings' ? 49 : 1}</midi-program></midi-instrument>`).join('')}</score-part>`;
  }).join('\n');
  let outputBytes = encoder.encode(partList).length + encoder.encode(JSON.stringify(extension)).length;
  const countOutput = text => {
    outputBytes += encoder.encode(text).length;
    if (outputBytes > LIMITS.fileBytes) fail('导出的 MusicXML 超出文件大小上限');
    return text;
  };
  const parts = project.tracks.map((track, partIndex) => {
    const notes = track.notes.map(n => ({ ...n, from: Math.round(n.start * PPQ), to: Math.round((n.start + n.duration) * PPQ) })).sort((a, b) => a.from - b.from || a.id - b.id);
    const laneEnds = [];
    for (const note of notes) {
      if (note.to <= note.from) fail('音符时值小于 MusicXML 960 PPQ 的一个 tick');
      let lane = laneEnds.findIndex(last => last <= note.from);
      if (lane < 0) lane = laneEnds.length;
      laneEnds[lane] = note.to; note.voice = lane + 1;
    }
    let nextNote = 0, activeNotes = [];
    const renderedBars = bars.map((bar, index) => {
      const lines = [];
      if (!index || bars[index - 1].meter !== bar.meter) lines.push(`<attributes><divisions>${PPQ}</divisions><time><beats>${bar.meter.numerator}</beats><beat-type>${bar.meter.denominator}</beat-type></time></attributes>`);
      // Direction offsets do not advance the MusicXML note cursor.
      if (!partIndex) for (const tempo of project.tempoChanges) {
        const tick = Math.round(tempo.beat * PPQ);
        if (tick >= bar.start && (tick < bar.end || index === bars.length - 1 && tick === bar.end)) lines.push(`<direction><direction-type><words>Tempo</words></direction-type><offset>${tick - bar.start}</offset><sound tempo="${tempo.bpm}"/></direction>`);
      }
      // Standard pedal is provided for external readers on the default channel.
      // The extension retains exact channel and half-pedal values on reimport.
      for (const c of track.controls) {
        const tick = Math.round(c.beat * PPQ);
        if (c.controller === 64 && c.channel === 0 && tick >= bar.start && (tick < bar.end || index === bars.length - 1 && tick === bar.end)) lines.push(`<direction><direction-type><pedal type="${c.value >= 64 ? 'start' : 'stop'}"/></direction-type><offset>${tick - bar.start}</offset></direction>`);
      }
      const lanes = new Map();
      activeNotes = activeNotes.filter(note => note.to > bar.start);
      while (nextNote < notes.length && notes[nextNote].from < bar.end) activeNotes.push(notes[nextNote++]);
      for (const note of activeNotes) {
        if (!lanes.has(note.voice)) lanes.set(note.voice, []);
        lanes.get(note.voice).push(note);
      }
      if (!lanes.size) lanes.set(1, []);
      let laneIndex = 0;
      for (const [voice, lane] of lanes) {
        if (laneIndex++) lines.push(`<backup><duration>${bar.end - bar.start}</duration></backup>`);
        let cursor = bar.start;
        for (const note of lane) {
          const from = Math.max(note.from, bar.start), to = Math.min(note.to, bar.end);
          if (from < cursor) fail('内部错误：导出 voice 内存在重叠');
          if (from > cursor) lines.push(`<note><rest/><duration>${from - cursor}</duration><voice>${voice}</voice></note>`);
          const ties = [];
          if (note.from < bar.start) ties.push('stop');
          if (note.to > bar.end) ties.push('start');
          const lyric = note.lyric && from === note.from ? `<lyric><text>${xmlEscape(note.lyric)}</text></lyric>` : '';
          lines.push(`<note dynamics="${note.velocity / .9}">${pitchXml(note.midi)}<duration>${to - from}</duration>${ties.map(t => `<tie type="${t}"/>`).join('')}<instrument id="P${partIndex + 1}C${note.channel}"/><voice>${voice}</voice>${ties.length ? `<notations>${ties.map(t => `<tied type="${t}"/>`).join('')}</notations>` : ''}${lyric}</note>`);
          cursor = to;
        }
        if (cursor < bar.end) lines.push(`<note><rest/><duration>${bar.end - cursor}</duration><voice>${voice}</voice></note>`);
      }
      if (index === bars.length - 1 && meters.at(-1).tick === end) {
        const terminal = meters.at(-1);
        lines.push(`<attributes><time><beats>${terminal.numerator}</beats><beat-type>${terminal.denominator}</beat-type></time></attributes>`);
      }
      return countOutput(`<measure number="${index + 1}"${bar.implicit ? ' implicit="yes"' : ''}>\n${lines.join('\n')}\n</measure>`);
    });
    return `<part id="P${partIndex + 1}">\n${renderedBars.join('\n')}\n</part>`;
  });
  const xml = `<?xml version="1.0" encoding="UTF-8"?>\n<score-partwise version="4.0">\n<work><work-title>${xmlEscape(project.title)}</work-title></work>\n<identification><encoding><software>Classical DAW Web Alpha</software></encoding><miscellaneous><miscellaneous-field name="${EXTENSION}">${xmlEscape(JSON.stringify(extension))}</miscellaneous-field></miscellaneous></identification>\n<part-list>${partList}</part-list>\n${parts.join('\n')}\n</score-partwise>\n`;
  if (encoder.encode(xml).length > LIMITS.fileBytes) fail('导出的 MusicXML 超出文件大小上限');
  return xml;
}
