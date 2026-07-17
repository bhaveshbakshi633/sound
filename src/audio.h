#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct _snd_pcm snd_pcm_t;   // keep <alsa/asoundlib.h> out of every TU

namespace uchat {

// Capture straight off the ALSA hardware device. NOT through the browser, NOT through a
// resampler. Verified on this box: hw:0,6 (DMIC) opens at 48 kHz alongside PipeWire, and
// no echo-cancel module is loaded — which matters, because AEC exists specifically to
// delete your own speaker output from your mic, and our entire signal IS our speaker output.
class Capture {
public:
    ~Capture();
    bool open(const std::string& dev, std::string* err);   // hard-fails unless rate == 48000
    bool start(std::function<void(const float*, int)> cb, std::string* err);
    void stop();
    unsigned channels() const { return ch_; }

private:
    void loop();

    snd_pcm_t*                              h_   = nullptr;
    std::thread                             th_;
    std::atomic<bool>                       run_{false};
    std::function<void(const float*, int)>  cb_;
    unsigned                                ch_  = 2;
};

// Playback via "default" (PipeWire) so it coexists with the rest of the desktop instead of
// seizing the card. Proven to pass 18–22 kHz intact by the sweep measurement in README.md.
class Playback {
public:
    ~Playback();
    bool open(const std::string& dev, std::string* err);
    bool start(std::string* err);
    void enqueue(std::vector<float> burst);
    void stop();
    bool busy() const { return busy_.load(); }

private:
    void loop();

    snd_pcm_t*                      h_ = nullptr;
    std::thread                     th_;
    std::atomic<bool>               run_{false};
    std::atomic<bool>               busy_{false};
    std::deque<std::vector<float>>  q_;
    std::mutex                      m_;
    std::condition_variable         cv_;
    unsigned                        ch_ = 2;
};

} // namespace uchat
