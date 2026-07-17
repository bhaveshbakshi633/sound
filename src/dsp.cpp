#include "dsp.h"
#include "rs.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace uchat {

// Visits every tone once: a failed preamble tells you WHICH tone the room killed,
// and it exercises the full band before a single data bit is committed.
const int PREAMBLE[PREAMBLE_LEN] = {0, 7, 1, 6, 2, 5, 3, 4};

double goertzel(const float* x, int n, double freq_hz) {
    const double w  = 2.0 * M_PI * freq_hz / static_cast<double>(FS);
    const double cw = std::cos(w), sw = std::sin(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = static_cast<double>(x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return std::sqrt(re * re + im * im) * 2.0 / static_cast<double>(n);
}

uint16_t crc16(const uint8_t* data, size_t n) {   // CRC-16/CCITT-FALSE
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021)
                             : static_cast<uint16_t>(c << 1);
    }
    return c;
}

namespace {

void push_bits(std::vector<int>& bits, uint32_t v, int n) {   // MSB first
    for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1);
}

// The RS-protected block: [len][payload][crc16], then RS_PARITY parity bytes appended.
std::vector<uint8_t> body_codeword(const std::string& msg) {
    std::vector<uint8_t> b;
    b.push_back(static_cast<uint8_t>(msg.size()));
    b.insert(b.end(), msg.begin(), msg.end());
    const uint16_t c = crc16(b.data(), b.size());
    b.push_back(static_cast<uint8_t>(c >> 8));
    b.push_back(static_cast<uint8_t>(c & 0xFF));

    const size_t k = b.size();
    b.resize(k + RS_PARITY);
    rs_encode(b.data(), static_cast<int>(k), b.data() + k);
    return b;
}

} // namespace

int body_bytes_for(int len) { return len + 3 + RS_PARITY; }   // len + payload + crc16 + parity

int body_syms_for(int len) {
    return (body_bytes_for(len) * 8 + BITS_PER_SYM - 1) / BITS_PER_SYM;
}

