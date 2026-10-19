// Guitar Pro 3, 4 and 5: a little-endian binary stream, read in order.
// A port of alphaTab's Gp3To5Importer, keeping only what dailylick uses.
#include <cstring>

#include "gp.h"

namespace dl {
namespace {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}

    uint8_t U8() {
        need(1);
        return d_[pos_++];
    }
    int8_t I8() { return int8_t(U8()); }
    int16_t I16() {
        need(2);
        int16_t v = int16_t(d_[pos_] | d_[pos_ + 1] << 8);
        pos_ += 2;
        return v;
    }
    int32_t I32() {
        need(4);
        uint32_t v = d_[pos_] | d_[pos_ + 1] << 8 | d_[pos_ + 2] << 16 | uint32_t(d_[pos_ + 3]) << 24;
        pos_ += 4;
        return int32_t(v);
    }
    double F64BE() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = v << 8 | d_[pos_ + i];
        pos_ += 8;
        double f;
        std::memcpy(&f, &v, 8);
        return f;
    }
    bool Bool() { return U8() != 0; }
    void Skip(size_t n) {
        need(n);
        pos_ += n;
    }
    std::string Bytes(int n) {
        if (n < 0 || n > 1 << 20) throw FormatError("bad string length");
        need(size_t(n));
        std::string s(reinterpret_cast<const char*>(d_.data() + pos_), size_t(n));
        pos_ += size_t(n);
        return s;
    }
    // A length byte, then a field of fixed size.
    std::string StringByteLength(int field) {
        int len = U8();
        std::string s = Bytes(field);
        return ToUtf8(s.substr(0, std::min(len, field)));
    }
    // An unused int, then a length byte and the string.
    std::string StringIntUnused() {
        Skip(4);
        return ToUtf8(Bytes(U8()));
    }
    std::string StringInt() { return ToUtf8(Bytes(I32())); }
    // An int length (counting the next byte), a byte, the string.
    std::string StringIntByte() {
        int len = I32() - 1;
        U8();
        return ToUtf8(Bytes(len));
    }

private:
    void need(size_t n) const {
        if (pos_ + n > d_.size()) throw FormatError("unexpected end of file");
    }
    const std::vector<uint8_t>& d_;
    size_t pos_ = 0;
};

struct Channel {
    int program = 25, volume = 13, balance = 8;
};

class Gp3To5 {
public:
    explicit Gp3To5(const std::vector<uint8_t>& d) : r_(d) {}

    Score Read() {
        readVersion();
        readInfo();
        if (v_ < 500) r_.Bool();  // triplet feel
        if (v_ >= 400) readLyrics();
        if (v_ >= 510) r_.Skip(19);
        if (v_ >= 500) {
            readPageSetup();
            r_.StringIntByte();  // tempo label
        }
        s_.tempo = r_.I32();
        if (v_ >= 510) r_.Bool();
        r_.I32();  // key
        if (v_ >= 400) r_.U8();
        for (auto& c : channels_) {
            c.program = r_.I32();
            c.volume = r_.U8();
            c.balance = r_.U8();
            r_.Skip(6);
        }
        if (v_ >= 500) r_.Skip(19 * 2 + 4);  // directions
        int bars = r_.I32(), tracks = r_.I32();
        if (bars < 0 || bars > 1000 || tracks < 0 || tracks > 100) throw FormatError("implausible bar or track count");
        for (int i = 0; i < bars; i++) readMasterBar();
        for (int i = 0; i < tracks; i++) readTrack();
        for (int b = 0; b < bars; b++)
            for (auto& t : s_.tracks) readBar(t);
        if (!s_.masterBars.empty() && s_.tempo > 0) {
            auto& t = s_.masterBars[0].tempos;
            if (t.empty() || t[0].ratio != 0) t.insert(t.begin(), TempoChange{0, s_.tempo});
        }
        return std::move(s_);
    }

private:
    void readVersion() {
        std::string v = r_.StringByteLength(30);
        const std::string prefix = "FICHIER GUITAR PRO ";
        if (v.rfind(prefix, 0) != 0) throw FormatError("not a Guitar Pro 3-5 file");
        v = v.substr(prefix.size() + 1);
        size_t dot = v.find('.');
        if (dot == std::string::npos) throw FormatError("bad version");
        v_ = 100 * std::atoi(v.substr(0, dot).c_str()) + std::atoi(v.substr(dot + 1).c_str());
    }

