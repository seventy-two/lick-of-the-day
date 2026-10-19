#include "model.h"

#include <algorithm>

namespace dl {

int Beat::NominalTicks() const {
    int base = kTicksPerQuarter * 4 / std::max(1, duration);
    if (dots == 1) base = base * 3 / 2;
    else if (dots >= 2) base = base * 7 / 4;
    if (tupletNumerator > 0 && tupletDenominator > 0 && tupletNumerator != tupletDenominator)
        base = base * tupletDenominator / tupletNumerator;
    return base;
}

namespace {

// tieOrigin finds the note a tied note continues: the last one on the
// same string in the beats before it, looking back up to three bars, as
// alphaTab does.
const Note* tieOrigin(const Staff& st, size_t b, size_t v, size_t i, int string) {
    for (size_t bb = b + 1; bb-- > 0 && bb + 3 >= b;) {
        if (st.bars[bb].voices.size() <= v) continue;
        const auto& beats = st.bars[bb].voices[v].beats;
        size_t k = bb == b ? i : beats.size();
        while (k-- > 0)
            for (const auto& n : beats[k].notes)
                if (n.string == string) return &n;
    }
    return nullptr;
}

Beat* nextBeat(Staff& st, size_t b, size_t v, size_t i) {
    auto& beats = st.bars[b].voices[v].beats;
    for (size_t k = i + 1; k < beats.size(); k++)
        if (beats[k].grace == Grace::None) return &beats[k];
    if (b + 1 >= st.bars.size() || st.bars[b + 1].voices.size() <= v) return nullptr;
    for (auto& nb : st.bars[b + 1].voices[v].beats)
        if (nb.grace == Grace::None) return &nb;
    return nullptr;
}

}  // namespace

void Finish(Score& score) {
    int tick = 0;
    for (size_t i = 0; i < score.masterBars.size(); i++) {
        auto& mb = score.masterBars[i];
        mb.index = int(i);
        mb.start = tick;
        tick += mb.Duration();
    }
    for (size_t t = 0; t < score.tracks.size(); t++) {
        Track& track = score.tracks[t];
        track.index = int(t);
        Staff& st = track.staff;
        st.bars.resize(score.masterBars.size());
        for (size_t b = 0; b < st.bars.size(); b++) {
            // A bar with nothing in it gets a placeholder beat, as in alphaTab.
            if (st.bars[b].voices.empty()) st.bars[b].voices.emplace_back();
            for (auto& voice : st.bars[b].voices) {
                if (voice.beats.empty()) {
                    voice.beats.emplace_back();
                    voice.beats.back().empty = true;
                }
                int at = 0;
                for (auto& beat : voice.beats) {
                    beat.start = at;
                    beat.ticks = beat.grace == Grace::None ? beat.NominalTicks() : 0;
                    at += beat.ticks;
                    if (beat.tempo > 0 && t == 0) {
                        auto& mb = score.masterBars[b];
                        double ratio = double(beat.start) / mb.Duration();
                        bool dup = false;
                        for (auto& tc : mb.tempos) dup |= tc.ratio == ratio && tc.bpm == beat.tempo;
                        if (!dup) mb.tempos.push_back({ratio, beat.tempo});
                    }
                }
            }
        }
        // Ties take their fret from the note they continue; hammer-ons and
        // pull-offs mark the note they go to.
        for (size_t b = 0; b < st.bars.size(); b++) {
            for (size_t v = 0; v < st.bars[b].voices.size(); v++) {
                auto& beats = st.bars[b].voices[v].beats;
                for (size_t i = 0; i < beats.size(); i++) {
                    for (auto& n : beats[i].notes) {
                        if (n.tieDestination) {
                            if (const Note* o = tieOrigin(st, b, v, i, n.string)) n.fret = o->fret;
                            else n.tieDestination = false;
                        }
                        if (n.hammerPullOrigin && beats[i].grace == Grace::None) {
                            if (Beat* nb = nextBeat(st, b, v, i)) {
                                for (auto& nn : nb->notes)
                                    if (nn.string == n.string) nn.hammerPullDestination = true;
                            }
                        }
                    }
                }
            }
        }
    }
    for (auto& mb : score.masterBars)
        std::stable_sort(mb.tempos.begin(), mb.tempos.end(),
                         [](const TempoChange& a, const TempoChange& b) { return a.ratio < b.ratio; });
}

}  // namespace dl
