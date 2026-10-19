// dump prints what the readers make of Guitar Pro files, in the same
// shape as tools/dump.mjs prints alphaTab's, to compare the two.
#include <cstdio>
#include <fstream>
#include <iterator>

#include "gp.h"

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
        printf("FILE %s\n", argv[i]);
        try {
            dl::Score s = dl::LoadScore(data);
            printf("bars %zu tempo %g\n", s.masterBars.size(), s.tempo);
            for (auto& mb : s.masterBars)
                if (mb.hasSection) printf("section %d %s\n", mb.index, mb.section.c_str());
            for (auto& t : s.tracks) {
                if (t.staff.percussion) {
                    printf("track %d perc\n", t.index);
                    continue;
                }
                printf("track %d prog %d strings %zu capo %d\n", t.index, t.program, t.staff.tuning.size(),
                       t.staff.capo);
                for (size_t b = 0; b < t.staff.bars.size(); b++) {
                    std::string p;
                    auto& bar = t.staff.bars[b];
                    if (!bar.voices.empty())
                        for (auto& beat : bar.voices[0].beats) {
                            if (!p.empty()) p += ' ';
                            if (beat.IsRest()) {
                                p += "r" + std::to_string(beat.duration);
                                continue;
                            }
                            p += (beat.grace != dl::Grace::None ? std::string("g") : std::to_string(beat.duration)) + ":";
                            for (size_t k = 0; k < beat.notes.size(); k++)
                                p += (k ? "," : "") + std::to_string(beat.notes[k].string) + "/" +
                                     std::to_string(beat.notes[k].fret);
                        }
                    printf("bar %d.%zu %s\n", t.index, b, p.c_str());
                }
            }
        } catch (const std::exception& e) {
            printf("ERR %s\n", e.what());
        }
    }
}