std::vector<float> modulate(const std::string& msg) {
    std::vector<int> syms(PREAMBLE, PREAMBLE + PREAMBLE_LEN);

    // Header: the length byte three times → 24 bits → exactly HEADER_SYMS symbols.
    std::vector<int> hbits;
    for (int r = 0; r < 3; ++r) push_bits(hbits, static_cast<uint32_t>(msg.size()), 8);
    for (size_t i = 0; i < hbits.size(); i += BITS_PER_SYM)
        syms.push_back(hbits[i] * 4 + hbits[i + 1] * 2 + hbits[i + 2]);

    std::vector<int> bits;
    for (uint8_t b : body_codeword(msg)) push_bits(bits, b, 8);
    while (bits.size() % BITS_PER_SYM) bits.push_back(0);   // pad to whole symbols
    for (size_t i = 0; i < bits.size(); i += BITS_PER_SYM)
        syms.push_back(bits[i] * 4 + bits[i + 1] * 2 + bits[i + 2]);

    std::vector<float> x;
    x.reserve(syms.size() * SYM_LEN);

    // CPFSK: one phase accumulator across the entire burst. Frequency changes at symbol
    // boundaries but phase never jumps — a phase discontinuity is a step, a step is
    // broadband, and broadband up here means an audible click on every symbol.
    double phase = 0.0;
    for (int s : syms) {
        const double dp = 2.0 * M_PI * tone_hz(s) / static_cast<double>(FS);
        for (int i = 0; i < SYM_LEN; ++i) {
            x.push_back(static_cast<float>(std::sin(phase)));
            phase += dp;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }

    // Ramp only the outer edges of the whole burst (5 ms raised cosine). Ramping every
    // symbol would gate the carrier 50×/s and splatter energy straight into the audible band.
    const int r = std::min<int>(FS / 200, static_cast<int>(x.size()) / 2);
    for (int i = 0; i < r; ++i) {
        const float w = static_cast<float>(0.5 * (1.0 - std::cos(M_PI * i / r)));
        x[i] *= w;
        x[x.size() - 1 - i] *= w;
    }
    for (auto& v : x) v *= 0.7f;
    return x;
}

// ── Demodulator ─────────────────────────────────────────────────────────────

int Demod::decode_symbol(size_t abs_off) const {
    const float* w = at(abs_off + DEMOD_SKIP);   // skip the guard interval
    int best = 0;
    double bestm = -1.0;
    for (int k = 0; k < N_TONES; ++k) {
        const double m = goertzel(w, DEMOD_LEN, tone_hz(k));
        if (m > bestm) { bestm = m; best = k; }
    }
    return best;
}

bool Demod::score_preamble(size_t abs_off, double* score) const {
    if (abs_off < base_) return false;
    if (abs_off + static_cast<size_t>(PREAMBLE_LEN) * SYM_LEN > have()) return false;

    double acc = 0.0;
    for (int k = 0; k < PREAMBLE_LEN; ++k) {
        const float* w = at(abs_off + static_cast<size_t>(k) * SYM_LEN);
        double mag[N_TONES], sum = 1e-12;
        for (int t = 0; t < N_TONES; ++t) {
            mag[t] = goertzel(w, SYM_LEN, tone_hz(t));
            sum += mag[t];
        }
        // Full SYM_LEN window on purpose — NOT the guarded one. The guard interval flattens
        // this correlation peak, and fine-align then locks anywhere on the plateau: great
        // preamble score, wrong sample offset, unreadable payload. Alignment needs sharpness;
        // only decode_symbol() wants the guard.
        // Purity, not absolute level: a loud room and a quiet room score the same.
        // FSK only ever asks "which tone won", never "how loud was it".
        acc += mag[PREAMBLE[k]] / sum;
    }
    *score = acc / PREAMBLE_LEN;
    return true;
}

void Demod::try_decode_frame() {
    const size_t hdr_end = frame_ + static_cast<size_t>(PREAMBLE_LEN + HEADER_SYMS) * SYM_LEN;
    if (have() < hdr_end) return;                       // not enough audio yet — wait

    // ── Header: three copies of the length byte, per-bit majority vote.
    std::vector<int> hbits;
    for (int i = 0; i < HEADER_SYMS; ++i) {
        const int s = decode_symbol(frame_ + static_cast<size_t>(PREAMBLE_LEN + i) * SYM_LEN);
        push_bits(hbits, static_cast<uint32_t>(s), BITS_PER_SYM);
    }
    int len = 0;
    for (int i = 0; i < 8; ++i) {
        const int votes = hbits[i] + hbits[8 + i] + hbits[16 + i];
        len = (len << 1) | (votes >= 2 ? 1 : 0);
    }

    if (len <= 0 || len > MAX_PAYLOAD) {                // garbage length → false preamble
        state_  = State::SEARCH;
        search_ = lock_search_ + HOP;                   // forward progress, guaranteed
        return;
    }

    const int    nsyms     = body_syms_for(len);
    const size_t frame_end = frame_ + static_cast<size_t>(PREAMBLE_LEN + HEADER_SYMS + nsyms) * SYM_LEN;
    if (have() < frame_end) return;                     // wait for the tail

    std::vector<int> bits;
    for (int i = 0; i < nsyms; ++i) {
        const int s = decode_symbol(
            frame_ + static_cast<size_t>(PREAMBLE_LEN + HEADER_SYMS + i) * SYM_LEN);
        push_bits(bits, static_cast<uint32_t>(s), BITS_PER_SYM);
    }

    const int nbytes = body_bytes_for(len);
    std::vector<uint8_t> cw(static_cast<size_t>(nbytes));
    for (int i = 0; i < nbytes; ++i) {
        uint8_t b = 0;
        for (int j = 0; j < 8; ++j) b = static_cast<uint8_t>((b << 1) | bits[i * 8 + j]);
        cw[static_cast<size_t>(i)] = b;
    }

    // ── Reed–Solomon: repair up to RS_PARITY/2 byte errors in place.
    const bool rs_ok = rs_decode(cw.data(), nbytes);

    // ── CRC is the final authority, always. RS can algebraically land on a different
    //    valid-looking codeword, and the majority-voted len can disagree with the len
    //    carried inside the protected block. Both of those fail here, silently and safely.
    bool good = false;
    if (rs_ok) {
        const int k = 1 + len;                          // [len][payload]
        const uint16_t rx_crc = static_cast<uint16_t>(cw[k] << 8 | cw[k + 1]);
        good = (cw[0] == static_cast<uint8_t>(len)) && (crc16(cw.data(), k) == rx_crc);
    }

    if (good) {
        out_.emplace_back(reinterpret_cast<const char*>(cw.data()) + 1, len);
        state_  = State::SEARCH;
        search_ = frame_end;                            // skip past what we just consumed
    } else {
        state_  = State::SEARCH;                        // Never emit a maybe.
        search_ = lock_search_ + HOP;                   // forward progress, guaranteed
    }
}

void Demod::push(const float* x, int n) {
    buf_.insert(buf_.end(), x, x + n);

    for (;;) {
        if (state_ == State::SEARCH) {
            double sc = 0.0;
            bool advanced = false;
            while (score_preamble(search_, &sc)) {
                last_score_ = sc;
                if (sc >= DETECT_THRESH) {
                    // Coarse hit. Refine to ±0.5 ms — a 5 ms error on a 20 ms symbol
                    // straddles two symbols and turns every Goertzel into mush.
                    size_t best = search_;
                    double bs = sc;
                    for (long d = -HOP; d <= HOP; d += FINE_STEP) {
                        const long cand = static_cast<long>(search_) + d;
                        if (cand < static_cast<long>(base_)) continue;
                        double s2;
                        if (score_preamble(static_cast<size_t>(cand), &s2) && s2 > bs) {
                            bs = s2;
                            best = static_cast<size_t>(cand);
                        }
                    }
                    last_score_  = bs;
                    lock_search_ = search_;   // resume point if this frame turns out to be junk
                    frame_       = best;
                    state_       = State::LOCKED;
                    break;
                }
                search_ += HOP;
                advanced = true;
            }
            if (state_ != State::LOCKED) { (void)advanced; break; }
        }

        if (state_ == State::LOCKED) {
            const State before = state_;
            try_decode_frame();
            if (state_ == before) break;   // still waiting on audio
        }
    }

    // Trim consumed history, but never below what SEARCH's fine-align window can reach back to.
    const size_t keep_from = (state_ == State::LOCKED) ? frame_ : search_;
    const size_t margin = 2 * SYM_LEN + HOP;
    if (keep_from > base_ + margin) {
        const size_t drop = keep_from - base_ - margin;
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(drop));
        base_ += drop;
    }
}

bool Demod::pop(std::string& out) {
    if (out_.empty()) return false;
    out = out_.front();
    out_.erase(out_.begin());
    return true;
}

} // namespace uchat