    void readInfo() {
        s_.title = r_.StringIntUnused();
        r_.StringIntUnused();  // subtitle
        s_.artist = r_.StringIntUnused();
        s_.album = r_.StringIntUnused();
        r_.StringIntUnused();  // words
        if (v_ >= 500) r_.StringIntUnused();  // music
        r_.StringIntUnused();  // copyright
        r_.StringIntUnused();  // tab
        r_.StringIntUnused();  // instructions
        int lines = r_.I32();
        if (lines < 0 || lines > 1000) throw FormatError("implausible notice");
        for (int i = 0; i < lines; i++) r_.StringIntUnused();
    }

    void readLyrics() {
        r_.I32();
        for (int i = 0; i < 5; i++) {
            r_.I32();
            r_.StringInt();
        }
    }

    void readPageSetup() {
        r_.Skip(28);
        r_.I16();
        for (int i = 0; i < 10; i++) r_.StringIntByte();
    }

    void readMasterBar() {
        const MasterBar* prev = s_.masterBars.empty() ? nullptr : &s_.masterBars.back();
        MasterBar mb;
        mb.index = int(s_.masterBars.size());
        int flags = r_.U8();
        if (flags & 1) mb.numerator = r_.U8();
        else if (prev) mb.numerator = prev->numerator;
        if (flags & 2) mb.denominator = r_.U8();
        else if (prev) mb.denominator = prev->denominator;
        mb.repeatStart = flags & 4;
        if (flags & 8) mb.repeatCount = r_.U8() + (v_ >= 500 ? 0 : 1);
        if ((flags & 16) && v_ < 500) {
            // GP3 and 4 give the number of the ending; the mask leaves out
            // endings the bars before have already taken.
            int existing = 0;
            for (int i = int(s_.masterBars.size()) - 1; i >= 0; i--) {
                const auto& m = s_.masterBars[i];
                if (m.repeatCount > 0 && i != int(s_.masterBars.size()) - 1) break;
                if (m.repeatStart) break;
                existing |= m.alternateEndings;
            }
            int n = r_.U8(), mask = 0;
            for (int i = 0; i < 8; i++)
                if (n > i && !(existing & (1 << i))) mask |= 1 << i;
            mb.alternateEndings = mask;
        }
        if (flags & 32) {
            mb.hasSection = true;
            mb.section = r_.StringIntByte();
            r_.Skip(4);  // colour
        }
        if (flags & 64) r_.Skip(2);  // key signature
        if (v_ >= 500 && (flags & 3)) r_.Skip(4);
        if (v_ >= 500) mb.alternateEndings = r_.U8();
        if (v_ >= 500) r_.Skip(2);  // triplet feel, and a byte
        if (mb.numerator <= 0 || mb.denominator <= 0) throw FormatError("bad time signature");
        s_.masterBars.push_back(mb);
    }

