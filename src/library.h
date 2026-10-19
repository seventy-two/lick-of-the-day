// The tabs on disk and what dailylick remembers about them: the folders
// searched, the lick chosen each day, files with no solo in them.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dl {

// TabFile is one Guitar Pro file found on disk. Paths are UTF-8.
struct TabFile {
    std::string path;
    int64_t size = 0;
    int64_t modified = 0;  // seconds since 1970
    // song groups versions of one song: "Composure (ver 8 by X).gp5" and
    // "Composure.gpx" share it.
    std::string song;

    // Key identifies the file as it is now; it changes when the file does.
    std::string Key() const;
};

// Scan lists the Guitar Pro files in the folders and their subfolders.
std::vector<TabFile> Scan(const std::vector<std::string>& folders);

// SongKey is a file's song: its name without versions, copies and
// punctuation.
std::string SongKey(const std::string& path);

// ExtRank orders formats best first (newer ones keep more of the tab);
// -1 for files that aren't tabs.
int ExtRank(const std::string& path);

uint32_t Hash(const std::string& s);  // FNV-1a

// Candidates are the files to try, in order, for a day's lick: songs in
// an order fixed by seed, leaving out songs in recent and files in noSolo
// (by Key). Each song gives its two best versions.
std::vector<TabFile> Candidates(const std::vector<TabFile>& files, const std::string& seed,
                                const std::set<std::string>& recent, const std::set<std::string>& noSolo,
                                size_t max);

// A day's lick, as saved.
struct SavedLick {
    std::string path, title, artist, trackName, section, tuning;
    int track = 0, startBar = 0, bars = 0, notes = 0, soloStart = 0, soloEnd = 0, tempo = 0;
    double secs = 0;
};

struct Day {
    std::optional<SavedLick> lick;
    std::vector<std::string> skipped;  // paths
};

struct Ramp {
    int start = 60, step = 5, end = 100;  // percent of the tab's tempo
};

struct Config {
    std::vector<std::string> folders;
    Ramp ramp;
    int repeatDays = 120;  // before a song can come round again
    // The player's settings, kept between runs.
    bool loop = true, ramping = false, click = false, band = false;
    int speed = 100;
    // The top string of the guitar played along on (MIDI), to shift the
    // pitch to suit; 0 plays tabs as written.
    int tuneTo = 0;
    // The window's place, if it's been moved; else it opens at the
    // default place.
    bool placed = false;
    int x = 0, y = 0, w = 0, h = 0;
};

struct State {
    std::map<std::string, Day> days;  // by date, YYYY-MM-DD
    std::set<std::string> noSolo;     // TabFile keys
};

// Load and save the files in dir; missing files give defaults.
Config LoadConfig(const std::string& dir, const std::vector<std::string>& defaultFolders);
bool SaveConfig(const std::string& dir, const Config& c);
State LoadState(const std::string& dir);
bool SaveState(const std::string& dir, const State& s);

// Recent is the songs used on days in the window before date (not date
// itself), plus those skipped on date.
std::set<std::string> Recent(const State& s, const std::string& date, int days);

// DaysBefore gives the date n days before date (both YYYY-MM-DD).
std::string DaysBefore(const std::string& date, int n);

}  // namespace dl
