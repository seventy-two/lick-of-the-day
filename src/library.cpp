#include "library.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace dl {
namespace {

const char* const kExts[] = {".gp", ".gpx", ".gp5", ".gp4", ".gp3"};

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

fs::path toPath(const std::string& utf8) { return fs::u8path(utf8); }
std::string fromPath(const fs::path& p) { return p.u8string(); }

bool writeAtomically(const fs::path& path, const std::string& data) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

json readJson(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return json::object();
    try {
        return json::parse(f);
    } catch (const std::exception&) {
        return json::object();
    }
}

// Days since 1970-01-01 for a civil date, and back (Howard Hinnant's
// algorithms).
long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + long(doe) - 719468;
}

void civilFromDays(long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = int(long(yoe) + era * 400 + (m <= 2));
}

}  // namespace

std::string TabFile::Key() const { return path + "|" + std::to_string(size) + "|" + std::to_string(modified); }

int ExtRank(const std::string& path) {
    std::string ext = lower(fromPath(toPath(path).extension()));
    for (int i = 0; i < 5; i++)
        if (ext == kExts[i]) return i;
    return -1;
}

uint32_t Hash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::string SongKey(const std::string& path) {
    static const std::regex version(R"(\((ver(sion)? ?\d+[^)]*|\d+|pro|guitar pro|gp\d?|tab)\))", std::regex::icase);
    static const std::regex nonWord("[^a-z0-9]+");
    std::string name = fromPath(toPath(path).stem());
    name = std::regex_replace(name, version, " ");
    name = std::regex_replace(lower(name), nonWord, " ");
    // Trim, then drop "the" so "The Black Dahlia Murder" matches "black_dahlia_murder".
    size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
    name = a == std::string::npos ? "" : name.substr(a, b - a + 1);
    if (name.rfind("the ", 0) == 0) name = name.substr(4);
    for (size_t at; (at = name.find(" the ")) != std::string::npos;) name.replace(at, 5, " ");
    return name;
}

std::vector<TabFile> Scan(const std::vector<std::string>& folders) {
    std::vector<TabFile> out;
    std::set<std::string> seen;
    for (const auto& folder : folders) {
        std::error_code ec;
        fs::path root = toPath(folder);
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto& entry = *it;
            std::string name = fromPath(entry.path().filename());
            if (entry.is_directory(ec)) {
                if (name.empty() || name[0] == '.' || name == "node_modules" || name == "$RECYCLE.BIN")
                    it.disable_recursion_pending();
                continue;
            }
            std::string path = fromPath(entry.path());
            if (ExtRank(path) < 0 || !entry.is_regular_file(ec) || seen.count(path)) continue;
            auto size = entry.file_size(ec);
            if (ec || size == 0 || size > (20u << 20)) continue;
            auto mod = entry.last_write_time(ec);
            TabFile f;
            f.path = path;
            f.size = int64_t(size);
            f.modified = int64_t(std::chrono::duration_cast<std::chrono::seconds>(mod.time_since_epoch()).count());
            f.song = SongKey(path);
            seen.insert(path);
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<TabFile> Candidates(const std::vector<TabFile>& files, const std::string& seed,
                                const std::set<std::string>& recent, const std::set<std::string>& noSolo,
                                size_t max) {
    std::map<std::string, std::vector<TabFile>> songs;
    for (const auto& f : files)
        if (!recent.count(f.song) && !noSolo.count(f.Key())) songs[f.song].push_back(f);
    std::vector<std::string> keys;
    for (auto& [k, _] : songs) keys.push_back(k);
    std::sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) {
        uint32_t ha = Hash(seed + a), hb = Hash(seed + b);
        return ha != hb ? ha < hb : a < b;
    });
    std::vector<TabFile> out;
    for (const auto& k : keys) {
        auto& fs = songs[k];
        std::sort(fs.begin(), fs.end(), [](const TabFile& a, const TabFile& b) {
            int ra = ExtRank(a.path), rb = ExtRank(b.path);
            if (ra != rb) return ra < rb;
            if (a.size != b.size) return a.size > b.size;
            return a.path < b.path;
        });
        for (size_t i = 0; i < fs.size() && i < 2; i++) {
            out.push_back(fs[i]);
            if (out.size() >= max) return out;
        }
    }
    return out;
}

// --- config and state ---

namespace {

SavedLick lickFromJson(const json& l) {
    SavedLick sl;
    sl.path = l.value("path", "");
    sl.title = l.value("title", "");
    sl.artist = l.value("artist", "");
    sl.trackName = l.value("trackName", "");
    sl.section = l.value("section", "");
    sl.tuning = l.value("tuning", "");
    sl.track = l.value("track", 0);
    sl.startBar = l.value("startBar", 0);
    sl.bars = l.value("bars", 0);
    sl.notes = l.value("notes", 0);
    sl.soloStart = l.value("soloStart", 0);
    sl.soloEnd = l.value("soloEnd", 0);
    sl.tempo = l.value("tempo", 0);
    sl.secs = l.value("secs", 0.0);
    return sl;
}

json lickToJson(const SavedLick& l) {
    return {{"path", l.path},           {"title", l.title},         {"artist", l.artist},
            {"trackName", l.trackName}, {"section", l.section},     {"tuning", l.tuning},
            {"track", l.track},         {"startBar", l.startBar},   {"bars", l.bars},
            {"notes", l.notes},         {"soloStart", l.soloStart}, {"soloEnd", l.soloEnd},
            {"tempo", l.tempo},         {"secs", l.secs}};
}

}  // namespace

bool SameLick(const SavedLick& a, const SavedLick& b) {
    return a.path == b.path && a.track == b.track && a.startBar == b.startBar && a.bars == b.bars;
}

