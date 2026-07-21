// End-to-end browser test for web/mesh.html in real Chromium (Playwright).
//
// This exercises the user path no headless-DSP test can reach: the actual page loads, the
// AudioWorklet starts, the WASM demodulator runs inside the browser, and the UI updates —
// everything up to real speaker→air→mic acoustics, which is the on-device test only a phone
// in a room can do.
//
// The trick for the one hop we can't do for real: Chromium's fake audio device is fed a WAV
// we generate from the SAME WASM modulate() the app uses, carrying a real app-layer frame.
// So the microphone "hears" a genuine ultrasonic burst, and a green/"them" bubble appearing
// is proof the browser decode chain (worklet → push → pop → unpack → render) works.
//
// Run:  node --no-experimental-fetch tests/browser.mjs
// (fetch is disabled so the in-node WASM load that builds the WAV falls back to fs; Playwright
//  drives Chromium over its own transport and does not need global fetch.)

import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const WEB = path.join(__dirname, '..', 'web');
const require = createRequire(import.meta.url);

let T = 0, F = 0;
const ok  = (n) => { T++; console.log(`  \x1b[32m[PASS]\x1b[0m ${n}`); };
const bad = (n, why) => { T++; F++; console.log(`  \x1b[31m[FAIL]\x1b[0m ${n} — ${why}`); };

// ── build a WAV that carries a decodable frame, using the app's own modulator ──
async function genWav(wavPath) {
  const UChatDSP = require(path.join(WEB, 'uchat.js'));
  // Hand the module the .wasm bytes directly so the emscripten glue never calls fetch() —
  // Node 18's global fetch (which Playwright itself needs) chokes on a filesystem path.
  const wasmBinary = fs.readFileSync(path.join(WEB, 'uchat.wasm'));
  const M = await UChatDSP({ wasmBinary });
  const hashByte = s => { let h = 5381; for (const c of s) h = ((h * 33) ^ c.charCodeAt(0)) & 255; return h; };
  const enc = new TextEncoder();
  const room = 'general', SENDER_ID = 123, nick = 'tester', text = 'hello over fake air';
  const n = enc.encode(nick), t = enc.encode(text);
  const frame = new Uint8Array(3 + n.length + t.length);
  frame[0] = hashByte(room); frame[1] = SENDER_ID; frame[2] = n.length;
  frame.set(n, 3); frame.set(t, 3 + n.length);

  const lenPtr = M._malloc(4), dPtr = M._malloc(frame.length);
  M.HEAPU8.set(frame, dPtr);
  const ptr = M._uc_modulate(dPtr, frame.length, lenPtr);
  const nSamp = M.getValue(lenPtr, 'i32');
  const burst = new Float32Array(M.HEAPF32.buffer, ptr, nSamp).slice();
  M._free(lenPtr); M._free(dPtr);

  // silence · burst · silence, so the looped file gives the demod clean gaps to resync in.
  const FS = M._uc_sample_rate(), pad = FS / 2;                 // 0.5 s
  const total = pad + burst.length + pad;
  const pcm = Buffer.alloc(44 + total * 2);
  // WAV header (PCM 16-bit mono @ FS)
  pcm.write('RIFF', 0); pcm.writeUInt32LE(36 + total * 2, 4); pcm.write('WAVE', 8);
  pcm.write('fmt ', 12); pcm.writeUInt32LE(16, 16); pcm.writeUInt16LE(1, 20);
  pcm.writeUInt16LE(1, 22); pcm.writeUInt32LE(FS, 24); pcm.writeUInt32LE(FS * 2, 28);
  pcm.writeUInt16LE(2, 32); pcm.writeUInt16LE(16, 34);
  pcm.write('data', 36); pcm.writeUInt32LE(total * 2, 40);
  for (let i = 0; i < burst.length; i++) {
    const s = Math.max(-1, Math.min(1, burst[i] * 0.9));
    pcm.writeInt16LE((s * 32767) | 0, 44 + (pad + i) * 2);
  }
  fs.writeFileSync(wavPath, pcm);
  return { text, nick, room };
}

