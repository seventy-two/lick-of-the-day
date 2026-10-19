// dailylick's tests: everything but the window. Builds on Windows (in
// build.bat) and Linux (see the README).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "daily.h"
#include "gp.h"
#include "library.h"
#include "lick.h"
#include "player.h"
#include "sequence.h"

namespace {

int failures = 0, checks = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            std::printf("%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        checks++;                                                                    \
        auto va = (a);                                                               \
        auto vb = (b);                                                               \
        if (!(va == vb)) {                                                           \
            failures++;                                                              \
            std::printf("%s:%d: failed: %s == %s\n", __FILE__, __LINE__, #a, #b);    \
        }                                                                            \
    } while (0)

using namespace dl;

void testSongKey() {
    CHECK_EQ(SongKey("C:/tabs/Alice Cooper - Poison (ver 4).gp5"), std::string("alice cooper poison"));
    CHECK_EQ(SongKey("alice_cooper_poison.gp3"), std::string("alice cooper poison"));
    CHECK_EQ(SongKey("August Burns Red - Composure (ver 8 by Dismissing)(1).gp5"), std::string("august burns red composure"));
    CHECK_EQ(SongKey("The Black Dahlia Murder - Statutory Ape.gp"), std::string("black dahlia murder statutory ape"));
    CHECK_EQ(SongKey("Annihilator - Phoenix Rising (guitar pro).gpx"), std::string("annihilator phoenix rising"));
    CHECK_EQ(ExtRank("a.GP"), 0);
    CHECK_EQ(ExtRank("a.gp3"), 4);
    CHECK_EQ(ExtRank("a.pdf"), -1);
}

void testHashAndRng() {
    // The same numbers as the JavaScript the heuristic was tuned in.
    CHECK_EQ(Hash("2026-10-19|0"), 2123885140u);
    Rng r(12345);
    CHECK(std::fabs(r.Next() - 0.979728267761) < 1e-9);
    CHECK(std::fabs(r.Next() - 0.306752264500) < 1e-9);
    CHECK(std::fabs(r.Next() - 0.484205421526) < 1e-9);
}

void testTuning() {
    CHECK_EQ(DescribeTuning({64, 59, 55, 50, 45, 40}, 0), std::string(""));
    CHECK_EQ(DescribeTuning({64, 59, 55, 50, 45, 38}, 0), std::string("Drop D"));
    CHECK_EQ(DescribeTuning({63, 58, 54, 49, 44, 39}, 0), std::string("Eb standard"));
    CHECK_EQ(DescribeTuning({62, 57, 53, 48, 43, 36}, 0), std::string("Drop C"));
    CHECK_EQ(DescribeTuning({64, 59, 55, 50, 45, 40, 35}, 0), std::string("7-string"));
    CHECK_EQ(DescribeTuning({64, 59, 55, 50, 45, 40, 33}, 0), std::string("7-string Drop A"));
    CHECK_EQ(DescribeTuning({62, 57, 55, 50, 45, 38}, 0), std::string("Tuning D A D G A D"));
    CHECK_EQ(DescribeTuning({64, 59, 55, 50, 45, 40}, 2), std::string("capo 2"));

    // A D standard guitar (top string D, 62) playing along.
    const std::vector<int> bStd = {59, 54, 50, 45, 40, 35}, eStd = {64, 59, 55, 50, 45, 40};
    CHECK_EQ(TopString(bStd, 0), 59);
    CHECK_EQ(TopString(eStd, 2), 66);
    CHECK_EQ(TransposeFor(bStd, 0, 62), 3);
    CHECK_EQ(TransposeFor(eStd, 0, 62), -2);
    CHECK_EQ(TransposeFor(eStd, 0, 0), 0);  // as written
    CHECK_EQ(TransposeFor(eStd, 0, 40), -12);
    CHECK_EQ(TransposeFor({62, 57, 53, 48, 43, 36}, 0, 62), 0);  // Drop C has D on top
    CHECK_EQ(ShiftedTuningName(bStd, 3), std::string("D standard"));
    CHECK_EQ(ShiftedTuningName(bStd, 5), std::string("E standard"));
    CHECK_EQ(ShiftedTuningName({64, 59, 55, 50, 45, 38}, -2), std::string("Drop C"));
    CHECK_EQ(ShiftedTuningName({62, 57, 55, 50, 45, 38}, 1), std::string("Eb Bb Eb Ab Bb Eb"));
}

void testDates() {
    CHECK_EQ(DaysBefore("2026-10-19", 1), std::string("2026-10-18"));
    CHECK_EQ(DaysBefore("2026-03-01", 1), std::string("2026-02-28"));
    CHECK_EQ(DaysBefore("2024-03-01", 1), std::string("2024-02-29"));
    CHECK_EQ(DaysBefore("2026-01-05", 120), std::string("2025-09-07"));
}

TabFile tf(const std::string& path, int64_t size) {
    TabFile f;
    f.path = path;
    f.size = size;
    f.song = SongKey(path);
    return f;
}

void testCandidates() {
    std::vector<TabFile> files = {tf("a/Song One.gp3", 10), tf("a/Song One (ver 2).gp5", 10), tf("a/song_one.gpx", 5),
                                  tf("a/Song Two.gp5", 10), tf("a/Song Three.gp4", 10)};
    auto c = Candidates(files, "seed", {}, {}, 60);
    CHECK_EQ(c.size(), size_t(4));  // two versions of Song One at most
    // Song One's versions come together, the GPX first.
    for (size_t i = 0; i < c.size(); i++)
        if (c[i].song == "song one") {
            CHECK_EQ(c[i].path, std::string("a/song_one.gpx"));
            CHECK_EQ(c[i + 1].path, std::string("a/Song One (ver 2).gp5"));
            break;
        }
    auto again = Candidates(files, "seed", {}, {}, 60);
    CHECK_EQ(again[0].path, c[0].path);  // the same seed, the same order
    auto without = Candidates(files, "seed", {"song two"}, {files[4].Key()}, 60);
    for (auto& f : without) CHECK(f.song == "song one");
    CHECK_EQ(Candidates(files, "seed", {}, {}, 2).size(), size_t(2));

    State s;
    s.days["2026-10-18"].lick = SavedLick{};
    s.days["2026-10-18"].lick->path = "a/Song Two.gp5";
    s.days["2026-01-01"].lick = SavedLick{};
    s.days["2026-01-01"].lick->path = "a/Song Three.gp4";
    s.days["2026-10-19"].skipped = {"a/song_one.gpx"};
    auto r = Recent(s, "2026-10-19", 120);
    CHECK(r.count("song two") && r.count("song one") && !r.count("song three"));
}

// A score in GPIF: one guitar track of two 4/4 bars, the first a riff of
// open-string quarters, the second eighths with a hammer-on and a bend.
const char* kGpif = R"(<?xml version="1.0" encoding="utf-8"?>
<GPIF>
 <Score><Title><![CDATA[Test Song]]></Title><Artist><![CDATA[Band]]></Artist></Score>
 <MasterTrack><Tracks>0</Tracks>
  <Automations><Automation><Type>Tempo</Type><Linear>false</Linear><Bar>0</Bar><Position>0</Position><Value>120 2</Value></Automation>
   <Automation><Type>Tempo</Type><Linear>false</Linear><Bar>1</Bar><Position>0</Position><Value>60 2</Value></Automation></Automations>
 </MasterTrack>
 <Tracks><Track id="0"><Name>Lead</Name>
  <Properties><Property name="Tuning"><Pitches>40 45 50 55 59 64</Pitches></Property></Properties>
  <GeneralMidi table="Instrument"><Program>29</Program></GeneralMidi></Track></Tracks>
 <MasterBars>
  <MasterBar><Time>4/4</Time><Section><Letter>A</Letter><Text>Solo</Text></Section><Bars>0</Bars></MasterBar>
  <MasterBar><Time>4/4</Time><Bars>1</Bars></MasterBar>
 </MasterBars>
 <Bars><Bar id="0"><Voices>0 -1 -1 -1</Voices></Bar><Bar id="1"><Voices>1 -1 -1 -1</Voices></Bar></Bars>
 <Voices><Voice id="0"><Beats>0 1 2 3</Beats></Voice><Voice id="1"><Beats>4 5 6 7 8 9 10 11</Beats></Voice></Voices>
 <Beats>
  <Beat id="0"><Rhythm ref="0"/><Notes>0</Notes></Beat><Beat id="1"><Rhythm ref="0"/><Notes>1</Notes></Beat>
  <Beat id="2"><Rhythm ref="0"/><Notes>2</Notes></Beat><Beat id="3"><Rhythm ref="0"/><Notes>3</Notes></Beat>
  <Beat id="4"><Rhythm ref="1"/><Notes>4</Notes></Beat><Beat id="5"><Rhythm ref="1"/><Notes>5</Notes></Beat>
  <Beat id="6"><Rhythm ref="1"/><Notes>6</Notes></Beat><Beat id="7"><Rhythm ref="1"/><Notes>7</Notes></Beat>
  <Beat id="8"><Rhythm ref="1"/><Notes>8</Notes></Beat><Beat id="9"><Rhythm ref="1"/><Notes>9</Notes></Beat>
  <Beat id="10"><Rhythm ref="1"/></Beat><Beat id="11"><Rhythm ref="1"/><Notes>10</Notes></Beat>
 </Beats>
 <Notes>
  <Note id="0"><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
  <Note id="1"><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
  <Note id="2"><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
  <Note id="3"><Tie origin="false" destination="true"/><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>9</Fret></Property></Properties></Note>
  <Note id="4"><Properties><Property name="String"><String>5</String></Property><Property name="Fret"><Fret>12</Fret></Property><Property name="HopoOrigin"><Enable/></Property></Properties></Note>
  <Note id="5"><Properties><Property name="String"><String>5</String></Property><Property name="Fret"><Fret>15</Fret></Property></Properties></Note>
  <Note id="6"><Properties><Property name="String"><String>4</String></Property><Property name="Fret"><Fret>15</Fret></Property><Property name="Bended"><Enable/></Property><Property name="BendDestinationValue"><Float>100</Float></Property></Properties></Note>
  <Note id="7"><Properties><Property name="String"><String>4</String></Property><Property name="Fret"><Fret>12</Fret></Property></Properties></Note>
  <Note id="8"><Properties><Property name="String"><String>3</String></Property><Property name="Fret"><Fret>14</Fret></Property></Properties></Note>
  <Note id="9"><Properties><Property name="String"><String>3</String></Property><Property name="Fret"><Fret>12</Fret></Property><Property name="PalmMuted"><Enable/></Property></Properties></Note>
  <Note id="10"><Properties><Property name="String"><String>2</String></Property><Property name="Fret"><Fret>14</Fret></Property></Properties></Note>
 </Notes>
 <Rhythms><Rhythm id="0"><NoteValue>Quarter</NoteValue></Rhythm><Rhythm id="1"><NoteValue>Eighth</NoteValue></Rhythm></Rhythms>
</GPIF>)";

void testGpif() {
    Score s = ReadGpif(kGpif);
    CHECK_EQ(s.title, std::string("Test Song"));
    CHECK_EQ(s.artist, std::string("Band"));
    CHECK_EQ(s.tracks.size(), size_t(1));
    CHECK_EQ(s.masterBars.size(), size_t(2));
    CHECK_EQ(s.masterBars[0].section, std::string("Solo"));
    CHECK_EQ(s.masterBars[1].start, 3840);
    CHECK(s.tempo == 120);
    const Track& t = s.tracks[0];
    CHECK_EQ(t.program, 29);
    CHECK_EQ(t.staff.tuning[0], 64);  // top string first
    CHECK_EQ(t.staff.StringPitch(1), 40);
    const auto& b0 = t.staff.bars[0].voices[0].beats;
    CHECK_EQ(b0.size(), size_t(4));
    CHECK_EQ(b0[3].notes[0].fret, 0);  // a tie takes its fret from the note before
    const auto& b1 = t.staff.bars[1].voices[0].beats;
    CHECK_EQ(b1[0].notes[0].string, 6);
    CHECK(b1[0].notes[0].hammerPullOrigin);
    CHECK(b1[1].notes[0].hammerPullDestination);
    CHECK_EQ(b1[2].notes[0].bend.back().value, 4);  // a full bend, in quarter tones
    CHECK(b1[6].IsRest());
    CHECK_EQ(b1[7].start, 7 * 480);
}

void testText() {
    CHECK_EQ(ToUtf8("Andr\xE9"), std::string("Andr\xC3\xA9"));                // Windows-1252
    CHECK_EQ(ToUtf8("\xC3\xA9t\xC3\xA9"), std::string("\xC3\xA9t\xC3\xA9"));  // UTF-8 stays
    CHECK_EQ(ToUtf8("\xC3\xE8\xF2\xE0\xF0\xE0 1"), std::string("\xD0\x93\xD0\xB8\xD1\x82\xD0\xB0\xD1\x80\xD0\xB0 1"));  // Гитара 1
    CHECK_EQ(ToUtf8(std::string("ab\0\0", 4)), std::string("ab"));
}

void testLoadScoreRejects() {
    bool threw = false;
    try {
        LoadScore({'n', 'o', 'p', 'e'});
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        LoadScore(std::vector<uint8_t>(64, 0));
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

void testSequence() {
    Score s = ReadGpif(kGpif);
    Sequence q = BuildSequence(s, 0, 2);
    // Bar 1 at 120 (2 s), bar 2 at 60 (4 s).
    CHECK(std::fabs(q.length - 6.0) < 1e-6);
    CHECK_EQ(q.SecToTick(2.0), 3840);
    CHECK(std::fabs(q.TickToSec(3840 + 480) - 2.5) < 1e-6);
    int ons = 0;
    double tiedOff = -1;
    for (const auto& e : q.events) {
        if (e.type == Event::On) ons++;
        if (e.type == Event::Off && e.key == 40 && e.time > 1.0) tiedOff = e.time;
    }
    CHECK_EQ(ons, 3 + 7);  // the tied note doesn't sound again; the rest is silent
    CHECK(tiedOff > 1.9);  // the third quarter rings on through its tie
    int clicks = 0;
    for (const auto& e : q.clicks) clicks += e.type == Event::On;
    CHECK_EQ(clicks, 8);
    CHECK_EQ(q.countIn.size(), size_t(8));
    CHECK(std::fabs(q.countIn.front().time + 2.0) < 1e-6);
    bool bent = false;
    for (const auto& e : q.events) bent |= e.type == Event::Wheel && e.wheel > 8192 + 8192 / kWheelRange;
    CHECK(bent);
    // Sorted.
    for (size_t i = 1; i < q.events.size(); i++) CHECK(q.events[i - 1].time <= q.events[i].time);

    Sequence one = BuildSequence(s, 1, 2);
    CHECK(std::fabs(one.length - 4.0) < 1e-6);
    CHECK_EQ(one.startTick, 3840);
}

void testLick() {
    // A riff repeated, then a solo: busy single notes up the neck.
    Score s;
    s.tempo = 120;
    Track t;
    t.name = "Guitar";
    t.program = 30;
    t.staff.tuning = {64, 59, 55, 50, 45, 40};
    for (int b = 0; b < 16; b++) {
        MasterBar mb;
        s.masterBars.push_back(mb);
        Bar bar;
        bar.voices.emplace_back();
        bool solo = b >= 8 && b < 14;
        for (int i = 0; i < 8; i++) {
            Beat beat;
            beat.duration = solo ? 16 : 8;
            Note n;
            if (solo) {
                n.string = 1 + (i + b) % 3;
                n.fret = 12 + (i * 3 + b) % 7;
                n.bend = i == 3 ? std::vector<BendPoint>{{0, 0}, {60, 4}} : std::vector<BendPoint>{};
            } else {
                n.string = 1;
                n.fret = 0;
                n.palmMute = true;
            }
            beat.notes.push_back(n);
            bar.voices[0].beats.push_back(beat);
        }
        if (solo)
            for (int i = 0; i < 8; i++) {
                Beat beat;
                beat.duration = 16;
                Note n;
                n.string = 2;
                n.fret = 13 + (i + b) % 5;
                beat.notes.push_back(n);
                bar.voices[0].beats.push_back(beat);
            }
        t.staff.bars.push_back(bar);
    }
    s.tracks.push_back(t);
    Finish(s);
    auto solos = FindSolos(s, false);
    CHECK(!solos.empty());
    if (!solos.empty()) {
        CHECK(solos[0].start >= 7 && solos[0].start <= 9);
        CHECK(solos[0].end >= 13 && solos[0].end <= 15);
    }
    auto l = FindLick(s, 7);
    CHECK(l.has_value());
    if (l) {
        // A whole phrase, from the top of the solo: bars 8 to 11.
        CHECK_EQ(l->startBar, 8);
        CHECK_EQ(l->bars, 4);
        CHECK_EQ(l->notes, 64);
    }
    // An extra riff bar before the solo (as a bar of open Ds before one):
    // the phrase starts with the solo, not on the song's count of four.
    Score odd = s;
    odd.masterBars.insert(odd.masterBars.begin(), odd.masterBars[0]);
    odd.tracks[0].staff.bars.insert(odd.tracks[0].staff.bars.begin(), odd.tracks[0].staff.bars[0]);
    for (size_t i = 0; i < odd.masterBars.size(); i++) odd.masterBars[i].index = int(i);
    Finish(odd);
    auto ol = FindLick(odd, 7);
    CHECK(ol.has_value());
    if (ol) {
        CHECK_EQ(ol->soloStart, 9);
        CHECK_EQ(ol->startBar, 9);
        CHECK_EQ(ol->bars, 4);
    }

    // Named sections: a solo counts, the bars after one don't.
    Score named = s;
    named.masterBars[8].hasSection = true;
    named.masterBars[8].section = "Post solo";
    auto ns = FindSolos(named);
    CHECK(!ns.empty() && !ns[0].labelled);
    named.masterBars[8].section = "Guitar Solo";
    ns = FindSolos(named);
    CHECK(!ns.empty() && ns[0].labelled);
    // No solo in a riff-only song.
    Score riff = s;
    riff.masterBars.resize(8);
    riff.tracks[0].staff.bars.resize(8);
    CHECK(!FindLick(riff, 7).has_value());
    // A tremolo riff (one low note picked over and over) isn't a solo, even
    // in a section the tab names as one.
    auto riffSong = [](int bars, int noteFret, bool tremolo) {
        Score r;
        r.tempo = 160;
        Track t;
        t.program = 30;
        t.staff.tuning = {64, 59, 55, 50, 45, 40};
        for (int b = 0; b < bars; b++) {
            r.masterBars.push_back(MasterBar{});
            Bar bar;
            bar.voices.emplace_back();
            for (int i = 0; i < 16; i++) {
                Beat beat;
                beat.duration = 16;
                Note n;
                n.string = 2;
                n.fret = tremolo ? (i % 8 == 0 ? noteFret + 2 : noteFret) : noteFret + i % 4;
                n.palmMute = !tremolo;
                beat.notes.push_back(n);
                bar.voices[0].beats.push_back(beat);
            }
            t.staff.bars.push_back(bar);
        }
        r.tracks.push_back(t);
        return r;
    };
    Score trem = riffSong(16, 3, true);
    trem.masterBars[4].hasSection = true;
    trem.masterBars[4].section = "Solo";
    Finish(trem);
    CHECK(FindSolos(trem).empty());
    // A solo named in the tab's last section counts for 16 bars, not to the
    // end of the song.
    Score open = riffSong(40, 7, false);
    open.masterBars[0].hasSection = true;
    open.masterBars[0].section = "Solo";
    Finish(open);
    auto os = FindSolos(open);
    CHECK(!os.empty() && os[0].end <= 17);

    // A solo marked done is left out.
    CHECK(!FindLick(s, 7, {{9, 12}}).has_value());
    CHECK(FindLick(s, 7, {{0, 4}}).has_value());
}

void testKept() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "dailylick-tests-kept";
    fs::remove_all(dir);
    fs::create_directories(dir);
    State s;
    SavedLick a;
    a.path = "a/Song One.gp5";
    a.title = "Song One";
    a.startBar = 8;
    a.bars = 4;
    a.soloStart = 8;
    a.soloEnd = 14;
    s.favourites.push_back({a, "2026-10-19"});
    s.done.push_back({a, "2026-10-20"});
    CHECK(SaveState(dir.u8string(), s));
    State r = LoadState(dir.u8string());
    CHECK_EQ(r.favourites.size(), size_t(1));
    CHECK_EQ(r.done.size(), size_t(1));
    if (!r.favourites.empty()) {
        CHECK(SameLick(r.favourites[0].lick, a));
        CHECK_EQ(r.favourites[0].date, std::string("2026-10-19"));
        CHECK_EQ(r.favourites[0].lick.title, std::string("Song One"));
    }
    // Done goes by the song's solo, in any version of the file.
    auto bars = DoneBars(r, "b/song_one (ver 2).gpx");
    CHECK_EQ(bars.size(), size_t(1));
    if (!bars.empty()) CHECK(bars[0] == std::make_pair(8, 14));
    CHECK(DoneBars(r, "a/Song Two.gp5").empty());
    SavedLick other = a;
    other.path = "b/Song One.gpx";
    other.startBar = 12;
    CHECK(SameSolo(a, other) && !SameLick(a, other));
    other.soloStart = 20;
    other.soloEnd = 28;
    CHECK(!SameSolo(a, other));

    // Daily's toggles keep the lists.
    Daily d(dir.u8string(), Config{});
    CHECK(!d.ToggleFavourite(a, "2026-10-21"));  // was on: now off
    CHECK(d.Favourites().empty());
    CHECK(d.ToggleFavourite(a, "2026-10-21"));
    CHECK(!d.ToggleDone(a, "2026-10-21"));
    CHECK(d.Done().empty());
    State saved = LoadState(dir.u8string());
    CHECK(saved.done.empty() && saved.favourites.size() == 1);
    fs::remove_all(dir);
}

void testPhrases() {
    Score s;
    for (int b = 0; b < 30; b++) s.masterBars.emplace_back();
    CHECK_EQ(PhraseBars(s.masterBars[0]), 4);
    // Counted in fours from the top, then again from a section marker.
    s.masterBars[10].hasSection = true;
    auto p = PhraseStarts(s, 0, 30);
    CHECK((p == std::vector<int>{0, 4, 8, 10, 14, 18, 22, 26}));
    CHECK((PhraseStarts(s, 15, 24) == std::vector<int>{15, 19, 23}));
    // 2/4 bars go in eights; a change of time signature restarts the count.
    for (int b = 20; b < 30; b++) s.masterBars[size_t(b)].numerator = 2;
    CHECK_EQ(PhraseBars(s.masterBars[20]), 8);
    CHECK((PhraseStarts(s, 14, 30) == std::vector<int>{14, 18, 20, 28}));
    MasterBar long7;
    long7.numerator = 7;
    CHECK_EQ(PhraseBars(long7), 4);
    long7.numerator = 8;
    long7.denominator = 4;
    CHECK_EQ(PhraseBars(long7), 2);
}

void testPlayer() {
    auto sf2 = ReadFile("res/sonivox.sf2");
    CHECK(!sf2.empty());
    if (sf2.empty()) return;
    Score s = ReadGpif(kGpif);
    Player p;
    CHECK(p.OpenOffline(sf2.data(), sf2.size()));
    p.Load(BuildSequence(s, 1, 2));  // 4 s
    p.SetLoop(true);
    p.SetSpeed(2.0);
    p.Play();
    std::vector<float> buf(2 * 1024);
    float peak = 0;
    for (int i = 0; i < 44100 * 5 / 1024; i++) {  // 5 s at double speed: round once, then partway
        p.Render(buf.data(), 1024);
        for (float x : buf) peak = std::max(peak, std::fabs(x));
    }
    CHECK_EQ(p.Loops(), 2);
    CHECK(peak > 0.01f && peak < 1.0f);
    CHECK(p.Playing());
    p.SetLoop(false);
    for (int i = 0; i < 44100 * 3 / 1024; i++) p.Render(buf.data(), 1024);
    CHECK(!p.Playing());

    // A count-in when the metronome's on.
    p.Stop();
    p.SetClick(true);
    p.Play();
    CHECK(p.Position() < 0);

    // Transposing: one held A (220 Hz) comes out an octave up at +12.
    auto pitch = [&](int semitones) {
        Sequence q;
        Channel c;
        c.program = 0;
        q.channels.push_back(c);
        Event on;
        on.type = Event::On;
        on.key = 57;
        on.velocity = 1;
        q.events.push_back(on);
        q.length = 2;
        Player pl;
        pl.OpenOffline(sf2.data(), sf2.size());
        pl.Load(q);
        pl.SetTranspose(semitones);
        pl.Play();
        std::vector<float> out(2 * 44100 / 2);
        pl.Render(out.data(), 44100 / 2);
        // Count upward zero crossings over the second half of it (harmonics
        // add some, the same at either pitch).
        int crossings = 0;
        for (size_t i = 44100 / 4; i + 1 < out.size() / 2; i++)
            if (out[2 * i] <= 0 && out[2 * i + 2] > 0) crossings++;
        return crossings * 4.0;  // a quarter of a second
    };
    double plain = pitch(0), up = pitch(12);
    CHECK(plain > 0);
    CHECK(up / plain > 1.9 && up / plain < 2.1);
}

}  // namespace

int main() {
    testSongKey();
    testHashAndRng();
    testTuning();
    testDates();
    testCandidates();
    testGpif();
    testText();
    testLoadScoreRejects();
    testSequence();
    testLick();
    testPhrases();
    testKept();
    testPlayer();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
