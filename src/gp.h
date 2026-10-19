// Guitar Pro readers: GP3, GP4 and GP5 (binary), GPX (Guitar Pro 6) and
// .gp (Guitar Pro 7 and 8), the last two holding the same score.gpif XML.
// Ported from alphaTab's importers.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "model.h"

namespace dl {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// LoadScore reads any of the formats, telling them apart by content.
// It throws FormatError if it can't.
Score LoadScore(const std::vector<uint8_t>& data);

// The readers, for tests. Each throws FormatError.
Score ReadGp3To5(const std::vector<uint8_t>& data);
Score ReadGpx(const std::vector<uint8_t>& data);
Score ReadGp7(const std::vector<uint8_t>& data);
Score ReadGpif(const std::string& xml);

// ToUtf8 turns text from a file into UTF-8: kept if it's valid UTF-8
// already, else read as Windows-1252 (or Windows-1251 for text that's
// mostly Cyrillic), as older Guitar Pro files are.
std::string ToUtf8(const std::string& bytes);

}  // namespace dl
