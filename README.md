# uchat

<p align="center">
  <img src="assets/demo.gif" alt="uchat demo — typing a message that travels as near-ultrasonic sound and decodes back on the same page" width="360">
</p>

Near-ultrasonic acoustic chat on a single Ubuntu 24.04 laptop.

Type a message into a browser chat box at `http://127.0.0.1:8080`. A C++ binary modulates it
into the 18.25–20 kHz band, plays it out of the laptop speaker via ALSA, captures it back
through the laptop's digital microphone, demodulates it, and pushes the decoded text back to
the same page over Server-Sent Events.

The only link between send and receive is air. Nothing crosses the network; the HTTP server
binds `127.0.0.1` only.

---

## Status — works end-to-end at 96%

Read this before anything else.

| Layer | Status |
|---|---|
| DSP (modulate → demodulate, no hardware) | **Working.** Self-test suite passes in full. |
| Speaker → air → mic link | **Working.** |
| Payload decode over the air | **Working at 96%** with Reed–Solomon FEC (23/24). |

A message typed into the browser chat box is modulated, played out the laptop speaker, picked
up by the laptop DMIC, demodulated, and pushed back to the same page. Verified through the
real HTTP path:

```
[tx] "hawa se message number 3"  (76800 samples, 1.60 s)
[rx] "hawa se message number 3"  (score 0.69, 1661 ms since tx)
```

**Measured success rate: 23/24 = 96%** with Reed–Solomon. Eight messages at each of three
transmit gains, through the running server: gain 0.14 → 8/8, gain 0.20 → 7/8, gain 0.28 → 8/8.

**Before RS it was 18/24 = 75%** (6/8 at every gain). That the pre-RS rate was identical at all
three gains is what identified the fix: transmit gain was not the bottleneck, symbol errors
were. With no FEC a single bad symbol failed the CRC and dropped the whole message. RS_PARITY=8
corrects up to 4 byte errors — see `src/rs.h` for why that number.

**Across all 48 trials (24 before RS, 24 after), zero corrupted or wrong messages were
emitted.** CRC-16 rejected every bad frame. The receiver never emits a maybe: a partially-correct frame produces nothing at
all. This is the one guarantee that held throughout development.

This is not a product. It is a working demonstration on one laptop, measured over 24 trials
in one room on one afternoon. 96% is not 100%, and nothing here has been tested across
different rooms, distances, or machines.

### Self-test results (verified 2026-07-17, all passing)

- 8 tones land exactly on 50 Hz bin centres.
- Tone orthogonality: 239 dB on/off ratio.
- CRC-16/CCITT-FALSE returns the published check value `0x29B1` for `"123456789"`.
- Clean round-trip decodes.
- Payload lengths 1, 2, 25, 29, and 200 all round-trip.
- Two frames in one stream both decode (the demodulator re-arms).
- AWGN: 200/200 decoded at each of 20, 10, 6, 3, and 0 dB SNR, with **zero**
  corrupt-but-accepted frames at every level.
- 300 × 0.5 s of pure noise produced 0 spurious messages.

Decoding at 0 dB wideband SNR is not magic. It is processing gain: the Goertzel integrates one
50 Hz bin out of a 24 kHz band, worth roughly `10·log10(24000/50) ≈ 27 dB`. The wideband
number is not the number the detector sees.

---

## Measured channel

All figures below were measured on this machine. Every constant in `src/dsp.h` is derived from
them; do not change the constants without re-measuring.

### Stepped-tone sweep, laptop speaker → laptop DMIC

Time-slot verified, so these are real tones and not clipping artifacts. SNR is against a
never-played control floor.

| Frequency | SNR vs control floor |
|---|---|
| 17.0 kHz | 24.3 dB |
| 18.0 kHz | **1.0 dB — DEAD** |
| 18.5 kHz | 15.6 dB |
| 19.0 kHz | 17.0 dB |
| 19.5 kHz | 10.1 dB |
| 20.0 kHz | 18.6 dB |
| 21.0 kHz | 13.0 dB |
| 22.0 kHz | 7.5 dB |

18.0 kHz sits in a comb-filter null. The speaker and the microphone are a fixed few centimetres
apart inside one chassis; at 19 kHz the wavelength is 18 mm, so a path difference of about 9 mm
nulls a tone outright. **This is why the band starts at 18250 and not at 18000.**

The null moves if the geometry moves, which is why the data rides on eight tones rather than
one.

### Per-tone magnitude arriving over the air (tx gain 0.20)

