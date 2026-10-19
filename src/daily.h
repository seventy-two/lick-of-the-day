// The day's lick: finding it (or loading the one already chosen), and
// skipping to another. Portable; the Windows app runs it on a thread.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "library.h"
#include "model.h"

namespace dl {

struct Found {
    SavedLick lick;
    std::shared_ptr<const Score> score;
};

class Daily {
public:
    // Daily keeps its state in dir.
    Daily(std::string dir, Config cfg);

    // Find gives the lick for date: the one already chosen if its file
    // still reads, else the first solo found among the day's candidates.
    // progress is told each file tried. Empty if there's no tab with a
    // solo; Message says why.
    std::optional<Found> Find(const std::string& date, const std::function<void(const std::string&)>& progress = {});

    // Skip drops date's lick, so the next Find chooses another.
    void Skip(const std::string& date);

    // Open loads a lick kept from before (a favourite, or a past day's).
    // Empty if its file no longer reads; Message says why.
    std::optional<Found> Open(const SavedLick& lick);

    // History is each day's lick, newest first, with its date.
    std::vector<Kept> History() const;
    std::vector<Kept> Favourites() const;
    std::vector<Kept> Done() const;
    // Each toggle adds the lick (kept on date) or takes it off, and says
    // which. Done goes by the solo: any lick of it counts.
    bool ToggleFavourite(const SavedLick& lick, const std::string& date);
    bool ToggleDone(const SavedLick& lick, const std::string& date);

    std::string Message() const;
    size_t Files() const;

private:
    void scanIfStale();

    std::string dir_;
    Config cfg_;
    mutable std::mutex mu_;
    State state_;
    std::vector<TabFile> files_;
    long long scannedAt_ = 0;
    std::string message_;
};

// DescribeTuning names a tuning ("Drop D", "Eb standard", "7-string") from
// a staff's strings, top first; standard six-string tuning gives "".
std::string DescribeTuning(const std::vector<int>& tuning, int capo);

// Pitch shifting, to play along on a guitar tuned differently from the
// tab. The guitar is remembered by its top string's note (MIDI), so the
// same guitar gets the right shift for any tab: a D standard guitar (62)
// needs +3 for a B standard tab and -2 for an E standard one.

// TopString is the note the tab's top string sounds, capo and all.
int TopString(const std::vector<int>& tuning, int capo);
// TransposeFor is the shift for a guitar whose top string is guitarTop (0
// for none: as written), between -12 and 12.
int TransposeFor(const std::vector<int>& tuning, int capo, int guitarTop);
// ShiftedTuningName names the tuning the tab sounds in once shifted:
// "D standard", "Drop C", and "E standard" rather than "".
std::string ShiftedTuningName(const std::vector<int>& tuning, int semitones);

// ReadFile reads a whole file (path in UTF-8).
std::vector<uint8_t> ReadFile(const std::string& path);

}  // namespace dl
