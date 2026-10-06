export const PPQ = 960;
export const FORMAT = 'classical-daw-web-project';
export const LIMITS = Object.freeze({ fileBytes: 8 * 1024 * 1024, tracks: 16, notes: 20000, controls: 50000, beats: 4096 });
export const INSTRUMENTS = Object.freeze({ piano: '合成钢琴', strings: '合成弦乐', sine: '纯音' });
const fail = message => { throw new Error(message); };
const finite = (v, lo, hi, label) => typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi ? v : fail(`${label} 必须在 ${lo}–${hi} 之间`);
const integer = (v, lo, hi, label) => Number.isSafeInteger(v) ? finite(v, lo, hi, label) : fail(`${label} 必须是整数`);
const text = (v, max, label) => typeof v === 'string' && v.length <= max ? v : fail(`${label} 过长或无效`);
const flag = (v, label) => typeof v === 'boolean' ? v : fail(`${label} 必须是布尔值`);

export function validateProject(input) {
  if (!input || input.format !== FORMAT || input.version !== 2 || input.ppq !== PPQ) fail('工程格式不支持；需要网页工程 v1/v2');
  const lengthBeats = finite(input.lengthBeats, 1 / PPQ, LIMITS.beats, '工程拍数');
  if (!Array.isArray(input.tracks) || !input.tracks.length || input.tracks.length > LIMITS.tracks) fail('工程需要 1–16 个声部');
  const map = (items, mapper, label) => {
    if (!Array.isArray(items) || !items.length || items.length > 4096) fail(`${label} 无效或超限`);
    let previous = -1;
    return items.map(item => {
      if (!item || typeof item !== 'object') fail(`${label} 项无效`);
      const beat = finite(item.beat, 0, lengthBeats, `${label} 位置`);
      if (beat <= previous) fail(`${label} 位置需要严格递增`);
      if (previous < 0 && beat !== 0) fail(`${label} 需要从第 0 拍开始`);
      previous = beat;
      return { beat, ...mapper(item) };
    });
  };
  const tempoChanges = map(input.tempoChanges, p => ({ bpm: finite(p.bpm, 20, 400, '速度') }), '速度图');
  const meterChanges = map(input.meterChanges, p => {
    const numerator = integer(p.numerator, 1, 32, '拍号分子');
    if (![1, 2, 4, 8, 16, 32].includes(p.denominator)) fail('拍号分母不支持');
    return { numerator, denominator: p.denominator };
  }, '拍号图');
  const trackIds = new Set(), noteIds = new Set();
  let noteCount = 0, controlCount = 0, maxId = 0;
  const tracks = input.tracks.map(source => {
    if (!source || typeof source !== 'object') fail('声部数据无效');
    const id = text(source.id, 80, '声部 ID');
    if (!id || trackIds.has(id)) fail('声部 ID 为空或重复');
    trackIds.add(id);
    if (!Object.hasOwn(INSTRUMENTS, source.instrument)) fail('声部音色不支持');
    if (!Array.isArray(source.notes) || (noteCount += source.notes.length) > LIMITS.notes) fail('最多支持 20,000 个音符');
    const notes = source.notes.map(n => {
      if (!n || typeof n !== 'object') fail('音符数据无效');
      const id = integer(n.id, 1, Number.MAX_SAFE_INTEGER - 1, '音符 ID');
      if (noteIds.has(id)) fail('音符 ID 重复');
      noteIds.add(id); maxId = Math.max(maxId, id);
      const start = finite(n.start, 0, lengthBeats, '音符起点');
      const duration = finite(n.duration, 1 / PPQ, lengthBeats, '音符时值');
      if (start + duration > lengthBeats + 1e-8) fail('音符超出工程结尾，请先延长工程');
      return { id, midi: integer(n.midi, 0, 127, '音高'), start, duration,
        velocity: integer(n.velocity, 1, 127, '力度'), channel: integer(n.channel ?? 0, 0, 15, 'MIDI 通道'),
        lyric: text(n.lyric ?? '', 256, '歌词') };
    });
    const controls = source.controls ?? [];
    if (!Array.isArray(controls) || (controlCount += controls.length) > LIMITS.controls) fail('控制器事件超过 50,000 个');
    return { id, name: text(source.name, 128, '声部名'), instrument: source.instrument,
      gainDb: finite(source.gainDb, -60, 12, '声部增益'), pan: finite(source.pan, -1, 1, '声像'),
      mute: flag(source.mute, '静音'), solo: flag(source.solo, '独奏'), notes,
      controls: controls.map(c => ({ beat: finite(c.beat, 0, lengthBeats, '控制器位置'),
        channel: integer(c.channel ?? 0, 0, 15, '控制器通道'), controller: integer(c.controller, 0, 127, '控制器编号'),
        value: integer(c.value, 0, 127, '控制器值') })).sort((a, b) => a.beat - b.beat) };
  });
  const nextId = integer(input.nextId, maxId + 1, Number.MAX_SAFE_INTEGER, '下一个音符 ID');
  const importReport = input.importReport ?? [];
  if (!Array.isArray(importReport) || importReport.length > 256) fail('导入记录超限');
  const result = { format: FORMAT, version: 2, ppq: PPQ, title: text(input.title ?? '未命名作品', 160, '标题'), lengthBeats,
    masterGainDb: finite(input.masterGainDb ?? -6, -60, 0, '主增益'), tempoChanges, meterChanges, tracks, nextId,
    importReport: importReport.map(item => text(item, 1024, '导入记录')) };
  // Every accepted state must be able to save and reopen through the same
  // public file limit. Count UTF-8 bytes of the actual pretty-printed download,
  // not JS character count (lyrics may contain multi-byte text).
  if (new TextEncoder().encode(JSON.stringify(result, null, 2) + '\n').length > LIMITS.fileBytes)
    fail('工程保存后将超过 8 MiB；本次操作未应用，请减少音符、控制点或歌词');
  return result;
}

