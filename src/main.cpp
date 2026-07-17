#include "audio.h"
#include "dsp.h"
#include "http.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace uchat;

namespace {

std::atomic<bool> g_quit{false};
void on_sig(int) { g_quit = true; }

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* USAGE =
    "uchat — near-ultrasonic acoustic chat (18.25–20 kHz, 8-FSK, 150 bps)\n"
    "\n"
    "  --capture  DEV   ALSA capture device (default \"default\" — via PipeWire)\n"
    "                   Use hw:0,6 for the raw DMIC with no server in the path, but note\n"
    "                   it needs EXCLUSIVE access: if any app (a browser tab, a call) is\n"
    "                   holding the mic, PipeWire keeps the device open and this fails\n"
    "                   with 'Device or resource busy'. The default path coexists.\n"
    "  --playback DEV   ALSA playback device  (default default — PipeWire)\n"
    "  --port     N     HTTP port on 127.0.0.1 (default 8080)\n"
    "  --gain     G     tx amplitude 0..1 (default 0.2). Louder is NOT better: the mic\n"
    "                   is centimetres from the speaker and clips, which kills decoding.\n"
    "  --timeout  MS    extra ms to wait for a message to come back after its burst has\n"
    "                   finished playing, before declaring it lost (default 2000)\n"
    "  --web      DIR   directory holding index.html (default ./web)\n"
    "  --bind     ADDR  interface to bind (default 127.0.0.1). Use 0.0.0.0 to let other\n"
    "                   devices reach this host. That exposes this process to the LAN;\n"
    "                   it is opt-in on purpose.\n"
    "  --tls      C K   serve HTTPS with cert C and key K. Browsers only grant microphone\n"
    "                   access in a secure context, and anything that is not localhost\n"
    "                   needs HTTPS — a phone on plain http:// is refused the mic.\n"
    "\n"
    "  Do NOT point --capture at hw:0,7: it is the 16 kHz DMIC, Nyquist 8 kHz,\n"
    "  and the signal will not exist. uchat refuses any device that isn't 48 kHz.\n";

} // namespace

