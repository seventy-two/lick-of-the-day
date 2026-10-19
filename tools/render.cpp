// render plays a file's lick into a WAV, to listen to the synth offline:
// render <soundfont.sf2> <file.gp5> <out.wav> [seed] [semitones]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "gp.h"
#include "lick.h"
#include "player.h"

static std::vector<uint8_t> readFile(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    auto sf2 = readFile(argv[1]);
    dl::Score s = dl::LoadScore(readFile(argv[2]));
    uint32_t seed = argc > 4 ? uint32_t(std::strtoul(argv[4], nullptr, 10)) : 1;
    auto l = dl::FindLick(s, seed);
    if (!l) {
        printf("no lick\n");
        return 1;
    }
    printf("track %d (%s) bars %d-%d, %d notes\n", l->track, s.tracks[size_t(l->track)].name.c_str(), l->startBar + 1,
           l->startBar + l->bars, l->notes);
    dl::Player p;
    if (!p.OpenOffline(sf2.data(), sf2.size())) return 1;
    p.Load(dl::BuildSequence(s, l->startBar, l->startBar + l->bars));
    p.SetSolo(l->track);
    p.SetTranspose(argc > 5 ? std::atoi(argv[5]) : 0);
    p.SetClick(true);
    p.SetLoop(true);
    p.Play();
    // Twice round: the second time a little faster, as the ramp would.
    std::vector<float> out;
    std::vector<float> buf(2 * 512);
    int speedups = 0;
    while (p.Loops() < 2 && out.size() < size_t(dl::Player::kRate) * 2 * 60) {
        p.Render(buf.data(), 512);
        out.insert(out.end(), buf.begin(), buf.end());
        if (p.Loops() > speedups) {
            speedups = p.Loops();
            p.SetSpeed(1.0 + 0.25 * speedups);
        }
    }
    FILE* f = fopen(argv[3], "wb");
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(out.size() * 2);
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2);
    u32(dl::Player::kRate); u32(dl::Player::kRate * 4); u16(4); u16(16); fwrite("data", 1, 4, f); u32(bytes);
    for (float x : out) {
        int v = int(x * 32767);
        u16(uint16_t(int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v)));
    }
    fclose(f);
    printf("%.1fs of audio\n", out.size() / 2.0 / dl::Player::kRate);
}