bool SameSolo(const SavedLick& a, const SavedLick& b) {
    return (a.path == b.path || SongKey(a.path) == SongKey(b.path)) && a.soloStart < b.soloEnd && b.soloStart < a.soloEnd;
}

std::vector<std::pair<int, int>> DoneBars(const State& s, const std::string& path) {
    std::vector<std::pair<int, int>> out;
    const std::string song = SongKey(path);
    for (const auto& k : s.done)
        if (k.lick.path == path || SongKey(k.lick.path) == song) out.push_back({k.lick.soloStart, k.lick.soloEnd});
    return out;
}

Config LoadConfig(const std::string& dir, const std::vector<std::string>& defaultFolders) {
    Config c;
    c.folders = defaultFolders;
    json j = readJson(toPath(dir) / "config.json");
    auto get = [&](const char* k, auto& v) {
        if (j.contains(k)) try {
                j.at(k).get_to(v);
            } catch (const std::exception&) {
            }
    };
    get("folders", c.folders);
    if (j.contains("ramp") && j["ramp"].is_object()) {
        auto& r = j["ramp"];
        c.ramp.start = r.value("start", c.ramp.start);
        c.ramp.step = r.value("step", c.ramp.step);
        c.ramp.end = r.value("end", c.ramp.end);
    }
    if (c.ramp.start < 25 || c.ramp.end > 150 || c.ramp.end < c.ramp.start || c.ramp.step < 1 || c.ramp.step > 50)
        c.ramp = Ramp{};
    get("repeatDays", c.repeatDays);
    get("loop", c.loop);
    get("ramping", c.ramping);
    get("click", c.click);
    get("band", c.band);
    get("speed", c.speed);
    c.speed = std::clamp(c.speed, 25, 150);
    get("tuneTo", c.tuneTo);
    get("panel", c.panel);
    get("shown", c.shown);
    if (c.tuneTo < 0 || c.tuneTo > 127) c.tuneTo = 0;
    if (j.contains("window") && j["window"].is_object()) {
        auto& w = j["window"];
        c.x = w.value("x", 0);
        c.y = w.value("y", 0);
        c.w = w.value("w", 0);
        c.h = w.value("h", 0);
        c.placed = c.w >= 200 && c.h >= 80;
    }
    return c;
}

bool SaveConfig(const std::string& dir, const Config& c) {
    json j = {{"folders", c.folders},
              {"ramp", {{"start", c.ramp.start}, {"step", c.ramp.step}, {"end", c.ramp.end}}},
              {"repeatDays", c.repeatDays},
              {"loop", c.loop},
              {"ramping", c.ramping},
              {"click", c.click},
              {"band", c.band},
              {"speed", c.speed},
              {"tuneTo", c.tuneTo},
              {"panel", c.panel},
              {"shown", c.shown}};
    if (c.placed) j["window"] = {{"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}};
    return writeAtomically(toPath(dir) / "config.json", j.dump(1));
}

State LoadState(const std::string& dir) {
    State s;
    json j = readJson(toPath(dir) / "state.json");
    try {
        if (j.contains("days"))
            for (auto& [date, d] : j["days"].items()) {
                Day day;
                if (d.contains("lick") && d["lick"].is_object()) {
                    SavedLick sl = lickFromJson(d["lick"]);
                    if (!sl.path.empty() && sl.bars > 0) day.lick = sl;
                }
                if (d.contains("skipped")) d["skipped"].get_to(day.skipped);
                s.days[date] = std::move(day);
            }
        if (j.contains("noSolo")) j["noSolo"].get_to(s.noSolo);
        for (const char* key : {"favourites", "done"}) {
            if (!j.contains(key) || !j[key].is_array()) continue;
            auto& list = std::string(key) == "done" ? s.done : s.favourites;
            for (auto& e : j[key]) {
                if (!e.is_object() || !e.contains("lick")) continue;
                Kept k{lickFromJson(e["lick"]), e.value("date", "")};
                if (!k.lick.path.empty() && k.lick.bars > 0) list.push_back(std::move(k));
            }
        }
    } catch (const std::exception&) {
    }
    return s;
}

bool SaveState(const std::string& dir, const State& s) {
    json days = json::object();
    for (const auto& [date, d] : s.days) {
        json jd = json::object();
        if (d.lick) jd["lick"] = lickToJson(*d.lick);
        if (!d.skipped.empty()) jd["skipped"] = d.skipped;
        days[date] = jd;
    }
    auto kept = [](const std::vector<Kept>& list) {
        json a = json::array();
        for (const auto& k : list) a.push_back({{"date", k.date}, {"lick", lickToJson(k.lick)}});
        return a;
    };
    json j = {{"days", days}, {"noSolo", s.noSolo}, {"favourites", kept(s.favourites)}, {"done", kept(s.done)}};
    return writeAtomically(toPath(dir) / "state.json", j.dump(1));
}

std::string DaysBefore(const std::string& date, int n) {
    int y = 1970;
    unsigned m = 1, d = 1;
    if (std::sscanf(date.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return date;
    civilFromDays(daysFromCivil(y, m, d) - n, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
}

std::set<std::string> Recent(const State& s, const std::string& date, int days) {
    std::set<std::string> out;
    std::string cutoff = DaysBefore(date, days);
    for (const auto& [d, day] : s.days) {
        if (d == date) {
            for (const auto& p : day.skipped) out.insert(SongKey(p));
        } else if (d >= cutoff && d < date && day.lick) {
            out.insert(SongKey(day.lick->path));
        }
    }
    return out;
}

}  // namespace dl
