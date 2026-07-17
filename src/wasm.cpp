// WASM boundary for the browser build.
//
// This file deliberately contains NO signal processing. It is a thin C shim over the exact
// same dsp.cpp / rs.cpp that the native binary and the self-test use, so a browser and the
// native host cannot drift apart: one modulator, one demodulator, one set of constants,
// one test suite covering all of them. A JS reimplementation would be a second
// implementation to keep in sync, and the two would diverge silently — the transmitter and
// receiver would each be self-consistent and mutually wrong.
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include "dsp.h"
#include <cstring>
#include <string>
#include <vector>

using namespace uchat;

namespace {
Demod              g_demod;
std::vector<float> g_burst;   // keeps the last modulate() result alive for the JS side

// Most recent SYM_LEN samples, for carrier sense. The air is a shared medium: if two
// devices transmit at once both frames die, and Reed–Solomon will not save them — RS
// repairs byte errors, not two overlapping signals. So a sender has to listen first.
std::vector<float> g_recent(SYM_LEN, 0.0f);
size_t             g_rpos = 0;
}

extern "C" {

// Modulate `len` raw bytes and return a pointer to float samples; length lands in *out_len.
// The buffer is owned here and stays valid until the next uc_modulate() call.
//
// Takes a POINTER AND A LENGTH, never a C string. The payload is arbitrary binary — a room
// hash, a session id, a nickname length — and routing that through a JS string costs you
// twice: stringToUTF8 re-encodes every byte above 127 into two bytes (0xB3 becomes 0xC2 0xB3),
// shifting the whole frame, and a zero byte terminates the string early. Both were hit.
EMSCRIPTEN_KEEPALIVE
const float* uc_modulate(const char* data, int len, int* out_len) {
    g_burst = modulate(std::string(data, static_cast<size_t>(len)));
    *out_len = static_cast<int>(g_burst.size());
    return g_burst.data();
}

// Feed captured audio. Mono float, MUST be 48 kHz — the caller is responsible for that,
// and uc_sample_rate() exists so it can check rather than assume.
EMSCRIPTEN_KEEPALIVE
void uc_push(const float* x, int n) {
    g_demod.push(x, n);
    for (int i = 0; i < n; ++i) {          // keep the tail for carrier sense
        g_recent[g_rpos] = x[i];
        g_rpos = (g_rpos + 1) % g_recent.size();
    }
}

// Carrier sense: mean Goertzel magnitude across our 8 tones over the last 20 ms.
// Deliberately measures OUR BAND ONLY, not broadband energy — someone talking, a fan, or
// mains hum are all far louder than the signal and would jam the channel permanently if
// we sensed wideband. Compare against a floor measured on this device, not a constant:
// gain structure differs wildly between machines, so an absolute threshold is a guess.
EMSCRIPTEN_KEEPALIVE
double uc_band_energy() {
    std::vector<float> lin(g_recent.size());
    for (size_t i = 0; i < g_recent.size(); ++i)
        lin[i] = g_recent[(g_rpos + i) % g_recent.size()];
    double acc = 0.0;
    for (int k = 0; k < N_TONES; ++k)
        acc += goertzel(lin.data(), static_cast<int>(lin.size()), tone_hz(k));
    return acc / N_TONES;
}

// Pop one decoded message into `buf`. Returns its length, or -1 if nothing is ready.
EMSCRIPTEN_KEEPALIVE
int uc_pop(char* buf, int cap) {
    std::string m;
    if (!g_demod.pop(m)) return -1;
    const int n = static_cast<int>(m.size() < static_cast<size_t>(cap) ? m.size() : cap);
    std::memcpy(buf, m.data(), static_cast<size_t>(n));
    return n;
}

// Only meaningful immediately after uc_pop() returns >= 0 — it is then the winning frame's
// lock score. Sampled at any other time it is the last score computed over noise. This
// caveat cost an hour once; see README "The root cause of all of them: a diagnostic that lied".
EMSCRIPTEN_KEEPALIVE
double uc_last_score() { return g_demod.last_score(); }

EMSCRIPTEN_KEEPALIVE int uc_sample_rate()  { return FS; }
EMSCRIPTEN_KEEPALIVE int uc_max_payload()  { return MAX_PAYLOAD; }
EMSCRIPTEN_KEEPALIVE int uc_n_tones()      { return N_TONES; }
EMSCRIPTEN_KEEPALIVE double uc_tone_hz(int k) { return tone_hz(k); }

} // extern "C"
