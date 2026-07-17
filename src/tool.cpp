// uchat diagnostics. Splits "it doesn't work" into answerable questions.
//
//   ./tool emit  out.wav "msg"     modulate a message to a 48 kHz mono WAV
//   ./tool decode in.wav           run the real Demod over a WAV, offline
//   ./tool probe DEV SECONDS       capture raw, report PER-CHANNEL rms + 18-20 kHz energy
//
// probe exists because the demod reads ONE channel of a mic array. If that channel is
// dead, everything downstream looks like a DSP bug and isn't.
#include "dsp.h"
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace uchat;

namespace {

void write_wav(const std::string& path, const std::vector<float>& x) {
    std::vector<int16_t> pcm(x.size());
    for (size_t i = 0; i < x.size(); ++i)
        pcm[i] = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, x[i])) * 32767.0f);
    const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(FS); u32(FS * 2); u16(2); u16(16);
    f.write("data", 4); u32(data);
    f.write(reinterpret_cast<const char*>(pcm.data()), data);
}

bool read_wav(const std::string& path, std::vector<float>& x, unsigned& rate, unsigned& ch) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char id[4];
    f.read(id, 4);
    if (std::strncmp(id, "RIFF", 4)) return false;
    f.seekg(8);
    f.read(id, 4);
    if (std::strncmp(id, "WAVE", 4)) return false;
    uint16_t bits = 16;
    ch = 1; rate = 0;
    for (;;) {
        char cid[4];
        uint32_t sz;
        if (!f.read(cid, 4) || !f.read(reinterpret_cast<char*>(&sz), 4)) return false;
        if (!std::strncmp(cid, "fmt ", 4)) {
            uint16_t fmt, c; uint32_t r, br; uint16_t ba, b;
            f.read(reinterpret_cast<char*>(&fmt), 2); f.read(reinterpret_cast<char*>(&c), 2);
            f.read(reinterpret_cast<char*>(&r), 4);   f.read(reinterpret_cast<char*>(&br), 4);
            f.read(reinterpret_cast<char*>(&ba), 2);  f.read(reinterpret_cast<char*>(&b), 2);
            ch = c; rate = r; bits = b;
            f.seekg(sz - 16, std::ios::cur);
        } else if (!std::strncmp(cid, "data", 4)) {
            const size_t n = sz / (bits / 8);
            x.resize(n / ch);
            for (size_t i = 0; i < n; ++i) {
                double v = 0;
                if (bits == 16) { int16_t s; f.read(reinterpret_cast<char*>(&s), 2); v = s / 32768.0; }
                else if (bits == 32) { int32_t s; f.read(reinterpret_cast<char*>(&s), 4); v = s / 2147483648.0; }
                else return false;
                if (i % ch == 0 && i / ch < x.size()) x[i / ch] = static_cast<float>(v);
            }
            return true;
        } else {
            f.seekg(sz, std::ios::cur);
        }
    }
}

