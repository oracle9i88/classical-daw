(() => {
  "use strict";

  // Keep exported MusicXML durations on the engine's canonical 960 PPQ grid.
  const PPQ = 960;
  const PROJECT_FORMAT = "classical-daw-web-project";
  const PROJECT_VERSION = 1;
  const RECOVERY_KEY = "classical-daw-web-recovery-v1";
  const RECOVERY_DELAY_MS = 900;
  const MEASURES = 4;
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
    add: document.querySelector("#add-note"),
    clear: document.querySelector("#clear"),
    saveProject: document.querySelector("#save-project"),
    openProject: document.querySelector("#open-project"),
    projectFile: document.querySelector("#project-file"),
    importButton: document.querySelector("#import"),
    musicXmlFile: document.querySelector("#musicxml-file"),
    remove: document.querySelector("#delete-note"),
    exportButton: document.querySelector("#export"),
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

  function measureBeats(meter = timeSignature) {
    return meter.numerator * 4 / meter.denominator;
  }

  function visibleBeats(meter = timeSignature) {
    return MEASURES * measureBeats(meter);
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
    dom.meterHint.textContent = `网格以四分音符为一拍，当前 ${text}，显示 ${MEASURES} 小节。`;
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
      timeSignature: { ...timeSignature }
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
      setStatus(`拍号变更失败：音符超出 ${meterText(nextMeter)} 的 4 小节范围`);
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
    let beatsPerMeasure = DEFAULT_METER.numerator;
    let beatType = DEFAULT_METER.denominator;
    let timeSignatureSeen = false;
    let measureOffset = 0;
    const imported = [];
    let skipped = 0;

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
          if (absoluteStart < visibleBeats({ numerator: beatsPerMeasure, denominator: beatType }) && absoluteStart + duration > 0) {
            const totalVisibleBeats = visibleBeats({ numerator: beatsPerMeasure, denominator: beatType });
            const start = Math.round(clamp(absoluteStart, 0, totalVisibleBeats - 0.25) * 4) / 4;
            const visibleDuration = Math.min(duration, totalVisibleBeats - start);
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
    return {
      notes: imported,
      tempo,
      skipped,
      timeSignature: { numerator: beatsPerMeasure, denominator: beatType }
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
    const totalBeats = visibleBeats();
    const lines = [];
    let cursor = 0;
    let index = 0;
    const emitRest = (duration) => {
      const ticks = Math.max(1, Math.round(duration * divisions));
      lines.push(`      <note><rest/><duration>${ticks}</duration><voice>1</voice><type>${typeForDuration(duration)}</type></note>`);
    };
    while (index < sorted.length) {
      const start = clamp(Number(sorted[index].start), 0, totalBeats);
      if (start > cursor) emitRest(start - cursor);
      const group = sorted.filter((note) => Math.abs(note.start - start) < 0.001);
      group.forEach((note, groupIndex) => {
        const duration = clamp(Number(note.duration), 0.25, totalBeats - start);
        const ticks = Math.max(1, Math.round(duration * divisions));
        const lyricXml = note.lyric ? `<lyric><text>${xmlEscape(note.lyric)}</text></lyric>` : "";
        lines.push(`      <note>${groupIndex ? "<chord/>" : ""}${pitchXml(note.midi)}<duration>${ticks}</duration><voice>1</voice><type>${typeForDuration(duration)}</type><velocity>${Math.round(note.velocity)}</velocity>${lyricXml}</note>`);
      });
      cursor = Math.max(cursor, ...group.map((note) => start + Number(note.duration)));
      index += group.length;
    }
    if (cursor < totalBeats) emitRest(totalBeats - cursor);
    const tempo = clamp(Number(dom.tempo.value) || 96, 30, 240);
    const meter = timeSignature;
    return `<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<score-partwise version="4.0">
  <work><work-title>Classical DAW Web Sketch</work-title></work>
  <identification><creator type="composer">Classical DAW Web Prototype</creator><encoding><software>Classical DAW Web Prototype</software></encoding></identification>
  <part-list><score-part id="P1"><part-name>Piano</part-name></score-part></part-list>
  <part id="P1">
    <measure number="1">
      <attributes><divisions>${divisions}</divisions><key><fifths>0</fifths><mode>major</mode></key><time><beats>${meter.numerator}</beats><beat-type>${meter.denominator}</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>
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
      timeSignature: { ...timeSignature },
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
    const importedTotalBeats = visibleBeats(importedMeter);

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
      timeSignature: importedMeter
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
        restoreState({ notes, nextId, selectedId, tempo: String(project.tempo), timeSignature: project.timeSignature });
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
  syncMeterControls();
  renderBeatLabels();
  populatePitchOptions();
  renderRoll();
  updateHistoryButtons();
  showRecoveryIfPresent();
})();
