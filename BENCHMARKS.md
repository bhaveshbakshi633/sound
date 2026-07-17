# Benchmarks

Every number here was measured on one machine, on 2026-07-17. Nothing is estimated,
extrapolated, or carried over from documentation. Where a thing was not measured, it says so
rather than guessing.

**Test machine:** Ubuntu 24.04, Tiger Lake-LP `sof-hda-dsp`, internal speaker and internal
digital microphone array (`hw:0,6`, 48 kHz), PipeWire 1.0.5, g++ 13.3.0.

The speaker and the microphone are a few centimetres apart inside one chassis. That geometry
drives most of what follows, and none of it should be assumed to transfer to another machine.

---

## 1. DSP correctness (no hardware in the loop)

`make selftest && ./selftest` — modulator into demodulator, in process.

| Check | Result |
|---|---|
| All 8 tones land on exact Goertzel bin centres | pass |
| Tone orthogonality (on-tone vs worst off-tone) | **239 dB** |
| CRC-16/CCITT-FALSE check value for `"123456789"` | `0x29B1` (published value) |
| Clean round-trip | pass |
| Payload lengths 1, 2, 25, 29, 200 | all round-trip |
| Two frames in one stream | both decode (demodulator re-arms) |
| 300 × 0.5 s of pure noise | **0** spurious messages |

### AWGN

200 trials at each level, message `"the quick brown fox"`:

| Wideband SNR | Decoded | Corrupt-but-accepted |
|---|---|---|
| 20 dB | 200/200 | 0 |
| 10 dB | 200/200 | 0 |
| 6 dB | 200/200 | 0 |
| 3 dB | 200/200 | 0 |
| 0 dB | 200/200 | 0 |

Decoding at 0 dB *wideband* SNR is not remarkable and should not be quoted as if it were. It
is processing gain: the Goertzel integrates one 50 Hz bin out of a 24 kHz band, worth about
`10·log10(24000/50) ≈ 27 dB`. The wideband figure is not the number the detector sees.

**This suite does not discriminate between good and bad channel handling.** It was already
200/200 at 0 dB before Reed–Solomon existed, and stayed 200/200 after. Only the over-air
numbers below distinguish anything.

---

## 2. Reed–Solomon

`RS(k+8, k)` over GF(256), 2000 trials per error count, random payload lengths 5–64 bytes:

| Byte errors injected | Corrected | Rejected | Wrong-but-accepted |
|---|---|---|---|
| 0 | 2000 | 0 | **0** |
| 1 | 2000 | 0 | **0** |
| 2 | 2000 | 0 | **0** |
| 3 | 2000 | 0 | **0** |
| 4 | 2000 | 0 | **0** |
| 5 | 0 | 2000 | **0** |
| 6 | 0 | 2000 | **0** |

`RS_PARITY = 8` corrects up to 4 byte errors. That number was chosen from measurement, not
intuition: 75% frame success over ~46 symbols implies a per-symbol error rate near 0.8%, i.e.
roughly 0.5 byte errors per frame. Four is about 8× the observed mean.

---

## 3. Acoustic channel

### Stepped-tone sweep, speaker → air → microphone

Each tone is verified to peak in its own time slot, so these are real tones and not clipping
artifacts aliasing down. SNR is against a never-played control floor.

| Frequency | SNR vs control floor |
|---|---|
| 17.0 kHz | 24.3 dB |
| 18.0 kHz | **1.0 dB — dead** |
| 18.5 kHz | 15.6 dB |
| 19.0 kHz | 17.0 dB |
| 19.5 kHz | 10.1 dB |
| 20.0 kHz | 18.6 dB |
| 21.0 kHz | 13.0 dB |
| 22.0 kHz | 7.5 dB |

18.0 kHz sits in a comb-filter null: at 19 kHz the wavelength is 18 mm, so a path difference
of about 9 mm between the direct and desk-reflected paths cancels a tone outright. **This is
why the band starts at 18250 and not 18000.** The null moves when the geometry moves, which is
why data rides on eight tones rather than one.

### Per-tone level arriving over the air (tx gain 0.20)

| Tone | Frequency | Over the air | Clean-file reference |
|---|---|---|---|
| 0 | 18250 Hz | 0.294 | 0.140 |
| 1 | 18500 Hz | 0.224 | 0.140 |
| 2 | 18750 Hz | 0.330 | 0.140 |
| 3 | 19000 Hz | 0.358 | 0.140 |
| 4 | 19250 Hz | 0.369 | 0.140 |
| 5 | 19500 Hz | 0.174 | 0.140 |
| 6 | 19750 Hz | 0.202 | 0.140 |
| 7 | 20000 Hz | 0.262 | 0.140 |

No tone is dead over the air; the spread is about ±6 dB of tilt.

### Spectrum of the captured signal

0–100 Hz peaks near **60 dB**; the 18–20 kHz band sits near **9 dB**.

Consequence: **broadband RMS is a useless health metric on this link.** It measures 50 Hz mains
hum. Always measure band-limited.

### Fan noise — measured, and it does not matter

| Condition | In-band (18.25–20 kHz) ambient floor |
|---|---|
| Fan at 3042 RPM | −62.2 dB |
| Fan at 0 RPM | −63.7 dB |

**1.5 dB.** The CPU fan sits centimetres from the microphone array and is a compelling suspect;
it is not the problem. This is recorded because a great deal of time was spent on the theory
that it was.

---

## 4. End-to-end, native host

Messages posted through the running server, decoded back from the microphone. Same protocol
for both rows, so they are directly comparable.

