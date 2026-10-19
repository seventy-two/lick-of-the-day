// Guitar Pro 6 (.gpx) and 7–8 (.gp) files. Both hold score.gpif, an XML
// document: GPX in its own compressed file system (BCFZ), GP7 in a zip.
// A port of alphaTab's GpxFileSystem and GpifParser, keeping only what
// dailylick uses.
#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>

#include "gp.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"
#include "pugixml/pugixml.hpp"

namespace dl {
namespace {

// --- GPX's file system ---

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& d, size_t start) : d_(d), byte_(start) {}
    bool AtEnd() const { return byte_ >= d_.size(); }
    int Bit() {
        if (byte_ >= d_.size()) throw std::out_of_range("end");
        int v = (d_[byte_] >> (7 - bit_)) & 1;
        if (++bit_ == 8) {
            bit_ = 0;
            byte_++;
        }
        return v;
    }
    int Bits(int n) {
        int v = 0;
        for (int i = n - 1; i >= 0; i--) v |= Bit() << i;
        return v;
    }
    int BitsReversed(int n) {
        int v = 0;
        for (int i = 0; i < n; i++) v |= Bit() << i;
        return v;
    }

private:
    const std::vector<uint8_t>& d_;
    size_t byte_;
    int bit_ = 0;
};

int32_t le32(const std::vector<uint8_t>& d, size_t at) {
    if (at + 4 > d.size()) return 0;
    return int32_t(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24);
}

// decompressBcfz undoes GPX's compression: a bit stream of literal runs
// and back references. data starts after the "BCFZ" header.
std::vector<uint8_t> decompressBcfz(const std::vector<uint8_t>& data) {
    int32_t expected = le32(data, 4);
    if (expected < 0 || expected > 64 << 20) throw FormatError("bad GPX size");
    std::vector<uint8_t> out;
    out.reserve(size_t(expected));
    BitReader r(data, 8);
    try {
        while (int32_t(out.size()) < expected) {
            if (r.Bits(1) == 1) {
                int word = r.Bits(4);
                int offset = r.BitsReversed(word);
                int size = r.BitsReversed(word);
                if (offset <= 0 || size_t(offset) > out.size()) throw FormatError("bad GPX back reference");
                size_t from = out.size() - size_t(offset);
                int n = std::min(offset, size);
                for (int i = 0; i < n; i++) out.push_back(out[from + size_t(i)]);
            } else {
                int size = r.BitsReversed(2);
                for (int i = 0; i < size; i++) out.push_back(uint8_t(r.Bits(8)));
            }
        }
    } catch (const std::out_of_range&) {
    }
    return out;
}

// gpxFile finds a file in an uncompressed GPX file system (after "BCFS").
std::string gpxFile(const std::vector<uint8_t>& fs, const std::string& want) {
    const size_t sector = 4096;
    for (size_t off = sector; off + 3 < fs.size(); off += sector) {
        if (le32(fs, off) != 2) continue;
        std::string name;
        for (size_t i = 0; i < 127 && off + 4 + i < fs.size() && fs[off + 4 + i]; i++) name += char(fs[off + 4 + i]);
        int32_t size = le32(fs, off + 140);
        std::string data;
        size_t ptr = off + 148;
        for (int k = 0;; k++) {
            int32_t s = le32(fs, ptr + 4 * size_t(k));
            if (s <= 0) break;
            size_t at = size_t(s) * sector;
            if (at >= fs.size()) break;
            if (name == want) data.append(reinterpret_cast<const char*>(fs.data() + at), std::min(sector, fs.size() - at));
            off = at;  // as alphaTab: carry on after the file's last sector
        }
        if (name == want) {
            if (size >= 0 && size_t(size) < data.size()) data.resize(size_t(size));
            return data;
        }
    }
    return {};
}

// --- score.gpif ---

