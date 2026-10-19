// today runs the daily pick for some dates over a folder, as the app
// would: today <state dir> <folder> <date>...
#include <chrono>
#include <cstdio>

#include "daily.h"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    dl::Config cfg;
    cfg.folders = {argv[2]};
    dl::Daily d(argv[1], cfg);
    for (int i = 3; i < argc; i++) {
        auto t0 = std::chrono::steady_clock::now();
        int tried = 0;
        auto f = d.Find(argv[i], [&](const std::string&) { tried++; });
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!f) {
            printf("%s: none (%s) after %d files, %.0f ms\n", argv[i], d.Message().c_str(), tried, ms);
            continue;
        }
        const auto& l = f->lick;
        printf("%s: %s - %s | %s | bars %d-%d of solo %d-%d [%s] %d notes %.1fs %d bpm %s | %d tried, %.0f ms\n", argv[i],
               l.artist.c_str(), l.title.c_str(), l.trackName.c_str(), l.startBar + 1, l.startBar + l.bars,
               l.soloStart + 1, l.soloEnd, l.section.c_str(), l.notes, l.secs, l.tempo, l.tuning.c_str(), tried, ms);
    }
}
