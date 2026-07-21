// Headless byte-level loopback for the browser mesh app-layer.
//
// This drives the EXACT same WASM dsp.cpp/rs.cpp the browser loads, through the EXACT same
// pack → modulate → push → pop → unpack path as web/mesh.html — minus the speaker→air→mic
// hop, which no CI box can reproduce and which is the user's on-device test. What it proves
// is everything up to the air: that a message a sender packs is the message a receiver
// unpacks, that binary-unsafe bytes survive, and that the frame format round-trips.
//
// Run:  node tests/dsp-loopback.cjs

const path = require('path');
const fs = require('fs');
const UChatDSP = require(path.join(__dirname, '..', 'web', 'uchat.js'));
// Feed the .wasm bytes straight in so the emscripten glue never calls fetch() — Node 18's
// global fetch treats the filesystem path as a URL and throws. Same fix the browser test uses.
const wasmBinary = fs.readFileSync(path.join(__dirname, '..', 'web', 'uchat.wasm'));

// ── app-layer frame, copied verbatim from web/mesh.html so the test fails if they drift ──
function hashByte(s) { let h = 5381; for (const c of s) h = ((h * 33) ^ c.charCodeAt(0)) & 255; return h; }
function pack(room, myId, nick, text) {
  const enc = new TextEncoder();
  const n = enc.encode(nick).slice(0, 24), t = enc.encode(text);
  const out = new Uint8Array(3 + n.length + t.length);
  out[0] = hashByte(room); out[1] = myId; out[2] = n.length;
  out.set(n, 3); out.set(t, 3 + n.length);
  return out;
}
function unpack(bytes) {
  if (bytes.length < 3) return null;
  const nl = bytes[2];
  if (bytes.length < 3 + nl) return null;
  const dec = new TextDecoder();
  return { room: bytes[0], id: bytes[1], nick: dec.decode(bytes.slice(3, 3 + nl)), text: dec.decode(bytes.slice(3 + nl)) };
}

let T = 0, F = 0;
const ok  = (name) => { T++; console.log(`  \x1b[32m[PASS]\x1b[0m ${name}`); };
const bad = (name, why) => { T++; F++; console.log(`  \x1b[31m[FAIL]\x1b[0m ${name} — ${why}`); };

