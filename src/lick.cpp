#include "lick.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <set>

namespace dl {
namespace {

const std::regex kSoloSection("solo|lead|shred|improv", std::regex::icase);
const std::regex kOtherSection("verse|chorus|riff|intro|outro|break|bridge|interlude|clean|chug|vocal",
                               std::regex::icase);
const std::regex kSoloTrack("solo|lead", std::regex::icase);
const std::regex kRhythmTrack("rhythm|rhy\\b", std::regex::icase);
const std::regex kNotGuitarTrack(
    "voc|voice|sing|bass|drum|perc|key|piano|synth|organ|string|violin|cello|choir|pad|fx|effect|sax|trumpet|horn|"
    "flute",
    std::regex::icase);

bool has(const std::string& s, const std::regex& re) { return std::regex_search(s, re); }

}  // namespace

std::vector<const Track*> GuitarTracks(const Score& score) {
    std::vector<const Track*> out;
    for (const auto& t : score.tracks) {
        const Staff& st = t.staff;
        if (st.percussion || st.tuning.size() < 6) continue;
        if (t.program < 24 || t.program > 31) continue;  // General MIDI guitars
        if (has(t.name, kNotGuitarTrack) && !has(t.name, kSoloTrack)) continue;
        out.push_back(&t);
    }
    return out;
}

std::vector<std::string> SectionNames(const Score& score) {
    std::vector<std::string> out;
    std::string cur;
    for (const auto& mb : score.masterBars) {
        if (mb.hasSection) cur = mb.section;
        out.push_back(cur);
    }
    return out;
}

BarFeatures Features(const Bar& bar, const MasterBar& mb, const Staff& staff) {
    BarFeatures f;
    f.quarters = mb.Quarters();
    int fretSum = 0, fretN = 0;
    std::set<int> pitches;
    for (size_t v = 0; v < bar.voices.size(); v++) {
        for (const auto& b : bar.voices[v].beats) {
            const bool first = v == 0;
            auto addPrint = [&](const std::string& s) {
                if (!f.print.empty()) f.print += ' ';
                f.print += s;
            };
            if (b.IsRest()) {
                if (first) addPrint("r" + std::to_string(b.duration));
                continue;
            }
            if (first) {
                std::string p = (b.grace != Grace::None ? std::string("g") : std::to_string(b.duration)) + ":";
                for (size_t k = 0; k < b.notes.size(); k++)
                    p += (k ? "," : "") + std::to_string(b.notes[k].string) + "/" + std::to_string(b.notes[k].fret);
                addPrint(p);
            }
            int sounding = 0;
            for (const auto& n : b.notes) sounding += !n.tieDestination && !n.dead;
            if (!sounding) continue;
            f.beats++;
            f.notes += sounding;
            if (sounding <= 2) f.single++;  // single notes and double stops
            else f.chords++;
            if (b.tap || b.slap || b.pop) f.tech++;
            for (const auto& n : b.notes) {
                if (n.tieDestination || n.dead) continue;
                if (!n.bend.empty()) f.tech++;
                if (n.slideOut != Slide::None || n.slideIn != Slide::None) f.tech++;
                if (n.hammerPullOrigin || n.hammerPullDestination) f.tech += 0.5;
                if (n.vibrato) f.tech += 0.5;
                if (int(n.harmonic) > 1) f.tech += 0.5;  // natural harmonics are often riffs
                if (n.palmMute) f.palm++;
                if (sounding <= 2) {
                    fretSum += n.fret;
                    fretN++;
                }
                pitches.insert(staff.StringPitch(n.string) + n.fret);
            }
        }
    }
    f.pitches = int(pitches.size());
    f.density = f.beats / f.quarters;
    f.singleFrac = f.beats ? double(f.single) / f.beats : 0;
    f.chordFrac = f.beats ? double(f.chords) / f.beats : 0;
    f.palmFrac = f.notes ? double(f.palm) / f.notes : 0;
    f.avgFret = fretN ? double(fretSum) / fretN : 0;
    return f;
}

double LeadScore(const BarFeatures& f, int repeats, bool unison) {
    if (f.beats < 3) return 0;
    double s = 0;
    s += f.singleFrac * 2;
    s -= f.chordFrac * 2.5;
    s += std::min(f.density, 4.0) / 2;
    s += std::min(f.tech * 0.35, 2.0);
    s += f.avgFret >= 12 ? 1.2 : f.avgFret >= 8 ? 0.7 : f.avgFret >= 5 ? 0.2 : -0.3;
    s -= f.palmFrac * 2.5;
    s += f.pitches >= 6 ? 0.6 : f.pitches >= 4 ? 0.2 : -0.6;
    if (repeats > 3) s -= 1.8;
    else if (repeats > 1) s -= 0.6;
    if (unison) s -= 0.8;  // another guitar plays exactly the same: a riff, or a harmony line
    return s;
}

std::vector<Solo> FindSolos(const Score& score, bool useLabels, double threshold) {
    auto tracks = GuitarTracks(score);
    auto names = SectionNames(score);
    std::map<const Track*, std::vector<BarFeatures>> feats;
    for (const Track* t : tracks) {
        auto& v = feats[t];
        for (size_t i = 0; i < t->staff.bars.size() && i < score.masterBars.size(); i++)
            v.push_back(Features(t->staff.bars[i], score.masterBars[i], t->staff));
    }
    std::vector<Solo> out;
    for (const Track* t : tracks) {
        const auto& fs = feats[t];
        std::map<std::string, int> counts;
        for (const auto& f : fs)
            if (f.beats) counts[f.print]++;
        double nameBonus = has(t->name, kSoloTrack) ? 0.5 : has(t->name, kRhythmTrack) ? -1 : 0;
        std::vector<double> raw(fs.size());
        for (size_t i = 0; i < fs.size(); i++) {
            const auto& f = fs[i];
            bool unison = false;
            for (const Track* o : tracks) {
                const auto& of = feats[o];
                if (o != t && i < of.size() && of[i].print == f.print) unison = true;
            }
            auto c = counts.find(f.print);
            double s = LeadScore(f, c == counts.end() ? 0 : c->second, unison && f.beats > 0) + nameBonus;
            if (useLabels && !names[i].empty()) {
                if (has(names[i], kSoloSection)) s += 2.5;
                else if (has(names[i], kOtherSection)) s -= 0.7;
            }
            raw[i] = s;
        }
        // Smooth over neighbours: a solo holds a long bend or a rest now and then.
        std::vector<double> sm(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            double sum = 2 * raw[i];
            int n = 2;
            if (i > 0) sum += raw[i - 1], n++;
            if (i + 1 < raw.size()) sum += raw[i + 1], n++;
            sm[i] = sum / n;
        }
        for (size_t i = 0; i < sm.size();) {
            if (sm[i] < threshold) {
                i++;
                continue;
            }
            size_t j = i;
            while (j < sm.size() && sm[j] >= threshold) j++;
            if (j - i >= 4) {
                Solo s;
                s.track = t->index;
                s.start = int(i);
                s.end = int(j);
                s.raw.assign(raw.begin() + long(i), raw.begin() + long(j));
                double sum = 0;
                for (double x : s.raw) sum += x;
                s.mean = sum / s.raw.size();
                s.score = s.mean * std::sqrt(double(j - i));
                for (size_t k = i; k < j; k++) s.labelled |= has(names[k], kSoloSection);
                out.push_back(std::move(s));
            }
            i = j;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Solo& a, const Solo& b) { return a.score > b.score; });
    return out;
}

double TempoAt(const Score& score, int i) {
    double t = score.tempo;
    for (int k = 0; k <= i && k < int(score.masterBars.size()); k++)
        for (const auto& a : score.masterBars[size_t(k)].tempos)
            if (k < i || a.ratio == 0) t = a.bpm;
    return t > 0 ? t : 120;
}

double Rng::Next() {
    a_ += 0x6d2b79f5u;
    uint32_t t = a_;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return double(t ^ (t >> 14)) / 4294967296.0;
}

std::optional<Lick> PickLick(const Score& score, const Solo& solo, uint32_t seed) {
    const Staff& st = score.tracks[size_t(solo.track)].staff;
    std::vector<BarFeatures> fs;
    for (size_t i = 0; i < st.bars.size() && i < score.masterBars.size(); i++)
        fs.push_back(Features(st.bars[i], score.masterBars[i], st));
    struct Option {
        int start, bars, notes;
        double secs, rank;
    };
    std::vector<Option> options;
    for (int start = solo.start; start < solo.end; start++) {
        if (fs[size_t(start)].beats < 3) continue;
        for (int len : {2, 1, 3, 4}) {
            int end = start + len;
            if (end > solo.end) break;
            bool crosses = false;
            for (int k = 0; k < len; k++) {
                const auto& m = score.masterBars[size_t(start + k)];
                if ((k > 0 && m.repeatStart) || (k < len - 1 && m.repeatCount > 0) || m.alternateEndings) crosses = true;
            }
            if (crosses) break;
            int notes = 0;
            double quarters = 0;
            for (int k = start; k < end; k++) {
                notes += fs[size_t(k)].notes;
                quarters += fs[size_t(k)].quarters;
            }
            double secs = quarters * 60 / TempoAt(score, start);
            if (notes < 6 || notes > 40 || secs > 12) continue;
            if (fs[size_t(end - 1)].beats == 0) continue;
            double sum = 0;
            for (int k = start; k < end; k++) sum += solo.raw[size_t(k - solo.start)];
            double mean = sum / len;
            // Two bars is the aim; others only when they suit the phrase better.
            double fit = len == 2 ? 0.4 : len == 1 && notes >= 12 ? 0.2 : len >= 3 && notes <= 20 ? 0.1 : -0.3;
            options.push_back({start, len, notes, secs, mean + fit});
            break;  // the first length that fits, in order of preference
        }
    }
    if (options.empty()) return std::nullopt;
    std::stable_sort(options.begin(), options.end(), [](const Option& a, const Option& b) { return a.rank > b.rank; });
    size_t top = std::max<size_t>(1, (options.size() + 1) / 2);
    const Option& o = options[size_t(std::floor(Rng(seed).Next() * double(top)))];
    Lick l;
    l.track = solo.track;
    l.startBar = o.start;
    l.bars = o.bars;
    l.notes = o.notes;
    l.secs = std::round(o.secs * 10) / 10;
    return l;
}

std::optional<Lick> FindLick(const Score& score, uint32_t seed) {
    auto solos = FindSolos(score);
    for (size_t i = 0; i < solos.size() && i < 3; i++) {
        if (auto l = PickLick(score, solos[i], seed)) {
            auto names = SectionNames(score);
            l->section = names[size_t(l->startBar)];
            l->soloStart = solos[i].start;
            l->soloEnd = solos[i].end;
            return l;
        }
    }
    return std::nullopt;
}

}  // namespace dl