// ── tiny static server with correct MIME (wasm streaming + worklet module need it) ──
// Also stands in for the native host so index.html can be tested: /events is an SSE stream
// and /send accepts a POST, exactly the two endpoints http.cpp exposes. The tx/rx pair is
// emitted only on the FIRST /events connection, so after a reload any "server hello" bubble
// can only have come from restored history — which is what the persistence test checks.
function startServer(dir) {
  const MIME = { '.html':'text/html', '.js':'text/javascript', '.wasm':'application/wasm',
                 '.json':'application/json', '.svg':'image/svg+xml' };
  let emitted = false;
  const srv = http.createServer((req, res) => {
    const p = decodeURIComponent(req.url.split('?')[0]);
    if (p === '/events') {
      res.writeHead(200, { 'Content-Type':'text/event-stream', 'Cache-Control':'no-store', 'Connection':'keep-alive' });
      const send = o => res.write(`data: ${JSON.stringify(o)}\n\n`);
      if (!emitted) {
        emitted = true;
        send({ type:'tx', text:'server hello', dur:1.6 });
        setTimeout(() => send({ type:'rx', text:'server hello', score:0.71 }), 300);
      }
      return;                                        // stays open
    }
    if (p === '/send') { res.writeHead(200, { 'Content-Type':'application/json' }); return res.end('{"ok":true}'); }
    const rel = p === '/' ? '/mesh.html' : p;
    const full = path.join(dir, rel);
    if (!full.startsWith(dir) || !fs.existsSync(full)) { res.writeHead(404); return res.end('no'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(full)] || 'application/octet-stream' });
    fs.createReadStream(full).pipe(res);
  });
  return new Promise(r => srv.listen(0, '127.0.0.1', () => r({ url: `http://127.0.0.1:${srv.address().port}`, close: () => srv.close() })));
}

