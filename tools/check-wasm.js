// Verify the built WASM module against what the page actually touches.
//
// This exists because a page shipped broken once, and the reason is worth remembering:
// emscripten 3.1.6 attaches HEAPF32/HEAPU8 to the Module by default, newer versions do not
// unless you export them. Local builds used 3.1.6, CI used `latest`, so the artifact that
// got published was never the artifact that got tested, and `M.HEAPF32.set(...)` threw on
// every audio callback while the page looked perfectly fine.
//
// Testing this locally is not enough — a toolchain that auto-exports will pass whether or
// not the export line is there. This has to run in CI against the artifact CI just built.
//
// Usage: node tools/check-wasm.js web/uchat.js web/uchat.wasm

const fs = require('fs');
const path = require('path');

const jsPath = path.resolve(process.argv[2] || 'web/uchat.js');
const wasmPath = path.resolve(process.argv[3] || 'web/uchat.wasm');

// Every symbol web/mesh.html dereferences. If one is missing the page dies at runtime,
// silently, inside an audio callback where nothing surfaces to the user.
const REQUIRED = [
  'HEAPF32',        // push(): M.HEAPF32.set(samples, ptr >> 2)
  'HEAPU8',         // modulate()/pop(): raw byte payloads
  'HEAP32',
  'getValue',       // reading the out-length from modulate()
  'UTF8ToString',
  'stringToUTF8',
  '_uc_modulate',
  '_uc_push',
  '_uc_pop',
  '_uc_last_score',
  '_uc_band_energy',
  '_uc_sample_rate',
  '_uc_max_payload',
  '_uc_n_tones',
  '_uc_tone_hz',
  '_malloc',
  '_free',
];

const UChatDSP = require(jsPath);

UChatDSP({ wasmBinary: fs.readFileSync(wasmPath) }).then(M => {
  let missing = 0;
  for (const k of REQUIRED) {
    const ok = M[k] !== undefined;
    if (!ok) missing++;
    console.log(`  ${ok ? 'ok  ' : 'MISS'}  ${k}`);
  }
  if (missing) {
    console.error(`\nFAIL: ${missing} symbol(s) the page needs are not on the Module.`);
    console.error('The page would load, join, and then throw on every audio callback.');
    process.exit(1);
  }

  // Exports existing is not the same as the DSP working. Round-trip a real frame through
  // the actual byte API the page uses, so a build that links but cannot decode also fails.
  const enc = new TextEncoder(), dec = new TextDecoder();
  const payload = enc.encode('³Mambeshci round trip');   // includes a byte > 127
  const lp = M._malloc(4), dp = M._malloc(payload.length);
  M.HEAPU8.set(payload, dp);
  const ptr = M._uc_modulate(dp, payload.length, lp);
  const n = M.getValue(lp, 'i32');
  const burst = new Float32Array(M.HEAPF32.buffer, ptr, n).slice();
  M._free(lp); M._free(dp);

  const all = new Float32Array(3000 + burst.length + 9000);
  all.set(burst, 3000);
  const pp = M._malloc(2048 * 4);
  let got = null;
  for (let i = 0; i < all.length; i += 2048) {
    const ch = all.subarray(i, Math.min(i + 2048, all.length));
    M.HEAPF32.set(ch, pp >> 2);
    M._uc_push(pp, ch.length);
    const cap = 512, q = M._malloc(cap);
    const r = M._uc_pop(q, cap);
    if (r >= 0) got = new Uint8Array(M.HEAPU8.buffer, q, r).slice();
    M._free(q);
  }

  const same = got && got.length === payload.length && got.every((b, i) => b === payload[i]);
  console.log(`\n  round trip: ${same ? 'decoded byte-for-byte' : 'FAILED'}`);
  if (!same) {
    console.error('FAIL: the module exports everything but cannot decode its own output.');
    process.exit(1);
  }
  console.log('\nPASS: every symbol present, and the DSP round-trips through the byte API.');
  process.exit(0);
}).catch(e => {
  console.error('FAIL: module would not even load:', e.message);
  process.exit(1);
});