export function parseProject(raw) {
  if (typeof raw !== 'string' || new TextEncoder().encode(raw).length > LIMITS.fileBytes) fail('工程文件超过 8 MiB');
  let value;
  try { value = JSON.parse(raw); } catch { fail('工程不是有效的 JSON'); }
  if (value?.format === FORMAT && value.version === 1) {
    const meter = value.timeSignature ?? { numerator: 4, denominator: 4 };
    value = { ...value, version: 2, title: '旧版草稿', lengthBeats: (value.measures ?? 4) * meter.numerator * 4 / meter.denominator,
      tempoChanges: [{ beat: 0, bpm: value.tempo }], meterChanges: [{ beat: 0, ...meter }],
      tracks: [{ id: 'piano', name: '钢琴', instrument: 'piano', gainDb: -9, pan: 0, mute: false, solo: false, notes: value.notes, controls: [] }],
      importReport: ['已从网页 v1 草稿迁移。原稿已裁剪或量化的信息不能自动恢复。'] };
  }
  return validateProject(value);
}

export function projectJson(project) { return JSON.stringify(validateProject(project), null, 2) + '\n'; }
export const clone = value => JSON.parse(JSON.stringify(value));
export function noteName(midi) { return ['C', 'C♯', 'D', 'E♭', 'E', 'F', 'F♯', 'G', 'A♭', 'A', 'B♭', 'B'][midi % 12] + (Math.floor(midi / 12) - 1); }
export function beatToSeconds(project, beat) {
  let seconds = 0;
  const map = project.tempoChanges;
  for (let i = 0; i < map.length; i++) {
    const from = map[i].beat, until = Math.min(beat, map[i + 1]?.beat ?? beat);
    if (until > from) seconds += (until - from) * 60 / map[i].bpm;
    if (until >= beat) break;
  }
  return seconds;
}
export function secondsToBeat(project, seconds) {
  let remaining = Math.max(0, seconds);
  const map = project.tempoChanges;
  for (let i = 0; i < map.length; i++) {
    const end = map[i + 1]?.beat ?? Infinity;
    const duration = (end - map[i].beat) * 60 / map[i].bpm;
    if (remaining <= duration) return map[i].beat + remaining * map[i].bpm / 60;
    remaining -= duration;
  }
  return 0;
}
export function bars(project) {
  const result = []; let beat = 0, index = 0;
  while (beat < project.lengthBeats - 1e-8) {
    while (index + 1 < project.meterChanges.length && project.meterChanges[index + 1].beat <= beat + 1e-8) index++;
    const meter = project.meterChanges[index];
    const end = Math.min(project.lengthBeats, project.meterChanges[index + 1]?.beat ?? Infinity, beat + meter.numerator * 4 / meter.denominator);
    result.push({ number: result.length + 1, start: beat, end, ...meter, beat });
    if (result.length > 32768 || end <= beat) fail('小节网格无效');
    beat = end;
  }
  return result;
}
export function createProject(demo = false) {
  const project = { format: FORMAT, version: 2, ppq: PPQ, title: demo ? '黄昏练习 · 钢琴与弦乐' : '未命名作品',
    lengthBeats: 32, masterGainDb: -6, tempoChanges: [{ beat: 0, bpm: 88 }], meterChanges: [{ beat: 0, numerator: 4, denominator: 4 }],
    tracks: [{ id: 'piano', name: '钢琴', instrument: 'piano', gainDb: -9, pan: -.18, mute: false, solo: false, notes: [], controls: [] }], nextId: 1 };
  if (demo) {
    const piano = project.tracks[0];
    project.masterGainDb = 0;
    piano.gainDb = 0;
    const chords = [[48, 55, 60, 64], [45, 52, 57, 60], [41, 48, 53, 57], [43, 50, 55, 59]];
    for (let bar = 0; bar < 8; bar++) for (let step = 0; step < 8; step++) {
      piano.notes.push({ id: project.nextId++, midi: chords[bar % 4][step % 4] + (step > 3 ? 12 : 0), start: bar * 4 + step * .5, duration: .48, velocity: 76 + step * 3, channel: 0, lyric: '' });
    }
    const strings = { id: 'strings', name: '大提琴声部', instrument: 'strings', gainDb: -6, pan: .22, mute: false, solo: false, notes: [], controls: [{ beat: 0, channel: 0, controller: 11, value: 90 }] };
    for (let bar = 0; bar < 8; bar++) strings.notes.push({ id: project.nextId++, midi: chords[bar % 4][0], start: bar * 4, duration: 3.8, velocity: 84, channel: 0, lyric: '' });
    project.tracks.push(strings);
  }
  return validateProject(project);
}