| Build | tx gain 0.14 | tx gain 0.20 | tx gain 0.28 | Total |
|---|---|---|---|---|
| Without Reed–Solomon | 6/8 | 6/8 | 6/8 | **18/24 = 75%** |
| With Reed–Solomon | 8/8 | 7/8 | 8/8 | **23/24 = 96%** |

Across all 48 trials: **zero corrupted or wrong messages emitted.** CRC-16 rejected every bad
frame. The receiver never emits a maybe.

That the pre-RS rate was *identical* at all three gains is the finding that mattered: it ruled
out link budget and pointed at symbol errors, which is what RS then fixed.

### Latency

| Message | Burst | Round trip (send → decoded back) |
|---|---|---|
| `"final proof 1"` | 1.02 s | 1081 ms |
| `"final proof 2"` | 1.02 s | 1079 ms |
| `"hawa se message number 3"` | 1.60 s | 1661 ms |
| `"this one should arrive"` | 2.08 s | 2102 ms |

Round trip tracks burst length almost exactly. At 150 bps the air time dominates; acoustic
transit across a desk is microseconds and the demodulator adds one symbol of lag.

### Transmit gain

Louder is worse, and this is the single mistake that cost the most time. The speaker and mic
are centimetres apart, so a loud transmit saturates the microphone:

| Capture peak | Outcome |
|---|---|
| 0.89 | decodes |
| 1.00 (clipped) | fails outright |

Default `--gain 0.2`. The link was broken for most of development purely because the
transmitter ran at amplitude 0.7.

---

## 5. WASM vs native

The browser runs the same `dsp.cpp` / `rs.cpp` as the native binary, compiled with emscripten.
Verified by dumping raw float32 from both (`tool emitraw`) and comparing sample by sample:

| Metric | Value |
|---|---|
| Sample count | 74880 vs 74880 |
| Bit-identical samples | **72741 / 74880 (97.14%)** |
| Max absolute difference | **1.521e-11** |

1.5e-11 against a 0.7 amplitude signal is −215 dB. The residual is `-ffast-math` FMA on native
x86 versus WASM float contraction. There is one implementation and one test suite.

Build size: **18 KB JS + 38 KB WASM.**

---

## 6. Browser audio path

Headless Chromium (Brave 150), real microphone, page served over HTTPS.

### Constraints actually granted

| Requested | Granted |
|---|---|
| `echoCancellation: false` | false |
| `noiseSuppression: false` | false |
| `autoGainControl: false` | false |
| `voiceIsolation: false` | false |
| `sampleRate: 48000` | 48000 |

This matters more than it looks. Echo cancellation exists to subtract your own speaker output
from your microphone input, and here the signal **is** the speaker output. A browser that
ignored these would silently delete the entire link.

### Band response through `getUserMedia`

Against a never-played control floor:

| Frequency | SNR |
|---|---|
| 18250 Hz | 16.7 dB |
| 19000 Hz | 12.9 dB |
| 19500 Hz | 13.1 dB |
| 20000 Hz | 6.7 dB |
| 21000 Hz | −2.5 dB (unused) |

Caveat: capture peak was 0.9999 — **clipped** — because this machine applies a +20 dB hardware
microphone boost (`Dmic0 Capture Volume` = 70/70). The trend matches the native measurements,
but the absolute dB figures should be treated as soft.

### Browser mesh, one device

Page transmits via WebAudio and receives through its own microphone:

| Condition | Result |
|---|---|
| Speaker unmuted | `heard back · 2285 ms · purity 0.58` |
| **Speaker muted** | `lost — never heard back` |

Same code both runs. The only difference is whether the speaker physically made a sound. This
is the proof that the path is genuinely acoustic and not an internal or virtual loopback —
independently confirmed by checking that the default capture source resolves to
`api.alsa.path = hw:sofhdadsp,6`, the physical microphone.

Measured in-band noise floor at join: **1.81e-3**, so carrier sense trips above **7.25e-3**.
Each device measures its own floor rather than using a constant, because gain structure varies
enormously between machines.

---

## What is NOT measured

Stated plainly, because a benchmark page that omits this is advertising:

- **Phone speakers.** Whether a phone emits usable 18–20 kHz is untested. The commonly cited
  ~20 kHz ceiling for phone speakers is something read, not measured here.
- **iOS Safari.** Whether it grants `getUserMedia` over a self-signed certificate, and whether
  it honours `echoCancellation: false`, is untested. Chromium honouring them proves nothing
  about Safari.
- **Two real devices.** Every over-air number above is one machine's speaker to its own
  microphone. Device-to-device has not been run.
- **Distance, rooms, other hardware.** One laptop, one room, one afternoon.
- **Collisions in practice.** Carrier sense is implemented and the busy threshold is measured,
  but two devices actually colliding has never been observed.
- **The remaining 4%.** One frame in 24 still fails and has not been characterised.
- **The guard interval.** `DEMOD_SKIP`/`DEMOD_LEN` cost ~1 dB of integration and are kept on
  physical reasoning. Their benefit over the air has never been isolated.

---

## Reproducing

```sh
make selftest && ./selftest      # DSP, no hardware
make && ./uchat                  # native host, then open http://127.0.0.1:8080
make tool
./tool emit out.wav "msg" 0.2    # write the exact transmit waveform
./tool decode capture.wav        # run the real demodulator over a recording
./tool probe hw:0,6 3            # per-channel rms + in-band energy
```

`tool probe` reports **per channel** because the demodulator reads one channel of a microphone
array. If that channel is dead, everything downstream looks exactly like a DSP bug and is not
one.