| Tone | Frequency | Magnitude over the air | Clean-file reference |
|---|---|---|---|
| 0 | 18250 Hz | 0.294 | 0.140 |
| 1 | 18500 Hz | 0.224 | 0.140 |
| 2 | 18750 Hz | 0.330 | 0.140 |
| 3 | 19000 Hz | 0.358 | 0.140 |
| 4 | 19250 Hz | 0.369 | 0.140 |
| 5 | 19500 Hz | 0.174 | 0.140 |
| 6 | 19750 Hz | 0.202 | 0.140 |
| 7 | 20000 Hz | 0.262 | 0.140 |

No tone is dead. The tilt across the band is roughly ±6 dB. The clean-file reference is flat at
0.140 for every tone, as expected from a synthetic signal.

### Spectrum of the captured microphone signal

The capture is dominated by low frequency: 0–100 Hz peaks at about **60 dB** while the
18–20 kHz band sits near **9 dB**.

Consequence: **RMS is a useless health metric here.** Broadband RMS measures 50 Hz mains hum,
not the signal. Always measure band-limited.

### DMIC capture boost

ALSA `Dmic0 Capture Volume` (numid=46) reads **70/70**, which is **+20 dB**
(dBscale: min −50.00 dB, step 1.00 dB). uchat does not touch this control, and **you should not
lower it.** It looks like it must be eating ADC headroom. It is not: dropping it 70 → 50 was
measured and made things *worse*, because it attenuates the signal along with the hum. Leave
it at 70. See "Theories that were wrong".

---

## Environment (verified on this machine)

| Item | Value |
|---|---|
| OS | Ubuntu 24.04 |
| Compiler | g++ 13.3.0 |
| Make | GNU Make 4.3 |
| ALSA dev headers | libasound2-dev 1.2.11 |
| Sound server | PipeWire 1.0.5 |
| Browsers | Brave (snap), Firefox |

**Not installed:** emscripten, node/npm, cmake, clang. The build depends on none of them.

### Audio devices

- `default` — the PipeWire capture path. **This is the default**, because it coexists with
  other applications. Measured 4/4 and 3/3 over the air through this path.
- `hw:0,6` — the raw 48 kHz DMIC. Needs EXCLUSIVE access: if any app is holding the mic (a
  browser tab, a call), PipeWire keeps the device open and uchat fails with
  `Device or resource busy`. Use it when you want no server in the path and can guarantee the
  device is free.
- `hw:0,7` — a 16 kHz DMIC. Its Nyquist limit is 8 kHz, so pointing capture at it means the
  signal **does not exist at all**, not merely that it is attenuated.

uchat refuses any device that does not negotiate exactly 48000 Hz. `src/audio.cpp` sets the
rate exactly with `snd_pcm_hw_params_set_rate()` (never `_near`), reads it back, and hard-fails
on a mismatch. `snd_pcm_hw_params_set_rate_near()` will hand you 16000 and return success —
that is the one silent failure that kills the entire project.

### Why native ALSA and not the browser

No echo-cancel module is loaded in PipeWire. This matters enormously: AEC exists specifically
to subtract your own speaker output from your microphone input, and here **the signal is the
speaker output**. An AEC in the path would delete exactly what we are trying to receive.

That is why capture is native ALSA rather than the browser. `getUserMedia` applies AEC, AGC and
noise suppression by default; `echoCancellation: false` is a request, not a guarantee, and the
whole link dies silently if it is ignored. Going through ALSA takes that entire class of
failure off the table.

Both capture and playback default to `default` (PipeWire), which coexists with the rest of the
desktop instead of seizing the card. Since no echo-cancel is loaded, that path is clean — the
original sweep measurement went through PipeWire and passed 18–22 kHz intact, and uchat still
hard-asserts 48000 Hz on it. `--capture hw:0,6` bypasses the server entirely if you want that.

---

## Design and rationale

### Modulation

| Parameter | Value |
|---|---|
| Sample rate | 48000 Hz (hard invariant) |
| Modulation | 8-FSK, CPFSK |
| Tones | 18250 … 20000 Hz, 250 Hz spacing |
| Symbol length | 960 samples = 20 ms |
| Bits per symbol | 3 |
| Bit rate | 150 bps |
| Detection threshold | 0.40 mean per-symbol tone purity (chance = 1/8 = 0.125) |

**8 tones at 250 Hz spacing, all exact multiples of 50 Hz.** A 960-sample rectangular window
gives a Goertzel bin width of 50 Hz. Every tone frequency is an exact multiple of that bin
width, so under a rectangular window each tone falls precisely on a bin centre and produces
true nulls at every other tone. That is what makes the tones perfectly orthogonal — the
self-test measures 239 dB rejection. The rectangular window is a deliberate choice, not
laziness: a tapered window would smear the bins and destroy the nulls.

