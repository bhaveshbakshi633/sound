#include "audio.h"
#include "dsp.h"
#include <alsa/asoundlib.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace uchat {

namespace {

// Configure a PCM and REFUSE to proceed at any rate but 48000.
// snd_pcm_hw_params_set_rate_near() will happily hand you 16000 and return success —
// that is the single silent failure that kills this entire project, so we set the rate
// exactly, then read it back and check. No _near, no hoping.
bool configure(snd_pcm_t* h, snd_pcm_format_t fmt, unsigned* ch, unsigned* rate_out,
               std::string* err) {
    snd_pcm_hw_params_t* p = nullptr;
    snd_pcm_hw_params_alloca(&p);
    int rc;

    if ((rc = snd_pcm_hw_params_any(h, p)) < 0)
        { *err = std::string("hw_params_any: ") + snd_strerror(rc); return false; }
    if ((rc = snd_pcm_hw_params_set_access(h, p, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        { *err = std::string("set_access: ") + snd_strerror(rc); return false; }
    if ((rc = snd_pcm_hw_params_set_format(h, p, fmt)) < 0)
        { *err = std::string("set_format: ") + snd_strerror(rc); return false; }
    if ((rc = snd_pcm_hw_params_set_channels(h, p, *ch)) < 0) {
        unsigned c = *ch;                       // fall back to whatever the device insists on
        if ((rc = snd_pcm_hw_params_set_channels_near(h, p, &c)) < 0)
            { *err = std::string("set_channels: ") + snd_strerror(rc); return false; }
        *ch = c;
    }

    unsigned rate = static_cast<unsigned>(FS);
    if ((rc = snd_pcm_hw_params_set_rate(h, p, rate, 0)) < 0)
        { *err = std::string("device refused 48000 Hz: ") + snd_strerror(rc); return false; }

    snd_pcm_uframes_t period = 1024, buffer = 8192;
    snd_pcm_hw_params_set_period_size_near(h, p, &period, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(h, p, &buffer);

    if ((rc = snd_pcm_hw_params(h, p)) < 0)
        { *err = std::string("hw_params: ") + snd_strerror(rc); return false; }

    unsigned got = 0;
    int dir = 0;
    if ((rc = snd_pcm_hw_params_get_rate(p, &got, &dir)) < 0)
        { *err = std::string("get_rate: ") + snd_strerror(rc); return false; }
    if (got != static_cast<unsigned>(FS)) {
        char b[160];
        std::snprintf(b, sizeof b,
                      "REFUSING: negotiated rate is %u Hz, not %d. At %u Hz the Nyquist "
                      "limit is %u Hz and the 18-20 kHz band does not exist.",
                      got, FS, got, got / 2);
        *err = b;
        return false;
    }
    *rate_out = got;
    return true;
}

} // namespace

// ── Capture ─────────────────────────────────────────────────────────────────

Capture::~Capture() { stop(); if (h_) snd_pcm_close(h_); }

bool Capture::open(const std::string& dev, std::string* err) {
    int rc = snd_pcm_open(&h_, dev.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) { *err = "open " + dev + ": " + snd_strerror(rc); h_ = nullptr; return false; }
    unsigned rate = 0;
    ch_ = 2;
    if (!configure(h_, SND_PCM_FORMAT_S32_LE, &ch_, &rate, err)) {
        snd_pcm_close(h_); h_ = nullptr; return false;
    }
    return true;
}

bool Capture::start(std::function<void(const float*, int)> cb, std::string* err) {
    if (!h_) { *err = "capture not open"; return false; }
    int rc = snd_pcm_prepare(h_);
    if (rc < 0) { *err = std::string("prepare: ") + snd_strerror(rc); return false; }
    cb_ = std::move(cb);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void Capture::loop() {
    const int frames = 1024;
    std::vector<int32_t> raw(static_cast<size_t>(frames) * ch_);
    std::vector<float>   mono(frames);

    while (run_.load()) {
        const snd_pcm_sframes_t n = snd_pcm_readi(h_, raw.data(), frames);
        if (n == -EPIPE) { snd_pcm_prepare(h_); continue; }        // overrun: resync, keep going
        if (n < 0) {
            if (snd_pcm_recover(h_, static_cast<int>(n), 1) < 0) {
                std::fprintf(stderr, "[capture] fatal: %s\n", snd_strerror(static_cast<int>(n)));
                break;
            }
            continue;
        }
        if (n == 0) continue;
        // Channel 0 only. Averaging a mic array would sum two spatially-separated
        // captures of an 18 mm wavelength and comb-filter our own signal.
        for (snd_pcm_sframes_t i = 0; i < n; ++i)
            mono[static_cast<size_t>(i)] =
                static_cast<float>(raw[static_cast<size_t>(i) * ch_]) / 2147483648.0f;
        if (cb_) cb_(mono.data(), static_cast<int>(n));
    }
}

void Capture::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
}

// ── Playback ────────────────────────────────────────────────────────────────

Playback::~Playback() { stop(); if (h_) snd_pcm_close(h_); }

bool Playback::open(const std::string& dev, std::string* err) {
    int rc = snd_pcm_open(&h_, dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) { *err = "open " + dev + ": " + snd_strerror(rc); h_ = nullptr; return false; }
    unsigned rate = 0;
    ch_ = 2;
    if (!configure(h_, SND_PCM_FORMAT_S16_LE, &ch_, &rate, err)) {
        snd_pcm_close(h_); h_ = nullptr; return false;
    }
    return true;
}

bool Playback::start(std::string* err) {
    if (!h_) { *err = "playback not open"; return false; }
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void Playback::enqueue(std::vector<float> burst) {
    { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(burst)); }
    cv_.notify_one();
}

void Playback::loop() {
    while (run_.load()) {
        std::vector<float> burst;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return !q_.empty() || !run_.load(); });
            if (!run_.load()) return;
            burst = std::move(q_.front());
            q_.pop_front();
        }
        busy_ = true;

        std::vector<int16_t> pcm(burst.size() * ch_);
        for (size_t i = 0; i < burst.size(); ++i) {
            const float v = std::clamp(burst[i], -1.0f, 1.0f);
            const int16_t s = static_cast<int16_t>(v * 32767.0f);
            for (unsigned c = 0; c < ch_; ++c) pcm[i * ch_ + c] = s;
        }

        snd_pcm_prepare(h_);
        size_t off = 0;
        while (off < burst.size() && run_.load()) {
            const snd_pcm_sframes_t n =
                snd_pcm_writei(h_, pcm.data() + off * ch_, burst.size() - off);
            if (n == -EPIPE) { snd_pcm_prepare(h_); continue; }
            if (n < 0) {
                if (snd_pcm_recover(h_, static_cast<int>(n), 1) < 0) {
                    std::fprintf(stderr, "[playback] fatal: %s\n",
                                 snd_strerror(static_cast<int>(n)));
                    break;
                }
                continue;
            }
            off += static_cast<size_t>(n);
        }
        snd_pcm_drain(h_);
        busy_ = false;
    }
}

void Playback::stop() {
    run_ = false;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

} // namespace uchat
