(() => {
  "use strict";

  // Keep exported MusicXML durations on the engine's canonical 960 PPQ grid.
  const PPQ = 960;
  const PROJECT_FORMAT = "classical-daw-web-project";
  const PROJECT_VERSION = 1;
  const BEATS = 16;
  const ROWS = 24;
  const LOWEST_MIDI = 60;
  const rowHeight = 31;
  const beatWidth = 64;
  const pitchNames = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
  const blackPitchClasses = new Set([1, 3, 6, 8, 10]);
  const dom = {
    roll: document.querySelector("#roll"),
    pitchLabels: document.querySelector("#pitch-labels"),
    beatLabels: document.querySelector("#beat-labels"),
    noteForm: document.querySelector("#note-form"),
    hint: document.querySelector("#selection-hint"),
    pitch: document.querySelector("#note-pitch"),
    start: document.querySelector("#note-start"),
    duration: document.querySelector("#note-duration"),
    velocity: document.querySelector("#note-velocity"),
    velocityValue: document.querySelector("#velocity-value"),
    lyric: document.querySelector("#note-lyric"),
    count: document.querySelector("#note-count"),
    status: document.querySelector("#transport-status"),
    play: document.querySelector("#play"),
    stop: document.querySelector("#stop"),
    undo: document.querySelector("#undo"),
    redo: document.querySelector("#redo"),
    tempo: document.querySelector("#tempo"),
    add: document.querySelector("#add-note"),
    clear: document.querySelector("#clear"),
    saveProject: document.querySelector("#save-project"),
    openProject: document.querySelector("#open-project"),
    projectFile: document.querySelector("#project-file"),
    importButton: document.querySelector("#import"),
    musicXmlFile: document.querySelector("#musicxml-file"),
    remove: document.querySelector("#delete-note"),
    exportButton: document.querySelector("#export")
  };

  let notes = [
    { id: 1, midi: 72, start: 0, duration: 1, velocity: 96 },
    { id: 2, midi: 76, start: 1, duration: 1, velocity: 86 },
    { id: 3, midi: 79, start: 2, duration: 2, velocity: 90 },
    { id: 4, midi: 76, start: 4, duration: 1, velocity: 84 },
    { id: 5, midi: 74, start: 5, duration: 1, velocity: 82 },
    { id: 6, midi: 72, start: 6, duration: 2, velocity: 94 }
  ];
  let nextId = 7;
  let selectedId = null;
  let audioContext = null;
  let activeSources = [];
  let playTimer = null;
  let playStartedAt = 0;
  let playDuration = 0;
  let playhead = null;
  const historyLimit = 100;
  const history = { past: [], future: [] };
  let pendingLiveHistory = null;
  let pendingLiveTimer = null;

  function noteName(midi) {
    const octave = Math.floor(midi / 12) - 1;
    return `${pitchNames[midi % 12]}${octave}`;
  }

  function midiToRow(midi) { return LOWEST_MIDI + ROWS - 1 - midi; }

  function clamp(value, min, max) { return Math.min(max, Math.max(min, value)); }

  function cloneNotes(source) {
    return source.map((note) => ({ ...note }));
  }

  function snapshot() {
    return {
      notes: cloneNotes(notes),
      nextId,
      selectedId,
      tempo: String(dom.tempo.value)
    };
  }

  function snapshotKey(state) {
    return JSON.stringify(state);
  }

  function updateHistoryButtons() {
    if (dom.undo) dom.undo.disabled = history.past.length === 0;
    if (dom.redo) dom.redo.disabled = history.future.length === 0;
  }

  function pushHistory(before, after = snapshot()) {
    if (snapshotKey(before) === snapshotKey(after)) return false;
    history.past.push(before);
    if (history.past.length > historyLimit) history.past.shift();
    history.future = [];
    updateHistoryButtons();
    return true;
  }

  function finishLiveHistory() {
    if (pendingLiveTimer !== null) {
      window.clearTimeout(pendingLiveTimer);
      pendingLiveTimer = null;
    }
    if (!pendingLiveHistory) return;
    const before = pendingLiveHistory;
    pendingLiveHistory = null;
    pushHistory(before);
  }

  function beginLiveHistory() {
    if (!pendingLiveHistory) pendingLiveHistory = snapshot();
    if (pendingLiveTimer !== null) window.clearTimeout(pendingLiveTimer);
    pendingLiveTimer = window.setTimeout(finishLiveHistory, 450);
  }

  function commitMutation(mutator) {
    finishLiveHistory();
    const before = snapshot();
    mutator();
    pushHistory(before);
  }

  function restoreState(state) {
    notes = cloneNotes(state.notes);
    nextId = state.nextId;
    selectedId = state.selectedId;
    dom.tempo.value = state.tempo;
    const selected = notes.find((note) => note.id === selectedId);
    if (selected) {
      dom.noteForm.hidden = false;
      dom.hint.hidden = true;
      dom.pitch.value = String(selected.midi);
      dom.start.value = String(selected.start);
      dom.duration.value = String(selected.duration);
      dom.velocity.value = String(selected.velocity);
      dom.velocityValue.value = String(selected.velocity);
      dom.velocityValue.textContent = String(selected.velocity);
      dom.lyric.value = selected.lyric || "";
    } else {
      dom.noteForm.hidden = true;
      dom.hint.hidden = false;
      dom.lyric.value = "";
    }
    renderRoll();
  }

  function undo() {
    finishLiveHistory();
    if (!history.past.length) {
      setStatus("没有可撤销的操作");
      return;
    }
    const current = snapshot();
    const previous = history.past.pop();
    history.future.push(current);
    restoreState(previous);
    updateHistoryButtons();
    setStatus("已撤销");
  }

  function redo() {
    finishLiveHistory();
    if (!history.future.length) {
      setStatus("没有可重做的操作");
      return;
    }
    const current = snapshot();
    const next = history.future.pop();
    history.past.push(current);
    if (history.past.length > historyLimit) history.past.shift();
    restoreState(next);
    updateHistoryButtons();
    setStatus("已重做");
  }

  function renderPitchLabels() {
    dom.pitchLabels.replaceChildren();
    for (let row = 0; row < ROWS; row += 1) {
      const midi = LOWEST_MIDI + ROWS - 1 - row;
      const label = document.createElement("div");
      label.className = `pitch-label ${blackPitchClasses.has(midi % 12) ? "black" : ""}`;
      label.textContent = noteName(midi);
      dom.pitchLabels.append(label);
    }
  }

  function renderBeatLabels() {
    dom.beatLabels.replaceChildren();
    for (let beat = 0; beat < BEATS; beat += 1) {
      const label = document.createElement("div");
      label.className = `beat-label ${beat % 4 === 0 ? "measure" : ""}`;
      label.style.left = `${beat * beatWidth}px`;
      label.textContent = beat % 4 === 0 ? `小节 ${beat / 4 + 1}` : `${beat + 1}`;
      dom.beatLabels.append(label);
    }
  }

  function renderRoll() {
    dom.roll.querySelectorAll(".note, .measure-line, .playhead").forEach((element) => element.remove());
    for (let beat = 0; beat <= BEATS; beat += 4) {
      const line = document.createElement("div");
      line.className = "measure-line";
      line.style.left = `${beat * beatWidth}px`;
      dom.roll.append(line);
    }
    notes.forEach((note) => {
      const element = document.createElement("button");
      element.type = "button";
      element.className = `note ${note.id === selectedId ? "selected" : ""}`;
      element.dataset.id = String(note.id);
      element.style.left = `${note.start * beatWidth + 2}px`;
      element.style.top = `${midiToRow(note.midi) * rowHeight + 2}px`;
      element.style.width = `${Math.max(22, note.duration * beatWidth - 4)}px`;
      element.textContent = noteName(note.midi);
      const lyricSuffix = note.lyric ? ` · ${note.lyric}` : "";
      element.title = `${noteName(note.midi)} · ${note.duration} 拍 · 起始 ${note.start}${lyricSuffix}`;
      element.addEventListener("click", (event) => {
        event.stopPropagation();
        selectNote(note.id);
      });
      dom.roll.append(element);
    });
    if (playhead) dom.roll.append(playhead);
    dom.count.textContent = `${notes.length} 个音符`;
  }

  function populatePitchOptions() {
    dom.pitch.replaceChildren();
    for (let midi = LOWEST_MIDI; midi < LOWEST_MIDI + ROWS; midi += 1) {
      const option = document.createElement("option");
      option.value = String(midi);
      option.textContent = noteName(midi);
      dom.pitch.append(option);
    }
  }

  function selectNote(id) {
    selectedId = id;
    const note = notes.find((item) => item.id === id);
    if (!note) {
      dom.noteForm.hidden = true;
      dom.hint.hidden = false;
      renderRoll();
      return;
    }
    dom.noteForm.hidden = false;
    dom.hint.hidden = true;
    dom.pitch.value = String(note.midi);
    dom.start.value = String(note.start);
    dom.duration.value = String(note.duration);
    dom.velocity.value = String(note.velocity);
    dom.velocityValue.value = String(note.velocity);
    dom.velocityValue.textContent = String(note.velocity);
    dom.lyric.value = note.lyric || "";
    renderRoll();
  }

  function addNote(midi = 72, start = null, duration = 1) {
    commitMutation(() => {
      const occupied = notes.map((note) => note.start + note.duration);
      const suggestedStart = Math.min(BEATS - duration, Math.max(0, Math.ceil(Math.max(0, ...occupied) * 4) / 4));
      const note = { id: nextId++, midi, start: Number(start === null ? suggestedStart : start), duration, velocity: 90 };
      notes.push(note);
      selectNote(note.id);
    });
    setStatus("已添加音符");
  }

  function updateSelected(patch) {
    const note = notes.find((item) => item.id === selectedId);
    if (!note) return;
    Object.assign(note, patch);
    note.start = clamp(Math.round(Number(note.start) * 4) / 4, 0, BEATS - 0.25);
    note.duration = clamp(Number(note.duration), 0.25, BEATS - note.start);
    note.midi = clamp(Math.round(Number(note.midi)), LOWEST_MIDI, LOWEST_MIDI + ROWS - 1);
    note.velocity = clamp(Math.round(Number(note.velocity)), 1, 127);
    selectNote(note.id);
  }

  function setStatus(text) { dom.status.textContent = text; }

  function deleteSelected() {
    if (selectedId === null) return;
    commitMutation(() => {
      notes = notes.filter((note) => note.id !== selectedId);
      selectedId = null;
      dom.noteForm.hidden = true;
      dom.hint.hidden = false;
      renderRoll();
    });
    setStatus("已删除所选音符");
  }

  function stopPlayback() {
    activeSources.forEach((source) => {
      try { source.stop(); } catch (_) { /* already stopped */ }
    });
    activeSources = [];
    if (playTimer !== null) window.clearTimeout(playTimer);
    playTimer = null;
    if (playhead) {
      playhead.remove();
      playhead = null;
    }
    dom.play.disabled = false;
    dom.stop.disabled = true;
    setStatus("已停止");
  }

  // MusicXML is intentionally parsed without a third-party dependency.  The
  // web prototype accepts the single-part, single-voice subset that it emits,
  // plus ordinary rests, chords, one lyric text per note and measure-level
  // tempo changes.
  function childByName(element, name) {
    return Array.from(element.children || []).find((child) => child.localName === name || child.tagName === name) || null;
  }

  function childrenByName(element, name) {
    return Array.from(element.children || []).filter((child) => child.localName === name || child.tagName === name);
  }

  function descendantsByName(element, name) {
    return Array.from(element.getElementsByTagName("*"))
      .filter((child) => child.localName === name || child.tagName === name);
  }

  function numberChild(element, name, fallback = null) {
    const child = childByName(element, name);
    if (!child) return fallback;
    const value = Number(child.textContent.trim());
    return Number.isFinite(value) ? value : fallback;
  }

  function parseMusicXml(text) {
    if (typeof DOMParser === "undefined") throw new Error("当前浏览器不支持 XML 解析");
    const documentNode = new DOMParser().parseFromString(text, "application/xml");
    const parserError = descendantsByName(documentNode, "parsererror")[0];
    if (parserError) throw new Error("MusicXML 格式无效");
    const root = documentNode.documentElement;
    if (!root || root.localName !== "score-partwise") throw new Error("只支持 score-partwise MusicXML");
    const part = descendantsByName(root, "part")[0];
    if (!part) throw new Error("MusicXML 中没有可导入的声部");
    const measures = childrenByName(part, "measure");
    if (!measures.length) throw new Error("MusicXML 中没有小节");

    let divisions = PPQ;
    let beatsPerMeasure = 4;
    let beatType = 4;
    let measureOffset = 0;
    const imported = [];
    let skipped = 0;

    measures.forEach((measure) => {
      const attributes = childByName(measure, "attributes");
      if (attributes) {
        const nextDivisions = numberChild(attributes, "divisions", null);
        if (nextDivisions && nextDivisions > 0) divisions = nextDivisions;
        const time = childByName(attributes, "time");
        if (time) {
          const nextBeats = numberChild(time, "beats", null);
          const nextBeatType = numberChild(time, "beat-type", null);
          if (nextBeats && nextBeats > 0) beatsPerMeasure = nextBeats;
          if (nextBeatType && nextBeatType > 0) beatType = nextBeatType;
        }
      }
      const measureLength = beatsPerMeasure * 4 / beatType;
      let cursor = 0;
      let measureLastOnset = 0;
      childrenByName(measure, "note").forEach((noteElement) => {
        const durationTicks = numberChild(noteElement, "duration", 0);
        const duration = durationTicks > 0 ? durationTicks / divisions : 0;
        if (!(duration > 0)) return;
        const chord = Boolean(childByName(noteElement, "chord"));
        const rest = Boolean(childByName(noteElement, "rest"));
        const onset = chord ? measureLastOnset : cursor;
        if (rest) {
          cursor += duration;
          measureLastOnset = cursor;
          return;
        }
        const lyricNode = childByName(noteElement, "lyric");
        const lyricText = lyricNode ? childByName(lyricNode, "text") : null;
        const lyric = lyricText ? String(lyricText.textContent || "").trim() : "";
        const pitch = childByName(noteElement, "pitch");
        const step = pitch ? (childByName(pitch, "step") || {}).textContent : "";
        const octave = pitch ? numberChild(pitch, "octave", null) : null;
        const alter = pitch ? numberChild(pitch, "alter", 0) : 0;
        const stepIndex = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[String(step || "").trim().toUpperCase()];
        if (stepIndex === undefined || octave === null) {
          skipped += 1;
        } else {
          const midi = Math.round((octave + 1) * 12 + stepIndex + alter);
          const absoluteStart = measureOffset + onset;
          if (absoluteStart < BEATS && absoluteStart + duration > 0) {
            const start = Math.round(clamp(absoluteStart, 0, BEATS - 0.25) * 4) / 4;
            const visibleDuration = Math.min(duration, BEATS - start);
            if (visibleDuration >= 0.25 && midi >= LOWEST_MIDI && midi < LOWEST_MIDI + ROWS) {
              const velocity = clamp(numberChild(noteElement, "velocity", 90), 1, 127);
              imported.push({ midi, start, duration: Math.max(0.25, Math.round(visibleDuration * 4) / 4), velocity, lyric });
            } else {
              skipped += 1;
            }
          } else {
            skipped += 1;
          }
        }
        if (!chord) {
          measureLastOnset = onset;
          cursor += duration;
        }
      });
      childrenByName(measure, "forward").forEach((forward) => {
        const durationTicks = numberChild(forward, "duration", 0);
        if (durationTicks > 0) cursor += durationTicks / divisions;
      });
      childrenByName(measure, "backup").forEach((backup) => {
        const durationTicks = numberChild(backup, "duration", 0);
        if (durationTicks > 0) cursor = Math.max(0, cursor - durationTicks / divisions);
      });
      measureOffset += Math.max(measureLength, cursor);
    });

    const tempoNode = descendantsByName(root, "per-minute")[0];
    const parsedTempo = tempoNode ? Number(tempoNode.textContent.trim()) : Number(dom.tempo.value);
    const tempo = clamp(Number.isFinite(parsedTempo) ? parsedTempo : 96, 30, 240);
    if (!imported.length) throw new Error("没有找到当前卷帘可显示的音符（范围为 C4–B5、前 4 小节）");
    return { notes: imported, tempo, skipped };
  }

  async function importMusicXmlFile(file) {
    stopPlayback();
    try {
      const result = parseMusicXml(await file.text());
      commitMutation(() => {
        notes = result.notes.map((note) => ({ ...note, id: nextId++ }));
        selectedId = null;
        dom.noteForm.hidden = true;
        dom.hint.hidden = false;
        dom.tempo.value = String(result.tempo);
        renderRoll();
      });
      const suffix = result.skipped ? `，忽略 ${result.skipped} 个超出当前范围的事件` : "";
      setStatus(`已导入 ${notes.length} 个音符${suffix}`);
    } catch (error) {
      setStatus(`导入失败：${error.message}`);
    } finally {
      dom.musicXmlFile.value = "";
    }
  }

  function playNote(context, note, startAt, secondsPerBeat) {
    const oscillator = context.createOscillator();
    const gain = context.createGain();
    oscillator.type = "triangle";
    oscillator.frequency.value = 440 * Math.pow(2, (note.midi - 69) / 12);
    const attack = Math.min(0.025, secondsPerBeat * 0.08);
    const release = Math.min(0.14, Math.max(0.03, note.duration * secondsPerBeat * 0.12));
    const endAt = startAt + Math.max(0.06, note.duration * secondsPerBeat);
    const amplitude = 0.16 * (note.velocity / 127);
    gain.gain.setValueAtTime(0.0001, startAt);
    gain.gain.exponentialRampToValueAtTime(Math.max(0.001, amplitude), startAt + attack);
    gain.gain.setValueAtTime(Math.max(0.001, amplitude), Math.max(startAt + attack, endAt - release));
    gain.gain.exponentialRampToValueAtTime(0.0001, endAt);
    oscillator.connect(gain).connect(context.destination);
    oscillator.start(startAt);
    oscillator.stop(endAt + 0.02);
    activeSources.push(oscillator);
  }

  async function startPlayback() {
    if (!notes.length) { setStatus("没有可播放的音符"); return; }
    stopPlayback();
    audioContext = audioContext || new (window.AudioContext || window.webkitAudioContext)();
    await audioContext.resume();
    const tempo = clamp(Number(dom.tempo.value) || 96, 30, 240);
    dom.tempo.value = String(tempo);
    const secondsPerBeat = 60 / tempo;
    const now = audioContext.currentTime + 0.05;
    playDuration = Math.max(...notes.map((note) => note.start + note.duration)) * secondsPerBeat;
    notes.forEach((note) => playNote(audioContext, note, now + note.start * secondsPerBeat, secondsPerBeat));
    playStartedAt = performance.now();
    playhead = document.createElement("div");
    playhead.className = "playhead";
    dom.roll.append(playhead);
    const animate = () => {
      if (!playhead) return;
      const elapsed = (performance.now() - playStartedAt) / 1000;
      playhead.style.left = `${Math.min(BEATS * beatWidth - 2, elapsed / secondsPerBeat * beatWidth)}px`;
      if (elapsed < playDuration) window.requestAnimationFrame(animate);
    };
    window.requestAnimationFrame(animate);
    dom.play.disabled = true;
    dom.stop.disabled = false;
    setStatus(`播放中 · ${tempo} BPM`);
    playTimer = window.setTimeout(stopPlayback, (playDuration + 0.2) * 1000);
  }

  function xmlEscape(value) {
    return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&apos;");
  }

  function pitchXml(midi) {
    const stepNames = ["C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B"];
    const alters = [0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0];
    const pitchClass = midi % 12;
    const alter = alters[pitchClass];
    return `<pitch><step>${stepNames[pitchClass]}</step>${alter ? `<alter>${alter}</alter>` : ""}<octave>${Math.floor(midi / 12) - 1}</octave></pitch>`;
  }

  function typeForDuration(duration) {
    if (duration >= 4) return "whole";
    if (duration >= 2) return "half";
    if (duration >= 1) return "quarter";
    if (duration >= 0.5) return "eighth";
    return "16th";
  }

  function musicXml() {
    const sorted = [...notes].sort((a, b) => a.start - b.start || a.midi - b.midi);
    const divisions = PPQ;
    const lines = [];
    let cursor = 0;
    let index = 0;
    const emitRest = (duration) => {
      const ticks = Math.max(1, Math.round(duration * divisions));
      lines.push(`      <note><rest/><duration>${ticks}</duration><voice>1</voice><type>${typeForDuration(duration)}</type></note>`);
    };
    while (index < sorted.length) {
      const start = clamp(Number(sorted[index].start), 0, BEATS);
      if (start > cursor) emitRest(start - cursor);
      const group = sorted.filter((note) => Math.abs(note.start - start) < 0.001);
      group.forEach((note, groupIndex) => {
        const duration = clamp(Number(note.duration), 0.25, BEATS - start);
        const ticks = Math.max(1, Math.round(duration * divisions));
        const lyricXml = note.lyric ? `<lyric><text>${xmlEscape(note.lyric)}</text></lyric>` : "";
        lines.push(`      <note>${groupIndex ? "<chord/>" : ""}${pitchXml(note.midi)}<duration>${ticks}</duration><voice>1</voice><type>${typeForDuration(duration)}</type><velocity>${Math.round(note.velocity)}</velocity>${lyricXml}</note>`);
      });
      cursor = Math.max(cursor, ...group.map((note) => start + Number(note.duration)));
      index += group.length;
    }
    if (cursor < BEATS) emitRest(BEATS - cursor);
    const tempo = clamp(Number(dom.tempo.value) || 96, 30, 240);
    return `<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<score-partwise version="4.0">
  <work><work-title>Classical DAW Web Sketch</work-title></work>
  <identification><creator type="composer">Classical DAW Web Prototype</creator><encoding><software>Classical DAW Web Prototype</software></encoding></identification>
  <part-list><score-part id="P1"><part-name>Piano</part-name></score-part></part-list>
  <part id="P1">
    <measure number="1">
      <attributes><divisions>${divisions}</divisions><key><fifths>0</fifths><mode>major</mode></key><time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>
      <direction placement="above"><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>${tempo}</per-minute></metronome></direction-type><sound tempo="${tempo}"/></direction>
${lines.join("\n")}
    </measure>
  </part>
</score-partwise>
`;
  }

  function downloadMusicXml() {
    const blob = new Blob([musicXml()], { type: "application/vnd.recordare.musicxml+xml" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "classical-daw-sketch.musicxml";
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 500);
    setStatus("已导出 MusicXML（单声部子集）");
  }

  function projectJson() {
    const project = {
      format: PROJECT_FORMAT,
      version: PROJECT_VERSION,
      ppq: PPQ,
      tempo: clamp(Number(dom.tempo.value) || 96, 30, 240),
      notes: cloneNotes(notes),
      selectedId,
      nextId
    };
    return `${JSON.stringify(project, null, 2)}\n`;
  }

  function isQuarterGridValue(value) {
    return Math.abs(value * 4 - Math.round(value * 4)) < 1e-6;
  }

  function parseProject(text) {
    let documentNode;
    try {
      documentNode = JSON.parse(text);
    } catch (_) {
      throw new Error("工程文件不是有效的 JSON");
    }
    if (!documentNode || typeof documentNode !== "object" || Array.isArray(documentNode)) {
      throw new Error("工程文件必须是 JSON 对象");
    }
    if (documentNode.format !== PROJECT_FORMAT) {
      throw new Error("工程文件格式不匹配");
    }
    if (documentNode.version !== PROJECT_VERSION) {
      throw new Error(`不支持的工程版本：${String(documentNode.version)}`);
    }
    if (!Array.isArray(documentNode.notes) || documentNode.notes.length > 10000) {
      throw new Error("工程文件的 notes 必须是数组，且不能超过 10000 个音符");
    }
    if (documentNode.ppq !== PPQ) {
      throw new Error(`工程文件必须使用 ${PPQ} PPQ`);
    }
    const tempo = documentNode.tempo;
    if (typeof tempo !== "number" || !Number.isFinite(tempo) || tempo < 30 || tempo > 240) {
      throw new Error("tempo 必须是 30–240 范围内的数字");
    }

    const ids = new Set();
    const importedNotes = documentNode.notes.map((source, index) => {
      if (!source || typeof source !== "object" || Array.isArray(source)) {
        throw new Error(`第 ${index + 1} 个音符不是对象`);
      }
      const { id, midi, start, duration, velocity, lyric } = source;
      if (!Number.isSafeInteger(id) || id < 1 || ids.has(id)) {
        throw new Error(`第 ${index + 1} 个音符的 id 无效或重复`);
      }
      if (!Number.isInteger(midi) || midi < LOWEST_MIDI || midi >= LOWEST_MIDI + ROWS) {
        throw new Error(`第 ${index + 1} 个音符的 midi 超出 C4–B5 范围`);
      }
      if (typeof start !== "number" || !Number.isFinite(start) || start < 0 || start > BEATS - 0.25 || !isQuarterGridValue(start)) {
        throw new Error(`第 ${index + 1} 个音符的 start 无效`);
      }
      if (typeof duration !== "number" || !Number.isFinite(duration) || duration < 0.25 || duration > BEATS - start || !isQuarterGridValue(duration)) {
        throw new Error(`第 ${index + 1} 个音符的 duration 无效`);
      }
      if (!Number.isInteger(velocity) || velocity < 1 || velocity > 127) {
        throw new Error(`第 ${index + 1} 个音符的 velocity 必须是 1–127 的整数`);
      }
      if (lyric !== undefined && (typeof lyric !== "string" || lyric.length > 256)) {
        throw new Error(`第 ${index + 1} 个音符的 lyric 无效`);
      }
      ids.add(id);
      const note = { id, midi, start, duration, velocity };
      if (lyric !== undefined) note.lyric = lyric;
      return note;
    });

    const selected = documentNode.selectedId;
    if (selected !== null && (!Number.isSafeInteger(selected) || !ids.has(selected))) {
      throw new Error("selectedId 必须为空或指向工程中的音符");
    }
    const maxId = importedNotes.reduce((max, note) => Math.max(max, note.id), 0);
    if (!Number.isSafeInteger(documentNode.nextId) || documentNode.nextId <= maxId) {
      throw new Error("nextId 必须大于所有音符 id");
    }
    return {
      notes: importedNotes,
      nextId: documentNode.nextId,
      selectedId: selected,
      tempo
    };
  }

  function downloadProject() {
    const blob = new Blob([projectJson()], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "classical-daw-project-v1.classical-daw.json";
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 500);
    setStatus("已保存工程 JSON");
  }

  async function importProjectFile(file) {
    try {
      const project = parseProject(await file.text());
      stopPlayback();
      commitMutation(() => {
        notes = cloneNotes(project.notes);
        nextId = project.nextId;
        selectedId = project.selectedId;
        dom.tempo.value = String(project.tempo);
        restoreState({ notes, nextId, selectedId, tempo: String(project.tempo) });
      });
      setStatus(`已打开工程（${notes.length} 个音符）`);
    } catch (error) {
      setStatus(`打开工程失败：${error.message}`);
    } finally {
      dom.projectFile.value = "";
    }
  }

  dom.roll.addEventListener("click", (event) => {
    if (event.target !== dom.roll) return;
    const rect = dom.roll.getBoundingClientRect();
    const start = clamp(Math.floor((event.clientX - rect.left) / beatWidth * 4) / 4, 0, BEATS - 0.25);
    const row = clamp(Math.floor((event.clientY - rect.top) / rowHeight), 0, ROWS - 1);
    addNote(LOWEST_MIDI + ROWS - 1 - row, start, 1);
  });
  dom.add.addEventListener("click", () => addNote());
  dom.remove.addEventListener("click", deleteSelected);
  dom.clear.addEventListener("click", () => {
    if (!notes.length) return;
    commitMutation(() => {
      stopPlayback();
      notes = [];
      selectedId = null;
      dom.noteForm.hidden = true;
      dom.hint.hidden = false;
      renderRoll();
    });
    setStatus("已清空音符");
  });
  dom.undo.addEventListener("click", undo);
  dom.redo.addEventListener("click", redo);
  dom.play.addEventListener("click", () => { startPlayback().catch(() => setStatus("浏览器音频未能启动，请再次点击播放")); });
  dom.stop.addEventListener("click", stopPlayback);
  dom.importButton.addEventListener("click", () => dom.musicXmlFile.click());
  dom.musicXmlFile.addEventListener("change", () => {
    const [file] = dom.musicXmlFile.files || [];
    if (file) importMusicXmlFile(file);
  });
  dom.exportButton.addEventListener("click", downloadMusicXml);
  dom.saveProject.addEventListener("click", downloadProject);
  dom.openProject.addEventListener("click", () => dom.projectFile.click());
  dom.projectFile.addEventListener("change", () => {
    const [file] = dom.projectFile.files || [];
    if (file) importProjectFile(file);
  });
  dom.pitch.addEventListener("change", () => commitMutation(() => updateSelected({ midi: Number(dom.pitch.value) })));
  dom.start.addEventListener("change", () => commitMutation(() => updateSelected({ start: Number(dom.start.value) })));
  dom.duration.addEventListener("change", () => commitMutation(() => updateSelected({ duration: Number(dom.duration.value) })));
  dom.velocity.addEventListener("input", () => {
    beginLiveHistory();
    dom.velocityValue.value = dom.velocity.value;
    dom.velocityValue.textContent = dom.velocity.value;
    updateSelected({ velocity: Number(dom.velocity.value) });
  });
  dom.lyric.addEventListener("input", () => {
    const note = notes.find((item) => item.id === selectedId);
    if (!note) return;
    beginLiveHistory();
    note.lyric = dom.lyric.value;
    renderRoll();
  });
  dom.velocity.addEventListener("change", finishLiveHistory);
  dom.lyric.addEventListener("change", finishLiveHistory);
  dom.roll.addEventListener("keydown", (event) => {
    if (event.key === "Delete" || event.key === "Backspace") { event.preventDefault(); deleteSelected(); }
    if (event.key === " ") { event.preventDefault(); dom.play.click(); }
  });
  document.addEventListener("keydown", (event) => {
    const modifier = event.metaKey || event.ctrlKey;
    if (modifier && !event.altKey && event.key.toLowerCase() === "z") {
      event.preventDefault();
      if (event.shiftKey) redo();
      else undo();
      return;
    }
    if (event.target.matches("input, select, textarea")) return;
    if (event.key === "Delete" || event.key === "Backspace") deleteSelected();
    if (event.key === " ") { event.preventDefault(); dom.play.click(); }
  });

  renderPitchLabels();
  renderBeatLabels();
  populatePitchOptions();
  renderRoll();
  updateHistoryButtons();
})();
