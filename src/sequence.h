// Turning a stretch of the score into timed synth events: notes, bends,
// slides and vibrato as pitch-wheel moves, and metronome clicks.
#pragma once

#include <vector>

#include "model.h"

namespace dl {

struct Event {
    enum Type : uint8_t { Off, On, Wheel };
    double time = 0;  // seconds from the start of the stretch, at full speed
    Type type = On;
    int channel = 0;
    int key = 0;
    float velocity = 0;  // 0–1
    int wheel = 8192;    // 0–16383, 8192 is no bend
};

// A synth channel per track; the metronome has its own.
struct Channel {
    int channel = 0;
    int track = -1;  // -1 for the metronome
    int program = 0;
    bool drums = false;
    float volume = 1;  // 0–1
    float pan = 0.5f;  // 0 left, 1 right
};

constexpr int kClickChannel = 99;
// Pitch-wheel range on guitar channels, in semitones: room for two-tone
// bends and slides.
constexpr int kWheelRange = 12;

struct Sequence {
    std::vector<Event> events;   // sorted by time
    std::vector<Event> clicks;   // the metronome, one per beat
    std::vector<Event> countIn;  // a bar of clicks before the start, negative times
    std::vector<Channel> channels;
    double length = 0;  // seconds
    int startTick = 0, endTick = 0;  // the stretch, in the score's ticks

    // Tempo map: where each tempo starts.
    struct Segment {
        int tick;
        double sec, secPerTick;
    };
    std::vector<Segment> tempo;

    double TickToSec(int tick) const;
    int SecToTick(double sec) const;
};

// BuildSequence makes the events for bars [startBar, endBar) of every
// track, played as written (repeats aren't followed).
Sequence BuildSequence(const Score& score, int startBar, int endBar);

// PitchOf is the sounding MIDI note of a note on a staff, harmonics
// included.
int PitchOf(const Staff& staff, const Note& note);

}  // namespace dl