    void readTrack() {
        Track t;
        t.index = int(s_.tracks.size());
        int flags = r_.U8();
        t.name = r_.StringByteLength(40);
        t.staff.percussion = flags & 1;
        int strings = r_.I32();
        for (int i = 0; i < 7; i++) {
            int tuning = r_.I32();
            if (strings > i) t.staff.tuning.push_back(tuning);
        }
        r_.I32();  // port
        int index = r_.I32() - 1;
        r_.I32();  // effect channel
        r_.Skip(4);
        if (index >= 0 && index < 64) {
            const auto& c = channels_[index];
            t.program = c.program;
            t.volume = c.volume;
            t.balance = c.balance;
            t.mute = flags & 32;
            if (index == 9) t.staff.percussion = true;
        }
        t.staff.capo = r_.I32();
        r_.Skip(4);  // colour
        if (v_ >= 500) {
            r_.U8();
            r_.U8();
            r_.U8();
            t.bank = r_.U8();
            r_.U8();
            r_.I32();
            r_.I32();
            r_.I32();
            r_.Skip(10);
            r_.U8();
            r_.U8();
            r_.Skip(16);  // RSE bank
            if (v_ >= 510) {
                r_.Skip(4);
                r_.StringIntByte();
                r_.StringIntByte();
            }
        }
        s_.tracks.push_back(std::move(t));
    }

    void readBar(Track& t) {
        Bar bar;
        int voices = 1;
        if (v_ >= 500) {
            r_.U8();
            voices = 2;
        }
        for (int v = 0; v < voices; v++) readVoice(t, bar);
        t.staff.bars.push_back(std::move(bar));
    }

    void readVoice(Track& t, Bar& bar) {
        int beats = r_.I32();
        if (beats == 0) return;
        if (beats < 0 || beats > 100) throw FormatError("implausible beat count");
        bar.voices.emplace_back();
        Voice& voice = bar.voices.back();
        for (int i = 0; i < beats; i++) readBeat(t, voice);
    }

    void readBeat(Track& t, Voice& voice) {
        voice.beats.emplace_back();
        size_t at = voice.beats.size() - 1;
        auto beat = [&]() -> Beat& { return voice.beats[at]; };
        int flags = r_.U8();
        if (flags & 1) beat().dots = 1;
        if (flags & 64) beat().empty = (r_.U8() & 2) == 0;
        switch (r_.I8()) {
        case -2: beat().duration = 1; break;
        case -1: beat().duration = 2; break;
        case 1: beat().duration = 8; break;
        case 2: beat().duration = 16; break;
        case 3: beat().duration = 32; break;
        case 4: beat().duration = 64; break;
        default: beat().duration = 4; break;
        }
        if (flags & 32) {
            int n = r_.I32(), d = 1;
            switch (n) {
            case 1: d = 1; break;
            case 3: d = 2; break;
            case 5: case 6: case 7: d = 4; break;
            case 9: case 10: case 11: case 12: case 13: d = 8; break;
            default: n = 1; d = 1; break;  // 2, 4 and 8 too: alphaTab ignores them
            }
            beat().tupletNumerator = n;
            beat().tupletDenominator = d;
        }
        if (flags & 2) readChord(t);
        if (flags & 4) r_.StringIntUnused();  // text
        Harmonic allHarmonic = Harmonic::None;
        if (flags & 8) allHarmonic = readBeatEffects(beat());
        if (flags & 16) readMixTableChange(beat());
        int stringFlags = r_.U8();
        int count = int(t.staff.tuning.size());
        for (int i = 6; i >= 0; i--) {
            if ((stringFlags & (1 << i)) && 6 - i < count) {
                Note* n = readNote(t, voice, at, count - (6 - i));
                if (allHarmonic != Harmonic::None) n->harmonic = allHarmonic;
            }
        }
        if (v_ >= 500) {
            int flags2 = r_.I16();
            if (flags2 & 2048) r_.U8();
        }
    }

    void readChord(const Track& t) {
        if (v_ >= 500) {
            r_.Skip(17);
            r_.StringByteLength(21);
            r_.Skip(4);
            r_.I32();
            r_.Skip(7 * 4);
            r_.U8();
            r_.Skip(5);
            r_.Skip(26);
        } else if (r_.U8() != 0) {
            if (v_ >= 400) {
                r_.Skip(16);
                r_.StringByteLength(21);
                r_.Skip(4);
                r_.I32();
                r_.Skip(7 * 4);
                r_.U8();
                r_.Skip(5);
                r_.Skip(26);
            } else {
                r_.Skip(25);
                r_.StringByteLength(34);
                r_.I32();
                r_.Skip(6 * 4);
                r_.Skip(36);
            }
        } else {
            int strings = v_ >= 406 ? 7 : 6;
            r_.StringIntByte();
            int first = r_.I32();
            if (first > 0) r_.Skip(size_t(strings) * 4);
        }
        (void)t;
    }

