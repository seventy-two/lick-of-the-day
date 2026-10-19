// The tab model: what the Guitar Pro readers produce and everything else
// works from. It follows alphaTab's model where the two overlap (strings
// numbered from the lowest, 1-based; tuning listed from the top string), so
// results can be checked against alphaTab's.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dl {

constexpr int kTicksPerQuarter = 960;

enum class Slide : uint8_t { None, Shift, Legato, OutDown, OutUp, IntoFromBelow, IntoFromAbove, PickDown, PickUp };
enum class Harmonic : uint8_t { None, Natural, Artificial, Tap, Pinch, Semi, Feedback };
enum class Grace : uint8_t { None, BeforeBeat, OnBeat };

// A bend's shape: offsets 0–60 along the note, values in quarter tones.
struct BendPoint {
    int offset = 0;
    int value = 0;
};

struct Note {
    int string = 0;  // 1 is the lowest string
    int fret = 0;
    bool tieDestination = false;
    bool dead = false;
    bool ghost = false;
    bool palmMute = false;
    bool letRing = false;
    bool hammerPullOrigin = false;
    bool hammerPullDestination = false;  // filled in after reading
    bool vibrato = false;
    bool staccato = false;
    Slide slideOut = Slide::None;
    Slide slideIn = Slide::None;
    Harmonic harmonic = Harmonic::None;
    std::vector<BendPoint> bend;

    bool HasBend() const {
        for (const auto& p : bend)
            if (p.value != 0) return true;
        return false;
    }
};

struct Beat {
    int duration = 4;  // 1 whole, 2 half, 4 quarter, 8 eighth …
    int dots = 0;
    int tupletNumerator = 1, tupletDenominator = 1;
    bool empty = false;  // a placeholder, not even a rest
    Grace grace = Grace::None;
    bool tap = false, slap = false, pop = false;
    bool whammyVibrato = false;
    int tremoloPicking = 0;  // 0, or the note value it's picked in (8, 16, 32)
    std::vector<Note> notes;
    double tempo = 0;  // a tempo change at this beat, if > 0
    // Filled in after reading: where the beat starts in its bar and how
    // long it lasts, in ticks. Grace notes take no time here.
    int start = 0;
    int ticks = 0;

    bool IsRest() const { return empty || notes.empty(); }
    // Ticks is how long the beat's note value lasts.
    int NominalTicks() const;
};

struct Voice {
    std::vector<Beat> beats;
};

struct Bar {
    std::vector<Voice> voices;
};

struct Staff {
    std::vector<int> tuning;  // MIDI notes, top string first
    int capo = 0;
    bool percussion = false;
    std::vector<Bar> bars;

    // StringPitch is the open pitch of string s (1 = lowest).
    int StringPitch(int s) const {
        int i = int(tuning.size()) - s;
        return i >= 0 && i < int(tuning.size()) ? tuning[i] : 0;
    }
};

struct Track {
    int index = 0;
    std::string name;
    int program = 25;
    int bank = 0;
    int volume = 13;   // 0–16, Guitar Pro's scale
    int balance = 8;   // 0–16
    bool mute = false;
    Staff staff;  // only the first staff is kept
};

struct TempoChange {
    double ratio = 0;  // where in the bar, 0–1
    double bpm = 120;
};

struct MasterBar {
    int index = 0;
    int numerator = 4, denominator = 4;
    bool repeatStart = false;
    int repeatCount = 0;  // > 0 on a bar that closes a repeat
    int alternateEndings = 0;  // bit mask
    bool hasSection = false;
    std::string section;  // its text, if any
    std::vector<TempoChange> tempos;
    int start = 0;  // ticks from the start of the score, as written

    int Duration() const { return numerator * kTicksPerQuarter * 4 / denominator; }
    double Quarters() const { return numerator * 4.0 / denominator; }
};

struct Score {
    std::string title, artist, album;
    double tempo = 120;
    std::vector<MasterBar> masterBars;
    std::vector<Track> tracks;
};

// Finish fills in what readers leave: bar starts, beat timing, tied notes'
// frets, hammer-on destinations.
void Finish(Score& score);

}  // namespace dl