std::vector<std::string> split(const char* s) {
    std::vector<std::string> out;
    std::istringstream in(s ? s : "");
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

int toInt(const char* s, int def = 0) {
    if (!s || !*s) return def;
    char* end;
    long v = std::strtol(s, &end, 10);
    return end == s ? def : int(v);
}

double toFloat(const char* s, double def = 0) {
    if (!s || !*s) return def;
    char* end;
    double v = std::strtod(s, &end);
    return end == s ? def : v;
}

const char* text(pugi::xml_node n) { return n.text().get(); }

struct Rhythm {
    int duration = 4, dots = 0, num = 1, den = 1;
};

std::vector<int> tuningOf(pugi::xml_node prop) {
    auto parts = split(text(prop.child("Pitches")));
    std::vector<int> t(parts.size());
    for (size_t i = 0; i < parts.size(); i++) t[parts.size() - 1 - i] = toInt(parts[i].c_str());
    return t;
}

class Gpif {
public:
    Score Parse(const std::string& xml) {
        pugi::xml_document doc;
        if (!doc.load_buffer(xml.data(), xml.size())) throw FormatError("couldn't read score.gpif");
        pugi::xml_node root = doc.child("GPIF");
        if (!root) throw FormatError("no GPIF in score.gpif");
        auto sc = root.child("Score");
        s_.title = text(sc.child("Title"));
        s_.artist = text(sc.child("Artist"));
        s_.album = text(sc.child("Album"));

        auto mt = root.child("MasterTrack");
        trackOrder_ = split(text(mt.child("Tracks")));
        for (auto a : mt.child("Automations").children("Automation")) {
            if (std::strcmp(text(a.child("Type")), "Tempo") != 0) continue;
            int bar = toInt(text(a.child("Bar")));
            auto parts = split(text(a.child("Value")));
            if (parts.empty()) continue;
            double v = toFloat(parts[0].c_str());
            int ref = parts.size() > 1 ? toInt(parts[1].c_str()) : 1;
            static const double refs[] = {1, 0.5, 1, 1.5, 2, 3};
            if (ref < 1 || ref > 5) ref = 2;
            tempos_[bar].push_back({toFloat(text(a.child("Position"))), v * refs[ref]});
        }

        for (auto t : root.child("Tracks").children("Track")) parseTrack(t);
        for (auto m : root.child("MasterBars").children("MasterBar")) parseMasterBar(m);
        for (auto b : root.child("Bars").children("Bar")) voicesOfBar_[b.attribute("id").value()] = split(text(b.child("Voices")));
        for (auto v : root.child("Voices").children("Voice")) beatsOfVoice_[v.attribute("id").value()] = split(text(v.child("Beats")));
        for (auto b : root.child("Beats").children("Beat")) parseBeat(b);
        for (auto n : root.child("Notes").children("Note")) parseNote(n);
        for (auto r : root.child("Rhythms").children("Rhythm")) parseRhythm(r);
        build();
        return std::move(s_);
    }

private:
    void parseTrack(pugi::xml_node node) {
        Track t;
        t.name = text(node.child("Name"));
        bool haveSound = false;
        int staves = 1;
        std::string instrument = node.child("Instrument").attribute("ref").value();
        if (instrument.size() >= 3 && (instrument.compare(instrument.size() - 3, 3, "-gs") == 0 ||
                                       instrument.find("GrandStaff") != std::string::npos))
            staves = 2;
        for (auto c : node.children()) {
            std::string n = c.name();
            if (n == "Properties") {
                for (auto p : c.children("Property")) trackProperty(t, p);
            } else if (n == "GeneralMidi" || n == "MidiConnection" || n == "MIDISettings") {
                if (auto p = c.child("Program")) t.program = toInt(text(p));
                if (std::strcmp(c.attribute("table").value(), "Percussion") == 0) t.staff.percussion = true;
            } else if (n == "InstrumentSet") {
                if (std::strcmp(text(c.child("Type")), "drumKit") == 0) t.staff.percussion = true;
                auto& arts = articulations_[node.attribute("id").value()];
                for (auto e : c.child("Elements").children("Element"))
                    for (auto a : e.child("Articulations").children("Articulation"))
                        arts.push_back(toInt(text(a.child("OutputMidiNumber")), 38));
            } else if (n == "Sounds") {
                for (auto snd : c.children("Sound")) {
                    if (haveSound) break;
                    auto midi = snd.child("MIDI");
                    t.program = toInt(text(midi.child("Program")));
                    t.bank = (toInt(text(midi.child("MSB"))) & 127) << 7 | toInt(text(midi.child("LSB")));
                    haveSound = true;
                }
            } else if (n == "PlaybackState") {
                t.mute = std::strcmp(text(c), "Mute") == 0;
            } else if (n == "Staves") {
                // Only the first staff is kept.
                int count = 0;
                for (auto st : c.children("Staff")) {
                    if (count++ == 0)
                        for (auto p : st.child("Properties").children("Property")) trackProperty(t, p);
                }
                staves = std::max(staves, count);
            }
        }
        stavesById_[node.attribute("id").value()] = staves;
        tracksById_[node.attribute("id").value()] = std::move(t);
    }

    void trackProperty(Track& t, pugi::xml_node p) {
        std::string name = p.attribute("name").value();
        if (name == "Tuning") t.staff.tuning = tuningOf(p);
        else if (name == "CapoFret") t.staff.capo = toInt(text(p.child("Fret")));
    }

    void parseMasterBar(pugi::xml_node node) {
        MasterBar mb;
        mb.index = int(s_.masterBars.size());
        for (auto c : node.children()) {
            std::string n = c.name();
            if (n == "Time") {
                std::string t = text(c);
                size_t slash = t.find('/');
                mb.numerator = toInt(t.substr(0, slash).c_str(), 4);
                mb.denominator = slash == std::string::npos ? 4 : toInt(t.substr(slash + 1).c_str(), 4);
                if (mb.numerator <= 0) mb.numerator = 4;
                if (mb.denominator <= 0) mb.denominator = 4;
            } else if (n == "Section") {
                mb.hasSection = true;
                mb.section = text(c.child("Text"));
                if (mb.section.empty()) mb.section = text(c.child("Letter"));
            } else if (n == "Repeat") {
                std::string start = c.attribute("start").value(), end = c.attribute("end").value();
                auto lower = [](std::string s) {
                    for (auto& ch : s) ch = char(std::tolower(ch));
                    return s;
                };
                if (lower(start) == "true") mb.repeatStart = true;
                if (lower(end) == "true" && *c.attribute("count").value()) mb.repeatCount = toInt(c.attribute("count").value(), 1);
            } else if (n == "AlternateEndings") {
                int mask = 0;
                for (auto& e : split(text(c))) {
                    int k = toInt(e.c_str());
                    if (k >= 1 && k <= 31) mask |= 1 << (k - 1);
                }
                mb.alternateEndings = mask;
            } else if (n == "Bars") {
                barsOfMasterBar_.push_back(split(text(c)));
            }
        }
        if (barsOfMasterBar_.size() < s_.masterBars.size() + 1) barsOfMasterBar_.emplace_back();
        s_.masterBars.push_back(mb);
    }

    void parseBeat(pugi::xml_node node) {
        Beat b;
        std::string id = node.attribute("id").value();
        for (auto c : node.children()) {
            std::string n = c.name();
            if (n == "Notes") notesOfBeat_[id] = split(text(c));
            else if (n == "Rhythm") rhythmOfBeat_[id] = c.attribute("ref").value();
            else if (n == "GraceNotes") b.grace = std::strcmp(text(c), "OnBeat") == 0 ? Grace::OnBeat : Grace::BeforeBeat;
            else if (n == "Tremolo") {
                std::string t = text(c);
                b.tremoloPicking = t == "1/2" ? 8 : t == "1/4" ? 16 : 32;
            } else if (n == "Properties") {
                for (auto p : c.children("Property")) {
                    std::string pn = p.attribute("name").value();
                    if (pn == "Slapped" && p.child("Enable")) b.slap = true;
                    else if (pn == "Popped" && p.child("Enable")) b.pop = true;
                    else if (pn == "VibratoWTremBar") b.whammyVibrato = true;
                }
            }
        }
        beatById_[id] = std::move(b);
    }

    void parseNote(pugi::xml_node node) {
        Note n;
        std::string id = node.attribute("id").value();
        bool bended = false;
        int origin = 0, middle = -1, destination = 0;
        for (auto c : node.children()) {
            std::string name = c.name();
            if (name == "LetRing") n.letRing = true;
            else if (name == "Tie") {
                std::string d = c.attribute("destination").value();
                if (d == "true" || d == "True") n.tieDestination = true;
            } else if (name == "Vibrato") n.vibrato = true;
            else if (name == "Accent") n.staccato = toInt(text(c)) & 1;
            else if (name == "AntiAccent") n.ghost = true;
            else if (name == "InstrumentArticulation") drums_[id].articulation = toInt(text(c), -1);
            else if (name == "Properties") {
                for (auto p : c.children("Property")) {
                    std::string pn = p.attribute("name").value();
                    if (pn == "String") n.string = toInt(text(p.child("String"))) + 1;
                    else if (pn == "Element") drums_[id].element = toInt(text(p.child("Element")), -1);
                    else if (pn == "Variation") drums_[id].variation = toInt(text(p.child("Variation")), 0);
                    else if (pn == "Fret") n.fret = toInt(text(p.child("Fret")));
                    else if (pn == "Tapped") tapped_[id] = true;
                    else if (pn == "HarmonicType") {
                        std::string h = text(p.child("HType"));
                        for (auto& ch : h) ch = char(std::tolower(ch));
                        n.harmonic = h == "natural"      ? Harmonic::Natural
                                     : h == "artificial" ? Harmonic::Artificial
                                     : h == "pinch"      ? Harmonic::Pinch
                                     : h == "tap"        ? Harmonic::Tap
                                     : h == "semi"       ? Harmonic::Semi
                                     : h == "feedback"   ? Harmonic::Feedback
                                                         : Harmonic::None;
                    } else if (pn == "Muted" && p.child("Enable")) n.dead = true;
                    else if (pn == "PalmMuted" && p.child("Enable")) n.palmMute = true;
                    else if (pn == "Bended") bended = true;
                    else if (pn == "BendOriginValue") origin = int(toFloat(text(p.child("Float"))) / 25);
                    else if (pn == "BendMiddleValue") middle = int(toFloat(text(p.child("Float"))) / 25);
                    else if (pn == "BendDestinationValue") destination = int(toFloat(text(p.child("Float"))) / 25);
                    else if (pn == "HopoOrigin" && p.child("Enable")) n.hammerPullOrigin = true;
                    else if (pn == "Slide") {
                        int f = toInt(text(p.child("Flags")));
                        if (f & 1) n.slideOut = Slide::Shift;
                        else if (f & 2) n.slideOut = Slide::Legato;
                        else if (f & 4) n.slideOut = Slide::OutDown;
                        else if (f & 8) n.slideOut = Slide::OutUp;
                        if (f & 16) n.slideIn = Slide::IntoFromBelow;
                        else if (f & 32) n.slideIn = Slide::IntoFromAbove;
                        if (f & 64) n.slideOut = Slide::PickDown;
                        else if (f & 128) n.slideOut = Slide::PickUp;
                    }
                }
            }
        }
        if (bended) {
            n.bend.push_back({0, origin});
            if (middle >= 0) n.bend.push_back({30, middle});
            n.bend.push_back({60, destination});
        }
        noteById_[id] = std::move(n);
    }

    void parseRhythm(pugi::xml_node node) {
        Rhythm r;
        std::string v = text(node.child("NoteValue"));
        static const std::map<std::string, int> values = {{"Whole", 1}, {"Half", 2}, {"Quarter", 4}, {"Eighth", 8},
                                                          {"16th", 16}, {"32nd", 32}, {"64th", 64}, {"128th", 128},
                                                          {"256th", 256}, {"DoubleWhole", 1}, {"Long", 1}};
        auto it = values.find(v);
        if (it != values.end()) r.duration = it->second;
        if (auto t = node.child("PrimaryTuplet")) {
            r.num = toInt(t.attribute("num").value(), 1);
            r.den = toInt(t.attribute("den").value(), 1);
        }
        if (auto d = node.child("AugmentationDot")) r.dots = toInt(d.attribute("count").value());
        rhythmById_[node.attribute("id").value()] = r;
    }

    void build() {
        std::vector<std::string> ids;
        for (auto& id : trackOrder_)
            if (tracksById_.count(id)) ids.push_back(id);
        if (ids.empty())
            for (auto& [id, _] : tracksById_) ids.push_back(id);
        std::vector<int> staves;
        for (auto& id : ids) {
            s_.tracks.push_back(tracksById_[id]);
            staves.push_back(stavesById_[id]);
        }
        // Each master bar lists a bar per staff, track after track ("-1"
        // skips the rest of a track). Only each track's first staff is kept.
        size_t nt = s_.tracks.size();
        for (size_t m = 0; m < barsOfMasterBar_.size() && m < s_.masterBars.size(); m++) {
            const auto& barIds = barsOfMasterBar_[m];
            size_t t = 0;
            int staff = 0;
            for (size_t k = 0; k < barIds.size() && t < nt; k++) {
                if (barIds[k] == "-1") {
                    t++;
                    staff = 0;
                    continue;
                }
                if (staff == 0) {
                    Bar bar;
                    fillBar(bar, barIds[k], s_.tracks[t].staff.percussion, ids[t]);
                    s_.tracks[t].staff.bars.push_back(std::move(bar));
                }
                if (staff == staves[t] - 1) {
                    t++;
                    staff = 0;
                } else {
                    staff++;
                }
            }
            // Tracks the list missed get an empty bar, so bars line up.
            for (auto& tr : s_.tracks)
                if (tr.staff.bars.size() < m + 1) tr.staff.bars.resize(m + 1);
        }
        for (auto& [bar, list] : tempos_)
            if (bar >= 0 && bar < int(s_.masterBars.size()))
                for (auto& tc : list) s_.masterBars[size_t(bar)].tempos.push_back(tc);
        if (!s_.masterBars.empty() && !s_.masterBars[0].tempos.empty()) s_.tempo = s_.masterBars[0].tempos[0].bpm;
    }

    // drumNote is the MIDI note a drum note plays: GP7 names one of the
    // track's articulations, GP6 an element and variation of its kit.
    int drumNote(const std::string& noteId, const std::string& trackId) {
        auto d = drums_.find(noteId);
        if (d == drums_.end()) return 38;
        const auto& arts = articulations_[trackId];
        if (d->second.articulation >= 0 && d->second.articulation < int(arts.size())) return arts[size_t(d->second.articulation)];
        static const int gp6[17][3] = {{35, 35, 35}, {38, 38, 37}, {56, 56, 56}, {56, 56, 56}, {56, 56, 56}, {43, 43, 43},
                                       {45, 45, 45}, {47, 47, 47}, {48, 48, 48}, {50, 50, 50}, {42, 46, 46}, {44, 44, 44},
                                       {57, 57, 57}, {49, 49, 49}, {55, 55, 55}, {51, 51, 59}, {52, 52, 52}};
        int e = d->second.element, v = d->second.variation;
        if (e >= 0 && e < 17) return gp6[e][v >= 0 && v < 3 ? v : 0];
        return 38;
    }

    void fillBar(Bar& bar, const std::string& barId, bool percussion, const std::string& trackId) {
        auto vit = voicesOfBar_.find(barId);
        if (vit == voicesOfBar_.end()) return;
        for (auto& voiceId : vit->second) {
            if (voiceId == "-1") continue;
            Voice voice;
            auto bit = beatsOfVoice_.find(voiceId);
            if (bit != beatsOfVoice_.end()) {
                for (auto& beatId : bit->second) {
                    auto b = beatById_.find(beatId);
                    if (b == beatById_.end()) continue;
                    Beat beat = b->second;
                    auto r = rhythmById_.find(rhythmOfBeat_[beatId]);
                    if (r != rhythmById_.end()) {
                        beat.duration = r->second.duration;
                        beat.dots = r->second.dots;
                        beat.tupletNumerator = r->second.num;
                        beat.tupletDenominator = r->second.den;
                    }
                    for (auto& noteId : notesOfBeat_[beatId]) {
                        auto n = noteById_.find(noteId);
                        if (n == noteById_.end()) continue;
                        Note note = n->second;
                        if (percussion) {
                            note.string = -1;
                            note.fret = drumNote(noteId, trackId);
                        }
                        beat.notes.push_back(note);
                        if (tapped_.count(noteId)) beat.tap = true;
                    }
                    voice.beats.push_back(std::move(beat));
                }
            }
            bar.voices.push_back(std::move(voice));
        }
    }

    Score s_;
    std::vector<std::string> trackOrder_;
    std::map<std::string, Track> tracksById_;
    std::map<std::string, int> stavesById_;
    struct Drum {
        int articulation = -1, element = -1, variation = 0;
    };
    std::unordered_map<std::string, Drum> drums_;
    std::map<std::string, std::vector<int>> articulations_;
    std::vector<std::vector<std::string>> barsOfMasterBar_;
    std::unordered_map<std::string, std::vector<std::string>> voicesOfBar_, beatsOfVoice_, notesOfBeat_;
    std::unordered_map<std::string, Beat> beatById_;
    std::unordered_map<std::string, std::string> rhythmOfBeat_;
    std::unordered_map<std::string, Rhythm> rhythmById_;
    std::unordered_map<std::string, Note> noteById_;
    std::unordered_map<std::string, bool> tapped_;
    std::map<int, std::vector<TempoChange>> tempos_;
};

}  // namespace