int cmd_probe(const char* dev, double secs) {
    snd_pcm_t* h = nullptr;
    int rc = snd_pcm_open(&h, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) { std::fprintf(stderr, "open %s: %s\n", dev, snd_strerror(rc)); return 1; }

    snd_pcm_hw_params_t* p;
    snd_pcm_hw_params_alloca(&p);
    snd_pcm_hw_params_any(h, p);
    snd_pcm_hw_params_set_access(h, p, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(h, p, SND_PCM_FORMAT_S32_LE);
    unsigned ch = 2;
    snd_pcm_hw_params_set_channels_near(h, p, &ch);
    if (snd_pcm_hw_params_set_rate(h, p, FS, 0) < 0) {
        std::fprintf(stderr, "%s refused 48000 Hz\n", dev); return 1;
    }
    if ((rc = snd_pcm_hw_params(h, p)) < 0) { std::fprintf(stderr, "hw_params: %s\n", snd_strerror(rc)); return 1; }
    unsigned got = 0; int dir = 0;
    snd_pcm_hw_params_get_rate(p, &got, &dir);
    std::printf("device %s: %u ch @ %u Hz\n", dev, ch, got);
    if (got != FS) { std::fprintf(stderr, "REFUSING: %u Hz != 48000\n", got); return 1; }

    const int total = static_cast<int>(secs * FS);
    std::vector<std::vector<float>> chans(ch);
    std::vector<int32_t> raw(1024 * ch);
    snd_pcm_prepare(h);
    int have = 0;
    while (have < total) {
        const snd_pcm_sframes_t n = snd_pcm_readi(h, raw.data(), 1024);
        if (n == -EPIPE) { snd_pcm_prepare(h); continue; }
        if (n < 0) { std::fprintf(stderr, "read: %s\n", snd_strerror(static_cast<int>(n))); return 1; }
        for (snd_pcm_sframes_t i = 0; i < n; ++i)
            for (unsigned c = 0; c < ch; ++c)
                chans[c].push_back(static_cast<float>(raw[i * ch + c]) / 2147483648.0f);
        have += static_cast<int>(n);
    }
    snd_pcm_close(h);

    std::printf("\n%-4s %10s %10s   %s\n", "ch", "rms", "peak", "peak tone in 18–20 kHz");
    std::printf("---------------------------------------------------------------\n");
    for (unsigned c = 0; c < ch; ++c) {
        double sum = 0, pk = 0;
        for (float v : chans[c]) { sum += double(v) * v; pk = std::max(pk, std::fabs((double)v)); }
        const double rms = std::sqrt(sum / chans[c].size());

        double best = 0, bestf = 0;
        for (double f = 18000; f <= 20050; f += 50) {
            double m = 0;
            for (size_t o = 0; o + SYM_LEN <= chans[c].size(); o += SYM_LEN * 4)
                m = std::max(m, goertzel(chans[c].data() + o, SYM_LEN, f));
            if (m > best) { best = m; bestf = f; }
        }
        std::printf("%-4u %10.6f %10.6f   %8.0f Hz  mag %.6f  %s\n", c, rms, pk, bestf, best,
                    rms < 1e-6 ? "  ← CHANNEL IS DEAD SILENT" : "");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage:\n"
                   "  tool emit  out.wav \"msg\"\n"
                   "  tool emitraw out.f32 \"msg\"\n"
                   "  tool decode in.wav\n"
                   "  tool probe DEV SECONDS\n", stderr);
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "emit" && argc >= 4) {
        auto x = modulate(argv[3]);
        const double g = (argc >= 5) ? std::atof(argv[4]) : 1.0;
        for (auto& v : x) v = static_cast<float>(v * g);
        write_wav(argv[2], x);
        std::printf("wrote %s: %zu samples, %.2f s, gain %.3f, msg=\"%s\"\n",
                    argv[2], x.size(), double(x.size()) / FS, g, argv[3]);
        return 0;
    }

    // Raw float32 dump. Exists so the WASM build can be compared against the native one
    // bit-for-bit: going through a 16-bit WAV injects a quantisation step of 1/32767 and
    // makes two identical implementations look like they differ.
    if (cmd == "emitraw" && argc >= 4) {
        const auto x = modulate(argv[3]);
        std::ofstream f(argv[2], std::ios::binary);
        f.write(reinterpret_cast<const char*>(x.data()),
                static_cast<std::streamsize>(x.size() * sizeof(float)));
        std::printf("wrote %s: %zu float32 samples\n", argv[2], x.size());
        return 0;
    }

    if (cmd == "decode" && argc >= 3) {
        std::vector<float> x;
        unsigned rate = 0, ch = 0;
        if (!read_wav(argv[2], x, rate, ch)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        std::printf("read %s: %zu frames, %u ch @ %u Hz\n", argv[2], x.size(), ch, rate);
        if (rate != FS) { std::fprintf(stderr, "REFUSING: %u Hz != 48000\n", rate); return 1; }

        double sum = 0, pk = 0;
        for (float v : x) { sum += double(v) * v; pk = std::max(pk, std::fabs((double)v)); }
        std::printf("rms %.6f  peak %.6f%s\n", std::sqrt(sum / x.size()), pk,
                    pk > 0.99 ? "  ← CLIPPED, results are not trustworthy" : "");

        // What does the band actually look like?
        std::printf("\nper-tone peak magnitude across the file:\n");
        for (int k = 0; k < N_TONES; ++k) {
            double m = 0;
            for (size_t o = 0; o + SYM_LEN <= x.size(); o += HOP)
                m = std::max(m, goertzel(x.data() + o, SYM_LEN, tone_hz(k)));
            std::printf("  tone %d %8.0f Hz : %.6f\n", k, tone_hz(k), m);
        }

        Demod d;
        std::string m;
        int n = 0;
        // Track the max ourselves. Demod::last_score() is the LAST score computed, not the
        // best: after a frame decodes the demod keeps scanning and last_score_ ends up
        // holding whatever the final noise candidate scored. Printing it as "best score"
        // is a diagnostic that lies — it reported 0.55 on a run that had already decoded
        // nothing, and 0.11 on runs that decoded perfectly, and cost an hour of chasing it.
        double best = 0.0;
        for (size_t i = 0; i < x.size(); i += 512) {
            d.push(x.data() + i, static_cast<int>(std::min<size_t>(512, x.size() - i)));
            best = std::max(best, d.last_score());
            while (d.pop(m)) { std::printf("\n[DECODED] \"%s\"\n", m.c_str()); ++n; }
        }
        std::printf("\nbest preamble score seen: %.3f  (threshold %.2f, chance %.3f)\n",
                    best, DETECT_THRESH, 1.0 / N_TONES);
        std::printf("%s\n", n ? "✅ decoded" : "❌ nothing decoded");
        return n ? 0 : 1;
    }

    if (cmd == "probe" && argc >= 4) return cmd_probe(argv[2], std::atof(argv[3]));

    std::fputs("bad args\n", stderr);
    return 2;
}
