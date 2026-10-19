// The window's contents: the day's lick, its tab, and the player's
// controls. Drawn with Dear ImGui each frame.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "daily.h"
#include "imgui.h"
#include "player.h"
#include "sequence.h"

namespace dl {

class App {
public:
    App(HWND hwnd, std::function<void()> wake);
    ~App();

    void Frame();
    // Pump handles work finished off the UI thread; call it every loop.
    void Pump();

    // InCaption reports whether a point in client pixels drags the window.
    bool InCaption(float x, float y) const;
    // Animating is true while frames should be drawn continuously.
    bool Animating() const;

    void SetScale(float scale);
    void SetDark(bool dark);
    bool FontsDirty() const { return fontsDirty_; }
    void ReloadFonts();

    // The window's place, from the config, if it's been placed before.
    bool Place(RECT& r) const;
    void SavePlace(const RECT& r);

private:
    void startFind(bool skip);
    void show(Found f);
    void loadSequence();
    void playPause();
    void toggle(bool& v);
    void applySpeed();
    void applyPitch();
    void changePitch(int delta);  // 0 goes back to as written
    void changeRamp();
    void saveConfig();
    std::string today() const;

    void drawInfo(float width, float height);
    void drawTab(float width, float height);
    bool button(const char* label, bool on = false, bool accent = false, const char* tip = nullptr);
    bool link(const char* label, bool on = false, const char* tip = nullptr);

    HWND hwnd_;
    std::function<void()> wake_;
    std::string dir_;
    Config cfg_;
    std::unique_ptr<Daily> daily_;
    std::vector<uint8_t> soundfont_;
    Player player_;
    std::string playerError_;

    // The search runs on a thread; it leaves its result here.
    std::thread worker_;
    std::mutex mu_;
    std::optional<Found> pending_;
    bool pendingDone_ = false;
    std::string progress_;
    std::atomic<bool> searching_{false};

    std::optional<Found> lick_;
    std::string date_;  // the day shown
    std::string message_;
    bool whole_ = false;  // the whole solo, not just the lick
    int speed_ = 100;     // percent, now; the ramp changes it
    int loopsSeen_ = 0;
    bool upToSpeed_ = false;
    int transpose_ = 0;   // semitones, from cfg_.tuneTo and the tab's tuning

    float scale_ = 1;
    bool dark_ = true;
    bool fontsDirty_ = true;
    ImFont* body_ = nullptr;
    ImFont* small_ = nullptr;
    ImFont* title_ = nullptr;
    ImFont* fret_ = nullptr;
    ImFont* label_ = nullptr;

    // Where the panel is and its buttons, from the last frame, for dragging.
    ImVec2 panelMin_, panelMax_;
    std::vector<std::pair<ImVec2, ImVec2>> hot_;
    float tabScroll_ = 0;
};

}  // namespace dl