**250 Hz spacing against 1/T = 50 Hz** gives 5× the minimum orthogonality margin, so the scheme
tolerates frequency error and Doppler from a moving chassis without tones bleeding into each
other.

**CPFSK — one phase accumulator across the whole burst.** Frequency changes at symbol
boundaries but phase never jumps. A phase discontinuity is a step; a step is broadband; and
broadband up here means an audible click on every symbol. Fifty clicks per second in a
supposedly inaudible system defeats the point.

**Only the outer edges of the burst are ramped** (5 ms raised cosine, `FS/200` samples). Ramping
every symbol would gate the carrier 50 times a second and splatter energy straight down into the
audible band.

**Goertzel rather than FFT** because only 8 known bins matter. An FFT computes hundreds of bins
nobody reads.

### Framing

```
[preamble 8 sym][len 3 sym = 9 bits][payload len×8 bits][crc16 16 bits]  → 3-bit padded
```

Maximum payload is 200 bytes. CRC is CRC-16/CCITT-FALSE over `[len][payload]`.

The preamble is `{0, 7, 1, 6, 2, 5, 3, 4}`. It visits **every** tone exactly once, so it doubles
as a channel probe: a failed preamble tells you which tone the room killed, and the full band is
exercised before a single data bit is committed.

Preamble scoring uses **purity, not absolute level** — `mag[expected] / sum(mag)`. A loud room
and a quiet room score identically. FSK only ever asks which tone won, never how loud it was.

### The guard interval, and why it applies to data symbols only

`DEMOD_SKIP = 192` (4 ms discarded) and `DEMOD_LEN = 768` (16 ms integrated); together they sum
to `SYM_LEN`.

The speaker and microphone are centimetres apart and the desk reflects almost as strongly as the
direct path, so when a symbol changes frequency the previous tone is still ringing in the room.
Integrating from sample 0 folds that echo into the wrong bin.

`48000/768 = 62.5 Hz` bins, and `18250 = 62.5 × 292` with a step of `250 = 62.5 × 4`. Every tone
is still exactly on a bin centre in the guarded window, so orthogonality survives untouched.

**The guard is applied only in `decode_symbol()`. Preamble scoring deliberately uses the full
960-sample window.** The guard flattens the correlation peak; fine alignment then locks anywhere
on the resulting plateau, producing a good preamble score at the wrong sample offset and an
unreadable payload. Alignment needs sharpness; only symbol decoding wants the guard.

### Acquisition

Coarse search hops `HOP = 240` samples (5 ms). On a coarse hit, fine alignment refines over
`±HOP` in `FINE_STEP = 24` sample (0.5 ms) steps. A 5 ms error on a 20 ms symbol straddles two
symbols and turns every Goertzel into mush.

Capture reads **channel 0 only** of the microphone array. Averaging the channels would sum two
spatially-separated captures of an 18 mm wavelength and comb-filter our own signal.

---

## Build

No cmake, no node, no external libraries beyond ALSA.

```sh
make            # builds ./uchat
make selftest   # builds ./selftest — DSP round-trip suite, no audio hardware in the loop
make tool       # builds ./tool     — diagnostics
```

Requires `libasound2-dev`. Note `-march=native` and `-ffast-math` in `CXXFLAGS`.

## Run

```sh
./uchat
# then open http://127.0.0.1:8080
```

Flags:

| Flag | Default | Meaning |
|---|---|---|
| `--capture DEV` | `default` | ALSA capture device (via PipeWire) |
| `--playback DEV` | `default` | ALSA playback device (PipeWire) |
| `--port N` | `8080` | HTTP port, bound to 127.0.0.1 only |
| `--gain G` | `0.2` | transmit amplitude, 0..1 |
| `--timeout MS` | `2000` | extra ms after the burst finishes before a message is declared lost |
| `--web DIR` | `web` | directory holding `index.html` |

Do not point `--capture` at `hw:0,7`. It is the 16 kHz DMIC and the signal will not exist. uchat
will refuse it anyway.

**Why `default` and not `hw:0,6`:** the raw device needs EXCLUSIVE access. If any application is
holding the microphone — a browser tab, a call — PipeWire keeps the DMIC open and uchat fails
with `Device or resource busy`. The `default` path goes through PipeWire and coexists, and was
measured at 4/4 and 3/3 while other apps held the mic. No echo-cancel module is loaded, so
nothing in that path subtracts our own speaker output. Use `--capture hw:0,6` when you want the
raw device with no server in the path and can guarantee it is free.

