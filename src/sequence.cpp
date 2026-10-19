#include "sequence.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace dl {
namespace {

// GP5's drum numbers that aren't General MIDI notes, as alphaTab maps them.
int drumKey(int n) {
    static const std::map<int, int> m = {{27, 42}, {28, 60}, {29, 59}, {30, 49}, {31, 40}, {32, 31}, {33, 37}, {34, 38},
                                         {91, 38}, {92, 46}, {93, 51}, {94, 51}, {95, 55}, {96, 52}, {97, 49}, {98, 57},
                                         {99, 56}, {100, 56}, {101, 56}, {102, 56}, {103, 56}, {104, 60}, {105, 60},
                                         {106, 61}, {107, 61}, {108, 64}, {109, 64}, {110, 63}, {111, 54}, {112, 54},
                                         {113, 54}, {114, 43}, {115, 49}, {116, 49}, {117, 69}, {118, 70}, {119, 70},
                                         {120, 70}, {122, 82}, {123, 53}, {124, 62}, {125, 62}, {126, 59}, {127, 59}};
    auto it = m.find(n);
    return it == m.end() ? n : it->second;
}

int wheelFor(double semitones) {
    int w = int(std::lround(8192 + semitones / kWheelRange * 8192));
    return std::clamp(w, 0, 16383);
}

// level turns Guitar Pro's volume or balance (0–16, or 0–127 in some
// files) into 0–1.
float level(int v) { return std::clamp(v > 16 ? v / 127.0f : v / 16.0f, 0.0f, 1.0f); }

constexpr double kPi = 3.14159265358979323846;

}  // namespace

double Sequence::TickToSec(int tick) const {
    if (tempo.empty()) return 0;
    size_t i = 0;
    while (i + 1 < tempo.size() && tempo[i + 1].tick <= tick) i++;
    return tempo[i].sec + (tick - tempo[i].tick) * tempo[i].secPerTick;
}

int Sequence::SecToTick(double sec) const {
    if (tempo.empty()) return startTick;
    size_t i = 0;
    while (i + 1 < tempo.size() && tempo[i + 1].sec <= sec) i++;
    return tempo[i].tick + int((sec - tempo[i].sec) / tempo[i].secPerTick);
}

int PitchOf(const Staff& staff, const Note& n) {
    int p = staff.StringPitch(n.string) + staff.capo + n.fret;
    switch (n.harmonic) {
    case Harmonic::None: break;
    case Harmonic::Natural: {
        // The harmonic's pitch above the open string, by fret.
        int open = staff.StringPitch(n.string) + staff.capo;
        switch (n.fret) {
        case 12: p = open + 12; break;
        case 7: case 19: p = open + 19; break;
        case 5: case 24: p = open + 24; break;
        case 4: case 9: case 16: p = open + 28; break;
        case 3: p = open + 31; break;
        default: p = open + 12 + n.fret % 12; break;
        }
        break;
    }
    default: p += 12; break;
    }
    return std::clamp(p, 0, 127);
}

