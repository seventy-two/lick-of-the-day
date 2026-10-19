#include "daily.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "gp.h"
#include "lick.h"

namespace dl {
namespace {

long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::shared_ptr<const Score> load(const std::string& path) {
    try {
        auto data = ReadFile(path);
        if (data.empty()) return nullptr;
        return std::make_shared<const Score>(LoadScore(data));
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

std::string DescribeTuning(const std::vector<int>& tuning, int capo) {
    static const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    static const int standard[] = {64, 59, 55, 50, 45, 40, 35, 30, 25};
    auto note = [&](int midi) { return std::string(names[((midi % 12) + 12) % 12]); };
    const int n = int(tuning.size());
    std::string name;
    bool isStandard6 = n == 6;
    for (int i = 0; i < n && i < 9; i++) isStandard6 &= tuning[size_t(i)] == standard[i];
    if (n >= 6 && n <= 9) {
        int shift = tuning[0] - standard[0];
        bool upper = true;
        for (int i = 0; i < n - 1; i++) upper &= tuning[size_t(i)] - standard[i] == shift;
        int low = tuning[size_t(n - 1)] - standard[n - 1] - shift;
        std::string prefix = n == 6 ? "" : std::to_string(n) + "-string ";
        if (upper && low == 0)
            name = shift == 0 ? (n == 6 ? "" : std::to_string(n) + "-string") : prefix + note(standard[n - 1] + shift) + " standard";
        else if (upper && low == -2)
            name = prefix + "Drop " + note(tuning[size_t(n - 1)]);
    }
    if (name.empty() && !isStandard6 && n > 0) {
        name = "Tuning";
        for (int i = n - 1; i >= 0; i--) name += " " + note(tuning[size_t(i)]);
    }
    if (capo > 0) name += (name.empty() ? "" : ", ") + std::string("capo ") + std::to_string(capo);
    return name;
}

int TopString(const std::vector<int>& tuning, int capo) { return tuning.empty() ? 64 : tuning[0] + capo; }

int TransposeFor(const std::vector<int>& tuning, int capo, int guitarTop) {
    if (guitarTop <= 0) return 0;
    return std::clamp(guitarTop - TopString(tuning, capo), -12, 12);
}

std::string ShiftedTuningName(const std::vector<int>& tuning, int semitones) {
    std::vector<int> t = tuning;
    for (int& s : t) s += semitones;
    std::string name = DescribeTuning(t, 0);
    if (name.empty()) return "E standard";
    if (name.rfind("Tuning ", 0) == 0) return name.substr(7);
    return name;
}

Daily::Daily(std::string dir, Config cfg) : dir_(std::move(dir)), cfg_(std::move(cfg)) { state_ = LoadState(dir_); }

void Daily::scanIfStale() {
    if (!files_.empty() && nowSeconds() - scannedAt_ < 3600) return;
    files_ = Scan(cfg_.folders);
    scannedAt_ = nowSeconds();
}

std::string Daily::Message() const {
    std::lock_guard<std::mutex> lock(mu_);
    return message_;
}

size_t Daily::Files() const {
    std::lock_guard<std::mutex> lock(mu_);
    return files_.size();
}

std::optional<Found> Daily::Open(const SavedLick& lick) {
    auto score = load(lick.path);
    std::lock_guard<std::mutex> lock(mu_);
    message_.clear();
    if (!score || lick.track >= int(score->tracks.size()) || lick.startBar + lick.bars > int(score->masterBars.size()) ||
        lick.soloEnd > int(score->masterBars.size())) {
        message_ = "Can't read " + std::filesystem::u8path(lick.path).filename().u8string() + " any more.";
        return std::nullopt;
    }
    return Found{lick, score};
}

std::vector<Kept> Daily::History() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Kept> out;
    for (auto it = state_.days.rbegin(); it != state_.days.rend(); ++it)
        if (it->second.lick) out.push_back({*it->second.lick, it->first});
    return out;
}

std::vector<Kept> Daily::Favourites() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_.favourites;
}

std::vector<Kept> Daily::Done() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_.done;
}

bool Daily::ToggleFavourite(const SavedLick& lick, const std::string& date) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& fs = state_.favourites;
    auto it = std::find_if(fs.begin(), fs.end(), [&](const Kept& k) { return SameLick(k.lick, lick); });
    bool on = it == fs.end();
    if (on) fs.push_back({lick, date});
    else fs.erase(it);
    SaveState(dir_, state_);
    return on;
}

