#!/usr/bin/env node
// Browser UI regression; uses an ALREADY INSTALLED browser and never downloads
// one. AudioContext is a scheduling/copy mock, so this cannot play loudspeakers
// or certify perceived sound quality. The module Worker and PCM renderer are real.
// One extra smoke test uses native WebAudio with Chrome's --mute-audio flag;
// that verifies the actual browser API chain, never physical sound or listening.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import { dirname, resolve, extname, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createProject, validateProject, projectJson } from '../web/core.mjs';
import { midiFile, musicXml } from '../web/formats.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const web = resolve(root, 'web');
const output = resolve(process.env.WEB_BROWSER_OUTPUT || resolve(root, 'out/web-alpha-20261007'));
const require = createRequire(import.meta.url);
let playwright;
try { playwright = require(process.env.PLAYWRIGHT_MODULE_PATH || 'playwright'); }
catch { throw new Error('请设置 PLAYWRIGHT_MODULE_PATH 指向已安装的 playwright；此脚本不会安装或下载浏览器。'); }
const mime = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const server = createServer(async (req, res) => {
  try {
    let pathname = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
    if (pathname === '/classical-daw') { res.writeHead(302, { location: '/classical-daw/' }); res.end(); return; }
    if (pathname.startsWith('/classical-daw/')) pathname = pathname.slice('/classical-daw'.length);
    if (pathname.endsWith('/')) pathname += 'index.html';
    const file = resolve(web, '.' + pathname);
    if (!file.startsWith(web + sep) || !(await stat(file)).isFile()) throw new Error('not found');
    res.writeHead(200, { 'content-type': mime[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(await readFile(file));
  } catch { res.writeHead(404); res.end('Not found'); }
});
await mkdir(output, { recursive: true });
await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await playwright.chromium.launch({ headless: true,
  ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  args: ['--mute-audio'] });
const browserVersion = browser.version();
const results = [];
const cleanups = [];
async function check(name, run) {
  try { await run(); results.push({ name, passed: true }); console.log(`PASS ${name}`); }
  catch (error) { results.push({ name, passed: false, error: error.stack || error.message }); console.error(`FAIL ${name}\n${error.stack || error.message}`); }
}
function fixture() {
  const p = createProject();
  p.title = '浏览器长谱回归 · 多声部'; p.lengthBeats = 256;
  const note = (id, midi, start, duration, channel = 0) => ({ id, midi, start, duration, channel, velocity: 87, lyric: id === 1 ? '原词' : '' });
  p.tracks = [
    { id: 'piano', name: '钢琴', instrument: 'piano', gainDb: -6, pan: -.2, mute: false, solo: false,
      notes: [note(1, 0, 0, 1), note(2, 60, 3, 1), note(3, 127, 200, 2, 15)], controls: [] },
    { id: 'cello', name: '大提琴', instrument: 'strings', gainDb: -8, pan: .2, mute: false, solo: false,
      notes: [note(4, 36, 0, 8), note(5, 43, 190, 3)], controls: [{ beat: 0, controller: 11, value: 95, channel: 0 }] },
    { id: 'flute', name: '长笛草稿', instrument: 'sine', gainDb: -9, pan: 0, mute: false, solo: false,
      notes: [note(6, 84, 205, 2)], controls: [] },
  ];
  p.nextId = 7;
  return validateProject(p);
}
const original = fixture();
function musicalShape(project) {
  return { lengthBeats: project.lengthBeats,
    notes: project.tracks.map(track => track.notes.map(n => [n.midi, n.start, n.duration, n.channel, n.lyric]).sort((a, b) => a[1] - b[1] || a[0] - b[0])) };
}
async function createPage(base, options = {}, seed = null, nativeAudio = false) {
  const context = await browser.newContext({ viewport: { width: 1440, height: 1100 }, acceptDownloads: true, ...options });
  cleanups.push(() => context.close());
  await context.addInitScript(({ seed, nativeAudio }) => {
    if (seed && !localStorage.getItem('browser-test-seeded')) {
      localStorage.setItem(seed.key, seed.value); localStorage.setItem('browser-test-seeded', '1');
    }
    const audit = globalThis.__audioAudit = { starts: 0, stops: 0, workers: 0, terminated: 0, copies: 0 };
    class NoDeviceAudioContext {
      constructor() { this.destination = {}; this.sampleRate = 48000; }
      get currentTime() { return performance.now() / 1000; }
      async resume() {}
      createBuffer(channels, frames, sampleRate) {
        const copied = Array.from({ length: channels }, () => new Float32Array(frames));
        globalThis.__lastAudioBuffer = { channels, frames, sampleRate, copied };
        return { copyToChannel(data, channel) { copied[channel].set(data); ++audit.copies; }, length: frames, sampleRate };
      }
      createBufferSource() {
        return { connect() {}, disconnect() {}, start() { ++audit.starts; }, stop() { ++audit.stops; }, onended: null, buffer: null };
      }
    }
    if (nativeAudio) {
      // Observe the real instance without replacing resume, buffers, sources,
      // clocks or destination. --mute-audio is supplied by browser.launch.
      const NativeAudioContext = globalThis.AudioContext || globalThis.webkitAudioContext;
      globalThis.__realAudioContexts = [];
      globalThis.AudioContext = new Proxy(NativeAudioContext, {
        construct(Target, args) {
          const instance = Reflect.construct(Target, args);
          globalThis.__realAudioContexts.push(instance);
          return instance;
        },
      });
    } else {
      globalThis.AudioContext = NoDeviceAudioContext; globalThis.webkitAudioContext = NoDeviceAudioContext;
    }
    const NativeWorker = globalThis.Worker;
    globalThis.Worker = class extends NativeWorker {
      constructor(...args) { super(...args); ++audit.workers; }
      terminate() { ++audit.terminated; return super.terminate(); }
    };
  }, { seed, nativeAudio });
  const page = await context.newPage();
  const errors = []; page.on('pageerror', error => errors.push(error.message));
  page.on('dialog', dialog => dialog.accept());
  await page.goto(base, { waitUntil: 'networkidle' });
  await page.locator('#status').filter({ hasText: '准备就绪' }).waitFor();
  return { page, context, errors };
}
async function importBytes(page, name, contents, expectedTitle = original.title) {
  await page.locator('#file-input').setInputFiles({ name, mimeType: 'application/octet-stream', buffer: Buffer.from(contents) });
  await page.waitForFunction(title => document.getElementById('project-title').value === title, expectedTitle);
}
async function download(page, selector) {
  if (selector.startsWith('#export-')) await page.locator('.exports').evaluate(node => { node.open = true; });
  const promise = page.waitForEvent('download');
  await page.locator(selector).click();
  const item = await promise;
  return { name: item.suggestedFilename(), bytes: await readFile(await item.path()) };
}
async function snapshot(page) { return JSON.parse((await download(page, '#save-project')).bytes.toString('utf8')); }
async function change(page, selector, value) {
  await page.locator(selector).fill(String(value));
  await page.locator(selector).press('Tab');
}

try {
  for (const prefix of ['/', '/classical-daw/']) {
    const base = origin + prefix;
    await check(`${prefix} JSON/MIDI/MusicXML full multi-part UI import + round-trip downloads`, async () => {
      const { page, errors } = await createPage(base);
      const inputs = [['fixture.json', projectJson(original)], ['fixture.mid', midiFile(original)], ['fixture.musicxml', musicXml(original)]];
      for (const [name, bytes] of inputs) {
        await page.locator('#new-project').click();
        await importBytes(page, name, bytes);
        assert.equal(await page.locator('.track-row').count(), 3);
        const current = await snapshot(page);
        assert.deepEqual(musicalShape(current), musicalShape(original), name);
        assert.equal(await page.locator('#length-beats').inputValue(), '256');
        assert.match(await page.locator('#project-summary').textContent(), /3 声部 · 6 音符/);
      }
      const midi = await download(page, '#export-midi');
      const xml = await download(page, '#export-xml');
      const json = await download(page, '#save-project');
      for (const item of [midi, xml, json]) {
        await page.locator('#new-project').click(); await importBytes(page, item.name, item.bytes);
        assert.deepEqual(musicalShape(await snapshot(page)), musicalShape(original), `reopened ${item.name}`);
      }
      assert.deepEqual(errors, []);
    });
    await check(`${prefix} malformed import keeps document and delayed inspector edits undo/redo`, async () => {
      const { page, errors } = await createPage(base);
      await importBytes(page, 'fixture.json', projectJson(original));
      const before = await snapshot(page);
      for (const [name, buffer] of [['broken.json', '{oops'], ['broken.xml', '<score-partwise>'], ['broken.mid', 'not MIDI']]) {
        await page.locator('#file-input').setInputFiles({ name, mimeType: 'application/octet-stream', buffer: Buffer.from(buffer) });
        await page.locator('#status').filter({ hasText: '原工程保留' }).waitFor();
        assert.deepEqual(await snapshot(page), before);
      }
      await page.locator('.note[data-note-id="1"]').click();
      await page.locator('#note-lyric').focus(); await page.waitForTimeout(650);
      await change(page, '#note-lyric', '认真编辑');
      assert.equal((await snapshot(page)).tracks[0].notes[0].lyric, '认真编辑');
      await page.locator('#undo').click();
      assert.equal((await snapshot(page)).tracks[0].notes[0].lyric, '原词');
      await page.locator('#redo').click();
      assert.equal((await snapshot(page)).tracks[0].notes[0].lyric, '认真编辑');
      await page.locator('#note-start').focus(); await page.waitForTimeout(650);
      await change(page, '#note-start', .25);
      assert.equal((await snapshot(page)).tracks[0].notes[0].start, .25);
      await page.locator('#undo').click();
      assert.equal((await snapshot(page)).tracks[0].notes[0].start, 0);
      assert.deepEqual(errors, []);
    });
    await check(`${prefix} real worker playback scheduling / WAV / stop cancellation`, async () => {
      const { page, errors } = await createPage(base);
      await page.locator('#range-end').fill('4');
      await page.locator('#play').click();
      await page.waitForFunction(() => globalThis.__audioAudit.starts === 1);
      assert.equal((await page.evaluate(() => globalThis.__audioAudit)).copies, 2);
      const playback = await page.evaluate(() => ({ frames: globalThis.__lastAudioBuffer.frames, sampleRate: globalThis.__lastAudioBuffer.sampleRate,
        firstSamples: [Array.from(globalThis.__lastAudioBuffer.copied[0].slice(0, 100)), Array.from(globalThis.__lastAudioBuffer.copied[1].slice(0, 100))] }));
      await page.locator('#stop').click();
      assert.equal((await page.evaluate(() => globalThis.__audioAudit)).stops, 1);
      const wav = await download(page, '#export-wav'), view = new DataView(wav.bytes.buffer, wav.bytes.byteOffset, wav.bytes.byteLength);
      assert.equal(view.getUint32(24, true), playback.sampleRate); assert.equal(view.getUint32(40, true), playback.frames * 4);
      for (let f = 0; f < 100; ++f) for (let c = 0; c < 2; ++c) {
        const sample = playback.firstSamples[c][f];
        assert.equal(view.getInt16(44 + f * 4 + c * 2, true), Math.round(sample * (sample < 0 ? 32768 : 32767)));
      }
      const busy = createProject(); busy.title = '取消长运算'; busy.lengthBeats = 16; busy.masterGainDb = -12;
      busy.tracks[0].notes = Array.from({ length: 100 }, (_, i) => ({ id: i + 1, midi: 36 + i % 24, start: 0, duration: 16, velocity: 80, channel: 0, lyric: '' })); busy.nextId = 101;
      await importBytes(page, 'busy.json', projectJson(busy), busy.title);
      const starts = (await page.evaluate(() => globalThis.__audioAudit)).starts;
      await page.locator('#play').click(); await page.locator('#stop').click();
      await page.waitForTimeout(500);
      assert.equal((await page.evaluate(() => globalThis.__audioAudit)).starts, starts, 'cancelled worker later scheduled audio');
      assert.equal(await page.locator('#play').isEnabled(), true);
      assert.equal(await page.locator('#stop').isDisabled(), true);
      assert.match(await page.locator('#status').textContent(), /已停止/);
      assert.deepEqual(errors, []);
    });
    await check(`${prefix} keyboard produces one edit and ignores held-space auto-repeat`, async () => {
      const { page } = await createPage(base);
      await importBytes(page, 'fixture.json', projectJson(original));
      await page.locator('.note[data-note-id="2"]').click(); await page.locator('#roll').focus();
      await page.keyboard.press('ArrowRight');
      assert.equal((await snapshot(page)).tracks[0].notes[1].start, 3.25);
      await page.locator('#roll').focus(); await page.locator('#range-end').fill('4'); await page.locator('#roll').focus();
      await page.keyboard.press('Space');
      await page.waitForFunction(() => globalThis.__audioAudit.starts === 1);
      await page.locator('#roll').dispatchEvent('keydown', { key: ' ', code: 'Space', repeat: true, bubbles: true });
      assert.equal(await page.locator('#stop').isEnabled(), true, 'held-space repeat toggled playback off');
      assert.equal((await page.evaluate(() => globalThis.__audioAudit)).stops, 0);
      await page.locator('#stop').click();
    });
    await check(`${prefix} unresolved recovery survives new edits`, async () => {
      const key = 'classical-daw-web-recovery-v2:older-session';
      const value = JSON.stringify({ savedAt: 1730000000000, project: original });
      const { page } = await createPage(base, {}, { key, value });
      await page.locator('#recovery').waitFor({ state: 'visible' });
      await change(page, '#project-title', '新的编辑不覆盖旧副本'); await page.waitForTimeout(700);
      const result = await page.evaluate(key => ({ old: localStorage.getItem(key),
        keys: Object.keys(localStorage).filter(k => k.startsWith('classical-daw-web-recovery-v2:')) }), key);
      assert.equal(result.old, value); assert.equal(result.keys.length, 2);
      const row = page.locator('.recovery-item').filter({ hasText: original.title });
      const pending = page.waitForEvent('download'); await row.getByRole('button', { name: '下载副本' }).click();
      const saved = await pending;
      assert.deepEqual(musicalShape(JSON.parse(await readFile(await saved.path(), 'utf8'))), musicalShape(original));
    });
    await check(`${prefix} independent MusicXML browser regression page`, async () => {
      const context = await browser.newContext(); cleanups.push(() => context.close());
      const page = await context.newPage(); await page.goto(`${base}tests/fixtures/formats-browser.html`);
      await page.waitForFunction(() => globalThis.formatsTestResults);
      const report = await page.evaluate(() => globalThis.formatsTestResults);
      assert.equal(report.failed, 0, report.results.join('\n'));
      console.log(`  MusicXML browser tests ${report.passed}/${report.total}`);
    });
  }
  await check('native WebAudio resume/buffer/source playback and advancing clock (headless muted)', async () => {
    const { page, errors } = await createPage(origin + '/classical-daw/', {}, null, true);
    await page.locator('#play').click();
    await page.locator('#status').filter({ hasText: '正在试听' }).waitFor();
    const before = await page.evaluate(() => {
      const contexts = globalThis.__realAudioContexts;
      return { count: contexts.length, state: contexts[0]?.state, time: contexts[0]?.currentTime,
        constructor: contexts[0]?.constructor.name, sampleRate: contexts[0]?.sampleRate };
    });
    assert.equal(before.count, 1); assert.equal(before.state, 'running');
    assert.equal(before.constructor, 'AudioContext'); assert.ok(before.sampleRate > 0);
    await page.waitForTimeout(250);
    const after = await page.evaluate(() => globalThis.__realAudioContexts[0].currentTime);
    assert.ok(after > before.time, `native clock did not advance: ${before.time} -> ${after}`);
    await page.locator('#stop').click();
    assert.match(await page.locator('#status').textContent(), /已停止/);
    assert.equal(await page.locator('#stop').isDisabled(), true);
    assert.equal(await page.locator('#play').isEnabled(), true);
    await page.waitForTimeout(100); assert.deepEqual(errors, []);
    console.log(`  Native AudioContext running @ ${before.sampleRate} Hz, clock ${before.time} -> ${after}; stopped without page errors; --mute-audio`);
  });
  await check('desktop/mobile layout screenshots and viewport fit', async () => {
    const desktop = await createPage(origin + '/classical-daw/');
    await desktop.page.screenshot({ path: resolve(output, 'desktop.png'), fullPage: true });
    const mobile = await createPage(origin + '/classical-daw/', { viewport: { width: 390, height: 844 }, isMobile: true, deviceScaleFactor: 1 });
    assert.equal(await mobile.page.locator('#track-name').isVisible(), true, 'mobile track name is unreachable');
    assert.equal(await mobile.page.locator('#track-gain').isVisible(), true, 'mobile track gain is unreachable');
    assert.equal(await mobile.page.locator('#track-instrument').isVisible(), true, 'mobile instrument selector is unreachable');
    await mobile.page.screenshot({ path: resolve(output, 'mobile.png'), fullPage: true });
    const width = await mobile.page.evaluate(() => ({ viewport: innerWidth, document: document.documentElement.scrollWidth }));
    assert.ok(width.document <= width.viewport + 1, JSON.stringify(width));
  });
} finally {
  for (const close of cleanups.reverse()) await close().catch(() => {});
  await browser.close(); await new Promise(resolve => server.close(resolve));
}
const failed = results.filter(result => !result.passed);
const report = { browser: 'installed Chromium, headless', browserVersion,
  audio: 'Most cases use AudioContext scheduling mock; one native WebAudio smoke test uses --mute-audio; real module Worker renderer throughout; no listening sign-off',
  passed: results.length - failed.length, failed: failed.length, screenshotDirectory: output, results };
await writeFile(resolve(output, 'browser-report.json'), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify({ ...report, results: undefined }, null, 2));
if (failed.length) process.exitCode = 1;
