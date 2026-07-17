#include "rs.h"
#include <cstring>

namespace uchat {
namespace {

// ── GF(256) with primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D) ─────
struct GF {
    uint8_t exp[512];   // doubled so exp[a+b] needs no modulo
    uint8_t log[256];
    GF() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        log[0] = 0;   // log(0) is undefined; guarded at every call site
    }
};
const GF g;

inline uint8_t mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0;
    return g.exp[g.log[a] + g.log[b]];
}
inline uint8_t div_(uint8_t a, uint8_t b) {   // b != 0 required
    if (!a) return 0;
    return g.exp[g.log[a] + 255 - g.log[b]];
}
inline uint8_t inv(uint8_t a) { return g.exp[255 - g.log[a]]; }
inline uint8_t pow_(uint8_t a, int n) {
    if (!a) return 0;
    return g.exp[(g.log[a] * n) % 255];
}

// Generator polynomial g(x) = ∏ (x - α^i), i = 0 .. RS_PARITY-1
struct Gen {
    uint8_t p[RS_PARITY + 1];
    int len;
    Gen() {
        uint8_t t[RS_PARITY + 1] = {1};
        int n = 1;
        for (int i = 0; i < RS_PARITY; ++i) {
            uint8_t next[RS_PARITY + 2] = {0};
            const uint8_t root = g.exp[i];
            for (int j = 0; j < n; ++j) {
                next[j]     ^= mul(t[j], root);   // t(x) * α^i
                next[j + 1] ^= t[j];              // t(x) * x
            }
            ++n;
            std::memcpy(t, next, static_cast<size_t>(n));
        }
        len = n;
        std::memcpy(p, t, static_cast<size_t>(n));
    }
};
const Gen gen;

} // namespace

void rs_encode(const uint8_t* data, int k, uint8_t* parity) {
    std::memset(parity, 0, RS_PARITY);
    for (int i = 0; i < k; ++i) {
        const uint8_t f = data[i] ^ parity[0];
        std::memmove(parity, parity + 1, RS_PARITY - 1);
        parity[RS_PARITY - 1] = 0;
        if (f) for (int j = 0; j < RS_PARITY; ++j)
            parity[j] ^= mul(gen.p[RS_PARITY - 1 - j], f);
    }
}

bool rs_decode(uint8_t* buf, int n) {
    // ── Syndromes: S_i = r(α^i). All zero → codeword is already clean.
    uint8_t S[RS_PARITY] = {0};
    bool bad = false;
    for (int i = 0; i < RS_PARITY; ++i) {
        uint8_t s = 0;
        for (int j = 0; j < n; ++j) s = mul(s, g.exp[i]) ^ buf[j];
        S[i] = s;
        if (s) bad = true;
    }
    if (!bad) return true;

    // ── Berlekamp–Massey: find the error-locator polynomial Λ(x).
    uint8_t L[RS_PARITY + 1] = {1}, B[RS_PARITY + 1] = {1};
    int Ln = 1, Bn = 1, m = 1;
    uint8_t b = 1;
    for (int r = 0; r < RS_PARITY; ++r) {
        uint8_t d = S[r];
        for (int i = 1; i < Ln; ++i) d ^= mul(L[i], S[r - i]);
        if (!d) { ++m; continue; }

        uint8_t T[RS_PARITY + 1];
        std::memcpy(T, L, sizeof T);
        const int Tn = Ln;
        const uint8_t scale = div_(d, b);
        for (int i = 0; i < Bn; ++i) {
            const int j = i + m;
            if (j <= RS_PARITY) { L[j] ^= mul(scale, B[i]); if (j + 1 > Ln) Ln = j + 1; }
        }
        if (2 * (Tn - 1) <= r) {
            std::memcpy(B, T, sizeof B);
            Bn = Tn;
            b = d;
            m = 1;
        } else {
            ++m;
        }
    }

    const int nerr = Ln - 1;
    if (nerr <= 0 || nerr > RS_PARITY / 2) return false;   // beyond correction capacity

    // ── Chien search: roots of Λ(x) give the error positions.
    int pos[RS_PARITY / 2];
    int found = 0;
    for (int i = 0; i < n; ++i) {
        // evaluate Λ at α^-(n-1-i)
        uint8_t v = 0;
        const uint8_t x = g.exp[(255 - ((n - 1 - i) % 255)) % 255];
        for (int j = Ln - 1; j >= 0; --j) v = mul(v, x) ^ L[j];
        if (!v) {
            if (found >= RS_PARITY / 2) return false;
            pos[found++] = i;
        }
    }
    if (found != nerr) return false;   // Λ has roots outside the codeword → uncorrectable

    // ── Forney: Ω(x) = S(x)·Λ(x) mod x^RS_PARITY, then e = Ω(x)/Λ'(x) at each root.
    uint8_t O[RS_PARITY] = {0};
    for (int i = 0; i < RS_PARITY; ++i)
        for (int j = 0; j <= i; ++j)
            if (j < Ln) O[i] ^= mul(S[i - j], L[j]);

    for (int e = 0; e < found; ++e) {
        const int    p  = pos[e];
        const uint8_t Xi = g.exp[(n - 1 - p) % 255];       // error locator value
        const uint8_t Xn = inv(Xi);

        uint8_t num = 0;
        for (int i = RS_PARITY - 1; i >= 0; --i) num = mul(num, Xn) ^ O[i];

        uint8_t den = 0;                                    // Λ'(x) — odd terms only, in GF(2)
        for (int i = 1; i < Ln; i += 2) den ^= mul(L[i], pow_(Xn, i - 1));
        if (!den) return false;

        buf[p] ^= mul(Xi, div_(num, den));
    }

    // ── Verify: syndromes must now be zero. RS can "correct" noise into a different
    //    valid-looking codeword, so re-check rather than trust the algebra succeeded.
    for (int i = 0; i < RS_PARITY; ++i) {
        uint8_t s = 0;
        for (int j = 0; j < n; ++j) s = mul(s, g.exp[i]) ^ buf[j];
        if (s) return false;
    }
    return true;
}

} // namespace uchat