**`--gain` defaults low on purpose, and louder is not better.** The speaker and the microphone
are centimetres apart in one chassis, so raising the transmit amplitude saturates the microphone
and decoding fails outright. Transmitting at 0.7 is what broke the link for most of this
project's development. Measured: 0.14, 0.20 and 0.28 all decode at the same 6/8 rate; anything
that pins the capture peak at 1.0 fails.

The page shows blue bubbles for transmitted messages and green bubbles for text decoded back out
of the microphone. A green bubble is the only proof the audio made the round trip — and as of
today, over the air, you will not get one.

## Diagnostics tool

`./tool` splits "it doesn't work" into answerable questions.

```sh
./tool emit out.wav "msg" [gain]   # modulate a message to a 48 kHz mono WAV
./tool decode in.wav               # run the real Demod over a WAV, offline
./tool probe DEV SECONDS           # capture raw, report per-channel rms + 18-20 kHz energy
```

- **`emit`** writes the exact waveform the transmitter would play, optionally scaled by `gain`.
- **`decode`** runs the production `Demod` class over a file, reports rms and peak (warning if
  peak > 0.99, i.e. clipped), prints per-tone peak magnitude across the whole file, and reports
  the best preamble score seen against the threshold and the chance level. The "best score" it
  prints is a **true maximum that `tool.cpp` tracks itself** — do not replace it with
  `Demod::last_score()`, which is the last score computed and is meaningless outside the moment
  right after `pop()`. That exact confusion produced every wrong theory in this document.
- **`probe`** reports **per-channel** rms, peak, and the peak tone in 18–20 kHz, and flags a
  channel that is dead silent. It reports per channel because the demodulator reads **one**
  channel of a microphone array — if that channel is dead, everything downstream looks exactly
  like a DSP bug and is not one.

---

## Bugs

### FIXED — infinite loop in `Demod::push()`

Fine alignment can place `frame_` up to `HOP` samples **behind** `search_`. Resuming a rejected
frame at `frame_ + HOP` could therefore land exactly back on `search_`, re-detect the same false
preamble, fail the same way, and loop forever.

This wedged the capture thread inside `push()`, which in turn hung shutdown, because `stop()`
joins a thread that never returns. One bug, two symptoms: no RX, and SIGTERM not killing the
process. Those looked like two unrelated failures and were not.

Fixed by recording `lock_search_` (the value of `search_` at the moment of lock) and resuming at
`lock_search_ + HOP`, so `search_` advances monotonically on every reject path.

Verified: a capture file that hung a pre-fix binary for over 2 minutes now decodes in ~150 ms.

### KNOWN GAP — the regression test for that bug is vacuous

The test in `src/selftest.cpp` titled *"corrupt frames must reject WITHOUT hanging"* **still
passes when the bug is deliberately reintroduced.** Its synthetic corruption never drives fine
alignment to land behind `search_`, which is the precondition for the loop.

It is not coverage. It needs to be replaced with a case that reproduces the actual geometry —
a preamble whose best fine-aligned offset is strictly less than the coarse search offset.
Do not treat the green PASS line as protection.

### FIXED — the real cause of end-to-end failure was transmit amplitude

Mundane, after a long hunt: `uchat` transmitted at amplitude 0.7 while the bench test that
worked used 0.14. The speaker and the microphone are centimetres apart in one chassis, so a
loud transmit **saturates the microphone** and decoding fails outright. Louder is actively
worse. Fixed by adding `--gain` (default 0.2).

### FIXED — frame loss, via Reed–Solomon

Was 18/24 = 75%, now 23/24 = 96%. The diagnosis came from the measurement being identical at
all three transmit gains: that ruled out link budget and pointed at symbol errors. `RS_PARITY=8`
corrects 4 byte errors, against a measured mean near 0.5 byte errors per frame.

### OPEN — the remaining 4%

One frame in 24 still fails. Not yet characterised. Options if it matters: more parity, longer
symbols, or an application-level retry (the CRC makes a failed frame safe to simply resend).

A lost message is at least **visible**: the server arms a deadline of the burst length plus
`--timeout` ms, and on expiry logs `[timeout] "..." never came back` and pushes a `timeout`
event, which the page renders as a red dashed bubble reading *lost — never decoded back*.
Verified both ways: muting the speaker mid-run produced a timeout, and the next message
unmuted decoded normally — a timeout that only ever fires is as useless as one that never does.

Returns are matched to sends **by text**, not by "the most recent send". With more than one
burst in flight, "the last one" is a guess, and that guess attributes a decode to the wrong
message.

### OPEN — the guard interval's benefit is unverified

