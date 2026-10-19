// Drawing tab with ImGui: one line of tablature for a range of bars, with
// techniques above the strings and rhythm below, and a playing cursor.
#pragma once

#include "imgui.h"
#include "model.h"

namespace dl {

struct TabStyle {
    ImU32 text, muted, line, accent, bg;
    ImFont* fret = nullptr;   // fret numbers
    float fretSize = 14;
    ImFont* label = nullptr;  // labels: bends, P.M., bar numbers (not "small": windows.h defines that)
    float labelSize = 11;
    float scale = 1;
};

struct TabResult {
    float width = 0;     // the drawing's natural width, to scroll by
    float cursorX = -1;  // where the cursor is, relative to the origin
};

// DrawTab draws bars [first, last) of a track at origin, fitting size's
// height and stretching to fill its width if the bars are short. tick is
// the playing position in the score's ticks, or -1.
TabResult DrawTab(ImDrawList* dl, ImVec2 origin, ImVec2 size, const Score& score, int track, int first, int last,
                  int tick, const TabStyle& st);

}  // namespace dl
