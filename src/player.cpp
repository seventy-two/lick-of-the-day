#include "player.h"

#include <algorithm>
#include <cstring>

#define TSF_IMPLEMENTATION
#include "tsf/tsf.h"
// miniaudio's implementation is compiled in miniaudio.c.
#include "miniaudio/miniaudio.h"

namespace dl {

struct Player::Device {
    ma_device device{};
    bool open = false;
};

Player::Player() : dev_(std::make_unique<Device>()) {}

Player::~Player() {
    if (dev_->open) ma_device_uninit(&dev_->device);
    if (synth_) tsf_close(synth_);
}

bool Player::OpenOffline(const void* sf2, size_t size) {
    synth_ = tsf_load_memory(sf2, int(size));
    if (!synth_) return false;
    tsf_set_output(synth_, TSF_STEREO_INTERLEAVED, kRate, -4.0f);
    tsf_set_max_voices(synth_, 256);
    return true;
}

bool Player::Open(const void* sf2, size_t size, std::string& err) {
    if (!OpenOffline(sf2, size)) {
        err = "couldn't read the soundfont";
        return false;
    }
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = kRate;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.pUserData = this;
    cfg.dataCallback = [](ma_device* d, void* out, const void*, ma_uint32 frames) {
        static_cast<Player*>(d->pUserData)->Render(static_cast<float*>(out), frames);
    };
    if (ma_device_init(nullptr, &cfg, &dev_->device) != MA_SUCCESS) {
        err = "couldn't open the sound output";
        return false;
    }
    dev_->open = true;
    if (ma_device_start(&dev_->device) != MA_SUCCESS) {
        err = "couldn't start the sound output";
        return false;
    }
    return true;
}

void Player::setupChannels() {
    for (const auto& c : seq_.channels) {
        tsf_channel_set_presetnumber(synth_, c.channel, c.program, c.drums ? 1 : 0);
        tsf_channel_set_volume(synth_, c.channel, c.volume);
        tsf_channel_set_pan(synth_, c.channel, c.pan);
        tsf_channel_set_pitchrange(synth_, c.channel, float(kWheelRange));
        tsf_channel_set_pitchwheel(synth_, c.channel, 8192);
        tsf_channel_set_tuning(synth_, c.channel, c.drums || c.channel == kClickChannel ? 0.0f : float(transpose_));
    }
}

void Player::silence() {
    if (!synth_) return;
    for (const auto& c : seq_.channels) {
        tsf_channel_note_off_all(synth_, c.channel);
        tsf_channel_set_pitchwheel(synth_, c.channel, 8192);
    }
}

void Player::Load(Sequence seq) {
    std::lock_guard<std::mutex> lock(mu_);
    playing_ = false;
    silence();
    seq_ = std::move(seq);
    next_ = nextClick_ = nextCount_ = 0;
    pos_ = 0;
    loops_ = 0;
    if (synth_) setupChannels();
}

void Player::Play() {
    std::lock_guard<std::mutex> lock(mu_);
    if (playing_ || seq_.length <= 0) return;
    if (pos_ == 0 && loops_ == 0 && click_ && !seq_.countIn.empty()) {
        pos_ = seq_.countIn.front().time;
        nextCount_ = 0;
    }
    playing_ = true;
}

void Player::Pause() {
    std::lock_guard<std::mutex> lock(mu_);
    playing_ = false;
    silence();
}

void Player::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    playing_ = false;
    silence();
    next_ = nextClick_ = nextCount_ = 0;
    pos_ = 0;
    loops_ = 0;
}

void Player::Rewind() {
    std::lock_guard<std::mutex> lock(mu_);
    silence();
    next_ = nextClick_ = nextCount_ = 0;
    pos_ = click_ && !seq_.countIn.empty() ? seq_.countIn.front().time : 0.0;
}

void Player::SetSpeed(double s) {
    std::lock_guard<std::mutex> lock(mu_);
    speed_ = std::clamp(s, 0.1, 2.0);
}

void Player::SetLoop(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    loop_ = on;
}

void Player::SetClick(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    click_ = on;
    if (!on && synth_) tsf_channel_note_off_all(synth_, kClickChannel);
}

void Player::SetSolo(int track) {
    std::lock_guard<std::mutex> lock(mu_);
    solo_ = track;
    if (!synth_) return;
    for (const auto& c : seq_.channels)
        if (!audible(c.channel)) tsf_channel_note_off_all(synth_, c.channel);
}

void Player::SetTranspose(int semitones) {
    std::lock_guard<std::mutex> lock(mu_);
    transpose_ = semitones;
    if (!synth_) return;
    for (const auto& c : seq_.channels)
        if (!c.drums && c.channel != kClickChannel) tsf_channel_set_tuning(synth_, c.channel, float(semitones));
}

bool Player::audible(int channel) const {
    if (channel == kClickChannel) return click_;
    if (solo_ < 0) return true;
    for (const auto& c : seq_.channels)
        if (c.channel == channel) return c.track == solo_;
    return false;
}

void Player::Render(float* out, unsigned frames) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!synth_) {
        std::memset(out, 0, sizeof(float) * 2 * frames);
        return;
    }
    auto apply = [&](const Event& e) {
        if (e.type == Event::Off) {
            tsf_channel_note_off(synth_, e.channel, e.key);
        } else if (e.type == Event::Wheel) {
            tsf_channel_set_pitchwheel(synth_, e.channel, e.wheel);
        } else if (audible(e.channel)) {
            tsf_channel_note_on(synth_, e.channel, e.key, e.velocity);
        }
    };
    const unsigned kBlock = 64;
    while (frames > 0) {
        unsigned n = std::min(frames, kBlock);
        if (playing_) {
            double pos = pos_;
            double end = pos + double(n) / kRate * speed_;
            if (pos < 0) {
                // The count-in, then on into the stretch.
                while (nextCount_ < seq_.countIn.size() && seq_.countIn[nextCount_].time < std::min(end, 0.0))
                    apply(seq_.countIn[nextCount_++]);
                if (end >= 0) {
                    tsf_channel_note_off_all(synth_, kClickChannel);
                    pos = 0;
                    end = std::max(end, 0.0);
                }
            }
            if (pos >= 0) {
                double stop = std::min(end, seq_.length);
                while (next_ < seq_.events.size() && seq_.events[next_].time < stop) apply(seq_.events[next_++]);
                while (nextClick_ < seq_.clicks.size() && seq_.clicks[nextClick_].time < stop)
                    apply(seq_.clicks[nextClick_++]);
                if (end >= seq_.length) {
                    // The end of the stretch: round again, or stop.
                    for (const auto& c : seq_.channels)
                        if (c.channel != kClickChannel) tsf_channel_note_off_all(synth_, c.channel);
                    for (const auto& c : seq_.channels) tsf_channel_set_pitchwheel(synth_, c.channel, 8192);
                    next_ = nextClick_ = 0;
                    loops_++;
                    if (loop_) {
                        end = 0;
                    } else {
                        playing_ = false;
                        end = 0;
                        loops_ = 0;
                    }
                }
            }
            pos_ = end;
        }
        tsf_render_float(synth_, out, int(n), 0);
        out += 2 * n;
        frames -= n;
    }
}

}  // namespace dl