`DEMOD_SKIP` / `DEMOD_LEN` were added to chase a phantom (see below). ISI is real on physical
grounds, so it is kept, but it costs ~1 dB of integration and its benefit over the air has
never been isolated by measurement. Do not assume it is helping.

---

## Theories that were wrong

The most useful section in this file. Every one of these was investigated, believed, and killed
by measurement. The actual cause was transmit amplitude, above.

| Theory | How it died |
|---|---|
| "DMIC channel 0 is dead" | It was alive the whole time. |
| "Clipping intermod kills the demod" | Non-clipping runs failed identically. |
| "The 18.0 kHz comb null eats a tone" | No tone was dead over the air — measured spread was ±6 dB of tilt. (The 18.0 kHz null in the sweep is real, and is still why the band starts at 18250. It just was not the cause.) |
| "The +20 dB `Dmic0` boost eats ADC headroom" | Lowering it 70 → 50 made things **worse**: it attenuated the signal along with the hum. |
| "ISI needs a guard interval" | Improved the phantom metric below. Did not fix the decode. |
| "The CPU fan drowns the signal" | The most compelling and most wrong. Ambient in-band floor measured **−62.2 dB at 3042 RPM** and **−63.7 dB at 0 RPM** — a 1.5 dB difference. The "19 dB rise" came from comparing a `pw-record` measurement against an `arecord` measurement: **two different scales.** |

### The root cause of all of them: a diagnostic that lied

`Demod::last_score()` returns the **last** preamble score computed, not the best. After a frame
decodes, the demodulator keeps scanning, and `last_score_` ends up holding whatever the final
noise candidate scored. `tool decode` printed it as **"best preamble score seen"**.

It reported **0.55 on runs that decoded nothing**, and **0.11 on runs that decoded perfectly.**

Every theory in the table above was built on that number. `tool.cpp` now tracks a true maximum
itself, and `dsp.h` documents that `last_score()` is only meaningful immediately after `pop()`.

**A diagnostic that lies is worse than no diagnostic**, because it manufactures confident wrong
theories and each one looks like progress.

---

## Debugging lessons

These are the traps that actually cost time on this project.

- **Never compare numbers from two different capture paths.** A `pw-record` figure and an
  `arecord` figure are on different scales; subtracting them invents a 19 dB effect that does
  not exist. See the fan theory above.
- **Distrust your own instrumentation first.** Six wrong theories came from one mislabelled
  variable. Before theorising about physics, verify the number you are theorising about means
  what its label says.

- **RMS is a useless health metric here.** The capture is dominated by 50 Hz mains hum —
  0–100 Hz peaks near 60 dB while the 18–20 kHz band sits near 9 dB. A healthy RMS tells you the
  hum is present. Always measure band-limited.
- **A stale binary will happily reproduce a bug you already fixed.** Rebuild the exact binary
  you are testing. `make selftest` does not rebuild `./tool`. If you fix `dsp.cpp` and then test
  with `./tool`, run `make tool`.
- **`pkill -f <pattern>` matches the invoking shell's own command line** and will kill your own
  shell — taking any cleanup or volume-restore lines after it with it. The pattern appears in
  the shell's argv.
- **System volume left in a wrong state silently invalidates every subsequent acoustic
  measurement.** Nothing errors. The numbers are just quietly wrong, and they look plausible.
  Restore volume explicitly, and verify it.
- **Clipping makes harmonics that alias down and masquerade as real tones.** If peak == 1.0,
  distrust every magnitude in the band. This is why the sweep results are time-slot verified:
  always confirm a tone appears in its correct time slot before believing it is the tone you
  played. `./tool decode` prints a clipping warning for this reason.

---

## Layout

| Path | Contents |
|---|---|
| `src/dsp.h`, `src/dsp.cpp` | Goertzel, CRC-16, modulator, streaming `Demod`. No hardware. |
| `src/audio.h`, `src/audio.cpp` | ALSA `Capture` and `Playback`, with the 48 kHz hard check. |
| `src/http.h`, `src/http.cpp` | Minimal HTTP/1.1 + SSE server, loopback only. |
| `src/main.cpp` | Wiring: capture → demod → SSE broadcast; POST /send → modulate → playback. |
| `src/selftest.cpp` | DSP suite. If it fails, nothing downstream can work. |
| `src/tool.cpp` | `emit` / `decode` / `probe` diagnostics. |
| `web/index.html` | The chat page. Single file, no build step, no dependencies. |

SSE rather than WebSocket: same push semantics, but it is plain `text/event-stream` over HTTP —
no SHA-1/base64 handshake and no frame codec to get wrong, and no dependencies.
