import { createProject, validateProject, parseProject, projectJson, clone, noteName, INSTRUMENTS, LIMITS, PPQ, beatToSeconds, secondsToBeat, bars } from './core.mjs';
import { Editor, Recovery } from './editor-state.mjs';
import { parseMusicXml, parseMidiFile, musicXml, midiFile } from './formats.mjs';
import { encodeWav } from './audio-engine.mjs';

const $ = id => document.getElementById(id);
const editor = new Editor(createProject(true));
let trackId = 'piano', selectedId = null, viewStart = 0, viewSpan = 16;
let recovery = null, recoveryTimer = null, dirty = false, importGeneration = 0;
let audioContext = null, audioSource = null, worker = null, operation = 0, animation = 0;
let clipboard = null;
const rowHeight = 18, beatWidth = 64, keyWidth = 56;
const track = () => editor.project.tracks.find(t => t.id === trackId) ?? editor.project.tracks[0];
const selected = () => track().notes.find(n => n.id === selectedId);
const snap = beat => { const grid = Number($('snap').value); return Math.round((grid ? Math.round(beat / grid) * grid : beat) * PPQ) / PPQ; };
const color = index => ['#527e6d', '#a07f4b', '#657a96', '#967386', '#71949b', '#918954'][index % 6];
const el = (tag, text, className) => { const node = document.createElement(tag); if (text !== undefined) node.textContent = text; if (className) node.className = className; return node; };
function status(message, error = false) { $('status').textContent = message; $('status').classList.toggle('error', error); }
function guard(fn) { try { return fn(); } catch (error) { status(error.message, true); render(); return false; } }
function fileName(suffix) { return (editor.project.title.replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').slice(0, 70) || 'classical-daw') + suffix; }
function download(contents, type, filename) {
  const url = URL.createObjectURL(new Blob([contents], { type }));
  const link = el('a'); link.href = url; link.download = filename; document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function saveRecovery() {
  clearTimeout(recoveryTimer); recoveryTimer = null;
  if (!recovery) { $('autosave-status').textContent = '浏览器存储不可用，请下载保存工程'; return; }
  try { recovery.save(editor.project); $('autosave-status').textContent = '本地副本已保存 · 修订 ' + editor.revision; }
  catch { $('autosave-status').textContent = '本地副本保存失败，请下载保存工程'; }
}
function afterEdit(message = '已修改；可撤销') {
  dirty = true; stop(false); render(); status(message);
  clearTimeout(recoveryTimer); recoveryTimer = setTimeout(saveRecovery, 600);
}
function edit(fn, message) { return guard(() => { if (editor.edit(fn)) afterEdit(message); }); }
function stop(show = true) {
  operation++;
  if (worker) { worker.terminate(); worker = null; }
  if (audioSource) { audioSource.onended = null; try { audioSource.stop(); } catch {} audioSource.disconnect(); audioSource = null; }
  cancelAnimationFrame(animation); $('playhead').hidden = true;
  $('play').disabled = false; $('stop').disabled = true; $('export-wav').disabled = false; $('render-progress').hidden = true;
  if (show) status('已停止，音频任务已取消');
}
function setRangeForProject() {
  $('range-start').value = '0';
  $('range-end').value = String(beatToSeconds(editor.project, editor.project.lengthBeats) <= 120 ? editor.project.lengthBeats : Math.min(16, editor.project.lengthBeats));
}
function replace(project, message) {
  guard(() => { editor.replace(project); trackId = editor.project.tracks[0].id; selectedId = null; viewStart = 0; setRangeForProject(); afterEdit(message); fitPitch(); });
}
function renderTracks() {
  $('track-list').replaceChildren();
  editor.project.tracks.forEach((t, i) => {
    const row = el('div', undefined, 'track-row' + (t.id === trackId ? ' selected' : '')); row.setAttribute('role', 'listitem');
    const button = el('button', t.name, 'track-select'); button.style.color = color(i);
    button.setAttribute('aria-pressed', String(t.id === trackId)); button.append(el('small', `${t.notes.length} 音符 · ${INSTRUMENTS[t.instrument]}`));
    button.onclick = () => { trackId = t.id; selectedId = null; render(); fitPitch(); };
    row.append(button);
    for (const [key, label, hint] of [['mute', 'M', '静音'], ['solo', 'S', '独奏']]) {
      const b = el('button', label, 'toggle'); b.title = `${hint} ${t.name}`; b.setAttribute('aria-label', b.title); b.setAttribute('aria-pressed', String(t[key]));
      b.onclick = () => edit(p => { const item = p.tracks.find(x => x.id === t.id); item[key] = !item[key]; }, `${t.name}：${hint}已切换`); row.append(b);
    }
    $('track-list').append(row);
  });
  const t = track();
  for (const [field, value] of [['track-name', t.name], ['track-instrument', t.instrument], ['track-gain', t.gainDb], ['track-pan', t.pan]]) $(field).value = value;
  $('delete-track').disabled = editor.project.tracks.length === 1;
  $('add-track').disabled = editor.project.tracks.length >= LIMITS.tracks;
}
function renderRoll() {
  const t = track(), width = keyWidth + viewSpan * beatWidth;
  $('roll').style.width = `${width}px`;
  $('ruler').replaceChildren();
  for (let beat = 0; beat <= viewSpan; beat++) {
    const mark = el('span', String(viewStart + beat)); mark.style.left = `${keyWidth + beat * beatWidth + 4}px`; $('ruler').append(mark);
  }
  for (const bar of bars(editor.project)) if (bar.start >= viewStart && bar.start < viewStart + viewSpan) {
    const mark = el('span', `│ ${bar.number} 小节`); mark.style.left = `${keyWidth + (bar.start - viewStart) * beatWidth + 3}px`; mark.style.top = '16px'; mark.style.fontSize = '8px'; $('ruler').append(mark);
  }
  $('note-layer').replaceChildren();
  const notes = t.notes.filter(n => n.start < viewStart + viewSpan && n.start + n.duration > viewStart);
  for (const n of notes) {
    const left = Math.max(n.start, viewStart), end = Math.min(n.start + n.duration, viewStart + viewSpan);
    const node = el('button', `${noteName(n.midi)}${n.lyric ? ' · ' + n.lyric : ''}`, 'note' + (selectedId === n.id ? ' selected' : ''));
    node.dataset.noteId = n.id;
    node.setAttribute('aria-label', `音符 ${n.id} ${noteName(n.midi)}，第 ${n.start} 拍，时值 ${n.duration} 拍`);
    node.title = node.getAttribute('aria-label');
    node.style.left = `${keyWidth + (left - viewStart) * beatWidth}px`; node.style.top = `${30 + (127 - n.midi) * rowHeight + 1}px`; node.style.width = `${Math.max(5, (end - left) * beatWidth - 1)}px`;
    node.style.opacity = String(.55 + n.velocity / 127 * .45);
    node.onpointerdown = event => beginDrag(event, n, node);
    node.onclick = event => { event.stopPropagation(); selectNote(n.id); };
    $('note-layer').append(node);
  }
  $('active-track-name').textContent = t.name;
  $('note-count').textContent = `${t.notes.length} 音符 · 当前可见 ${notes.length}`;
  $('view-start').value = viewStart;
  $('view-prev').disabled = viewStart <= 0;
  $('view-next').disabled = viewStart + viewSpan >= editor.project.lengthBeats;
}
function renderSelected() {
  const n = selected(); $('note-form').hidden = !n; $('empty-selection').hidden = !!n;
  $('note-label').textContent = n ? `ID ${n.id}` : '尚未选中';
  if (!n) return;
  for (const [id, value] of [['note-pitch', n.midi], ['note-start', n.start], ['note-duration', n.duration], ['note-velocity', n.velocity], ['note-channel', n.channel + 1], ['note-lyric', n.lyric]]) $(id).value = value;
}
function selectNote(id) { selectedId = id; renderRoll(); renderSelected(); const n = selected(); if (n) $('cc-beat').value = n.start; }
function eventRow(parent, text, remove, canRemove = true) {
  const row = el('div', undefined, 'event'); row.append(el('span', text));
  const button = el('button', '×'); button.title = '删除此事件'; button.setAttribute('aria-label', `删除 ${text}`); button.disabled = !canRemove; button.onclick = remove; row.append(button); parent.append(row);
}
function renderEvents() {
  $('control-list').replaceChildren();
  track().controls.forEach((c, i) => { if (i < 100) eventRow($('control-list'), `${c.beat} 拍 · Ch${c.channel + 1} CC${c.controller} = ${c.value}`, () => edit(p => p.tracks.find(t => t.id === trackId).controls.splice(i, 1), '已移除控制点')); });
  if (track().controls.length > 100) $('control-list').append(el('small', `仅列出前 100 个，完整 ${track().controls.length} 个控制点仍保留在工程中。`));
  $('tempo-list').replaceChildren(); $('meter-list').replaceChildren();
  for (const key of ['tempoChanges', 'meterChanges']) editor.project[key].forEach((event, i) => {
    if (i >= 100) return;
    const text = key === 'tempoChanges' ? `${event.beat} 拍 → ${event.bpm} BPM` : `${event.beat} 拍 → ${event.numerator}/${event.denominator}`;
    eventRow($(key === 'tempoChanges' ? 'tempo-list' : 'meter-list'), text, () => edit(p => p[key].splice(i, 1)), i > 0);
  });
  $('map-summary').textContent = `${editor.project.tempoChanges.length} 速度点 · ${editor.project.meterChanges.length} 拍号点`;
  $('report-count').textContent = editor.project.importReport.length ? `${editor.project.importReport.length} 条` : '无导入处理';
  $('report-list').replaceChildren(...editor.project.importReport.map(text => el('li', text)));
}
function render() {
  trackId = track().id;
  if (!selected()) selectedId = null;
  $('project-title').value = editor.project.title; $('master-gain').value = editor.project.masterGainDb; $('length-beats').value = editor.project.lengthBeats;
  $('undo').disabled = !editor.past.length; $('redo').disabled = !editor.future.length;
  const count = editor.project.tracks.reduce((s, t) => s + t.notes.length, 0);
  $('project-summary').textContent = `${editor.project.tracks.length} 声部 · ${count} 音符 · ${beatToSeconds(editor.project, editor.project.lengthBeats).toFixed(1)} 秒`;
  renderTracks(); renderRoll(); renderSelected(); renderEvents();
}
function fitPitch() {
  const notes = track().notes.filter(n => n.start < viewStart + viewSpan && n.start + n.duration > viewStart);
  const highest = notes.length ? Math.max(...notes.map(n => n.midi)) : 76;
  $('roll-scroll').scrollTop = Math.max(0, (127 - Math.min(127, highest + 5)) * rowHeight);
}
function addNote(midi = 60, start = viewStart) {
  const duration = Math.min(1, editor.project.lengthBeats - start);
  if (duration < 1 / PPQ) return status('工程已到结尾，请先延长总拍数', true);
  edit(p => { const id = p.nextId++; p.tracks.find(t => t.id === trackId).notes.push({ id, midi, start, duration, velocity: 90, channel: 0, lyric: '' }); selectedId = id; }, '已添加音符');
}
function beginDrag(event, n, node) {
  if (event.button !== 0) return;
  event.preventDefault(); event.stopPropagation();
  const origin = { x: event.clientX, y: event.clientY, note: clone(n), moved: false };
  node.setPointerCapture(event.pointerId);
  const move = e => {
    if (Math.abs(e.clientX - origin.x) + Math.abs(e.clientY - origin.y) < 4) return;
    origin.moved = true;
    const start = Math.max(0, Math.min(editor.project.lengthBeats - n.duration, snap(origin.note.start + (e.clientX - origin.x) / beatWidth)));
    const midi = Math.max(0, Math.min(127, origin.note.midi - Math.round((e.clientY - origin.y) / rowHeight)));
    origin.patch = { start, midi }; node.style.left = `${keyWidth + (start - viewStart) * beatWidth}px`; node.style.top = `${30 + (127 - midi) * rowHeight + 1}px`;
  };
  const finish = e => {
    node.removeEventListener('pointermove', move); node.removeEventListener('pointerup', finish); node.removeEventListener('pointercancel', cancel);
    if (node.hasPointerCapture(e.pointerId)) node.releasePointerCapture(e.pointerId);
    selectedId = n.id;
    if (origin.moved) edit(p => Object.assign(p.tracks.find(t => t.id === trackId).notes.find(item => item.id === n.id), origin.patch), '已移动音符');
    else selectNote(n.id);
  };
  const cancel = () => { node.removeEventListener('pointermove', move); node.removeEventListener('pointerup', finish); renderRoll(); };
  node.addEventListener('pointermove', move); node.addEventListener('pointerup', finish); node.addEventListener('pointercancel', cancel);
}
function removeNote() { if (selected()) edit(p => { const t = p.tracks.find(t => t.id === trackId); t.notes = t.notes.filter(n => n.id !== selectedId); }, '已删除音符'); }
function copyNote() { if (selected()) { clipboard = clone(selected()); status('已复制音符；⌘/Ctrl+V 粘贴'); } }
function pasteNote() {
  if (!clipboard) return;
  edit(p => { const n = { ...clipboard, id: p.nextId++, start: Math.min(p.lengthBeats - clipboard.duration, clipboard.start + .25) }; p.tracks.find(t => t.id === trackId).notes.push(n); selectedId = n.id; }, '已粘贴音符');
}
async function importFile(file) {
  const generation = ++importGeneration, revision = editor.revision;
  try {
    if (file.size > LIMITS.fileBytes) throw new Error('文件超过 8 MiB，未读取；请拆分后导入');
    const ext = file.name.split('.').pop().toLowerCase();
    const data = ext === 'mid' || ext === 'midi' ? await file.arrayBuffer() : await file.text();
    let result;
    if (ext === 'json') result = { project: parseProject(data), warnings: [] };
    else if (['mid', 'midi'].includes(ext)) result = parseMidiFile(data);
    else if (['xml', 'musicxml'].includes(ext)) result = parseMusicXml(data);
    else throw new Error('请打开 JSON、未压缩 MusicXML 或 MIDI；.mxl 请先解压/转换');
    const project = validateProject({ ...result.project, importReport: [...(result.project.importReport ?? []), ...result.warnings].slice(0, 256) });
    if (generation !== importGeneration || revision !== editor.revision) throw new Error('读取期间工程已改变；请重新选择文件，当前编辑已保留');
    replace(project, `已打开 ${file.name}：${project.tracks.length} 个声部`);
    if (project.importReport.length) { $('import-report').open = true; status(`已导入，请查看 ${project.importReport.length} 条处理记录`); }
  } catch (error) { status(`打开失败：${error.message}。原工程保留。`, true); }
  finally { $('file-input').value = ''; }
}
function showRecovery() {
  if (!recovery) return;
  try {
    const entries = recovery.list(); $('recovery').hidden = !entries.length; $('recovery-list').replaceChildren();
    for (const item of entries.slice(0, 12)) {
      const row = el('div', undefined, 'recovery-item'); row.append(el('span', item.error || `${item.project.title} · ${item.savedAt ? new Date(item.savedAt).toLocaleString() : '旧版副本'}`));
      const restore = el('button', '恢复'); restore.disabled = !!item.error;
      restore.onclick = () => { replace(item.project, '已恢复副本；请保存工程下载独立文件'); saveRecovery(); };
      const backup = el('button', '下载副本'); backup.onclick = () => download(item.project ? projectJson(item.project) : recovery.storage.getItem(item.key), 'application/json', 'recovered.classical-daw.json');
      const remove = el('button', '丢弃副本'); remove.onclick = () => { if (window.confirm('确定移除这份浏览器副本？已下载的文件不会受影响。')) { guard(() => recovery.remove(item.key)); showRecovery(); } };
      row.append(restore, backup, remove); $('recovery-list').append(row);
    }
  } catch { $('autosave-status').textContent = '恢复副本无法读取，请定期下载保存'; }
}
async function renderAudioAction(mode) {
  stop(false);
  const token = operation, project = clone(editor.project);
  const startBeat = Number($('range-start').value), endBeat = Number($('range-end').value);
  if (!Number.isFinite(startBeat) || !Number.isFinite(endBeat) || startBeat < 0 || endBeat > project.lengthBeats || endBeat <= startBeat) return status('试听范围无效：起点须早于终点，并位于工程内', true);
  try {
    if (mode === 'play') {
      const Context = window.AudioContext || window.webkitAudioContext;
      if (!Context) throw new Error('当前浏览器不支持音频播放');
      audioContext ??= new Context();
      await audioContext.resume();
      if (token !== operation) return;
    }
    $('play').disabled = true; $('export-wav').disabled = true; $('stop').disabled = false;
    $('render-progress').hidden = false; $('render-progress').value = 0;
    status('正在合成所选片段；可以停止或取消…');
    worker = new Worker(new URL('./render-worker.mjs', import.meta.url), { type: 'module' });
    const rendered = await new Promise((resolve, reject) => {
      worker.onmessage = e => {
        if (e.data.type === 'progress') $('render-progress').value = e.data.value;
        else if (e.data.type === 'result') resolve(e.data);
        else if (e.data.type === 'error') reject(new Error(e.data.message));
      };
      worker.onerror = () => reject(new Error('音频线程启动失败，请使用本地 HTTP 或 HTTPS 服务打开页面'));
      worker.postMessage({ project, range: { startBeat, endBeat }, sampleRate: 48000 });
    });
    if (token !== operation) return;
    worker.terminate(); worker = null; $('render-progress').hidden = true;
    const clipping = rendered.clippedSamples ? ` · ${rendered.clippedSamples} 个削波采样，请降低主增益` : ' · 无削波';
    if (mode === 'wav') {
      download(encodeWav(rendered), 'audio/wav', fileName(`-${startBeat}-${endBeat}.wav`)); stop(false);
      status(`已导出 ${rendered.frames} 帧 · 48 kHz 立体声${clipping}`, rendered.clippedSamples > 0); return;
    }
    const buffer = audioContext.createBuffer(2, rendered.frames, rendered.sampleRate);
    buffer.copyToChannel(rendered.left, 0); buffer.copyToChannel(rendered.right, 1);
    audioSource = audioContext.createBufferSource(); audioSource.buffer = buffer; audioSource.connect(audioContext.destination);
    const started = audioContext.currentTime; audioSource.onended = () => { if (token === operation) { stop(false); status('片段播放完毕' + clipping, rendered.clippedSamples > 0); } }; audioSource.start();
    $('export-wav').disabled = false; status(`正在试听 ${startBeat}–${endBeat} 拍${clipping}`, rendered.clippedSamples > 0);
    const animate = () => {
      if (token !== operation || !audioSource) return;
      const beat = secondsToBeat(project, rendered.startSeconds + Math.max(0, audioContext.currentTime - started));
      $('playhead').hidden = beat < viewStart || beat >= viewStart + viewSpan;
      $('playhead').style.left = `${keyWidth + (beat - viewStart) * beatWidth}px`; animation = requestAnimationFrame(animate);
    }; animate();
  } catch (error) { if (token === operation) { stop(false); status(error.message, true); } }
}

for (let midi = 127; midi >= 0; midi--) {
  const option = el('option', `${noteName(midi)} · ${midi}`); option.value = midi; $('note-pitch').append(option);
  const pitch = el('div', noteName(midi), 'pitch' + ([1, 3, 6, 8, 10].includes(midi % 12) ? ' black' : '') + (midi % 12 === 0 ? ' c' : '')); $('pitches').append(pitch);
}
for (const form of document.querySelectorAll('form')) form.addEventListener('submit', event => event.preventDefault());
$('new-project').onclick = () => replace(createProject(), '已新建；上一个工程可撤销恢复');
$('load-demo').onclick = () => replace(createProject(true), '已载入原创钢琴与弦乐练习');
$('open-file').onclick = () => $('file-input').click();
$('file-input').onchange = () => { if ($('file-input').files[0]) importFile($('file-input').files[0]); };
$('save-project').onclick = () => guard(() => { download(projectJson(editor.project), 'application/json', fileName('.classical-daw.json')); dirty = false; saveRecovery(); status('工程已生成下载；请确认浏览器保存位置'); });
$('export-midi').onclick = () => guard(() => { download(midiFile(editor.project), 'audio/midi', fileName('.mid')); status('已导出 MIDI；每轨独立端口，请在外部 DAW 检查路由。混音不写入；时间按 960 PPQ 舍入。'); });
$('export-xml').onclick = () => guard(() => { download(musicXml(editor.project), 'application/vnd.recordare.musicxml+xml', fileName('.musicxml')); status('已导出 MusicXML；控制器/混音扩展可能被其他软件忽略，时间按 960 PPQ 舍入。'); });
$('export-wav').onclick = () => renderAudioAction('wav'); $('play').onclick = () => renderAudioAction('play'); $('stop').onclick = () => stop();
$('range-view').onclick = () => { $('range-start').value = viewStart; $('range-end').value = Math.min(editor.project.lengthBeats, viewStart + viewSpan); status('已选择当前视窗'); };
$('range-all').onclick = () => { $('range-start').value = 0; $('range-end').value = editor.project.lengthBeats; status('已选择全曲；音频片段上限为 120 秒'); };
$('undo').onclick = () => { if (editor.undo()) afterEdit('已撤销'); }; $('redo').onclick = () => { if (editor.redo()) afterEdit('已重做'); };
$('project-title').onchange = () => edit(p => { p.title = $('project-title').value; });
$('master-gain').onchange = () => edit(p => { p.masterGainDb = Number($('master-gain').value); });
$('length-beats').onchange = () => edit(p => { p.lengthBeats = Number($('length-beats').value); }, '已修改工程结尾；音符和控制点未裁剪');
for (const [id, key, numeric] of [['track-name', 'name', false], ['track-instrument', 'instrument', false], ['track-gain', 'gainDb', true], ['track-pan', 'pan', true]]) $(id).onchange = () => edit(p => { p.tracks.find(t => t.id === trackId)[key] = numeric ? Number($(id).value) : $(id).value; });
$('add-track').onclick = () => edit(p => { let i = 1; while (p.tracks.some(t => t.id === `part-${i}`)) i++; trackId = `part-${i}`; selectedId = null; p.tracks.push({ id: trackId, name: `声部 ${i}`, instrument: 'piano', gainDb: -9, pan: 0, mute: false, solo: false, notes: [], controls: [] }); }, '已添加独立声部');
$('delete-track').onclick = () => { if (editor.project.tracks.length > 1) edit(p => { p.tracks = p.tracks.filter(t => t.id !== trackId); trackId = p.tracks[0].id; selectedId = null; }, '已移除声部；可撤销'); };
for (const [id, key, transform] of [['note-pitch', 'midi', Number], ['note-start', 'start', Number], ['note-duration', 'duration', Number], ['note-velocity', 'velocity', Number], ['note-channel', 'channel', x => Number(x) - 1], ['note-lyric', 'lyric', String]]) $(id).onchange = () => { if (selected()) edit(p => { p.tracks.find(t => t.id === trackId).notes.find(n => n.id === selectedId)[key] = transform($(id).value); }); };
$('add-note').onclick = () => addNote(); $('delete-note').onclick = removeNote; $('copy-note').onclick = copyNote;
$('roll').ondblclick = event => { if (event.target.closest('.note,.pitches,.ruler')) return; const rect = $('roll').getBoundingClientRect(); const start = snap(viewStart + (event.clientX - rect.left - keyWidth) / beatWidth), midi = 127 - Math.floor((event.clientY - rect.top - 30) / rowHeight); if (midi >= 0 && midi <= 127 && start >= 0 && start < editor.project.lengthBeats) addNote(midi, start); };
function changeView(start) { viewStart = Math.max(0, Math.min(Math.max(0, editor.project.lengthBeats - 1), start)); renderRoll(); }
$('view-prev').onclick = () => changeView(viewStart - viewSpan); $('view-next').onclick = () => changeView(viewStart + viewSpan);
$('view-start').onchange = () => changeView(Number($('view-start').value) || 0); $('view-span').onchange = () => { viewSpan = Number($('view-span').value); renderRoll(); }; $('fit-pitch').onclick = fitPitch;
$('control-form').addEventListener('submit', () => edit(p => { p.tracks.find(t => t.id === trackId).controls.push({ beat: Number($('cc-beat').value), channel: Number($('cc-channel').value) - 1, controller: Number($('cc-kind').value), value: Number($('cc-value').value) }); }, '已加入阶梯控制点；同拍事件按添加顺序保留'));
for (const [form, key, make] of [['tempo-form', 'tempoChanges', () => ({ beat: Number($('tempo-beat').value), bpm: Number($('tempo-bpm').value) })], ['meter-form', 'meterChanges', () => ({ beat: Number($('meter-beat').value), numerator: Number($('meter-num').value), denominator: Number($('meter-den').value) })]]) $(form).addEventListener('submit', () => edit(p => { const point = make(); p[key] = [...p[key].filter(x => x.beat !== point.beat), point].sort((a, b) => a.beat - b.beat); }, '已修改时间图；音符拍位置保持不变'));
document.addEventListener('keydown', event => {
  if (event.target.matches('input,select,textarea')) return;
  const modifier = event.ctrlKey || event.metaKey;
  if (modifier && event.key.toLowerCase() === 'z') { event.preventDefault(); (event.shiftKey ? $('redo') : $('undo')).click(); return; }
  if (modifier && event.key.toLowerCase() === 'c') { event.preventDefault(); copyNote(); return; }
  if (modifier && event.key.toLowerCase() === 'v') { event.preventDefault(); pasteNote(); return; }
  if (event.key === ' ') {
    if (event.repeat) { event.preventDefault(); return; }
    if (event.target.closest('button,summary,a')) return; // native keyboard activation
    event.preventDefault(); if (audioSource || worker) stop(); else renderAudioAction('play'); return;
  }
  if (event.key === 'Delete' || event.key === 'Backspace') { event.preventDefault(); removeNote(); return; }
  const n = selected(); if (!n) return;
  const dx = event.key === 'ArrowLeft' ? -.25 : event.key === 'ArrowRight' ? .25 : 0;
  const dy = event.key === 'ArrowUp' ? 1 : event.key === 'ArrowDown' ? -1 : 0;
  if (dx || dy) { event.preventDefault(); edit(p => { const note = p.tracks.find(t => t.id === trackId).notes.find(n => n.id === selectedId); note.start += dx; note.midi += dy; }); }
});
window.addEventListener('beforeunload', event => { if (dirty) { saveRecovery(); event.preventDefault(); event.returnValue = ''; } });
try { recovery = new Recovery(window.localStorage, crypto.randomUUID?.() ?? `${Date.now()}-${Math.random()}`); } catch {}
setRangeForProject(); render(); fitPitch(); showRecovery(); status('准备就绪 · 原创双声部练习；可直接试听或导入自己的乐谱');