async function main() {
  const wav = path.join(__dirname, 'fake-mic.wav');
  const { text: SENT } = await genWav(wav);
  const server = await startServer(WEB);

  const browser = await chromium.launch({
    args: [
      '--no-sandbox',
      '--use-fake-device-for-media-stream',        // synthesize the mic instead of real hardware
      '--use-fake-ui-for-media-stream',            // auto-accept the mic prompt (grantPermissions alone isn't enough here)
      `--use-file-for-fake-audio-capture=${wav}`,  // ...and feed it our modulated frame on a loop
      '--autoplay-policy=no-user-gesture-required',
    ],
  });
  const context = await browser.newContext({ permissions: ['microphone'] });
  const page = await context.newPage();

  // Any uncaught page error is an automatic failure — this is how a syntax slip in mesh.html
  // or a broken WASM shim gets caught.
  const pageErrors = [];
  page.on('pageerror', e => pageErrors.push(String(e)));

  try {
    console.log('\nBrowser E2E (real Chromium, fake ultrasonic mic):\n');

    await page.goto(`${server.url}/mesh.html`, { waitUntil: 'load' });
    ok(`page loads (title "${await page.title()}")`);

    // Join the room.
    await page.fill('#room', 'general');
    await page.fill('#nick', 'me');
    await page.click('#join');

    // Gate must give way to the live UI, and the spec line must show the real DSP params
    // read back from WASM — proof the module initialised in the browser.
    await page.waitForSelector('#hdr:not([hidden])', { timeout: 10000 });
    ok('joined — header/log/footer revealed');
    const spec = await page.textContent('#spec');
    if (/18250.*20000.*48000/.test(spec.replace(/\s+/g, ' '))) ok(`WASM DSP live in browser (${spec.trim()})`);
    else bad('WASM DSP spec', `unexpected spec "${spec}"`);

    // Calibration must complete (proves the worklet is delivering mic audio to band_energy).
    await page.waitForFunction(
      () => [...document.querySelectorAll('.sys .bub')].some(n => /noise floor/.test(n.textContent)),
      { timeout: 10000 });
    ok('AudioWorklet capture running — noise floor measured');

    // THE decode test: the fake mic is playing our modulated frame on a loop. A "them"
    // bubble with the sent text appearing is end-to-end proof of the browser decode chain.
    try {
      await page.waitForFunction(
        (want) => [...document.querySelectorAll('.row.them .bub')].some(n => n.textContent.includes(want)),
        SENT, { timeout: 20000 });
      ok(`decoded a frame from the (fake) air → "them" bubble: "${SENT}"`);
    } catch {
      bad('browser decode of fake-mic frame',
          'no "them" bubble in 20s — Chromium fake-audio may resample the 18–20 kHz band. ' +
          'DSP itself is proven by tests/dsp-loopback.cjs; this hop needs real hardware.');
    }

    // Focus gate: drive the page's real visibility handler — override visibilityState and
    // fire the same visibilitychange event the browser fires on a tab switch, then assert
    // the app's own activeNow()/setCapture() reacts. Capture must pause.
    await page.evaluate(() => {
      Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' });
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => true });
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForFunction(
      () => /mic paused/.test(document.getElementById('status').textContent), { timeout: 4000 })
      .then(() => ok('mic pauses when window is hidden (focus gate)'))
      .catch(async () => bad('focus gate pause', `status was "${await page.textContent('#status')}"`));
    await page.evaluate(() => {
      Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'visible' });
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => false });
      document.hasFocus = () => true;
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForFunction(
      () => /listening|channel busy|transmitting/.test(document.getElementById('status').textContent), { timeout: 5000 })
      .then(() => ok('mic resumes when window is visible again'))
      .catch(async () => bad('focus gate resume', `status stuck at "${await page.textContent('#status')}"`));

    // Activity log: send a message, then reload and confirm it was persisted and restored.
    await page.fill('#i', 'persist me across reload');
    await page.click('#b');
    await page.waitForFunction(
      () => [...document.querySelectorAll('.row.me .bub')].some(n => n.textContent.includes('persist me across reload')),
      { timeout: 5000 });
    ok('sent message renders as own bubble');

    await page.reload({ waitUntil: 'load' });
    await page.fill('#room', 'general');
    await page.fill('#nick', 'me');
    await page.click('#join');
    await page.waitForFunction(
      () => [...document.querySelectorAll('.row.me .bub')].some(n => n.textContent.includes('persist me across reload')),
      { timeout: 10000 })
      .then(() => ok('activity log persisted across reload and restored on rejoin'))
      .catch(() => bad('history persistence', 'restored log did not contain the earlier message'));

    // Export downloads a transcript containing the message.
    const [dl] = await Promise.all([
      page.waitForEvent('download', { timeout: 5000 }),
      page.click('#export'),
    ]);
    const dlPath = await dl.path();
    const body = fs.readFileSync(dlPath, 'utf8');
    if (body.includes('persist me across reload')) ok(`export produced a transcript (${dl.suggestedFilename()})`);
    else bad('export transcript', 'downloaded file did not contain the message');

    // ── index.html (native-host page) against the mock SSE host ──────────────────
    console.log('\n  — index.html (SSE native-host page) —');
    const p2 = await context.newPage();
    p2.on('pageerror', e => pageErrors.push('index.html: ' + e));
    await p2.goto(`${server.url}/index.html`, { waitUntil: 'load' });
    await p2.waitForFunction(() => /mic live/.test(document.getElementById('status').textContent), { timeout: 5000 })
      .then(() => ok('index.html connects to the SSE stream'))
      .catch(async () => bad('index.html SSE', `status "${await p2.textContent('#status')}"`));
    await p2.waitForFunction(() => [...document.querySelectorAll('.row.tx .bub')].some(n => n.textContent.includes('server hello')), { timeout: 5000 });
    await p2.waitForFunction(() => [...document.querySelectorAll('.row.rx .bub')].some(n => n.textContent.includes('server hello')), { timeout: 5000 });
    ok('index.html renders tx then rx (round-trip UI)');
    await p2.waitForFunction(() => [...document.querySelectorAll('.row.tx .meta')].some(n => /transmitted/.test(n.textContent)), { timeout: 5000 })
      .then(() => ok('index.html marks the tx bubble "transmitted" when its decode returns'))
      .catch(() => bad('index.html round-trip match', 'tx bubble never marked transmitted'));
    // Persistence: reload. The server won't re-emit, so a restored bubble proves it persisted.
    await p2.reload({ waitUntil: 'load' });
    await p2.waitForFunction(() => [...document.querySelectorAll('.row.tx .bub, .row.rx .bub')].some(n => n.textContent.includes('server hello')), { timeout: 5000 })
      .then(() => ok('index.html activity persisted across reload'))
      .catch(() => bad('index.html persistence', 'restored log did not contain the earlier traffic'));
    await p2.close();

    if (pageErrors.length) bad('no uncaught page errors', pageErrors.join(' | '));
    else ok('no uncaught JS errors on either page');

  } finally {
    await browser.close();
    server.close();
    try { fs.unlinkSync(wav); } catch {}
  }

  console.log(`\n${F === 0 ? '\x1b[32m✅ ALL PASSED' : '\x1b[31m❌ ' + F + ' FAILED'}\x1b[0m  (${T - F}/${T})\n`);
  process.exit(F === 0 ? 0 : 1);
}

main().catch(e => { console.error('browser harness error:', e); process.exit(2); });