    Harmonic readBeatEffects(Beat& beat) {
        int flags = r_.U8(), flags2 = 0;
        if (v_ >= 400) flags2 = r_.U8();
        if ((v_ < 400 && (flags & 1)) || (flags & 2)) beat.whammyVibrato = true;
        if (flags & 32) {
            switch (r_.I8()) {
            case 1: beat.tap = true; break;
            case 2: beat.slap = true; break;
            case 3: beat.pop = true; break;
            }
            if (v_ < 400) r_.Skip(4);
        }
        if (flags2 & 4) readBendPoints(nullptr);  // tremolo bar
        if (flags & 64) r_.Skip(2);  // stroke
        if (flags2 & 2) r_.I8();  // pick stroke
        if (v_ < 400) {
            if (flags & 4) return Harmonic::Natural;
            if (flags & 8) return Harmonic::Artificial;
        }
        return Harmonic::None;
    }

    void readBendPoints(std::vector<BendPoint>* out) {
        r_.U8();
        r_.I32();
        int n = r_.I32();
        if (n < 0 || n > 240) throw FormatError("implausible bend");
        for (int i = 0; i < n; i++) {
            BendPoint p;
            p.offset = r_.I32();
            p.value = r_.I32() / 25;
            r_.Bool();
            if (out) out->push_back(p);
        }
    }

    void readMixTableChange(Beat& beat) {
        int instrument = r_.I8();
        (void)instrument;
        if (v_ >= 500) r_.Skip(16);
        int volume = r_.I8(), balance = r_.I8(), chorus = r_.I8(), reverb = r_.I8(), phaser = r_.I8(), tremolo = r_.I8();
        if (v_ >= 500) r_.StringIntByte();
        int tempo = r_.I32();
        for (int x : {volume, balance, chorus, reverb, phaser, tremolo})
            if (x >= 0) r_.U8();
        if (tempo >= 0) {
            r_.I8();
            if (v_ >= 510) r_.U8();
        }
        if (v_ >= 400) r_.U8();
        if (v_ >= 500) r_.I8();
        if (v_ >= 510) {
            r_.StringIntByte();
            r_.StringIntByte();
        }
        if (tempo > 0) beat.tempo = tempo;
    }

    Note* readNote(Track& t, Voice& voice, size_t& at, int string) {
        Note n;
        n.string = string;
        int flags = r_.U8();
        n.ghost = flags & 4;
        if (flags & 32) {
            int type = r_.U8();
            if (type == 3) n.dead = true;
            else if (type == 2) n.tieDestination = true;
        }
        if ((flags & 1) && v_ < 500) r_.Skip(2);
        if (flags & 16) r_.I8();  // dynamics
        if (flags & 32) n.fret = r_.I8();
        if (flags & 128) r_.Skip(2);  // fingering
        if (v_ >= 500) {
            if (flags & 1) r_.F64BE();
            r_.U8();
        }
        voice.beats[at].notes.push_back(n);
        size_t ni = voice.beats[at].notes.size() - 1;
        if (flags & 8) readNoteEffects(t, voice, at, ni);
        Note& note = voice.beats[at].notes[ni];
        if (t.staff.percussion) {
            note.string = -1;
            // fret holds the drum's MIDI note
        }
        return &note;
    }

