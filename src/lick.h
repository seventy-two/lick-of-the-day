// Finding a lick: the guitar solo in a score, and a short piece of it to
// learn. Section names help when the tab has them ("Solo 2"); otherwise
// it's the notes: busy single notes and double stops up the neck, bends
// and slides, not palm-muted riffs, chords or bars repeated over and over.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model.h"

namespace dl {

struct BarFeatures {
    double quarters = 4;
    int beats = 0, single = 0, chords = 0, notes = 0, palm = 0, pitches = 0;
    double tech = 0;
    double density = 0, singleFrac = 0, chordFrac = 0, palmFrac = 0, avgFret = 0;
    std::string print;  // the first voice's notes, to spot repeated bars
};

// A stretch of a track that looks like a solo. Bars are indexes, end
// exclusive.
struct Solo {
    int track = 0, start = 0, end = 0;
    double mean = 0, score = 0;
    bool labelled = false;
    std::vector<double> raw;  // each bar's score
    double before = 0;        // the score of the bar before it
};

struct Lick {
    int track = 0, startBar = 0, bars = 0, notes = 0;
    double secs = 0;
    std::string section;
    int soloStart = 0, soloEnd = 0;
};

// GuitarTracks lists the tracks that could hold a guitar solo.
std::vector<const Track*> GuitarTracks(const Score& score);

// SectionNames gives each bar the name of the section it's in.
std::vector<std::string> SectionNames(const Score& score);

BarFeatures Features(const Bar& bar, const MasterBar& mb, const Staff& staff);
double LeadScore(const BarFeatures& f, int repeats, bool unison);

// FindSolos returns the stretches that look like solos, best first.
// useLabels lets section names count (off to test the notes alone).
std::vector<Solo> FindSolos(const Score& score, bool useLabels = true, double threshold = 3.1);

// TempoAt is the tempo in force at the start of bar i.
double TempoAt(const Score& score, int i);

// PhraseBars is how many bars of this time signature make a phrase:
// four, or eight short bars, or two long ones.
int PhraseBars(const MasterBar& m);

// PhraseStarts lists the bars in [anchor, to) where a phrase begins,
// counting in phrases from anchor and again from any section marker,
// repeat or change of time signature after it.
std::vector<int> PhraseStarts(const Score& score, int anchor, int to);

// PickLick chooses a phrase of a solo that stands on its own: one starting
// on the phrase grid, whole (usually four bars) when it can be learnt in a
// sitting, else half or a quarter of one.
std::optional<Lick> PickLick(const Score& score, const Solo& solo, uint32_t seed);

// FindLick is the whole job for one score.
std::optional<Lick> FindLick(const Score& score, uint32_t seed);

// Rng is mulberry32, a small seeded generator, giving numbers in [0, 1).
class Rng {
public:
    explicit Rng(uint32_t seed) : a_(seed) {}
    double Next();

private:
    uint32_t a_;
};

}  // namespace dl
