#include "gp.h"

#include <cstring>

namespace dl {

Score LoadScore(const std::vector<uint8_t>& data) {
    if (data.size() >= 4 && (std::memcmp(data.data(), "BCFZ", 4) == 0 || std::memcmp(data.data(), "BCFS", 4) == 0))
        return ReadGpx(data);
    if (data.size() >= 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0) return ReadGp7(data);
    if (data.size() >= 20 && std::memcmp(data.data() + 1, "FICHIER GUITAR PRO", 18) == 0) return ReadGp3To5(data);
    throw FormatError("not a Guitar Pro file");
}

namespace {

bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + size_t(n) >= s.size() + (n == 0 ? 1 : 0)) return false;
        for (int k = 1; k <= n; k++)
            if ((static_cast<unsigned char>(s[i + size_t(k)]) >> 6) != 2) return false;
        i += size_t(n) + 1;
    }
    return true;
}

// Windows-1252's 0x80–0x9F, as Unicode; the rest match Latin-1.
const char16_t kCp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

}  // namespace

std::string ToUtf8(const std::string& bytes) {
    std::string in = bytes;
    while (!in.empty() && in.back() == '\0') in.pop_back();
    if (validUtf8(in)) return in;
    // Text that's mostly letters from 0xC0 up and hardly any ASCII ones is
    // Russian in Windows-1251 (А–я there), which tabs from Russian sites
    // use; anything else is read as Windows-1252.
    int high = 0, ascii = 0;
    for (unsigned char c : in) {
        high += c >= 0xC0;
        ascii += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    const bool cyrillic = high >= 3 && high > ascii * 2;
    std::string out;
    for (unsigned char c : in) {
        unsigned cp = c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : c;
        if (cyrillic) cp = c >= 0xC0 ? 0x410 + (c - 0xC0) : c == 0xA8 ? 0x401 : c == 0xB8 ? 0x451 : cp;
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) {
            out += char(0xC0 | cp >> 6);
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xE0 | cp >> 12);
            out += char(0x80 | (cp >> 6 & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace dl
