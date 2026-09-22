(() => {
  "use strict";

  // Keep exported MusicXML durations on the engine's canonical 960 PPQ grid.
  const PPQ = 960;
  const PROJECT_FORMAT = "classical-daw-web-project";
  const PROJECT_VERSION = 1;
  const RECOVERY_KEY = "classical-daw-web-recovery-v1";
  const RECOVERY_DELAY_MS = 900;
  const DEFAULT_MEASURES = 4;
  const ALLOWED_MEASURES = Object.freeze([4, 8, 16]);
  const DEFAULT_METER = Object.freeze({ numerator: 4, denominator: 4 });
  const ALLOWED_METERS = Object.freeze([
    Object.freeze({ numerator: 2, denominator: 4 }),
    Object.freeze({ numerator: 3, denominator: 4 }),
    Object.freeze({ numerator: 4, denominator: 4 }),
    Object.freeze({ numerator: 6, denominator: 8 })
  ]);
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
    meterNumerator: document.querySelector("#meter-numerator"),
    meterDenominator: document.querySelector("#meter-denominator"),
    meterLabel: document.querySelector("#meter-label"),
    meterHint: document.querySelector("#meter-hint"),
    measureCount: document.querySelector("#measure-count"),
    add: document.querySelector("#add-note"),
    clear: document.querySelector("#clear"),
    saveProject: document.querySelector("#save-project"),
    openProject: document.querySelector("#open-project"),
    projectFile: document.querySelector("#project-file"),
    importButton: document.querySelector("#import"),
    musicXmlFile: document.querySelector("#musicxml-file"),
    importMidiButton: document.querySelector("#import-midi"),
    midiFile: document.querySelector("#midi-file"),
    remove: document.querySelector("#delete-note"),
    exportButton: document.querySelector("#export"),
    exportMidiButton: document.querySelector("#export-midi"),
    exportWavButton: document.querySelector("#export-wav"),
    recoveryBanner: document.querySelector("#recovery-banner"),
    recoveryMessage: document.querySelector("#recovery-message"),
    restoreRecovery: document.querySelector("#restore-recovery"),
    discardRecovery: document.querySelector("#discard-recovery")
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
  let timeSignature = { ...DEFAULT_METER };
  let measureCount = DEFAULT_MEASURES;
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
  let recoveryTimer = null;
  let clipboardNote = null;
  let dragState = null;

  function localStorageAvailable() {
    try {
      return typeof window.localStorage !== "undefined";
    } catch (_) {
      return false;
    }
  }

  function scheduleRecoverySave() {
    if (!localStorageAvailable()) return;
    if (recoveryTimer !== null) window.clearTimeout(recoveryTimer);
    recoveryTimer = window.setTimeout(() => {
      recoveryTimer = null;
      try {
        window.localStorage.setItem(RECOVERY_KEY, projectJson());
      } catch (_) {
        // Quota and privacy-mode failures must not interrupt editing.
      }
    }, RECOVERY_DELAY_MS);
  }

  function hideRecoveryBanner() {
    if (dom.recoveryBanner) dom.recoveryBanner.hidden = true;
  }

  function discardRecovery() {
    try {
      if (localStorageAvailable()) window.localStorage.removeItem(RECOVERY_KEY);
    } catch (_) {
      // A storage failure is harmless; the current document remains usable.
    }
    hideRecoveryBanner();
  }

  function showRecoveryIfPresent() {
    if (!localStorageAvailable()) return;
    let raw = null;
    try {
      raw = window.localStorage.getItem(RECOVERY_KEY);
    } catch (_) {
      return;
    }
    if (!raw) return;
    try {
      const recovered = parseProject(raw);
      if (dom.recoveryMessage) {
        dom.recoveryMessage.textContent = `检测到浏览器本地恢复副本（${recovered.notes.length} 个音符）。`;
      }
      if (dom.recoveryBanner) dom.recoveryBanner.hidden = false;
      if (dom.restoreRecovery) {
        dom.restoreRecovery.onclick = () => {
          stopPlayback();
          history.past = [];
          history.future = [];
          restoreState(recovered);
          updateHistoryButtons();
          hideRecoveryBanner();
          setStatus(`已恢复本地副本（${notes.length} 个音符）`);
          scheduleRecoverySave();
        };
      }
      if (dom.discardRecovery) {
        dom.discardRecovery.onclick = () => {
          discardRecovery();
          setStatus("已丢弃本地恢复副本");
        };
      }
    } catch (_) {
      // A stale or malformed sidecar should never block a fresh document.
      discardRecovery();
    }
  }

  function noteName(midi) {
    const octave = Math.floor(midi / 12) - 1;
    return `${pitchNames[midi % 12]}${octave}`;
  }

  function midiToRow(midi) { return LOWEST_MIDI + ROWS - 1 - midi; }

  function clamp(value, min, max) { return Math.min(max, Math.max(min, value)); }

  function isAllowedMeter(numerator, denominator) {
    return ALLOWED_METERS.some((meter) => meter.numerator === numerator && meter.denominator === denominator);
  }

  function isAllowedMeasureCount(value) {
    return ALLOWED_MEASURES.includes(value);
  }

  function measureBeats(meter = timeSignature) {
    return meter.numerator * 4 / meter.denominator;
  }

  function visibleBeats(meter = timeSignature, count = measureCount) {
    return count * measureBeats(meter);
  }

  function measureCountForBeats(beats, meter = timeSignature) {
    const required = Math.max(1, Math.ceil(beats / measureBeats(meter) - 1e-9));
    return required <= 4 ? 4 : required <= 8 ? 8 : 16;
  }

  function meterText(meter = timeSignature) {
    return `${meter.numerator}/${meter.denominator}`;
  }

  function syncMeterControls() {
    const text = meterText();
    dom.meterNumerator.value = String(timeSignature.numerator);
    dom.meterDenominator.value = String(timeSignature.denominator);
    Array.from(dom.meterDenominator.options).forEach((option) => {
      option.disabled = (timeSignature.numerator === 6 && option.value !== "8")
        || (timeSignature.numerator !== 6 && option.value !== "4");
    });
    dom.meterLabel.textContent = text;
    dom.measureCount.value = String(measureCount);
    dom.meterHint.textContent = `网格以四分音符为一拍，当前 ${text}，显示 ${measureCount} 小节。`;
    dom.start.max = String(Math.max(0.25, visibleBeats() - 0.25));
  }

  function cloneNotes(source) {
    return source.map((note) => ({ ...note }));
  }

  function snapshot() {
    return {
      notes: cloneNotes(notes),
      nextId,
      selectedId,
      tempo: String(dom.tempo.value),
      timeSignature: { ...timeSignature },
      measureCount
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
    scheduleRecoverySave();
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
    timeSignature = isAllowedMeter(state.timeSignature?.numerator, state.timeSignature?.denominator)
      ? { numerator: state.timeSignature.numerator, denominator: state.timeSignature.denominator }
      : { ...DEFAULT_METER };
    measureCount = isAllowedMeasureCount(state.measureCount) ? state.measureCount : DEFAULT_MEASURES;
    syncMeterControls();
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
    renderBeatLabels();
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
    scheduleRecoverySave();
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
    scheduleRecoverySave();
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
    const totalBeats = visibleBeats();
    const measureLength = measureBeats();
    dom.beatLabels.style.width = `${totalBeats * beatWidth}px`;
    for (let beat = 0; beat < totalBeats; beat += 1) {
      const label = document.createElement("div");
      const measureStart = Math.abs(beat % measureLength) < 0.001;
      label.className = `beat-label ${measureStart ? "measure" : ""}`;
      label.style.left = `${beat * beatWidth}px`;
      label.textContent = measureStart ? `小节 ${Math.floor(beat / measureLength) + 1}` : `${beat + 1}`;
      dom.beatLabels.append(label);
    }
  }

  function renderRoll() {
    dom.roll.querySelectorAll(".note, .measure-line, .playhead").forEach((element) => element.remove());
    const totalBeats = visibleBeats();
    const measureLength = measureBeats();
    dom.roll.style.width = `${totalBeats * beatWidth}px`;
    for (let beat = 0; beat <= totalBeats; beat += measureLength) {
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
      element.addEventListener("pointerdown", (event) => beginNoteDrag(event, note.id, element));
      dom.roll.append(element);
    });
    if (playhead) dom.roll.append(playhead);
    dom.count.textContent = `${notes.length} 个音符`;
  }

  function syncSelectedForm(note) {
    if (!note || note.id !== selectedId) return;
    dom.pitch.value = String(note.midi);
    dom.start.value = String(note.start);
    dom.duration.value = String(note.duration);
    dom.velocity.value = String(note.velocity);
    dom.velocityValue.value = String(note.velocity);
    dom.velocityValue.textContent = String(note.velocity);
    dom.lyric.value = note.lyric || "";
  }

  function beginNoteDrag(event, id, element) {
    if (event.button !== 0) return;
    const note = notes.find((item) => item.id === id);
    if (!note) return;
    event.preventDefault();
    event.stopPropagation();
    finishLiveHistory();
    selectedId = id;
    dom.noteForm.hidden = false;
    dom.hint.hidden = true;
    syncSelectedForm(note);
    dom.roll.querySelectorAll(".note.selected").forEach((selected) => selected.classList.remove("selected"));
    element.classList.add("selected");
    const rollRect = dom.roll.getBoundingClientRect();
    const elementRect = element.getBoundingClientRect();
    dragState = {
      id,
      pointerId: event.pointerId,
      before: snapshot(),
      element,
      offsetX: event.clientX - elementRect.left,
      offsetY: event.clientY - elementRect.top,
      moved: false,
      rollRect
    };
    element.setPointerCapture(event.pointerId);
    element.addEventListener("pointermove", handleNoteDrag);
    element.addEventListener("pointerup", finishNoteDrag, { once: true });
    element.addEventListener("pointercancel", cancelNoteDrag, { once: true });
  }

  function handleNoteDrag(event) {
    if (!dragState || event.pointerId !== dragState.pointerId) return;
    const note = notes.find((item) => item.id === dragState.id);
    if (!note) return;
    const rollRect = dom.roll.getBoundingClientRect();
    const rawStart = (event.clientX - rollRect.left - dragState.offsetX) / beatWidth;
    const nextStart = clamp(Math.round(rawStart * 4) / 4, 0, visibleBeats() - note.duration);
    const rawRow = (event.clientY - rollRect.top - dragState.offsetY) / rowHeight;
    const nextRow = clamp(Math.floor(rawRow + 0.5), 0, ROWS - 1);
    const nextMidi = LOWEST_MIDI + ROWS - 1 - nextRow;
    if (nextStart === note.start && nextMidi === note.midi) return;
    dragState.moved = true;
    note.start = nextStart;
    note.midi = nextMidi;
    dragState.element.style.left = `${note.start * beatWidth + 2}px`;
    dragState.element.style.top = `${midiToRow(note.midi) * rowHeight + 2}px`;
    dragState.element.title = `${noteName(note.midi)} · ${note.duration} 拍 · 起始 ${note.start}${note.lyric ? ` · ${note.lyric}` : ""}`;
    syncSelectedForm(note);
  }

  function finishNoteDrag(event) {
    if (!dragState || event.pointerId !== dragState.pointerId) return;
    const current = dragState;
    dragState = null;
    current.element.removeEventListener("pointermove", handleNoteDrag);
    current.element.removeEventListener("pointercancel", cancelNoteDrag);
    if (current.element.hasPointerCapture(event.pointerId)) current.element.releasePointerCapture(event.pointerId);
    if (current.moved) {
      pushHistory(current.before);
      setStatus("已移动音符");
    }
  }

  function cancelNoteDrag(event) {
    if (!dragState || event.pointerId !== dragState.pointerId) return;
    const current = dragState;
    dragState = null;
    current.element.removeEventListener("pointermove", handleNoteDrag);
    current.element.removeEventListener("pointerup", finishNoteDrag);
    restoreState(current.before);
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
      const suggestedStart = Math.min(visibleBeats() - duration, Math.max(0, Math.ceil(Math.max(0, ...occupied) * 4) / 4));
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
    note.start = clamp(Math.round(Number(note.start) * 4) / 4, 0, visibleBeats() - 0.25);
    note.duration = clamp(Number(note.duration), 0.25, visibleBeats() - note.start);
    note.midi = clamp(Math.round(Number(note.midi)), LOWEST_MIDI, LOWEST_MIDI + ROWS - 1);
    note.velocity = clamp(Math.round(Number(note.velocity)), 1, 127);
    selectNote(note.id);
  }

  function changeTimeSignature() {
    const numerator = Number(dom.meterNumerator.value);
    // The compact UI only exposes the four supported combinations. Selecting
    // 6 switches to 6/8; all other numerators use a quarter-note denominator.
    const denominator = numerator === 6 ? 8 : 4;
    dom.meterDenominator.value = String(denominator);
    if (!Number.isInteger(numerator) || !Number.isInteger(denominator) || !isAllowedMeter(numerator, denominator)) {
      syncMeterControls();
      setStatus("拍号不受支持：仅支持 2/4、3/4、4/4、6/8");
      return;
    }
    const nextMeter = { numerator, denominator };
    const nextTotalBeats = visibleBeats(nextMeter);
    const exceedsVisibleRange = notes.some((note) => note.start + note.duration > nextTotalBeats + 1e-6);
    if (exceedsVisibleRange) {
      syncMeterControls();
      setStatus(`拍号变更失败：音符超出 ${meterText(nextMeter)} 的 ${measureCount} 小节范围`);
      return;
    }
    finishLiveHistory();
    const before = snapshot();
    timeSignature = nextMeter;
    syncMeterControls();
    renderBeatLabels();
    renderRoll();
    pushHistory(before);
    setStatus(`已设置拍号 ${meterText()}`);
  }

  function changeMeasureCount() {
    const nextCount = Number(dom.measureCount.value);
    if (!Number.isInteger(nextCount) || !isAllowedMeasureCount(nextCount)) {
      syncMeterControls();
      setStatus("小节数不受支持：仅支持 4、8、16 小节");
      return;
    }
    const nextTotalBeats = visibleBeats(timeSignature, nextCount);
    const exceedsVisibleRange = notes.some((note) => note.start + note.duration > nextTotalBeats + 1e-6);
    if (exceedsVisibleRange) {
      syncMeterControls();
      setStatus(`小节数变更失败：音符超出 ${nextCount} 小节范围`);
      return;
    }
    finishLiveHistory();
    const before = snapshot();
    measureCount = nextCount;
    syncMeterControls();
    renderBeatLabels();
    renderRoll();
    pushHistory(before);
    setStatus(`已设置 ${measureCount} 小节`);
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

  function moveSelectedBy(deltaStart, deltaMidi) {
    const note = notes.find((item) => item.id === selectedId);
    if (!note) return false;
    const nextStart = clamp(
      Math.round((note.start + deltaStart) * 4) / 4,
      0,
      visibleBeats() - note.duration
    );
    const nextMidi = clamp(note.midi + deltaMidi, LOWEST_MIDI, LOWEST_MIDI + ROWS - 1);
    if (nextStart === note.start && nextMidi === note.midi) return false;
    commitMutation(() => {
      note.start = nextStart;
      note.midi = nextMidi;
      syncSelectedForm(note);
      renderRoll();
    });
    setStatus("已移动音符");
    return true;
  }

  function copySelected() {
    const note = notes.find((item) => item.id === selectedId);
    if (!note) return false;
    clipboardNote = { ...note };
    setStatus("已复制所选音符");
    return true;
  }

  function pasteNote() {
    if (!clipboardNote) {
      setStatus("剪贴板中没有音符");
      return false;
    }
    const duration = clamp(Number(clipboardNote.duration), 0.25, visibleBeats());
    const start = clamp(
      Math.round((Number(clipboardNote.start) + 0.25) * 4) / 4,
      0,
      visibleBeats() - duration
    );
    const note = {
      id: nextId++,
      midi: clamp(Math.round(Number(clipboardNote.midi)), LOWEST_MIDI, LOWEST_MIDI + ROWS - 1),
      start,
      duration,
      velocity: clamp(Math.round(Number(clipboardNote.velocity)), 1, 127)
    };
    if (clipboardNote.lyric) note.lyric = clipboardNote.lyric;
    commitMutation(() => {
      notes.push(note);
      selectNote(note.id);
    });
    setStatus("已粘贴音符");
    return true;
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
    const importedMeasureCount = measures.length <= 4 ? 4 : measures.length <= 8 ? 8 : 16;

    let divisions = PPQ;
    let beatsPerMeasure = DEFAULT_METER.numerator;
    let beatType = DEFAULT_METER.denominator;
    let timeSignatureSeen = false;
    let measureOffset = 0;
    const imported = [];
    const tiedSegments = new Map();
    let skipped = 0;
    let pendingVelocity = 90;

    measures.forEach((measure) => {
      const attributes = childByName(measure, "attributes");
      if (attributes) {
        const nextDivisions = numberChild(attributes, "divisions", null);
        if (nextDivisions && nextDivisions > 0) divisions = nextDivisions;
        const time = childByName(attributes, "time");
        if (time && !timeSignatureSeen) {
          const nextBeats = numberChild(time, "beats", null);
          const nextBeatType = numberChild(time, "beat-type", null);
          if (!Number.isInteger(nextBeats) || !Number.isInteger(nextBeatType) || !isAllowedMeter(nextBeats, nextBeatType)) {
            throw new Error("MusicXML 首个拍号不受支持（仅支持 2/4、3/4、4/4、6/8）");
          }
          beatsPerMeasure = nextBeats;
          beatType = nextBeatType;
          timeSignatureSeen = true;
        }
      }
      const measureLength = beatsPerMeasure * 4 / beatType;
      const importedTotalBeats = visibleBeats({ numerator: beatsPerMeasure, denominator: beatType }, importedMeasureCount);
      let cursor = 0;
      let measureLastOnset = 0;
      Array.from(measure.children || []).forEach((noteElement) => {
        const elementName = noteElement.localName || noteElement.tagName;
        if (elementName === "direction") {
          const sound = childByName(noteElement, "sound");
          const dynamics = sound ? Number(sound.getAttribute("dynamics")) : NaN;
          if (Number.isFinite(dynamics)) pendingVelocity = clamp(Math.round(dynamics), 1, 127);
          return;
        }
        if (elementName === "forward" || elementName === "backup") {
          const durationTicks = numberChild(noteElement, "duration", 0);
          if (durationTicks > 0) {
            const duration = durationTicks / divisions;
            cursor = elementName === "forward" ? cursor + duration : Math.max(0, cursor - duration);
          }
          return;
        }
        if (elementName !== "note") return;
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
          if (absoluteStart < importedTotalBeats && absoluteStart + duration > 0) {
            const totalVisibleBeats = importedTotalBeats;
            const start = Math.round(clamp(absoluteStart, 0, totalVisibleBeats - 0.25) * 4) / 4;
            const visibleDuration = Math.min(duration, totalVisibleBeats - start);
            if (visibleDuration >= 0.25 && midi >= LOWEST_MIDI && midi < LOWEST_MIDI + ROWS) {
              const velocity = clamp(numberChild(noteElement, "velocity", pendingVelocity), 1, 127);
              const roundedDuration = Math.max(0.25, Math.round(visibleDuration * 4) / 4);
              const voiceNode = childByName(noteElement, "voice");
              const voice = voiceNode ? String(voiceNode.textContent || "1").trim() : "1";
              const tieTypes = childrenByName(noteElement, "tie").map((tie) => tie.getAttribute("type"));
              const tieStart = tieTypes.includes("start");
              const tieStop = tieTypes.includes("stop");
              const tieKey = `${voice}:${midi}`;
              let importedIndex = imported.length;
              const previousIndex = tiedSegments.get(tieKey);
              const previous = previousIndex === undefined ? null : imported[previousIndex];
              if (tieStop && previous && Math.abs(previous.start + previous.duration - start) < 0.01) {
                previous.duration = Math.min(totalVisibleBeats - previous.start,
                  Math.max(previous.duration, start + roundedDuration - previous.start));
                if (!previous.lyric && lyric) previous.lyric = lyric;
                importedIndex = previousIndex;
              } else {
                imported.push({ midi, start, duration: roundedDuration, velocity, lyric });
              }
              if (tieStart) tiedSegments.set(tieKey, importedIndex);
              else if (tieStop || !tieStart) tiedSegments.delete(tieKey);
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
      measureOffset += Math.max(measureLength, cursor);
    });

    const tempoNode = descendantsByName(root, "per-minute")[0];
    const parsedTempo = tempoNode ? Number(tempoNode.textContent.trim()) : Number(dom.tempo.value);
    const tempo = clamp(Number.isFinite(parsedTempo) ? parsedTempo : 96, 30, 240);
    if (!imported.length) throw new Error(`没有找到当前卷帘可显示的音符（范围为 C4–B5、前 ${importedMeasureCount} 小节）`);
    return {
      notes: imported,
      tempo,
      skipped,
      timeSignature: { numerator: beatsPerMeasure, denominator: beatType },
      measureCount: importedMeasureCount
    };
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
        timeSignature = { ...result.timeSignature };
        measureCount = result.measureCount;
        syncMeterControls();
        renderBeatLabels();
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
      playhead.style.left = `${Math.min(visibleBeats() * beatWidth - 2, elapsed / secondsPerBeat * beatWidth)}px`;
      if (elapsed < playDuration) window.requestAnimationFrame(animate);
    };
    window.requestAnimationFrame(animate);
    dom.play.disabled = true;
    dom.stop.disabled = false;
    setStatus(`播放中 · ${tempo} BPM`);
    playTimer = window.setTimeout(stopPlayback, (playDuration + 0.2) * 1000);
  }

  // The MIDI bridge deliberately stays small and dependency-free.  It accepts
  // standard format 0 and 1 files, validates the complete event stream before
  // returning, and only then lets the caller replace the current document.
  function readMidiVlq(bytes, cursor, end) {
    let value = 0;
    for (let count = 0; count < 4; count += 1) {
      if (cursor.position >= end) throw new Error("MIDI 变长数值不完整");
      const byte = bytes[cursor.position++];
      value = (value * 128) + (byte & 0x7f);
      if (!(byte & 0x80)) return value;
    }
    throw new Error("MIDI 变长数值超过 4 字节");
  }

  function readMidiU16(bytes, offset) {
    return (bytes[offset] << 8) | bytes[offset + 1];
  }

  function readMidiU32(bytes, offset) {
    return bytes[offset] * 0x1000000 + (bytes[offset + 1] << 16)
      + (bytes[offset + 2] << 8) + bytes[offset + 3];
  }

  function midiChunkText(bytes, offset) {
    return String.fromCharCode(bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
  }

  function parseMidiFile(buffer) {
    const bytes = new Uint8Array(buffer);
    if (bytes.length < 14 || midiChunkText(bytes, 0) !== "MThd") {
      throw new Error("MIDI 文件头无效");
    }
    const headerLength = readMidiU32(bytes, 4);
    if (headerLength < 6 || headerLength > bytes.length - 8) throw new Error("MIDI 文件头长度无效");
    const format = readMidiU16(bytes, 8);
    const trackCount = readMidiU16(bytes, 10);
    const division = readMidiU16(bytes, 12);
    if (format !== 0 && format !== 1) throw new Error("只支持 MIDI Type 0 或 Type 1");
    if (!trackCount || (format === 0 && trackCount !== 1)) throw new Error("MIDI 轨道数量与文件类型不匹配");
    if (!division || (division & 0x8000)) throw new Error("只支持 PPQ 格式的 MIDI（不支持 SMPTE）");

    let offset = 8 + headerLength;
    const allNotes = [];
    let firstTempo = null;
    let firstMeter = null;
    let skipped = 0;
    for (let trackIndex = 0; trackIndex < trackCount; trackIndex += 1) {
      if (offset + 8 > bytes.length || midiChunkText(bytes, offset) !== "MTrk") {
        throw new Error(`第 ${trackIndex + 1} 条 MIDI 轨道头无效`);
      }
      const trackLength = readMidiU32(bytes, offset + 4);
      const trackStart = offset + 8;
      const trackEnd = trackStart + trackLength;
      if (trackEnd > bytes.length) throw new Error(`第 ${trackIndex + 1} 条 MIDI 轨道超出文件范围`);
      offset = trackEnd;
      const cursor = { position: trackStart };
      let absoluteTick = 0;
      let runningStatus = 0;
      let reachedEnd = false;
      const active = new Map();
      while (cursor.position < trackEnd) {
        const delta = readMidiVlq(bytes, cursor, trackEnd);
        absoluteTick += delta;
        if (!Number.isSafeInteger(absoluteTick)) throw new Error("MIDI 时间位置溢出");
        let status = bytes[cursor.position++];
        if (status < 0x80) {
          if (runningStatus < 0x80 || runningStatus >= 0xf0) throw new Error("MIDI running status 无效");
          cursor.position -= 1;
          status = runningStatus;
        } else if (status >= 0x80 && status < 0xf0) {
          runningStatus = status;
        }
        if (status === 0xff) {
          runningStatus = 0;
          if (cursor.position >= trackEnd) throw new Error("MIDI meta 事件缺少类型");
          const metaType = bytes[cursor.position++];
          const length = readMidiVlq(bytes, cursor, trackEnd);
          if (length > trackEnd - cursor.position) throw new Error("MIDI meta 事件超出轨道范围");
          if (metaType === 0x2f) {
            if (length !== 0) throw new Error("MIDI EOT 事件长度必须为 0");
            reachedEnd = true;
            cursor.position += length;
            if (cursor.position !== trackEnd) throw new Error("MIDI EOT 后存在额外事件");
            continue;
          }
          if (metaType === 0x51) {
            if (length !== 3) throw new Error("MIDI tempo 事件长度必须为 3");
            const micros = bytes[cursor.position] * 0x10000 + (bytes[cursor.position + 1] << 8) + bytes[cursor.position + 2];
            if (!micros) throw new Error("MIDI tempo 事件值无效");
            if (!firstTempo || absoluteTick < firstTempo.tick) firstTempo = { tick: absoluteTick, micros };
          }
          if (metaType === 0x58) {
            if (length !== 4) throw new Error("MIDI 拍号事件长度必须为 4");
            const numerator = bytes[cursor.position];
            const exponent = bytes[cursor.position + 1];
            if (!numerator || exponent > 7) throw new Error("MIDI 拍号事件值无效");
            const denominator = 2 ** exponent;
            if (!isAllowedMeter(numerator, denominator)) {
              throw new Error("MIDI 首个拍号不受支持（仅支持 2/4、3/4、4/4、6/8）");
            }
            if (!firstMeter || absoluteTick < firstMeter.tick) firstMeter = { tick: absoluteTick, numerator, denominator };
          }
          cursor.position += length;
          continue;
        }
        if (status === 0xf0 || status === 0xf7) {
          runningStatus = 0;
          const length = readMidiVlq(bytes, cursor, trackEnd);
          if (length > trackEnd - cursor.position) throw new Error("MIDI SysEx 事件超出轨道范围");
          cursor.position += length;
          continue;
        }
        if (status < 0x80 || status >= 0xf0) throw new Error("MIDI 事件状态字节无效");
        const eventType = status & 0xf0;
        const channel = status & 0x0f;
        const dataLength = eventType === 0xc0 || eventType === 0xd0 ? 1 : 2;
        if (cursor.position + dataLength > trackEnd) throw new Error("MIDI 通道事件数据不完整");
        const data1 = bytes[cursor.position++];
        const data2 = dataLength === 2 ? bytes[cursor.position++] : 0;
        if (data1 >= 0x80 || data2 >= 0x80) throw new Error("MIDI 通道事件数据字节无效");
        if (eventType === 0x90 && data2 > 0) {
          const key = `${channel}:${data1}`;
          const stack = active.get(key) || [];
          stack.push({ tick: absoluteTick, midi: data1, velocity: data2 });
          active.set(key, stack);
        } else if (eventType === 0x80 || (eventType === 0x90 && data2 === 0)) {
          const key = `${channel}:${data1}`;
          const stack = active.get(key);
          if (!stack || !stack.length) throw new Error("MIDI 音符关闭事件没有对应的开启事件");
          const note = stack.pop();
          if (absoluteTick <= note.tick) throw new Error("MIDI 音符时值必须为正数");
          allNotes.push({ startTick: note.tick, endTick: absoluteTick, midi: note.midi, velocity: note.velocity });
        }
      }
      if (!reachedEnd) throw new Error(`第 ${trackIndex + 1} 条 MIDI 轨道缺少 EOT 事件`);
      if (active.size && Array.from(active.values()).some((stack) => stack.length)) {
        throw new Error(`第 ${trackIndex + 1} 条 MIDI 轨道存在未关闭的音符`);
      }
    }
    if (offset !== bytes.length) throw new Error("MIDI 文件末尾存在额外数据");
    if (!allNotes.length) throw new Error("MIDI 中没有音符事件");

    const meter = firstMeter ? { numerator: firstMeter.numerator, denominator: firstMeter.denominator } : { ...DEFAULT_METER };
    const maxEndBeats = Math.max(...allNotes.map((note) => note.endTick / division));
    const importedMeasureCount = measureCountForBeats(maxEndBeats, meter);
    const totalBeats = visibleBeats(meter, importedMeasureCount);
    const imported = [];
    allNotes.sort((a, b) => a.startTick - b.startTick || a.midi - b.midi);
    allNotes.forEach((source) => {
      const rawStart = source.startTick / division;
      const rawDuration = (source.endTick - source.startTick) / division;
      const start = Math.round(rawStart * 4) / 4;
      const duration = Math.round(rawDuration * 4) / 4;
      if (source.midi < LOWEST_MIDI || source.midi >= LOWEST_MIDI + ROWS || start < 0 || start >= totalBeats || duration < 0.25) {
        skipped += 1;
        return;
      }
      const clippedDuration = Math.min(duration, totalBeats - start);
      if (clippedDuration < 0.25) {
        skipped += 1;
        return;
      }
      imported.push({ midi: source.midi, start, duration: clippedDuration, velocity: source.velocity });
    });
    if (!imported.length) throw new Error(`没有找到当前卷帘可显示的音符（范围为 C4–B5、前 ${importedMeasureCount} 小节）`);
    const tempo = firstTempo ? clamp(60000000 / firstTempo.micros, 30, 240) : clamp(Number(dom.tempo.value) || 96, 30, 240);
    return { notes: imported, tempo, skipped, timeSignature: meter, measureCount: importedMeasureCount, format };
  }

  function midiVlq(value) {
    if (!Number.isInteger(value) || value < 0 || value > 0x0fffffff) throw new Error("MIDI delta 超出范围");
    let buffer = value & 0x7f;
    const bytes = [];
    while ((value >>= 7)) {
      buffer = (value & 0x7f) | 0x80;
      bytes.unshift(buffer);
    }
    bytes.push(buffer & 0x7f);
    return bytes;
  }

  function midiU32(value) {
    return [(value >>> 24) & 0xff, (value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff];
  }

  function midiTrackChunk(content) {
    return [0x4d, 0x54, 0x72, 0x6b, ...midiU32(content.length), ...content];
  }

  function midiFile() {
    const tempo = clamp(Number(dom.tempo.value) || 96, 30, 240);
    const micros = Math.round(60000000 / tempo);
    const denominatorExponent = Math.round(Math.log2(timeSignature.denominator));
    const conductor = [
      0x00, 0xff, 0x51, 0x03, (micros >> 16) & 0xff, (micros >> 8) & 0xff, micros & 0xff,
      0x00, 0xff, 0x58, 0x04, timeSignature.numerator, denominatorExponent, 0x18, 0x08,
      0x00, 0xff, 0x2f, 0x00
    ];
    const events = [];
    notes.forEach((note) => {
      const start = Math.max(0, Math.round(Number(note.start) * PPQ));
      const end = Math.max(start + 1, Math.round((Number(note.start) + Number(note.duration)) * PPQ));
      events.push({ tick: start, kind: 1, midi: note.midi, velocity: note.velocity });
      events.push({ tick: end, kind: 0, midi: note.midi, velocity: 0 });
    });
    events.sort((a, b) => a.tick - b.tick || a.kind - b.kind || a.midi - b.midi);
    const noteTrack = [];
    let cursor = 0;
    events.forEach((event) => {
      noteTrack.push(...midiVlq(event.tick - cursor));
      noteTrack.push(event.kind ? 0x90 : 0x80, event.midi & 0x7f, event.velocity & 0x7f);
      cursor = event.tick;
    });
    noteTrack.push(0x00, 0xff, 0x2f, 0x00);
    const header = [0x4d, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, (PPQ >> 8) & 0x7f, PPQ & 0xff];
    return new Uint8Array([...header, ...midiTrackChunk(conductor), ...midiTrackChunk(noteTrack)]);
  }

  async function importMidiFile(file) {
    stopPlayback();
    try {
      const result = parseMidiFile(await file.arrayBuffer());
      // Parsing completed before this mutation, so malformed files leave the
      // current document, selection, undo stack and recovery copy untouched.
      commitMutation(() => {
        notes = result.notes.map((note) => ({ ...note, id: nextId++ }));
        selectedId = null;
        dom.noteForm.hidden = true;
        dom.hint.hidden = false;
        dom.tempo.value = String(result.tempo);
        timeSignature = { ...result.timeSignature };
        measureCount = result.measureCount;
        syncMeterControls();
        renderBeatLabels();
        renderRoll();
      });
      const suffix = result.skipped ? `，忽略 ${result.skipped} 个超出当前范围的事件` : "";
      setStatus(`已导入 MIDI Type ${result.format}（${notes.length} 个音符）${suffix}`);
    } catch (error) {
      setStatus(`MIDI 导入失败：${error.message}`);
    } finally {
      dom.midiFile.value = "";
    }
  }

  function downloadMidi() {
    const blob = new Blob([midiFile()], { type: "audio/midi" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "classical-daw-sketch.mid";
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 500);
    setStatus("已导出 MIDI Type 1（960 PPQ）");
  }

  function wavBytesFromNotes() {
    const sampleRate = 48000;
    const tempo = clamp(Number(dom.tempo.value) || 96, 30, 240);
    const secondsPerBeat = 60 / tempo;
    const tailSeconds = 0.1;
    const durationBeats = notes.length ? Math.max(...notes.map((note) => note.start + note.duration)) : 0;
    const frameCount = Math.max(1, Math.ceil(durationBeats * secondsPerBeat * sampleRate + tailSeconds * sampleRate));
    const samples = new Float32Array(frameCount);
    const twoPi = Math.PI * 2;
    notes.forEach((note) => {
      const startFrame = Math.max(0, Math.round(note.start * secondsPerBeat * sampleRate));
      const endFrame = Math.min(frameCount, Math.max(startFrame + 1,
        Math.round((note.start + note.duration) * secondsPerBeat * sampleRate)));
      const frequency = 440 * Math.pow(2, (note.midi - 69) / 12);
      const amplitude = 0.18 * (note.velocity / 127);
      const attackFrames = Math.max(1, Math.round(sampleRate * 0.008));
      const releaseFrames = Math.max(1, Math.round(sampleRate * 0.035));
      const noteFrames = endFrame - startFrame;
      for (let frame = startFrame; frame < endFrame; frame += 1) {
        const age = frame - startFrame;
        const remaining = noteFrames - age;
        const attack = Math.min(1, age / attackFrames);
        const release = Math.min(1, remaining / releaseFrames);
        const envelope = Math.max(0, Math.min(attack, release));
        samples[frame] += amplitude * envelope * Math.sin(twoPi * frequency * age / sampleRate);
      }
    });
    const dataBytes = samples.length * 2;
    const buffer = new ArrayBuffer(44 + dataBytes);
    const view = new DataView(buffer);
    const writeText = (offset, value) => {
      for (let index = 0; index < value.length; index += 1) view.setUint8(offset + index, value.charCodeAt(index));
    };
    writeText(0, "RIFF");
    view.setUint32(4, 36 + dataBytes, true);
    writeText(8, "WAVE");
    writeText(12, "fmt ");
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, sampleRate * 2, true);
    view.setUint16(32, 2, true);
    view.setUint16(34, 16, true);
    writeText(36, "data");
    view.setUint32(40, dataBytes, true);
    for (let index = 0; index < samples.length; index += 1) {
      const sample = Math.max(-1, Math.min(1, samples[index]));
      view.setInt16(44 + index * 2, Math.round(sample * 32767), true);
    }
    return new Uint8Array(buffer);
  }

  function downloadWav() {
    const blob = new Blob([wavBytesFromNotes()], { type: "audio/wav" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "classical-daw-sketch.wav";
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 500);
    setStatus("已导出 WAV（48 kHz / PCM16 单声道诊断音色）");
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
    const tempo = clamp(Number(dom.tempo.value) || 96, 30, 240);
    const meter = timeSignature;
    const measureLength = measureBeats(meter);
    const totalBeats = visibleBeats(meter);
    const voiceByNoteId = new Map();
    const globalLanes = [];
    sorted.forEach((note) => {
      const start = Number(note.start);
      const end = start + Number(note.duration);
      let lane = globalLanes.find((candidate) => candidate.lastStart === start
        || candidate.lastEnd <= start + 0.001);
      if (!lane) {
        lane = { lastStart: -1, lastEnd: 0 };
        globalLanes.push(lane);
      }
      lane.lastStart = start;
      lane.lastEnd = Math.max(lane.lastEnd, end);
      voiceByNoteId.set(note.id, globalLanes.indexOf(lane) + 1);
    });
    const measures = [];
    for (let measureIndex = 0; measureIndex < measureCount; measureIndex += 1) {
      const measureStart = measureIndex * measureLength;
      const measureEnd = Math.min(totalBeats, measureStart + measureLength);
      const measureNotes = sorted
        .filter((note) => Number(note.start) < measureEnd && Number(note.start) + Number(note.duration) > measureStart)
        .map((note) => ({
          note,
          start: Math.max(measureStart, Number(note.start)),
          end: Math.min(measureEnd, Number(note.start) + Number(note.duration))
        }));
      // MusicXML represents independent timelines as voices. Greedy lane
      // assignment keeps overlapping notes from advancing one shared cursor;
      // notes with the same onset stay in one lane and become a chord.
      const laneMap = new Map();
      measureNotes.forEach((segment) => {
        const voice = voiceByNoteId.get(segment.note.id) || 1;
        let lane = laneMap.get(voice);
        if (!lane) {
          lane = { voice, segments: [] };
          laneMap.set(voice, lane);
        }
        lane.segments.push(segment);
      });
      const lanes = laneMap.size
        ? Array.from(laneMap.values()).sort((a, b) => a.voice - b.voice)
        : [{ voice: 1, segments: [] }];
      const lines = [];
      lanes.forEach((lane, laneIndex) => {
        const voice = lane.voice;
        const segments = lane.segments.sort((a, b) => a.start - b.start || a.note.midi - b.note.midi);
        let cursor = measureStart;
        let index = 0;
        const emitRest = (duration) => {
          if (duration <= 0) return;
          const ticks = Math.max(1, Math.round(duration * divisions));
          lines.push(`      <note><rest/><duration>${ticks}</duration><voice>${voice}</voice><type>${typeForDuration(duration)}</type></note>`);
        };
        while (index < segments.length) {
          const start = segments[index].start;
          if (start > cursor) emitRest(start - cursor);
          const group = segments.filter((segment) => Math.abs(segment.start - start) < 0.001);
          group.forEach((segment, groupIndex) => {
            const note = segment.note;
            const duration = Math.max(0.25, segment.end - segment.start);
            const ticks = Math.max(1, Math.round(duration * divisions));
            const tieTypes = [];
            if (Number(note.start) < measureStart) tieTypes.push("stop");
            if (Number(note.start) + Number(note.duration) > measureEnd) tieTypes.push("start");
            const tieXml = tieTypes.map((type) => `<tie type="${type}"/>`).join("");
            const notationXml = tieTypes.length
              ? `<notations>${tieTypes.map((type) => `<tied type="${type}"/>`).join("")}</notations>` : "";
            const lyricXml = note.lyric && segment.start === Number(note.start)
              ? `<lyric><text>${xmlEscape(note.lyric)}</text></lyric>` : "";
            const dynamics = clamp(Math.round(note.velocity), 1, 127);
            const dynamicMark = dynamics < 32 ? "pp" : dynamics < 48 ? "p" : dynamics < 64 ? "mp"
              : dynamics < 80 ? "mf" : dynamics < 96 ? "f" : dynamics < 112 ? "ff" : "fff";
            const velocityDirection = `<direction placement="below"><direction-type><dynamics><${dynamicMark}/></dynamics></direction-type><sound dynamics="${dynamics}"/></direction>`;
            lines.push(`      ${velocityDirection}\n      <note>${groupIndex ? "<chord/>" : ""}${pitchXml(note.midi)}<duration>${ticks}</duration>${tieXml}<voice>${voice}</voice><type>${typeForDuration(duration)}</type>${notationXml}${lyricXml}</note>`);
          });
          cursor = Math.max(cursor, ...group.map((segment) => segment.end));
          index += group.length;
        }
        if (cursor < measureEnd) emitRest(measureEnd - cursor);
        if (laneIndex < lanes.length - 1) {
          const ticks = Math.max(1, Math.round((measureEnd - measureStart) * divisions));
          lines.push(`      <backup><duration>${ticks}</duration></backup>`);
        }
      });
      const attributes = measureIndex === 0
        ? `      <attributes><divisions>${divisions}</divisions><key><fifths>0</fifths><mode>major</mode></key><time><beats>${meter.numerator}</beats><beat-type>${meter.denominator}</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>\n      <direction placement="above"><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>${tempo}</per-minute></metronome></direction-type><sound tempo="${tempo}"/></direction>\n`
        : "";
      measures.push(`    <measure number="${measureIndex + 1}">\n${attributes}${lines.join("\n")}\n    </measure>`);
    }
    return `<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<score-partwise version="4.0">
  <work><work-title>Classical DAW Web Sketch</work-title></work>
  <identification><creator type="composer">Classical DAW Web Prototype</creator><encoding><software>Classical DAW Web Prototype</software></encoding></identification>
  <part-list><score-part id="P1"><part-name>Piano</part-name></score-part></part-list>
  <part id="P1">
${measures.join("\n")}
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
      timeSignature: { ...timeSignature },
      measures: measureCount,
      notes: cloneNotes(notes),
      selectedId,
      nextId
    };
    return `${JSON.stringify(project, null, 2)}\n`;
  }

  function isQuarterGridValue(value) {
    return Math.abs(value * 4 - Math.round(value * 4)) < 1e-6;
  }

  function parseProjectMeter(source) {
    if (source === undefined) return { ...DEFAULT_METER };
    if (!source || typeof source !== "object" || Array.isArray(source)) {
      throw new Error("timeSignature 必须是对象");
    }
    const { numerator, denominator } = source;
    if (!Number.isInteger(numerator) || !Number.isInteger(denominator) || !isAllowedMeter(numerator, denominator)) {
      throw new Error("timeSignature 只支持 2/4、3/4、4/4、6/8");
    }
    return { numerator, denominator };
  }

  function parseProjectMeasures(source) {
    if (source === undefined) return DEFAULT_MEASURES;
    if (!Number.isInteger(source) || !isAllowedMeasureCount(source)) {
      throw new Error("measures 只支持 4、8、16");
    }
    return source;
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
    const importedMeter = parseProjectMeter(documentNode.timeSignature);
    const importedMeasures = parseProjectMeasures(documentNode.measures);
    const importedTotalBeats = visibleBeats(importedMeter, importedMeasures);

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
      if (typeof start !== "number" || !Number.isFinite(start) || start < 0 || start > importedTotalBeats - 0.25 || !isQuarterGridValue(start)) {
        throw new Error(`第 ${index + 1} 个音符的 start 无效`);
      }
      if (typeof duration !== "number" || !Number.isFinite(duration) || duration < 0.25 || duration > importedTotalBeats - start || !isQuarterGridValue(duration)) {
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
      tempo,
      timeSignature: importedMeter,
      measureCount: importedMeasures
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
        restoreState({ notes, nextId, selectedId, tempo: String(project.tempo), timeSignature: project.timeSignature, measureCount: project.measureCount });
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
    const start = clamp(Math.floor((event.clientX - rect.left) / beatWidth * 4) / 4, 0, visibleBeats() - 0.25);
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
  dom.meterNumerator.addEventListener("change", changeTimeSignature);
  dom.meterDenominator.addEventListener("change", changeTimeSignature);
  dom.measureCount.addEventListener("change", changeMeasureCount);
  dom.importButton.addEventListener("click", () => dom.musicXmlFile.click());
  dom.musicXmlFile.addEventListener("change", () => {
    const [file] = dom.musicXmlFile.files || [];
    if (file) importMusicXmlFile(file);
  });
  dom.exportButton.addEventListener("click", downloadMusicXml);
  dom.importMidiButton.addEventListener("click", () => dom.midiFile.click());
  dom.midiFile.addEventListener("change", () => {
    const [file] = dom.midiFile.files || [];
    if (file) importMidiFile(file);
  });
  dom.exportMidiButton.addEventListener("click", downloadMidi);
  dom.exportWavButton.addEventListener("click", downloadWav);
  dom.saveProject.addEventListener("click", downloadProject);
  dom.openProject.addEventListener("click", () => dom.projectFile.click());
  dom.projectFile.addEventListener("change", () => {
    const [file] = dom.projectFile.files || [];
    if (file) importProjectFile(file);
  });
  dom.pitch.addEventListener("change", () => commitMutation(() => updateSelected({ midi: Number(dom.pitch.value) })));
  dom.start.addEventListener("change", () => commitMutation(() => updateSelected({ start: Number(dom.start.value) })));
  dom.duration.addEventListener("change", () => commitMutation(() => updateSelected({ duration: Number(dom.duration.value) })));
  dom.tempo.addEventListener("focus", beginLiveHistory);
  dom.tempo.addEventListener("input", () => {
    dom.tempo.value = String(clamp(Number(dom.tempo.value) || 96, 30, 240));
    scheduleRecoverySave();
  });
  dom.tempo.addEventListener("change", () => {
    dom.tempo.value = String(clamp(Number(dom.tempo.value) || 96, 30, 240));
    finishLiveHistory();
  });
  dom.velocity.addEventListener("focus", beginLiveHistory);
  dom.velocity.addEventListener("input", () => {
    dom.velocityValue.value = dom.velocity.value;
    dom.velocityValue.textContent = dom.velocity.value;
    updateSelected({ velocity: Number(dom.velocity.value) });
    scheduleRecoverySave();
  });
  dom.lyric.addEventListener("focus", beginLiveHistory);
  dom.lyric.addEventListener("input", () => {
    const note = notes.find((item) => item.id === selectedId);
    if (!note) return;
    note.lyric = dom.lyric.value;
    renderRoll();
    scheduleRecoverySave();
  });
  dom.velocity.addEventListener("change", finishLiveHistory);
  dom.lyric.addEventListener("change", finishLiveHistory);
  dom.roll.addEventListener("keydown", (event) => {
    if (event.key === "Delete" || event.key === "Backspace") { event.preventDefault(); deleteSelected(); }
    if (event.key === " ") { event.preventDefault(); dom.play.click(); }
  });
  document.addEventListener("keydown", (event) => {
    const modifier = event.metaKey || event.ctrlKey;
    const editingField = event.target.matches("input, select, textarea");
    if (modifier && !event.altKey && event.key.toLowerCase() === "c" && !editingField) {
      event.preventDefault();
      copySelected();
      return;
    }
    if (modifier && !event.altKey && event.key.toLowerCase() === "v" && !editingField) {
      event.preventDefault();
      pasteNote();
      return;
    }
    if (modifier && !event.altKey && event.key.toLowerCase() === "z") {
      event.preventDefault();
      if (event.shiftKey) redo();
      else undo();
      return;
    }
    if (editingField) return;
    if (event.key === "ArrowLeft") { event.preventDefault(); moveSelectedBy(-0.25, 0); return; }
    if (event.key === "ArrowRight") { event.preventDefault(); moveSelectedBy(0.25, 0); return; }
    if (event.key === "ArrowUp") { event.preventDefault(); moveSelectedBy(0, 1); return; }
    if (event.key === "ArrowDown") { event.preventDefault(); moveSelectedBy(0, -1); return; }
    if (event.key === "Delete" || event.key === "Backspace") deleteSelected();
    if (event.key === " ") { event.preventDefault(); dom.play.click(); }
  });

  renderPitchLabels();
  syncMeterControls();
  renderBeatLabels();
  populatePitchOptions();
  renderRoll();
  updateHistoryButtons();
  showRecoveryIfPresent();
})();