function main(M) {
  const FS = M._uc_sample_rate();

  // WASM shims, copied from mesh.html.
  function modulate(bytes) {
    const lenPtr = M._malloc(4), dPtr = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, dPtr);
    const ptr = M._uc_modulate(dPtr, bytes.length, lenPtr);
    const n = M.getValue(lenPtr, 'i32');
    const out = new Float32Array(M.HEAPF32.buffer, ptr, n).slice();
    M._free(lenPtr); M._free(dPtr);
    return out;
  }
  let pushPtr = 0, pushCap = 0;
  function push(x) {
    if (x.length > pushCap) { if (pushPtr) M._free(pushPtr); pushCap = x.length; pushPtr = M._malloc(pushCap * 4); }
    M.HEAPF32.set(x, pushPtr >> 2);
    M._uc_push(pushPtr, x.length);
  }
  function pop() {
    const cap = 512, p = M._malloc(cap);
    const n = M._uc_pop(p, cap);
    if (n < 0) { M._free(p); return null; }
    const bytes = new Uint8Array(M.HEAPU8.buffer, p, n).slice();
    M._free(p);
    return bytes;
  }

  // Feed a burst through the demod the way an audio callback would: silence, burst, silence,
  // in ~2048-sample chunks. gain scales the burst (the app plays at TX_GAIN); noise adds AWGN.
  function feedAndPop(burst, { gain = 0.2, noise = 0, pad = 4096 } = {}) {
    const total = pad + burst.length + pad;
    const sig = new Float32Array(total);
    for (let i = 0; i < burst.length; i++) sig[pad + i] = burst[i] * gain;
    if (noise > 0) {
      // Box–Muller AWGN. Deterministic seed so a failure reproduces.
      let s = 12345;
      const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; };
      for (let i = 0; i < total; i++) {
        const u1 = Math.max(rnd(), 1e-9), u2 = rnd();
        sig[i] += noise * Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
      }
    }
    const CH = 2048;
    const got = [];
    for (let off = 0; off < total; off += CH) {
      push(sig.subarray(off, Math.min(off + CH, total)));
      let b; while ((b = pop()) !== null) got.push(b);
    }
    return got;
  }

  function roundTrip(name, { room, myId, nick, text, gain, noise }) {
    const frame = pack(room, myId, nick, text);
    if (frame.length > M._uc_max_payload()) return bad(name, `frame ${frame.length} > max ${M._uc_max_payload()}`);
    const bursts = feedAndPop(modulate(frame), { gain, noise });
    if (bursts.length === 0) return bad(name, 'nothing decoded');
    const f = unpack(bursts[0]);
    if (!f) return bad(name, 'unpack returned null');
    if (f.room !== hashByte(room)) return bad(name, `room ${f.room} != ${hashByte(room)}`);
    if (f.id !== myId) return bad(name, `id ${f.id} != ${myId}`);
    if (f.nick !== nick) return bad(name, `nick "${f.nick}" != "${nick}"`);
    if (f.text !== text) return bad(name, `text "${f.text}" != "${text}"`);
    ok(`${name}  ("${text}" · ${frame.length}B)`);
  }

  console.log('\nDSP byte-loopback (browser mesh app-layer, no air):');
  console.log(`  FS=${FS} maxPayload=${M._uc_max_payload()} nTones=${M._uc_n_tones()} ` +
              `band=${M._uc_tone_hz(0)}–${M._uc_tone_hz(M._uc_n_tones() - 1)}Hz\n`);

  // 1. plain ASCII
  roundTrip('ascii', { room: 'general', myId: 42, nick: 'ambesh', text: 'hawa se message' });
  // 2. room hash > 127 (the binary-safety trap: routing the frame through a JS string would
  //    corrupt any byte above 127). Find a room name that lands there rather than hard-code
  //    one, so the assertion can't rot if hashByte ever changes.
  let hi = null;
  for (let i = 0; i < 5000 && hi === null; i++) if (hashByte('room' + i) > 127) hi = 'room' + i;
  if (hi === null) bad('precondition: high room hash', 'no room name hashed >127 in 5000 tries');
  else { ok(`precondition: room "${hi}" hashes to ${hashByte(hi)} (>127, binary-unsafe path)`);
         roundTrip('high-byte room hash', { room: hi, myId: 200, nick: 'a', text: 'high room byte survives' }); }
  // 3. unicode text + nick (multi-byte UTF-8 through the raw-bytes path)
  roundTrip('unicode', { room: 'general', myId: 7, nick: 'भवेश', text: 'नमस्ते 🎵 sound' });
  // 4. id = 0 and id = 255 boundaries
  roundTrip('id=0', { room: 'r', myId: 0, nick: 'n', text: 'edge id zero' });
  roundTrip('id=255', { room: 'r', myId: 255, nick: 'n', text: 'edge id max' });
  // 5. empty nick, short text
  roundTrip('empty nick', { room: 'general', myId: 5, nick: '', text: 'hi' });
  // 6. near-max payload
  const room = 'general', nickLen = 3;
  const budget = M._uc_max_payload() - 3 - nickLen;
  roundTrip('near-max payload', { room, myId: 9, nick: 'max', text: 'x'.repeat(budget) });
  // 7. noise robustness — same message, AWGN added
  roundTrip('with AWGN (noise=0.01)', { room: 'general', myId: 11, nick: 'noisy', text: 'survives some noise', noise: 0.01 });

  console.log(`\n${F === 0 ? '\x1b[32m✅ ALL PASSED' : '\x1b[31m❌ ' + F + ' FAILED'}\x1b[0m  (${T - F}/${T})\n`);
  process.exit(F === 0 ? 0 : 1);
}

UChatDSP({ wasmBinary }).then(main).catch(e => { console.error('harness error:', e); process.exit(2); });
