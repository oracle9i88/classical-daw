import { clone, validateProject, parseProject } from './core.mjs';

// Serialize bounded snapshots. One stack orders note, controller and mix edits.
export class Editor {
  constructor(project) { this.project = validateProject(project); this.past = []; this.future = []; this.revision = 0; }
  trim(stack) {
    let bytes = stack.reduce((sum, s) => sum + s.length, 0);
    while (stack.length > 32 || (bytes > 4 * 1024 * 1024 && stack.length > 1)) bytes -= stack.shift().length;
  }
  replace(project) {
    const candidate = validateProject(project), before = JSON.stringify(this.project);
    if (before === JSON.stringify(candidate)) return false;
    this.past.push(before); this.trim(this.past); this.future = [];
    this.project = candidate; this.revision++; return true;
  }
  edit(fn) { const candidate = clone(this.project); fn(candidate); return this.replace(candidate); }
  undo() {
    if (!this.past.length) return false;
    this.future.push(JSON.stringify(this.project)); this.trim(this.future);
    this.project = JSON.parse(this.past.pop()); this.revision++; return true;
  }
  redo() {
    if (!this.future.length) return false;
    this.past.push(JSON.stringify(this.project)); this.trim(this.past);
    this.project = JSON.parse(this.future.pop()); this.revision++; return true;
  }
}

export const RECOVERY_PREFIX = 'classical-daw-web-recovery-v2:';
export const LEGACY_RECOVERY = 'classical-daw-web-recovery-v1';
export class Recovery {
  constructor(storage, sessionId) { this.storage = storage; this.key = RECOVERY_PREFIX + sessionId; }
  list() {
    const found = [];
    for (let i = 0; i < this.storage.length; i++) {
      const key = this.storage.key(i);
      if (key === this.key || (!key?.startsWith(RECOVERY_PREFIX) && key !== LEGACY_RECOVERY)) continue;
      try {
        const raw = this.storage.getItem(key);
        const record = key === LEGACY_RECOVERY ? { project: JSON.parse(raw), savedAt: 0 } : JSON.parse(raw);
        const project = parseProject(JSON.stringify(record.project));
        found.push({ key, project, savedAt: Number(record.savedAt) || 0 });
      } catch { found.push({ key, error: '这份副本损坏或版本不支持，可下载原始数据后再处理。', savedAt: 0 }); }
    }
    return found.sort((a, b) => b.savedAt - a.savedAt);
  }
  save(project) {
    // Each tab owns a distinct key. Unresolved older copies are never overwritten.
    this.storage.setItem(this.key, JSON.stringify({ savedAt: Date.now(), project: validateProject(project) }));
  }
  remove(key) {
    if (key !== LEGACY_RECOVERY && !key.startsWith(RECOVERY_PREFIX)) throw new Error('恢复副本键无效');
    this.storage.removeItem(key);
  }
}
