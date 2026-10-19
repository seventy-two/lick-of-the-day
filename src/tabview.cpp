#include "tabview.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dl {
namespace {

constexpr float kPi = 3.14159265f;

// Where things go along the line: each bar's onsets (the ticks beats
// start on, across voices) and their x.
struct BarLayout {
    int index = 0;
    float x = 0, width = 0;
    float notesX = 0;  // after the bar's left padding and any time signature
    float endX = 0;    // where the bar's end leads: the next bar's first onset
    int duration = 0;  // ticks
    bool timeSig = false;
    std::vector<int> onsets;
    std::vector<float> onsetX, onsetW;

    float XAt(int rel) const {
        // The x of a tick within the bar: onsets' centres, in between
        // linearly, and on from the last towards the next bar's first, so
        // the cursor keeps moving through a bar's last note.
        if (onsets.empty()) return notesX;
        for (size_t i = 0; i < onsets.size(); i++) {
            bool lastOne = i + 1 == onsets.size();
            int next = lastOne ? duration : onsets[i + 1];
            if (lastOne || rel < next) {
                float x0 = onsetX[i], x1 = lastOne ? endX : onsetX[i + 1];
                if (next <= onsets[i]) return x0;
                float t = float(rel - onsets[i]) / float(next - onsets[i]);
                return x0 + std::clamp(t, 0.0f, 1.0f) * (x1 - x0);
            }
        }
        return onsetX.back();
    }
    int OnsetAt(int rel) const {
        int found = -1;
        for (size_t i = 0; i < onsets.size(); i++)
            if (onsets[i] <= rel) found = int(i);
        return found;
    }
};

std::string fretText(const Note& n) {
    std::string f = std::to_string(n.fret);
    if (n.dead) return "x";
    if (n.harmonic == Harmonic::Natural) return "<" + f + ">";
    if (n.harmonic != Harmonic::None) return f + "<>";
    if (n.tieDestination || n.ghost) return "(" + f + ")";
    return f;
}

std::string bendText(int quarterTones) {
    switch (quarterTones) {
    case 1: return "\xC2\xBC";          // ¼
    case 2: return "\xC2\xBD";          // ½
    case 3: return "\xC2\xBE";          // ¾
    case 4: return "full";
    case 6: return "1\xC2\xBD";
    case 8: return "2";
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%g", quarterTones / 4.0);
    return buf;
}

int beamsFor(int duration) {
    int n = 0;
    for (int d = duration; d > 4; d /= 2) n++;
    return n;
}

const Note* noteOnString(const Beat& b, int string) {
    for (const auto& n : b.notes)
        if (n.string == string) return &n;
    return nullptr;
}

}  // namespace

TabResult DrawTab(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Score& score, int track, int first, int last,
                  int tick, const TabStyle& st) {
    TabResult res;
    if (track < 0 || track >= int(score.tracks.size())) return res;
    const Staff& staff = score.tracks[size_t(track)].staff;
    first = std::clamp(first, 0, int(score.masterBars.size()));
    last = std::clamp(last, first, int(score.masterBars.size()));
    const int strings = std::max(1, int(staff.tuning.size()));
    const float k = st.scale;

    // --- horizontal layout ---
    std::vector<BarLayout> bars;
    float natural = 0;
    const float minW = 20 * k, perQuarter = 34 * k, padL = 12 * k, padR = 10 * k, sigW = 22 * k;
    for (int b = first; b < last; b++) {
        const auto& mb = score.masterBars[size_t(b)];
        BarLayout L;
        L.index = b;
        L.duration = mb.Duration();
        std::set<int> on;
        for (const auto& v : staff.bars[size_t(b)].voices)
            for (const auto& beat : v.beats)
                if (beat.grace == Grace::None && !beat.empty) on.insert(beat.start);
        if (on.empty()) on.insert(0);
        L.onsets.assign(on.begin(), on.end());
        const auto* prev = b > 0 ? &score.masterBars[size_t(b - 1)] : nullptr;
        L.timeSig = b == first || !prev || prev->numerator != mb.numerator || prev->denominator != mb.denominator;
        float w = padL + (L.timeSig ? sigW : 0) + padR;
        for (size_t i = 0; i < L.onsets.size(); i++) {
            int next = i + 1 < L.onsets.size() ? L.onsets[i + 1] : mb.Duration();
            float ow = minW + perQuarter * float(std::max(0, next - L.onsets[i])) / kTicksPerQuarter;
            L.onsetW.push_back(ow);
            w += ow;
        }
        L.width = w;
        natural += w;
        bars.push_back(std::move(L));
    }
    // Short licks are spread out to use the space, up to a point.
    float stretch = natural > 0 && natural < size.x ? std::min(size.x / natural, 2.4f) : 1.0f;
    float x = origin.x;
    for (auto& L : bars) {
        L.x = x;
        float fixed = padL + (L.timeSig ? sigW : 0);
        L.notesX = x + fixed * stretch;
        float ox = L.notesX;
        for (size_t i = 0; i < L.onsets.size(); i++) {
            L.onsetW[i] *= stretch;
            L.onsetX.push_back(ox + std::min(L.onsetW[i], minW * stretch) * 0.5f);
            ox += L.onsetW[i];
        }
        L.width *= stretch;
        x += L.width;
    }
    const float right = x;
    res.width = right - origin.x;
    for (size_t i = 0; i < bars.size(); i++)
        bars[i].endX = i + 1 < bars.size() ? bars[i + 1].XAt(0) : right - padR * stretch * 0.5f;

    // --- vertical layout ---
    const float topLane = 30 * k, bottomLane = 30 * k;
    float gap = (size.y - topLane - bottomLane - 6 * k) / float(std::max(1, strings - 1));
    gap = std::clamp(gap, 9 * k, 21 * k);
    const float staffH = gap * float(strings - 1);
    const float top = origin.y + std::max(0.0f, (size.y - topLane - staffH - bottomLane) / 2) + topLane;
    const float bottom = top + staffH;
    auto yOf = [&](int string) { return top + gap * float(strings - string); };  // string 1 is the lowest

    // --- strings and bar lines ---
    for (int s = 0; s < strings; s++) dl->AddLine(ImVec2(origin.x, top + gap * s), ImVec2(right, top + gap * s), st.line, 1);
    for (const auto& L : bars) dl->AddLine(ImVec2(L.x, top), ImVec2(L.x, bottom), st.line, 1);
    dl->AddLine(ImVec2(right - 1, top), ImVec2(right - 1, bottom), st.line, 1);

    // Where's the cursor, and which onset is sounding?
    int playBar = -1, playOnset = -1;
    if (tick >= 0) {
        for (const auto& L : bars) {
            const auto& mb = score.masterBars[size_t(L.index)];
            if (tick >= mb.start && tick < mb.start + mb.Duration()) {
                playBar = L.index;
                playOnset = L.OnsetAt(tick - mb.start);
                res.cursorX = L.XAt(tick - mb.start) - origin.x;
            }
        }
    }

    auto text = [&](ImFont* f, float sz, ImVec2 centre, ImU32 col, const std::string& s, bool box) {
        ImVec2 ts = f->CalcTextSizeA(sz, 1e9f, 0, s.c_str());
        ImVec2 p(std::round(centre.x - ts.x / 2), std::round(centre.y - ts.y / 2));
        if (box) dl->AddRectFilled(ImVec2(p.x - 1.5f * k, centre.y - gap * 0.42f), ImVec2(p.x + ts.x + 1.5f * k, centre.y + gap * 0.42f), st.bg);
        dl->AddText(f, sz, p, col, s.c_str());
        return ts;
    };

    // --- bar furniture: numbers, sections, time signatures ---
    for (const auto& L : bars) {
        const auto& mb = score.masterBars[size_t(L.index)];
        std::string label = std::to_string(L.index + 1);
        dl->AddText(st.label, st.labelSize, ImVec2(L.x + 2 * k, top - st.labelSize - 2 * k), st.muted, label.c_str());
        if (mb.hasSection && !mb.section.empty()) {
            ImVec2 ts = st.label->CalcTextSizeA(st.labelSize, 1e9f, 0, label.c_str());
            std::string sec = "[" + mb.section + "]";
            dl->AddText(st.label, st.labelSize, ImVec2(L.x + ts.x + 8 * k, top - topLane + 1 * k), st.muted, sec.c_str());
        }
        if (L.timeSig) {
            float cx = L.x + (padL + sigW / 2) * stretch * 0.6f + 6 * k;
            float big = std::min(staffH * 0.48f, 30 * k);
            text(st.fret, big, ImVec2(cx, top + staffH * 0.27f), st.text, std::to_string(mb.numerator), false);
            text(st.fret, big, ImVec2(cx, top + staffH * 0.73f), st.text, std::to_string(mb.denominator), false);
        }
    }

    // --- notes, techniques and rhythm ---
    for (const auto& L : bars) {
        const auto& mb = score.masterBars[size_t(L.index)];
        const Bar& bar = staff.bars[size_t(L.index)];
        for (size_t v = 0; v < bar.voices.size(); v++) {
            const auto& beats = bar.voices[v].beats;
            // Beats that sound, in order, with their x, for rhythm and for
            // joining notes to the next.
            struct Placed {
                const Beat* beat;
                float x;
                int onset;
            };
            std::vector<Placed> placed;
            for (size_t i = 0; i < beats.size(); i++) {
                const Beat& b = beats[i];
                if (b.grace != Grace::None || b.empty) continue;
                int on = L.OnsetAt(b.start);
                placed.push_back({&b, on >= 0 ? L.onsetX[size_t(on)] : L.notesX, on});
            }
            // The next sounding beat after placed[i], maybe in the next bar.
            auto nextBeat = [&](size_t i, float& nx) -> const Beat* {
                if (i + 1 < placed.size()) {
                    nx = placed[i + 1].x;
                    return placed[i + 1].beat;
                }
                for (size_t bi = 0; bi < bars.size(); bi++) {
                    if (bars[bi].index != L.index + 1) continue;
                    const Bar& nb = staff.bars[size_t(L.index + 1)];
                    if (v >= nb.voices.size()) return nullptr;
                    for (const auto& b2 : nb.voices[v].beats)
                        if (b2.grace == Grace::None && !b2.empty) {
                            int on = bars[bi].OnsetAt(b2.start);
                            nx = on >= 0 ? bars[bi].onsetX[size_t(on)] : bars[bi].notesX;
                            return &b2;
                        }
                }
                return nullptr;
            };

            // Grace notes, small, just before their beat.
            for (size_t i = 0; i < beats.size(); i++) {
                if (beats[i].grace == Grace::None) continue;
                size_t j = i;
                while (j < beats.size() && beats[j].grace != Grace::None) j++;
                if (j >= beats.size()) continue;
                int on = L.OnsetAt(beats[j].start);
                float bx = on >= 0 ? L.onsetX[size_t(on)] : L.notesX;
                float gx = bx - 13 * k * float(j - i);
                for (const auto& n : beats[i].notes)
                    if (n.string >= 1) text(st.label, st.labelSize, ImVec2(gx, yOf(n.string)), st.muted, fretText(n), true);
            }

            bool pmOpen = false;
            float pmStart = 0;
            for (size_t i = 0; i < placed.size(); i++) {
                const Beat& b = *placed[i].beat;
                const float bx = placed[i].x;
                const bool playing = playBar == L.index && placed[i].onset == playOnset;
                const ImU32 col = playing ? st.accent : st.text;
                float nx = 0;
                const Beat* nb = nextBeat(i, nx);

                // Rests: a small mark in the middle of the staff.
                if (b.IsRest()) {
                    float my = (top + bottom) / 2;
                    if (b.duration <= 2) {
                        float yy = b.duration == 1 ? my - gap * 0.5f : my;
                        dl->AddRectFilled(ImVec2(bx - 4 * k, yy - 2 * k), ImVec2(bx + 4 * k, yy + 1 * k), st.muted);
                    } else if (b.duration == 4) {
                        ImVec2 pts[] = {{bx - 2 * k, my - 7 * k}, {bx + 3 * k, my - 2 * k}, {bx - 2 * k, my + 2 * k}, {bx + 3 * k, my + 7 * k}};
                        dl->AddPolyline(pts, 4, st.muted, 0, 1.5f * k);
                    } else {
                        for (int f = 0; f < beamsFor(b.duration); f++) {
                            float fy = my - 5 * k + f * 4 * k;
                            dl->AddCircleFilled(ImVec2(bx - 2 * k, fy), 1.6f * k, st.muted);
                            dl->AddLine(ImVec2(bx - 2 * k, fy + 1 * k), ImVec2(bx + 3 * k, fy - 1 * k), st.muted, 1.2f * k);
                        }
                        dl->AddLine(ImVec2(bx + 3 * k, my - 6 * k), ImVec2(bx, my + 6 * k), st.muted, 1.2f * k);
                    }
                }

                for (const auto& n : b.notes) {
                    if (n.string < 1 || n.string > strings) continue;
                    const float y = yOf(n.string);
                    ImVec2 ts = text(st.fret, st.fretSize, ImVec2(bx, y), col, fretText(n), true);
                    const float half = ts.x / 2 + 2 * k;
                    const Note* target = nb ? noteOnString(*nb, n.string) : nullptr;

                    // Hammer-ons and pull-offs: an arc to the next note, h or p.
                    if (n.hammerPullOrigin && target) {
                        ImVec2 a(bx + half - 1 * k, y - gap * 0.45f), c(nx - half + 1 * k, y - gap * 0.45f);
                        ImVec2 m((a.x + c.x) / 2, y - gap * 1.05f);
                        dl->AddBezierQuadratic(a, m, c, st.muted, 1.2f * k, 12);
                        text(st.label, st.labelSize * 0.9f, ImVec2(m.x, m.y - 3 * k), st.muted, target->fret >= n.fret ? "h" : "p", false);
                    }
                    // Slides: a line to the next note, or off into nothing.
                    float sl = gap * 0.28f;
                    if ((n.slideOut == Slide::Shift || n.slideOut == Slide::Legato) && target) {
                        float dir = target->fret >= n.fret ? 1.0f : -1.0f;
                        dl->AddLine(ImVec2(bx + half, y + sl * dir), ImVec2(nx - half, y - sl * dir), col, 1.3f * k);
                    } else if (n.slideOut == Slide::OutDown || n.slideOut == Slide::PickDown) {
                        dl->AddLine(ImVec2(bx + half, y - sl), ImVec2(bx + half + 12 * k, y + sl), col, 1.3f * k);
                    } else if (n.slideOut == Slide::OutUp || n.slideOut == Slide::PickUp) {
                        dl->AddLine(ImVec2(bx + half, y + sl), ImVec2(bx + half + 12 * k, y - sl), col, 1.3f * k);
                    }
                    if (n.slideIn == Slide::IntoFromBelow)
                        dl->AddLine(ImVec2(bx - half - 12 * k, y + sl), ImVec2(bx - half, y - sl), col, 1.3f * k);
                    else if (n.slideIn == Slide::IntoFromAbove)
                        dl->AddLine(ImVec2(bx - half - 12 * k, y - sl), ImVec2(bx - half, y + sl), col, 1.3f * k);

                    // Bends: an arrow up to the label above the staff, and
                    // back down for a release.
                    if (!n.bend.empty()) {
                        int peak = 0;
                        for (const auto& p : n.bend) peak = std::max(peak, p.value);
                        int startV = n.bend.front().value, endV = n.bend.back().value;
                        float ax = bx + half, ay = y;
                        float tipX = ax + 10 * k, tipY = top - topLane * 0.42f;
                        if (peak > 0) {
                            if (startV > 0) {  // a pre-bend: straight up
                                dl->AddLine(ImVec2(ax - half, y - gap * 0.5f), ImVec2(ax - half, tipY + 3 * k), col, 1.3f * k);
                                tipX = ax - half;
                            } else {
                                dl->AddBezierQuadratic(ImVec2(ax, ay), ImVec2(tipX, ay), ImVec2(tipX, tipY + 3 * k), col, 1.3f * k, 12);
                            }
                            dl->AddTriangleFilled(ImVec2(tipX, tipY), ImVec2(tipX - 3 * k, tipY + 5 * k), ImVec2(tipX + 3 * k, tipY + 5 * k), col);
                            text(st.label, st.labelSize, ImVec2(tipX, tipY - st.labelSize * 0.6f), col, bendText(peak), false);
                            if (endV < peak) {  // release
                                float rx = tipX + 12 * k;
                                dl->AddBezierQuadratic(ImVec2(tipX + 1 * k, tipY + 3 * k), ImVec2(rx, tipY + 3 * k), ImVec2(rx, y - gap * 0.6f), col, 1.3f * k, 12);
                                dl->AddTriangleFilled(ImVec2(rx, y - gap * 0.4f), ImVec2(rx - 3 * k, y - gap * 0.4f - 5 * k), ImVec2(rx + 3 * k, y - gap * 0.4f - 5 * k), col);
                            }
                        }
                    }
                }

                // Above the staff: vibrato, taps, palm muting.
                bool vib = b.whammyVibrato, pm = false, tap = b.tap;
                for (const auto& n : b.notes) {
                    vib |= n.vibrato;
                    pm |= n.palmMute;
                }
                float laneY = top - topLane * 0.28f;
                if (vib && !b.IsRest()) {
                    float w = std::max(14 * k, (placed[i].onset >= 0 ? L.onsetW[size_t(placed[i].onset)] : 20 * k) * 0.8f);
                    std::vector<ImVec2> pts;
                    for (float t = 0; t <= w; t += 1.5f * k)
                        pts.push_back(ImVec2(bx - 4 * k + t, laneY - 2 * k + 2.2f * k * std::sin(t / (5 * k) * 2 * kPi)));
                    dl->AddPolyline(pts.data(), int(pts.size()), col, 0, 1.3f * k);
                }
                if (tap) text(st.label, st.labelSize, ImVec2(bx, laneY - 2 * k), col, "T", false);
                if (pm && !pmOpen) {
                    pmOpen = true;
                    pmStart = bx;
                }
                bool pmNext = false;
                if (pm && nb && i + 1 < placed.size())
                    for (const auto& n2 : nb->notes) pmNext |= n2.palmMute;
                if (pmOpen && (!pmNext || i + 1 == placed.size())) {
                    ImVec2 ts = text(st.label, st.labelSize, ImVec2(pmStart + 8 * k, laneY + 1 * k), st.muted, "P.M.", false);
                    float x0 = pmStart + 8 * k + ts.x / 2 + 3 * k, x1 = bx + 8 * k;
                    for (float dx = x0; dx < x1; dx += 6 * k)
                        dl->AddLine(ImVec2(dx, laneY + 1 * k), ImVec2(std::min(dx + 3 * k, x1), laneY + 1 * k), st.muted, 1);
                    pmOpen = false;
                }
            }

            // Rhythm, from the first voice only: stems below the staff,
            // beamed in groups of a beat (a dotted quarter in compound time).
            if (v != 0) continue;
            const float stemTop = bottom + 5 * k, stemLen = 17 * k, beamGap = 3.5f * k, beamH = 2.2f * k;
            const int group = mb.denominator == 8 && mb.numerator % 3 == 0 ? kTicksPerQuarter * 3 / 2 : kTicksPerQuarter;
            const float stemEnd = stemTop + stemLen;
            for (size_t i = 0; i < placed.size(); i++) {
                const Beat& b = *placed[i].beat;
                if (b.IsRest() || b.duration <= 1) continue;
                float sx = placed[i].x;
                float len = b.duration == 2 ? stemLen * 0.5f : stemLen;
                dl->AddLine(ImVec2(sx, stemTop), ImVec2(sx, stemTop + len), st.muted, 1.2f * k);
                if (b.dots) dl->AddCircleFilled(ImVec2(sx + 4 * k, stemTop + len - 2 * k), 1.6f * k, st.muted);
                if (b.tremoloPicking)
                    for (int t = 0; t < beamsFor(b.tremoloPicking); t++) {
                        float ty = stemTop + 4 * k + t * 3 * k;
                        dl->AddLine(ImVec2(sx - 3 * k, ty + 1.5f * k), ImVec2(sx + 3 * k, ty - 1.5f * k), st.muted, 1.4f * k);
                    }
            }
            // Beams: runs of beamable beats in one group.
            size_t i = 0;
            while (i < placed.size()) {
                const Beat& b = *placed[i].beat;
                if (b.IsRest() || b.duration < 8) {
                    i++;
                    continue;
                }
                int g = b.start / std::max(1, group);
                size_t j = i + 1;
                while (j < placed.size() && !placed[j].beat->IsRest() && placed[j].beat->duration >= 8 &&
                       placed[j].beat->start / std::max(1, group) == g)
                    j++;
                if (j - i == 1) {
                    // A lone note: flags.
                    for (int f = 0; f < beamsFor(b.duration); f++) {
                        float fy = stemEnd - f * beamGap;
                        dl->AddLine(ImVec2(placed[i].x, fy), ImVec2(placed[i].x + 6 * k, fy - 4 * k), st.muted, 1.4f * k);
                    }
                } else {
                    int maxLevel = 0;
                    for (size_t q = i; q < j; q++) maxLevel = std::max(maxLevel, beamsFor(placed[q].beat->duration));
                    for (int lvl = 1; lvl <= maxLevel; lvl++) {
                        float by = stemEnd - (lvl - 1) * beamGap;
                        for (size_t q = i; q < j; q++) {
                            bool here = beamsFor(placed[q].beat->duration) >= lvl;
                            if (!here) continue;
                            bool nextToo = q + 1 < j && beamsFor(placed[q + 1].beat->duration) >= lvl;
                            bool prevToo = q > i && beamsFor(placed[q - 1].beat->duration) >= lvl;
                            if (nextToo) {
                                dl->AddRectFilled(ImVec2(placed[q].x - 0.6f * k, by - beamH), ImVec2(placed[q + 1].x + 0.6f * k, by), st.muted);
                            } else if (!prevToo) {
                                // A stub, towards its neighbour.
                                float dir = q + 1 < j ? 1.0f : -1.0f;
                                float x0 = placed[q].x, x1 = x0 + dir * 7 * k;
                                dl->AddRectFilled(ImVec2(std::min(x0, x1), by - beamH), ImVec2(std::max(x0, x1), by), st.muted);
                            }
                        }
                    }
                }
                // Tuplets: the number under the run.
                const Beat& f = *placed[i].beat;
                if (f.tupletNumerator > 1 && f.tupletNumerator != f.tupletDenominator) {
                    float cx = (placed[i].x + placed[j - 1].x) / 2;
                    text(st.label, st.labelSize * 0.9f, ImVec2(cx, stemEnd + st.labelSize * 0.7f), st.muted, std::to_string(f.tupletNumerator), false);
                }
                i = j;
            }
        }
    }

    // The cursor: a line where playing is.
    if (res.cursorX >= 0) {
        float cx = origin.x + res.cursorX;
        dl->AddRectFilled(ImVec2(cx - 1.2f * k, top - gap * 0.6f), ImVec2(cx + 1.2f * k, bottom + gap * 0.6f),
                          (st.accent & 0x00FFFFFF) | 0xB0000000);
    }
    return res;
}

}  // namespace dl