int main(int argc, char** argv) {
    std::string cap_dev = "default", play_dev = "default", web = "web";
    int port = 8080;
    // Speaker and mic are centimetres apart in one chassis, so "loud" saturates the mic
    // rather than helping. Measured on this box: an amplitude that lands the capture peak
    // near 0.89 decodes; anything that pins it at 1.0 fails outright. Tune with --gain.
    double gain = 0.2;
    // Roughly 4% of frames do not decode. Without a deadline a sent message just sits
    // there looking like it is still in flight, which is indistinguishable from a hang.
    // A lost message must SAY it is lost.
    long long   timeout_ms = 2000;
    std::string bind_addr  = "127.0.0.1";   // opt in to the LAN explicitly, never by default
    std::string cert, key;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--capture")  cap_dev  = next();
        else if (a == "--playback") play_dev = next();
        else if (a == "--web")      web      = next();
        else if (a == "--port")     port     = std::atoi(next().c_str());
        else if (a == "--gain")     gain     = std::atof(next().c_str());
        else if (a == "--timeout")  timeout_ms = std::atoll(next().c_str());
        else if (a == "--bind")     bind_addr  = next();
        else if (a == "--tls")      { cert = next(); key = next(); }
        else { std::fputs(USAGE, stderr); return a == "-h" || a == "--help" ? 0 : 2; }
    }

    std::signal(SIGINT,  on_sig);
    std::signal(SIGTERM, on_sig);
    std::signal(SIGPIPE, SIG_IGN);

    std::printf("uchat\n");
    std::printf("  band      : %.0f–%.0f Hz, %d-FSK, %d-sample symbols → %.0f bps\n",
                tone_hz(0), tone_hz(N_TONES - 1), N_TONES, SYM_LEN,
                static_cast<double>(FS) / SYM_LEN * BITS_PER_SYM);
    std::printf("  capture   : %s\n", cap_dev.c_str());
    std::printf("  playback  : %s  (tx gain %.2f)\n", play_dev.c_str(), gain);

    std::string err;
    Capture cap;
    if (!cap.open(cap_dev, &err)) {
        std::fprintf(stderr, "\n[FATAL] capture: %s\n", err.c_str());
        return 1;
    }
    std::printf("  capture   : opened, %u ch @ %d Hz (verified, not assumed)\n",
                cap.channels(), FS);

    Playback play;
    if (!play.open(play_dev, &err)) {
        std::fprintf(stderr, "\n[FATAL] playback: %s\n", err.c_str());
        return 1;
    }
    std::printf("  playback  : opened @ %d Hz\n", FS);

    Demod       demod;
    std::mutex  dm;
    Server      srv;
    std::atomic<long long> last_tx_ms{0};

    // Messages that have been transmitted but not yet heard back. Matched by text: the
    // demodulator hands up a string with no notion of which burst produced it, and with
    // several messages in flight "the last one" is a guess.
    struct Pending { std::string text; long long deadline; };
    std::vector<Pending> pending;
    std::mutex           pmx;

    if (!cap.start([&](const float* x, int n) {
            std::string msg;
            std::lock_guard<std::mutex> lk(dm);
            demod.push(x, n);
            while (demod.pop(msg)) {
                {   // it came back — cancel its deadline
                    std::lock_guard<std::mutex> lk2(pmx);
                    for (auto it = pending.begin(); it != pending.end(); ++it)
                        if (it->text == msg) { pending.erase(it); break; }
                }
                const long long t  = last_tx_ms.load();
                const long long rt = t ? now_ms() - t : -1;
                std::printf("[rx] \"%s\"  (score %.2f%s)\n", msg.c_str(), demod.last_score(),
                            rt >= 0 ? (", " + std::to_string(rt) + " ms since tx").c_str() : "");
                std::fflush(stdout);
                char j[1024];
                std::snprintf(j, sizeof j, "{\"type\":\"rx\",\"text\":\"%s\",\"score\":%.3f}",
                              json_escape(msg).c_str(), demod.last_score());
                srv.broadcast(j);
            }
        }, &err)) {
        std::fprintf(stderr, "\n[FATAL] capture start: %s\n", err.c_str());
        return 1;
    }

    if (!play.start(&err)) {
        std::fprintf(stderr, "\n[FATAL] playback start: %s\n", err.c_str());
        return 1;
    }

    if (!srv.start(port, web, [&](const std::string& text) {
            std::string m = text.substr(0, MAX_PAYLOAD);
            auto burst = modulate(m);
            for (auto& v : burst) v = static_cast<float>(v * gain);
            last_tx_ms = now_ms();
            {
                const long long dur_ms =
                    static_cast<long long>(burst.size()) * 1000 / FS;
                std::lock_guard<std::mutex> lk(pmx);
                pending.push_back({m, now_ms() + dur_ms + timeout_ms});
            }
            play.enqueue(burst);
            std::printf("[tx] \"%s\"  (%zu samples, %.2f s)\n", m.c_str(), burst.size(),
                        static_cast<double>(burst.size()) / FS);
            std::fflush(stdout);
            char j[1024];
            std::snprintf(j, sizeof j, "{\"type\":\"tx\",\"text\":\"%s\",\"dur\":%.2f}",
                          json_escape(m).c_str(), static_cast<double>(burst.size()) / FS);
            srv.broadcast(j);
        }, &err, bind_addr, cert, key)) {
        std::fprintf(stderr, "\n[FATAL] http: %s\n", err.c_str());
        return 1;
    }

    std::printf("  timeout   : burst length + %lld ms\n", timeout_ms);
    if (bind_addr != "127.0.0.1")
        std::printf("  ** bound to %s — reachable from the network **\n", bind_addr.c_str());
    std::printf("\n  → %s://%s:%d   (Ctrl-C to stop)\n\n",
                srv.tls() ? "https" : "http", bind_addr.c_str(), port);
    std::fflush(stdout);

    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::vector<std::string> lost;
        {
            const long long t = now_ms();
            std::lock_guard<std::mutex> lk(pmx);
            for (auto it = pending.begin(); it != pending.end();) {
                if (t >= it->deadline) { lost.push_back(it->text); it = pending.erase(it); }
                else ++it;
            }
        }
        for (const auto& text : lost) {
            std::printf("[timeout] \"%s\"  never came back\n", text.c_str());
            std::fflush(stdout);
            char j[1024];
            std::snprintf(j, sizeof j, "{\"type\":\"timeout\",\"text\":\"%s\"}",
                          json_escape(text).c_str());
            srv.broadcast(j);
        }
    }

    std::printf("\nstopping…\n");
    srv.stop();
    cap.stop();
    play.stop();
    return 0;
}
