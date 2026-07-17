// DSP self-test: modulate → demodulate with no audio hardware in the loop.
// If this fails, nothing downstream can work; if it passes, any later failure is
// the channel or ALSA, not the DSP. Run: make selftest && ./selftest
#include "dsp.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace uchat;

static int failures = 0;

static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Feed audio through the demod in irregular chunks, the way ALSA actually delivers it —
// a decoder that only works on tidy symbol-aligned blocks is a decoder that works only in tests.
static std::vector<std::string> run(const std::vector<float>& x, unsigned seed = 1) {
    Demod d;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> chunk(37, 811);
    std::vector<std::string> got;
    size_t i = 0;
    while (i < x.size()) {
        const size_t n = std::min<size_t>(chunk(rng), x.size() - i);
        d.push(x.data() + i, static_cast<int>(n));
        i += n;
        std::string m;
        while (d.pop(m)) got.push_back(m);
    }
    return got;
}

static std::vector<float> pad(const std::vector<float>& sig, int lead, int tail) {
    std::vector<float> x(lead, 0.0f);
    x.insert(x.end(), sig.begin(), sig.end());
    x.insert(x.end(), tail, 0.0f);
    return x;
}

int main() {
    std::printf("uchat DSP self-test  (FS=%d, %d-FSK, %.0f..%.0f Hz, %d-sample symbols)\n\n",
                FS, N_TONES, tone_hz(0), tone_hz(N_TONES - 1), SYM_LEN);

    std::printf("tone plan:\n");
    for (int k = 0; k < N_TONES; ++k) {
        const double bin = tone_hz(k) * SYM_LEN / FS;
        std::printf("  tone %d = %8.1f Hz  → bin %7.2f  %s\n", k, tone_hz(k), bin,
                    std::fabs(bin - std::round(bin)) < 1e-9 ? "exact ✓" : "NOT A BIN CENTRE ✗");
        if (std::fabs(bin - std::round(bin)) > 1e-9) ++failures;
    }
    std::printf("\n");

    std::printf("orthogonality (tone 3 present, all 8 measured):\n");
    {
        std::vector<float> t(SYM_LEN);
        for (int i = 0; i < SYM_LEN; ++i)
            t[i] = static_cast<float>(std::sin(2.0 * M_PI * tone_hz(3) * i / FS));
        double on = 0, worst_off = 0;
        for (int k = 0; k < N_TONES; ++k) {
            const double m = goertzel(t.data(), SYM_LEN, tone_hz(k));
            std::printf("   tone %d: %.6f%s\n", k, m, k == 3 ? "   ← wanted" : "");
            if (k == 3) on = m; else worst_off = std::max(worst_off, m);
        }
        std::printf("   on/off ratio: %.1f dB\n", 20 * std::log10(on / (worst_off + 1e-12)));
        check(on / (worst_off + 1e-12) > 100.0, "tones are orthogonal (>40 dB rejection)");
    }
    std::printf("\n");

    std::printf("CRC-16/CCITT-FALSE:\n");
    {
        const char* v = "123456789";
        const uint16_t c = crc16(reinterpret_cast<const uint8_t*>(v), 9);
        std::printf("   crc16(\"123456789\") = 0x%04X (expect 0x29B1)\n", c);
        check(c == 0x29B1, "CRC matches the published check value");
    }
    std::printf("\n");

    std::printf("clean round-trip:\n");
    {
        const std::string msg = "hello ultrasound";
        auto got = run(pad(modulate(msg), 5000, 5000));
        std::printf("   sent: \"%s\"  got: %zu message(s)%s%s\n", msg.c_str(), got.size(),
                    got.empty() ? "" : " = \"", got.empty() ? "" : (got[0] + "\"").c_str());
        check(got.size() == 1 && got[0] == msg, "message survives a clean channel");
    }
    std::printf("\n");

    std::printf("payload edge cases:\n");
    {
        std::vector<std::string> cases = {
            "a", "hi", std::string(MAX_PAYLOAD, 'x'),
            "chutiya code likha hai bc", "!@#$%^&*()_+-=[]{}|;':\",./<>?",
        };
        for (auto& m : cases) {
            auto got = run(pad(modulate(m), 1000, 1000));
            char lbl[64];
            std::snprintf(lbl, sizeof lbl, "payload len %zu round-trips", m.size());
            check(got.size() == 1 && got[0] == m, lbl);
        }
    }
    std::printf("\n");

    std::printf("back-to-back frames (decoder must re-arm):\n");
    {
        auto a = modulate("first"), b = modulate("second");
        std::vector<float> x(2000, 0.0f);
        x.insert(x.end(), a.begin(), a.end());
        x.insert(x.end(), 4000, 0.0f);
        x.insert(x.end(), b.begin(), b.end());
        x.insert(x.end(), 2000, 0.0f);
        auto got = run(x);
        std::printf("   got %zu message(s)\n", got.size());
        check(got.size() == 2 && got[0] == "first" && got[1] == "second",
              "two frames in one stream both decode");
    }
    std::printf("\n");

    std::printf("noise tolerance (AWGN, 200 trials each):\n");
    {
        const std::string msg = "the quick brown fox";
        for (double snr_db : {20.0, 10.0, 6.0, 3.0, 0.0}) {
            const auto sig = modulate(msg);
            double p = 0;
            for (float v : sig) p += static_cast<double>(v) * v;
            p /= static_cast<double>(sig.size());
            const double sigma = std::sqrt(p / std::pow(10.0, snr_db / 10.0));

            int ok = 0, wrong = 0;
            const int trials = 200;
            for (int t = 0; t < trials; ++t) {
                std::mt19937 rng(1000 + t);
                std::normal_distribution<double> g(0.0, sigma);
                auto x = pad(sig, 3000, 3000);
                for (auto& v : x) v += static_cast<float>(g(rng));
                auto got = run(x, 7 + t);
                if (got.size() == 1 && got[0] == msg) ++ok;
                else for (auto& s : got) if (s != msg) ++wrong;
            }
            std::printf("   SNR %5.1f dB : %3d/%d decoded, %d CORRUPT-BUT-ACCEPTED\n",
                        snr_db, ok, trials, wrong);
            if (snr_db >= 10.0) {
                char lbl[64];
                std::snprintf(lbl, sizeof lbl, "≥95%% decode at %.0f dB SNR", snr_db);
                check(ok >= trials * 95 / 100, lbl);
            }
            // The one thing that must NEVER happen: silently handing up a wrong message.
            check(wrong == 0, "CRC rejected every corrupt frame (zero false accepts)");
        }
    }
    std::printf("\n");

    // Regression: a frame whose preamble is good but whose body is junk sends the decoder
    // down the reject path. Fine-alignment can place frame_ *behind* search_, so resuming
    // at frame_+HOP could land exactly back on search_ → same false lock → forever. That
    // wedges the capture thread inside push(), which in turn hangs shutdown, because
    // stop() joins a thread that is never coming back. Every reject must move search_
    // strictly forward. Guarded by a real timeout: a hang must fail the suite, not hang it.
    std::printf("corrupt frames must reject WITHOUT hanging:\n");
    {
        auto run_guarded = [](std::vector<float> x, int* n_out) {
            std::atomic<bool> done{false};
            std::thread t([&] {
                Demod d;
                std::string m;
                int n = 0;
                for (size_t i = 0; i < x.size(); i += 512) {
                    d.push(x.data() + i, static_cast<int>(std::min<size_t>(512, x.size() - i)));
                    while (d.pop(m)) ++n;
                }
                *n_out = n;
                done = true;
            });
            for (int ms = 0; ms < 5000 && !done.load(); ++ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!done.load()) { t.detach(); return false; }   // hung — leak it, fail the test
            t.join();
            return true;
        };

        int hangs = 0, emitted = 0;
        for (int seed = 0; seed < 40; ++seed) {
            auto x = pad(modulate("payload to be mangled"), 2000, 20000);
            std::mt19937 rng(seed);
            // Corrupt everything after the preamble, leaving the preamble intact so the
            // decoder definitely locks and definitely then fails.
            std::uniform_real_distribution<double> u(-0.5, 0.5);
            const size_t body = 2000 + static_cast<size_t>(PREAMBLE_LEN) * SYM_LEN;
            for (size_t i = body; i < x.size(); ++i) x[i] = static_cast<float>(u(rng));
            int n = 0;
            if (!run_guarded(std::move(x), &n)) { ++hangs; break; }
            emitted += n;
        }
        std::printf("   %d hang(s), %d message(s) emitted from 40 corrupted frames\n",
                    hangs, emitted);
        check(hangs == 0, "decoder never hangs on a rejected frame");
        check(emitted == 0, "decoder emits nothing from a corrupted frame");
    }
    std::printf("\n");

    std::printf("pure noise must decode NOTHING (300 trials):\n");
    {
        int spurious = 0;
        for (int t = 0; t < 300; ++t) {
            std::mt19937 rng(99000 + t);
            std::normal_distribution<double> g(0.0, 0.3);
            std::vector<float> x(FS / 2);
            for (auto& v : x) v = static_cast<float>(g(rng));
            spurious += static_cast<int>(run(x, t).size());
        }
        std::printf("   %d spurious message(s) from 300× 0.5 s of noise\n", spurious);
        check(spurious == 0, "no phantom messages out of pure noise");
    }

    std::printf("\n%s  (%d failure%s)\n", failures ? "❌ FAILED" : "✅ ALL PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