    void readNoteEffects(Track& t, Voice& voice, size_t& at, size_t ni) {
        int flags = r_.U8(), flags2 = 0;
        if (v_ >= 400) flags2 = r_.U8();
        if (flags & 1) readBendPoints(&voice.beats[at].notes[ni].bend);
        if (flags & 16) readGrace(voice, at, ni);
        Note& n = voice.beats[at].notes[ni];
        if (flags2 & 4) voice.beats[at].tremoloPicking = readTremolo();
        if (flags2 & 8) readSlide(n);
        else if (v_ < 400 && (flags & 4)) n.slideOut = Slide::Shift;
        if (flags2 & 16) readArtificialHarmonic(n);
        if (flags2 & 32) r_.Skip(2);  // trill
        n.letRing = flags & 8;
        n.hammerPullOrigin = flags & 2;
        if (flags2 & 64) n.vibrato = true;
        n.palmMute = flags2 & 2;
        n.staccato = flags2 & 1;
        (void)t;
    }

    int readTremolo() {
        switch (r_.U8()) {
        case 1: return 8;
        case 2: return 16;
        case 3: return 32;
        }
        return 16;
    }

    // readGrace puts a grace beat before the beat being read, as alphaTab
    // does; the beat moves along one.
    void readGrace(Voice& voice, size_t& at, size_t ni) {
        Beat g;
        g.duration = 32;
        g.grace = Grace::BeforeBeat;
        Note gn;
        gn.string = voice.beats[at].notes[ni].string;
        gn.fret = r_.I8();
        r_.I8();  // dynamics
        switch (r_.I8()) {
        case 1: gn.slideOut = Slide::Legato; break;
        case 3: gn.hammerPullOrigin = true; break;
        }
        r_.Skip(1);
        if (v_ >= 500) {
            int f = r_.U8();
            gn.dead = f & 1;
            g.grace = (f & 2) ? Grace::OnBeat : Grace::BeforeBeat;
        }
        g.notes.push_back(gn);
        voice.beats.insert(voice.beats.begin() + at, std::move(g));
        at++;
    }

    void readSlide(Note& n) {
        if (v_ >= 500) {
            int type = r_.I8();
            if (type & 1) n.slideOut = Slide::Shift;
            else if (type & 2) n.slideOut = Slide::Legato;
            else if (type & 4) n.slideOut = Slide::OutDown;
            else if (type & 8) n.slideOut = Slide::OutUp;
            if (type & 16) n.slideIn = Slide::IntoFromBelow;
            else if (type & 32) n.slideIn = Slide::IntoFromAbove;
        } else {
            switch (r_.I8()) {
            case 1: n.slideOut = Slide::Shift; break;
            case 2: n.slideOut = Slide::Legato; break;
            case 3: n.slideOut = Slide::OutDown; break;
            case 4: n.slideOut = Slide::OutUp; break;
            case -1: n.slideIn = Slide::IntoFromBelow; break;
            case -2: n.slideIn = Slide::IntoFromAbove; break;
            }
        }
    }

    void readArtificialHarmonic(Note& n) {
        int type = r_.U8();
        if (v_ >= 500) {
            switch (type) {
            case 1: n.harmonic = Harmonic::Natural; break;
            case 2: r_.Skip(3); n.harmonic = Harmonic::Artificial; break;
            case 3: r_.U8(); n.harmonic = Harmonic::Tap; break;
            case 4: n.harmonic = Harmonic::Pinch; break;
            case 5: n.harmonic = Harmonic::Semi; break;
            }
        } else if (v_ >= 400) {
            switch (type) {
            case 1: n.harmonic = Harmonic::Natural; break;
            case 3: n.harmonic = Harmonic::Tap; break;
            case 4: n.harmonic = Harmonic::Pinch; break;
            case 5: n.harmonic = Harmonic::Semi; break;
            case 15: case 17: case 22: n.harmonic = Harmonic::Artificial; break;
            }
        }
    }

    Reader r_;
    int v_ = 0;
    Score s_;
    Channel channels_[64];
};

}  // namespace

Score ReadGp3To5(const std::vector<uint8_t>& data) {
    Score s = Gp3To5(data).Read();
    Finish(s);
    return s;
}

}  // namespace dl
