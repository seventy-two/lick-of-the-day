// licks prints the lick dailylick would pick in each file, to compare
// with the JavaScript prototype the heuristic was tuned in.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "gp.h"
#include "lick.h"

int main(int argc, char** argv) {
    uint32_t seed = uint32_t(std::strtoul(argv[1], nullptr, 10));
    for (int i = 2; i < argc; i++) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
        printf("FILE %s\n", argv[i]);
        try {
            dl::Score s = dl::LoadScore(data);
            auto l = dl::FindLick(s, seed);
            if (!l) printf("none\n");
            else printf("lick t%d bar %d+%d notes %d secs %.1f solo %d-%d\n", l->track, l->startBar, l->bars, l->notes, l->secs, l->soloStart, l->soloEnd);
        } catch (const std::exception& e) {
            printf("ERR\n");
        }
    }
}
