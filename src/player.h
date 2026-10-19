// The player: a soundfont synth (TinySoundFont) fed by a Sequence on the
// audio thread (miniaudio), looping it if asked, at any speed.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "sequence.h"

struct tsf;

namespace dl {

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Open loads the soundfont (an .sf2 in memory, which must outlive the
    // player) and opens the default audio output.
    bool Open(const void* sf2, size_t size, std::string& err);
    // OpenOffline loads the soundfont without a sound output, for tests
    // that call Render themselves.
    bool OpenOffline(const void* sf2, size_t size);

    // Load replaces what's played, stopping first.
    void Load(Sequence seq);

    void Play();
    void Pause();
    void Stop();  // and back to the start
    // Rewind goes back to the top (with the count-in, if the click's on)
    // but keeps counting the loops, so a ramp carries on.
    void Rewind();
    bool Playing() const { return playing_; }

    void SetSpeed(double s);  // 1 is the tab's tempo
    void SetLoop(bool on);
    void SetClick(bool on);   // metronome, with a bar's count-in when starting
    // SetSolo plays only one track (-1 for all of them).
    void SetSolo(int track);
    // SetTranspose moves every part but the drums and the click up or down,
    // in semitones.
    void SetTranspose(int semitones);

    // Position is where playing is, in seconds into the stretch at full
    // speed; negative during the count-in.
    double Position() const { return pos_; }
    // Loops counts the times round since starting from the top.
    int Loops() const { return loops_; }
    const Sequence& Seq() const { return seq_; }

    // Render fills out with interleaved stereo frames; the audio thread's
    // work, public for tests.
    void Render(float* out, unsigned frames);

    static constexpr int kRate = 44100;

private:
    void setupChannels();
    void silence();
    bool audible(int channel) const;

    struct Device;
    std::unique_ptr<Device> dev_;
    tsf* synth_ = nullptr;
    std::mutex mu_;
    Sequence seq_;
    size_t next_ = 0, nextClick_ = 0, nextCount_ = 0;
    std::atomic<double> pos_{0};
    std::atomic<int> loops_{0};
    std::atomic<bool> playing_{false};
    double speed_ = 1;
    bool loop_ = true, click_ = false;
    int solo_ = -1;
    int transpose_ = 0;
};

}  // namespace dl