Sequence BuildSequence(const Score& score, int startBar, int endBar) {
    Sequence seq;
    startBar = std::clamp(startBar, 0, int(score.masterBars.size()));
    endBar = std::clamp(endBar, startBar, int(score.masterBars.size()));
    if (startBar == endBar) return seq;
    const auto& mbs = score.masterBars;
    seq.startTick = mbs[size_t(startBar)].start;
    seq.endTick = mbs[size_t(endBar - 1)].start + mbs[size_t(endBar - 1)].Duration();

    // The tempo map, from the tempo in force at the start.
    double bpm = score.tempo;
    for (int k = 0; k <= startBar; k++)
        for (const auto& a : mbs[size_t(k)].tempos)
            if (k < startBar || a.ratio == 0) bpm = a.bpm;
    if (bpm <= 0) bpm = 120;
    seq.tempo.push_back({seq.startTick, 0, 60.0 / (bpm * kTicksPerQuarter)});
    for (int b = startBar; b < endBar; b++) {
        for (const auto& tc : mbs[size_t(b)].tempos) {
            if (b == startBar && tc.ratio == 0) continue;
            if (tc.bpm <= 0) continue;
            int tick = mbs[size_t(b)].start + int(tc.ratio * mbs[size_t(b)].Duration());
            double sec = seq.TickToSec(tick);
            if (tick == seq.tempo.back().tick) seq.tempo.pop_back();
            seq.tempo.push_back({tick, sec, 60.0 / (tc.bpm * kTicksPerQuarter)});
        }
    }
    seq.length = seq.TickToSec(seq.endTick);
    auto sec = [&](int tick) { return seq.TickToSec(tick); };

    for (const auto& track : score.tracks) {
        const Staff& st = track.staff;
        Channel ch;
        ch.channel = track.index >= kClickChannel ? kClickChannel - 1 : track.index;
        ch.track = track.index;
        ch.program = track.program;
        ch.drums = st.percussion;
        ch.volume = track.mute ? 0 : level(track.volume);
        ch.pan = level(track.balance);
        seq.channels.push_back(ch);
        bool wheelMoved = false;
        auto& ev = seq.events;
        auto wheel = [&](double t, double semitones) {
            Event e;
            e.time = t;
            e.type = Event::Wheel;
            e.channel = ch.channel;
            e.wheel = wheelFor(semitones);
            ev.push_back(e);
            wheelMoved = true;
        };

        for (int b = startBar; b < endBar; b++) {
            const Bar& bar = st.bars[size_t(b)];
            const int barTick = mbs[size_t(b)].start;
            for (size_t v = 0; v < bar.voices.size(); v++) {
                const auto& beats = bar.voices[v].beats;
                for (size_t i = 0; i < beats.size(); i++) {
                    const Beat& beat = beats[i];
                    if (beat.grace != Grace::None || beat.IsRest()) continue;
                    // Grace notes just before this beat, a 64th each.
                    int graces = 0;
                    for (size_t k = i; k-- > 0 && beats[k].grace != Grace::None;) graces++;
                    for (int g = 0; g < graces; g++) {
                        const Beat& gb = beats[i - size_t(graces - g)];
                        int at = barTick + beat.start - (graces - g) * 60;
                        if (at < seq.startTick) continue;
                        for (const auto& n : gb.notes) {
                            if (n.string < 1 || n.dead) continue;
                            Event on;
                            on.time = sec(at);
                            on.channel = ch.channel;
                            on.key = PitchOf(st, n);
                            on.velocity = 0.6f;
                            Event off = on;
                            off.type = Event::Off;
                            off.time = sec(at + 58);
                            if (wheelMoved) {
                                wheel(on.time, 0);
                                wheelMoved = false;
                            }
                            ev.push_back(on);
                            ev.push_back(off);
                        }
                    }

                    const int onTick = barTick + beat.start;
                    for (const auto& n : beat.notes) {
                        if (n.tieDestination) continue;
                        if (!st.percussion && n.string < 1) continue;
                        int dur = beat.ticks;
                        // Follow ties forward, into the bars after if need be.
                        {
                            int bb = b;
                            size_t k = i;
                            int next = 0;
                            for (;;) {
                                const auto* vb = &st.bars[size_t(bb)].voices;
                                if (v >= vb->size()) break;
                                const auto* bs = &(*vb)[v].beats;
                                k++;
                                while (k >= bs->size()) {
                                    if (++bb >= endBar) break;
                                    vb = &st.bars[size_t(bb)].voices;
                                    if (v >= vb->size()) break;
                                    bs = &(*vb)[v].beats;
                                    k = 0;
                                }
                                if (bb >= endBar || v >= st.bars[size_t(bb)].voices.size() || k >= bs->size()) break;
                                const Beat& nb = (*bs)[k];
                                if (nb.grace != Grace::None) continue;
                                bool tied = false;
                                for (const auto& nn : nb.notes) tied |= nn.tieDestination && nn.string == n.string;
                                if (!tied) break;
                                next = mbs[size_t(bb)].start + nb.start + nb.ticks;
                                dur = next - onTick;
                            }
                        }
                        float vel = 0.85f;
                        if (n.ghost) vel = 0.5f;
                        if (n.hammerPullDestination) vel = 0.65f;
                        if (n.dead) {
                            dur = std::min(dur, 60);
                            vel = 0.5f;
                        }
                        if (n.palmMute) {
                            dur = std::max(60, dur / 2);
                            vel *= 0.9f;
                        }
                        if (n.staccato) dur = std::max(60, dur / 2);
                        if (n.letRing) dur = std::min(dur * 2, mbs[size_t(b)].Duration());
                        int offTick = std::min(onTick + dur, seq.endTick);
                        const double tOn = sec(onTick), tOff = sec(offTick);
                        const int key = st.percussion ? drumKey(n.fret) : PitchOf(st, n);
                        if (!st.percussion && wheelMoved) {
                            wheel(tOn, 0);
                            wheelMoved = false;
                        }
                        // Bends, slides and vibrato move this channel's pitch wheel.
                        if (!st.percussion && !n.dead) {
                            const double len = tOff - tOn;
                            if (!n.bend.empty()) {
                                for (int s = 0; s <= 16; s++) {
                                    double pos = s / 16.0 * 60;
                                    // Interpolate the bend's points at pos.
                                    double val = n.bend.front().value;
                                    for (size_t p = 0; p + 1 < n.bend.size(); p++) {
                                        const auto &a = n.bend[p], &c = n.bend[p + 1];
                                        if (pos >= a.offset && pos <= c.offset) {
                                            double f = c.offset == a.offset ? 1 : (pos - a.offset) / (c.offset - a.offset);
                                            val = a.value + (c.value - a.value) * f;
                                            break;
                                        }
                                        if (pos > c.offset) val = c.value;
                                    }
                                    wheel(tOn + len * s / 16.0 * 0.98, val / 2.0);
                                }
                            } else if (n.slideOut == Slide::Shift || n.slideOut == Slide::Legato) {
                                // Glide to the next note on this string.
                                const Note* target = nullptr;
                                for (size_t k = i + 1; k < beats.size() && !target; k++)
                                    for (const auto& nn : beats[k].notes)
                                        if (nn.string == n.string) target = &nn;
                                if (!target && b + 1 < endBar && v < st.bars[size_t(b + 1)].voices.size())
                                    for (const auto& nb : st.bars[size_t(b + 1)].voices[v].beats) {
                                        for (const auto& nn : nb.notes)
                                            if (nn.string == n.string) target = &nn;
                                        if (target || !nb.notes.empty()) break;
                                    }
                                if (target) {
                                    double d = PitchOf(st, *target) - key;
                                    for (int s = 0; s <= 6; s++) wheel(tOn + len * (0.6 + 0.38 * s / 6.0), d * s / 6.0);
                                }
                            } else if (n.slideOut == Slide::OutDown || n.slideOut == Slide::OutUp ||
                                       n.slideOut == Slide::PickDown || n.slideOut == Slide::PickUp) {
                                double d = n.slideOut == Slide::OutUp || n.slideOut == Slide::PickUp ? 5 : -5;
                                for (int s = 0; s <= 6; s++) wheel(tOn + len * (0.6 + 0.38 * s / 6.0), d * s / 6.0);
                            }
                            if (n.slideIn == Slide::IntoFromBelow || n.slideIn == Slide::IntoFromAbove) {
                                double d = n.slideIn == Slide::IntoFromBelow ? -3 : 3;
                                for (int s = 0; s <= 4; s++) wheel(tOn + std::min(0.08, len * 0.2) * s / 4.0, d * (1 - s / 4.0));
                            }
                            if ((n.vibrato || beat.whammyVibrato) && n.bend.empty() && len > 0.15) {
                                for (double t = tOn + len * 0.2; t < tOff - 0.02; t += 1 / 40.0)
                                    wheel(t, 0.3 * std::sin((t - tOn) * 2 * kPi * 5.5));
                            }
                        }
                        // Tremolo picking repeats the note.
                        int step = beat.tremoloPicking ? kTicksPerQuarter * 4 / beat.tremoloPicking : dur;
                        if (step <= 0) step = dur;
                        for (int t = onTick; t < onTick + dur && t < seq.endTick; t += step) {
                            Event on;
                            on.time = sec(t);
                            on.channel = ch.channel;
                            on.key = key;
                            on.velocity = vel;
                            Event off = on;
                            off.type = Event::Off;
                            off.time = std::min(sec(std::min(t + step, offTick)), tOff) - 0.002;
                            if (off.time <= on.time) off.time = on.time + 0.01;
                            ev.push_back(on);
                            ev.push_back(off);
                        }
                    }
                }
            }
        }
        if (wheelMoved) wheel(seq.length, 0);
    }

    // The metronome: a click a beat, the first of each bar higher.
    Channel click;
    click.channel = kClickChannel;
    click.drums = true;
    click.volume = 1;
    seq.channels.push_back(click);
    auto addClick = [](std::vector<Event>& out, double t, bool first) {
        Event on;
        on.time = t;
        on.channel = kClickChannel;
        on.key = first ? 76 : 77;  // high and low wood blocks
        on.velocity = first ? 1.0f : 0.75f;
        Event off = on;
        off.type = Event::Off;
        off.time = t + 0.05;
        out.push_back(on);
        out.push_back(off);
    };
    for (int b = startBar; b < endBar; b++) {
        const auto& mb = mbs[size_t(b)];
        int step = kTicksPerQuarter * 4 / mb.denominator;
        for (int k = 0; k < mb.numerator; k++) addClick(seq.clicks, sec(mb.start + k * step), k == 0);
    }
    {
        const auto& mb = mbs[size_t(startBar)];
        double beatSec = (kTicksPerQuarter * 4 / mb.denominator) * seq.tempo.front().secPerTick;
        for (int k = 0; k < mb.numerator; k++) addClick(seq.countIn, -beatSec * (mb.numerator - k), k == 0);
    }

    auto order = [](const Event& a, const Event& b) {
        if (a.time != b.time) return a.time < b.time;
        auto rank = [](Event::Type t) { return t == Event::Off ? 0 : t == Event::Wheel ? 1 : 2; };
        return rank(a.type) < rank(b.type);
    };
    for (auto* list : {&seq.events, &seq.clicks, &seq.countIn}) std::stable_sort(list->begin(), list->end(), order);
    return seq;
}

}  // namespace dl