Score ReadGpif(const std::string& xml) {
    Score s = Gpif().Parse(xml);
    Finish(s);
    return s;
}

Score ReadGpx(const std::vector<uint8_t>& data) {
    if (data.size() < 4) throw FormatError("not a GPX file");
    std::vector<uint8_t> fs;
    if (std::memcmp(data.data(), "BCFZ", 4) == 0) {
        fs = decompressBcfz(data);  // starts with "BCFS"
        if (fs.size() >= 4) fs.erase(fs.begin(), fs.begin() + 4);
    } else if (std::memcmp(data.data(), "BCFS", 4) == 0) {
        fs.assign(data.begin() + 4, data.end());
    } else {
        throw FormatError("not a GPX file");
    }
    std::string xml = gpxFile(fs, "score.gpif");
    if (xml.empty()) throw FormatError("no score.gpif in the GPX file");
    return ReadGpif(xml);
}

Score ReadGp7(const std::vector<uint8_t>& data) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) throw FormatError("not a zip file");
    std::string xml;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        char name[512];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        std::string s = name;
        if (s == "score.gpif" || (s.size() > 11 && s.compare(s.size() - 11, 11, "/score.gpif") == 0)) {
            size_t size = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (p) {
                xml.assign(static_cast<const char*>(p), size);
                mz_free(p);
            }
            break;
        }
    }
    mz_zip_reader_end(&zip);
    if (xml.empty()) throw FormatError("no score.gpif in the file");
    return ReadGpif(xml);
}

}  // namespace dl
