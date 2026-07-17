// uchat — near-ultrasonic acoustic chat. DSP core.
//
// Every constant here is derived from a measurement taken on THIS machine
// (sof-hda-dsp, DMIC hw:0,6). See README.md "Measured channel" before changing any of them.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace uchat {

// ── Hard invariants ─────────────────────────────────────────────────────────
// 48 kHz is not a preference, it is the whole project. This box also exposes a
// DMIC16kHz device (hw:0,7) whose Nyquist is 8 kHz — route there and the signal
// does not attenuate, it ceases to exist. Asserted at every audio open.
constexpr int FS = 48000;

// ── Symbol geometry ─────────────────────────────────────────────────────────
constexpr int SYM_LEN = 960;              // 20 ms  → Goertzel bin width = 50 Hz
constexpr int N_TONES = 8;                // 3 bits / symbol → 150 bps
constexpr int BITS_PER_SYM = 3;

// Measured 2026-07-17 (stepped-tone sweep, laptop speaker → laptop DMIC, time-slot
// verified so these are real tones and not clipping artifacts):
//   17.0k 24.3dB | 18.0k  1.0dB DEAD | 18.5k 15.6dB | 19.0k 17.0dB
//   19.5k 10.1dB | 20.0k 18.6dB      | 21.0k 13.0dB | 22.0k  7.5dB
// 18.0 kHz sits in a comb-filter null (speaker and mic are a fixed few cm apart in
// one chassis; at 19 kHz λ=18 mm, so a ~9 mm path difference nulls a tone outright).
// Hence the band starts at 18250, not 18000. The null moves if the geometry moves —
// this is why the data is spread over 8 tones instead of riding on one.
constexpr double TONE_BASE = 18250.0;     // ┐ 18250 … 20000 Hz, inside the requested
constexpr double TONE_STEP = 250.0;       // ┘ 18–20 kHz band. All exact 50 Hz bin
                                          //   multiples → orthogonal under Goertzel.
inline double tone_hz(int k) { return TONE_BASE + TONE_STEP * static_cast<double>(k); }

// ── Framing ─────────────────────────────────────────────────────────────────
//   [preamble 8 sym] [header 8 sym] [body: RS codeword, 3-bit padded]
//
//   header = the length byte sent THREE times (24 bits → exactly 8 symbols), recovered by
//            per-bit majority vote. This exists to break a chicken-and-egg: the body is
//            Reed–Solomon protected, but you need the length to know how many symbols the
//            codeword occupies before you can run RS over it. So the length cannot live
//            only inside the thing it is needed to read. Three copies + majority survives
//            any single symbol error in the header.
//   body   = RS( [len][payload][crc16 over len+payload] ) + RS_PARITY parity bytes.
//            len appears twice on purpose — once in the header for framing, once inside
//            the protected block for verification. If they disagree, the CRC fails.
//
//   Order of authority on receive: majority vote → RS correction → CRC. The CRC is final;
//   RS can algebraically "correct" noise into a different valid-looking codeword, so a
//   successful RS decode is never treated as proof the message is right.
constexpr int PREAMBLE_LEN = 8;
constexpr int HEADER_SYMS  = 8;           // 3 × 8-bit length = 24 bits = 8 symbols
constexpr int MAX_PAYLOAD  = 200;         // bytes
extern const int PREAMBLE[PREAMBLE_LEN];  // sweeps every tone → doubles as a channel probe

// Guard interval. The speaker and the mic are centimetres apart and the desk reflects
// almost as strongly as the direct path, so when a symbol changes frequency the previous
// tone is still ringing in the room; integrating from sample 0 folds that echo into the
// wrong bin. So skip the first 4 ms of every symbol and integrate only the settled
// remainder. 48000/768 = 62.5 Hz bins, and 18250 = 62.5×292 with a 250 Hz step = 62.5×4,
// so every tone is still exactly on a bin centre and orthogonality survives untouched.
//
// HONESTY NOTE: this was added to chase an apparent over-air tone purity of 0.55, a number
// that turned out to be an artefact of a lying diagnostic (Demod::last_score() is the LAST
// score computed, not the best; it was being printed as "best score seen"). The guard costs
// ~1 dB of integration and is kept because ISI here is real on physical grounds, but its
// benefit over the air has NOT been isolated by measurement. Do not treat it as verified.
constexpr int    DEMOD_SKIP    = 192;     // 4 ms settling, discarded
constexpr int    DEMOD_LEN     = 768;     // 16 ms integrated  (SKIP + LEN == SYM_LEN)

constexpr int    HOP           = 240;     // 5 ms coarse search hop
constexpr int    FINE_STEP     = 24;      // 0.5 ms fine alignment step
constexpr double DETECT_THRESH = 0.40;    // mean per-symbol tone purity; chance = 1/8 = 0.125

// ── Primitives ──────────────────────────────────────────────────────────────
// Goertzel magnitude at freq_hz over n samples. Rectangular window on purpose:
// with exact-bin tones it gives true nulls at every other tone → orthogonality.
double   goertzel(const float* x, int n, double freq_hz);
uint16_t crc16(const uint8_t* data, size_t n);   // CRC-16/CCITT-FALSE

// Modulate a message into mono float samples (CPFSK, phase continuous across symbols;
// only the burst's outer edges are ramped, so there are no per-symbol clicks).
std::vector<float> modulate(const std::string& msg);

// Frame geometry for a payload of `len` bytes. Shared by the modulator and the demodulator
// so the two can never disagree about how long a codeword is.
int body_bytes_for(int len);   // [len][payload][crc16] + RS parity
int body_syms_for(int len);

// Streaming demodulator. push() audio, pop() any messages that fell out.
class Demod {
public:
    void push(const float* x, int n);
    bool pop(std::string& out);          // true if a CRC-valid message was dequeued
    // NOTE: the LAST score computed, not the best. Valid to read immediately after pop()
    // (it is then the winning frame's lock score); meaningless if sampled at any other time.
    double last_score() const { return last_score_; }

private:
    enum class State { SEARCH, LOCKED };

    bool   score_preamble(size_t abs_off, double* score) const;
    int    decode_symbol(size_t abs_off) const;
    void   try_decode_frame();
    const float* at(size_t abs_off) const { return buf_.data() + (abs_off - base_); }
    size_t have() const { return base_ + buf_.size(); }   // one past last abs index held

    std::vector<float>       buf_;
    size_t                   base_ = 0;        // abs index of buf_[0]
    size_t                   search_ = 0;      // next abs offset to test
    size_t                   frame_ = 0;       // abs offset of locked frame start
    // search_ must advance MONOTONICALLY. Fine-alignment can place frame_ up to HOP
    // *behind* search_, so resuming a failed frame at frame_+HOP can land back exactly
    // where we already were → same false preamble → same failure → infinite loop, which
    // wedges the capture thread and hangs shutdown with it. Resume from here instead.
    size_t                   lock_search_ = 0; // value of search_ at the moment we locked
    State                    state_ = State::SEARCH;
    double                   last_score_ = 0.0;
    std::vector<std::string> out_;
};

} // namespace uchat