bool Daily::ToggleDone(const SavedLick& lick, const std::string& date) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& ds = state_.done;
    auto it = std::find_if(ds.begin(), ds.end(), [&](const Kept& k) { return SameSolo(k.lick, lick); });
    bool on = it == ds.end();
    if (on) ds.push_back({lick, date});
    else ds.erase(it);
    SaveState(dir_, state_);
    return on;
}

void Daily::Skip(const std::string& date) {
    std::lock_guard<std::mutex> lock(mu_);
    Day& day = state_.days[date];
    if (!day.lick) return;
    day.skipped.push_back(day.lick->path);
    day.lick.reset();
    SaveState(dir_, state_);
}

std::optional<Found> Daily::Find(const std::string& date, const std::function<void(const std::string&)>& progress) {
    std::unique_lock<std::mutex> lock(mu_);
    message_.clear();
    Day& day = state_.days[date];
    if (day.lick) {
        auto path = day.lick->path;
        lock.unlock();
        auto score = load(path);
        lock.lock();
        Day& d = state_.days[date];
        if (score && d.lick && d.lick->track < int(score->tracks.size()) &&
            d.lick->startBar + d.lick->bars <= int(score->masterBars.size()))
            return Found{*d.lick, score};
        // The file has gone or changed: choose again.
        d.lick.reset();
    }
    scanIfStale();
    const uint32_t seed = Hash(date + "|" + std::to_string(state_.days[date].skipped.size()));
    auto candidates = Candidates(files_, date + "|" + std::to_string(seed), Recent(state_, date, cfg_.repeatDays),
                                 state_.noSolo, 60);
    if (files_.empty()) {
        message_ = "No Guitar Pro files found. Add folders to config.json.";
        return std::nullopt;
    }
    std::map<std::string, std::vector<std::pair<int, int>>> done;
    for (const auto& c : candidates) done[c.path] = DoneBars(state_, c.path);
    lock.unlock();
    std::vector<std::string> noSolo;
    std::optional<Found> found;
    for (const auto& c : candidates) {
        if (progress) progress(std::filesystem::u8path(c.path).filename().u8string());
        auto score = load(c.path);
        std::optional<Lick> l;
        const auto& skip = done[c.path];
        if (score) l = FindLick(*score, seed, skip);
        if (!l) {
            // Only remember it has no solo if that's so with none done.
            if (!score || skip.empty() || !FindLick(*score, seed)) noSolo.push_back(c.Key());
            continue;
        }
        const Track& t = score->tracks[size_t(l->track)];
        SavedLick s;
        s.path = c.path;
        s.title = score->title.empty() ? std::filesystem::u8path(c.path).stem().u8string() : score->title;
        s.artist = score->artist;
        s.track = l->track;
        s.trackName = t.name;
        s.startBar = l->startBar;
        s.bars = l->bars;
        s.notes = l->notes;
        s.secs = l->secs;
        s.section = l->section;
        s.soloStart = l->soloStart;
        s.soloEnd = l->soloEnd;
        s.tempo = int(std::lround(TempoAt(*score, l->startBar)));
        s.tuning = DescribeTuning(t.staff.tuning, t.staff.capo);
        found = Found{s, score};
        break;
    }
    lock.lock();
    for (auto& k : noSolo) state_.noSolo.insert(k);
    if (found) state_.days[date].lick = found->lick;
    else message_ = "No solos in the next few tabs. Try Another.";
    SaveState(dir_, state_);
    return found;
}

}  // namespace dl
