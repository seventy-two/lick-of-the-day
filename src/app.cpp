#include "app.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "resource.h"
#include "tabview.h"

namespace dl {
namespace {

ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// kitchensink's colours, so the two sit together on screen.
struct Palette {
    ImVec4 bg, card, border, text, muted, accent, hover, frame, accentText;
};
const Palette kDark = {hex(0x15171C), hex(0x1E2128), hex(0x2B2F38), hex(0xE6E8EC), hex(0x8C929D),
                       hex(0x5AA8FF), hex(0x30353F), hex(0x262A33), hex(0xFFFFFF)};
const Palette kLight = {hex(0xF2F3F5), hex(0xFFFFFF), hex(0xD5D9DF), hex(0x1F2328), hex(0x656D76),
                        hex(0x0969DA), hex(0xE4E7EB), hex(0xEEF0F3), hex(0xFFFFFF)};
const Palette* pal = &kDark;

ImU32 col(const ImVec4& v, float a = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * a)); }

namespace icon {
const char* Play = "\xEE\x9D\xA8";      // U+E768
const char* Pause = "\xEE\x9D\xA9";     // U+E769
const char* Minimise = "\xEE\xA4\xA1";  // U+E921
const char* Close = "\xEE\xA2\xBB";     // U+E8BB
}  // namespace icon

std::string known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = std::filesystem::path(p).u8string();
    CoTaskMemFree(p);
    return out;
}

std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// fit shortens text with an ellipsis to fit width.
std::string fit(const std::string& s, ImFont* f, float size, float width) {
    if (f->CalcTextSizeA(size, 1e9f, 0, s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty()) {
        // Drop a whole UTF-8 character.
        size_t n = t.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) n--;
        t.resize(n);
        if (f->CalcTextSizeA(size, 1e9f, 0, (t + "\xE2\x80\xA6").c_str()).x <= width) return t + "\xE2\x80\xA6";
    }
    return t;
}

std::string formatDate(const std::string& iso) {
    int y, m, d;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return iso;
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // Zeller-style weekday: Sakamoto's method.
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = m < 3 ? y - 1 : y;
    int wd = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %d %s", days[wd], d, months[(m - 1) % 12]);
    return buf;
}

}  // namespace

App::App(HWND hwnd, std::function<void()> wake) : hwnd_(hwnd), wake_(std::move(wake)) {
    std::string appData = known(FOLDERID_RoamingAppData);
    dir_ = (std::filesystem::u8path(appData) / "dailylick").u8string();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir_), ec);
    std::string docs = known(FOLDERID_Documents), downloads = known(FOLDERID_Downloads);
    cfg_ = LoadConfig(dir_, {(std::filesystem::u8path(docs) / "guitar").u8string(), downloads});
    SaveConfig(dir_, cfg_);  // so every setting is there to edit
    daily_ = std::make_unique<Daily>(dir_, cfg_);
    speed_ = cfg_.ramping ? cfg_.ramp.start : cfg_.speed;

    // The soundfont is in the exe.
    if (HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SOUNDFONT), RT_RCDATA)) {
        HGLOBAL g = LoadResource(nullptr, r);
        const auto* p = static_cast<const uint8_t*>(LockResource(g));
        soundfont_.assign(p, p + SizeofResource(nullptr, r));
    }
    if (soundfont_.empty() || !player_.Open(soundfont_.data(), soundfont_.size(), playerError_)) {
        if (playerError_.empty()) playerError_ = "the soundfont is missing";
    }
    date_ = today();
    startFind(false);
}

App::~App() {
    if (worker_.joinable()) worker_.join();
}

std::string App::today() const {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return buf;
}

void App::startFind(bool skip) {
    if (searching_) return;
    if (worker_.joinable()) worker_.join();
    searching_ = true;
    player_.Stop();
    std::string date = date_;
    worker_ = std::thread([this, date, skip] {
        if (skip) daily_->Skip(date);
        auto f = daily_->Find(date, [this](const std::string& name) {
            std::lock_guard<std::mutex> lock(mu_);
            progress_ = name;
            wake_();
        });
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_ = std::move(f);
            pendingDone_ = true;
            progress_.clear();
        }
        searching_ = false;
        wake_();
    });
}

void App::Pump() {
    std::optional<Found> f;
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (pendingDone_) {
            done = true;
            pendingDone_ = false;
            f = std::move(pending_);
            pending_.reset();
        }
    }
    if (done) {
        if (f) {
            message_.clear();
            show(std::move(*f));
        } else {
            lick_.reset();
            message_ = daily_->Message();
        }
    }
    // A new day: a new lick, once nothing's playing.
    if (!searching_ && !player_.Playing() && today() != date_) {
        date_ = today();
        whole_ = false;
        startFind(false);
    }
    // The ramp: a step faster each time round.
    if (player_.Playing() && cfg_.ramping) {
        int loops = player_.Loops();
        if (loops > loopsSeen_) {
            loopsSeen_ = loops;
            if (speed_ < cfg_.ramp.end) {
                speed_ = std::min(cfg_.ramp.end, speed_ + cfg_.ramp.step);
                applySpeed();
            } else {
                upToSpeed_ = true;
            }
        }
    }
}

void App::show(Found f) {
    lick_ = std::move(f);
    whole_ = false;
    tabScroll_ = 0;
    loadSequence();
}

void App::loadSequence() {
    if (!lick_) return;
    const auto& l = lick_->lick;
    int a = whole_ ? l.soloStart : l.startBar, b = whole_ ? l.soloEnd : l.startBar + l.bars;
    player_.Load(BuildSequence(*lick_->score, a, b));
    player_.SetSolo(cfg_.band ? -1 : l.track);
    player_.SetClick(cfg_.click);
    player_.SetLoop(cfg_.loop);
    speed_ = cfg_.ramping ? cfg_.ramp.start : cfg_.speed;
    applySpeed();
    loopsSeen_ = 0;
    upToSpeed_ = false;
}

void App::applySpeed() { player_.SetSpeed(speed_ / 100.0); }

void App::saveConfig() { SaveConfig(dir_, cfg_); }

void App::playPause() {
    if (!lick_ || !playerError_.empty()) return;
    if (player_.Playing()) {
        player_.Pause();
        return;
    }
    if (player_.Position() == 0 && player_.Loops() == 0) {
        // From the top: the ramp starts again.
        loopsSeen_ = 0;
        upToSpeed_ = false;
        if (cfg_.ramping) {
            speed_ = cfg_.ramp.start;
            applySpeed();
        }
    }
    player_.Play();
}

void App::toggle(bool& v) {
    v = !v;
    if (&v == &cfg_.loop) {
        player_.SetLoop(cfg_.loop);
        if (!cfg_.loop && cfg_.ramping) cfg_.ramping = false;
    }
    if (&v == &cfg_.click) player_.SetClick(cfg_.click);
    if (&v == &cfg_.band && lick_) player_.SetSolo(cfg_.band ? -1 : lick_->lick.track);
    if (&v == &cfg_.ramping) {
        if (cfg_.ramping && !cfg_.loop) {
            cfg_.loop = true;
            player_.SetLoop(true);
        }
        speed_ = cfg_.ramping ? cfg_.ramp.start : cfg_.speed;
        loopsSeen_ = player_.Loops();
        upToSpeed_ = false;
        applySpeed();
    }
    saveConfig();
}

bool App::Animating() const { return player_.Playing() || searching_; }

bool App::Place(RECT& r) const {
    if (!cfg_.placed) return false;
    r = RECT{cfg_.x, cfg_.y, cfg_.x + cfg_.w, cfg_.y + cfg_.h};
    return true;
}

void App::SavePlace(const RECT& r) {
    cfg_.placed = true;
    cfg_.x = r.left;
    cfg_.y = r.top;
    cfg_.w = r.right - r.left;
    cfg_.h = r.bottom - r.top;
    saveConfig();
}

bool App::InCaption(float x, float y) const {
    if (x < panelMin_.x || y < panelMin_.y || x >= panelMax_.x || y >= panelMax_.y) return false;
    for (const auto& [a, b] : hot_)
        if (x >= a.x && y >= a.y && x < b.x && y < b.y) return false;
    return true;
}

void App::SetScale(float scale) {
    scale_ = scale;
    fontsDirty_ = true;
}

void App::SetDark(bool dark) {
    dark_ = dark;
    pal = dark ? &kDark : &kLight;
}

void App::ReloadFonts() {
    fontsDirty_ = false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar text[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2212, 0x2212, 0x25A0, 0x25FF, 0};
    static const ImWchar icons[] = {0xE700, 0xF8FF, 0};
    std::string regular = fontPath("segoeui.ttf"), semibold = fontPath("seguisb.ttf");
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    if (!exists(semibold)) semibold = regular;
    auto add = [&](const std::string& path, float px, bool withIcons) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = exists(path) ? io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale_, &cfg, text) : nullptr;
        if (!f) {
            ImFontConfig def;
            def.SizePixels = px * scale_;
            return io.Fonts->AddFontDefault(&def);
        }
        if (withIcons && exists(iconFont)) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = ImVec2(0, 1.5f * scale_);
            io.Fonts->AddFontFromFileTTF(iconFont.c_str(), px * scale_ * 0.9f, &merge, icons);
        }
        return f;
    };
    body_ = add(regular, 15, true);
    small_ = add(regular, 13, true);
    title_ = add(semibold, 17, false);
    fret_ = add(semibold, 16, false);
    label_ = add(semibold, 12, false);
    io.FontDefault = body_;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(5, 5);
    s.FrameRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.ScrollbarSize = 8;
    s.ScrollbarRounding = 4;
    s.ScaleAllSizes(scale_);
}

bool App::button(const char* label, bool on, bool accent, const char* tip) {
    const Palette& p = *pal;
    ImVec4 base = accent ? p.accent : on ? ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.18f) : p.frame;
    ImVec4 hover = accent ? ImVec4(p.accent.x * 1.1f, p.accent.y * 1.1f, p.accent.z * 1.1f, 1) : on ? ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.28f) : p.hover;
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover);
    ImGui::PushStyleColor(ImGuiCol_Text, accent ? p.accentText : on ? p.accent : p.text);
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

bool App::link(const char* label, bool on, const char* tip) {
    const Palette& p = *pal;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.15f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? p.accent : p.muted);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * scale_, 2 * scale_));
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

void App::Frame() {
    hot_.clear();
    const Palette& p = *pal;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = p.bg;
    ImGui::GetStyle().Colors[ImGuiCol_Text] = p.text;
    ImGui::GetStyle().Colors[ImGuiCol_PopupBg] = p.card;
    ImGui::GetStyle().Colors[ImGuiCol_Border] = p.border;
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrab] = p.border;
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrabHovered] = p.muted;

    // Keys, unless a modifier's held.
    if (!io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) playPause();
        if (ImGui::IsKeyPressed(ImGuiKey_L, false)) toggle(cfg_.loop);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) toggle(cfg_.ramping);
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) toggle(cfg_.click);
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) toggle(cfg_.band);
        if (ImGui::IsKeyPressed(ImGuiKey_S, false) && lick_) {
            whole_ = !whole_;
            tabScroll_ = 0;
            loadSequence();
        }
        int delta = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? -5 : ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 5 : 0;
        if (delta) {
            speed_ = std::clamp(speed_ + delta, 25, 150);
            if (!cfg_.ramping) {
                cfg_.speed = speed_;
                saveConfig();
            }
            applySpeed();
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("dailylick", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    const float panelW = std::min(380 * scale_, io.DisplaySize.x * 0.45f);
    drawInfo(panelW, io.DisplaySize.y);
    ImGui::SameLine(0, 0);
    drawTab(io.DisplaySize.x - panelW, io.DisplaySize.y);
    ImGui::End();
}

void App::drawInfo(float width, float height) {
    const Palette& p = *pal;
    const float k = scale_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k, 10 * k));
    ImGui::BeginChild("info", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    panelMin_ = ImGui::GetWindowPos();
    panelMax_ = ImVec2(panelMin_.x + width, panelMin_.y + height);
    const float inner = width - 28 * k;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The top line: what this is, the date, and the window's buttons.
    ImGui::PushFont(label_);
    ImGui::TextColored(p.accent, "DAILY LICK");
    ImGui::PopFont();
    ImGui::SameLine(0, 8 * k);
    ImGui::PushFont(small_);
    ImGui::TextColored(p.muted, "%s", formatDate(date_).c_str());
    ImGui::PopFont();
    {
        float bw = 26 * k, bh = 20 * k;
        ImVec2 start(panelMax_.x - 8 * k - 2 * bw, panelMin_.y + 6 * k);
        const char* icons[2] = {icon::Minimise, icon::Close};
        for (int i = 0; i < 2; i++) {
            ImGui::SetCursorScreenPos(ImVec2(start.x + i * bw, start.y));
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, i == 1 ? hex(0xC42B1C) : p.hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, i == 1 ? hex(0xC42B1C) : p.hover);
            ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
            ImGui::PushFont(small_);
            bool pressed = ImGui::Button(icons[i], ImVec2(bw, bh));
            ImGui::PopFont();
            ImGui::PopStyleColor(4);
            ImGui::PopID();
            hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            if (pressed) PostMessageW(hwnd_, i == 0 ? WM_SYSCOMMAND : WM_CLOSE, i == 0 ? SC_MINIMIZE : 0, 0);
        }
        ImGui::SetCursorScreenPos(ImVec2(panelMin_.x + 14 * k, panelMin_.y + 10 * k + ImGui::GetTextLineHeightWithSpacing()));
    }

    // The song and where the lick is in it.
    if (lick_) {
        const auto& l = lick_->lick;
        ImGui::PushFont(title_);
        std::string t = fit(l.title, title_, title_->FontSize, inner);
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopFont();
        if (!l.artist.empty()) {
            ImGui::PushFont(small_);
            ImGui::TextColored(p.muted, "%s", fit(l.artist, small_, small_->FontSize, inner).c_str());
            ImGui::PopFont();
        }
        std::string meta = l.trackName;
        auto add = [&](const std::string& s) {
            if (s.empty()) return;
            if (!meta.empty()) meta += "  \xC2\xB7  ";
            meta += s;
        };
        int a = whole_ ? l.soloStart : l.startBar, b = whole_ ? l.soloEnd : l.startBar + l.bars;
        add(b - a == 1 ? "bar " + std::to_string(a + 1) : "bars " + std::to_string(a + 1) + "\xE2\x80\x93" + std::to_string(b));
        std::string section = l.section;
        while (!section.empty() && (section.back() == ' ' || section.back() == ':')) section.pop_back();
        add(section.empty() ? "" : section.substr(section.find_first_not_of(' ') == std::string::npos ? 0 : section.find_first_not_of(' ')));
        add(std::to_string(l.tempo) + " bpm");
        add(l.tuning);
        ImGui::PushFont(small_);
        ImGui::TextColored(p.muted, "%s", fit(meta, small_, small_->FontSize, inner).c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", l.path.c_str());
    } else {
        ImGui::PushFont(title_);
        ImGui::TextColored(p.muted, "%s", searching_ ? "Finding today's lick\xE2\x80\xA6" : "No lick today");
        ImGui::PopFont();
    }

    // The controls, along the bottom.
    const float rowH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(panelMin_.x + 14 * k, panelMax_.y - 10 * k - rowH - 6 * k - small_->FontSize - 6 * k));
    ImGui::BeginDisabled(!lick_ || !playerError_.empty());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * k, 4 * k));
    if (button(player_.Playing() ? icon::Pause : icon::Play, false, true, "Play or pause (Space)")) playPause();
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (button("Loop", cfg_.loop, false, "Round and round (L)")) toggle(cfg_.loop);
    ImGui::SameLine();
    if (button("Ramp", cfg_.ramping, false, "Start slow and speed up a step each time round (R)")) toggle(cfg_.ramping);
    ImGui::SameLine();
    {
        // The speed: − 100% +
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        auto change = [&](int d) {
            speed_ = std::clamp(speed_ + d, 25, 150);
            if (!cfg_.ramping) {
                cfg_.speed = speed_;
                saveConfig();
            }
            applySpeed();
        };
        if (button("\xE2\x88\x92", false, false, "Slower (\xE2\x86\x90)")) change(-5);
        ImGui::SameLine();
        char sp[16];
        std::snprintf(sp, sizeof(sp), "%d%%", speed_);
        ImGui::PushStyleColor(ImGuiCol_Button, p.frame);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.frame);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.frame);
        ImGui::PushStyleColor(ImGuiCol_Text, cfg_.ramping ? p.accent : p.text);
        ImGui::Button(sp, ImVec2(46 * k, 0));
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
        if (button("+", false, false, "Faster (\xE2\x86\x92)")) change(5);
        ImGui::PopStyleVar();
    }
    ImGui::SameLine();
    if (button("Click", cfg_.click, false, "Metronome, with a bar counted in (M)")) toggle(cfg_.click);
    ImGui::SameLine();
    if (button("Band", cfg_.band, false, "Play the other parts too (B)")) toggle(cfg_.band);

    // Status, and the less used things.
    ImGui::SetCursorScreenPos(ImVec2(panelMin_.x + 14 * k, panelMax_.y - 10 * k - small_->FontSize - 4 * k));
    ImGui::PushFont(small_);
    std::string status;
    if (!playerError_.empty()) {
        status = "No sound: " + playerError_;
    } else if (searching_) {
        std::lock_guard<std::mutex> lock(mu_);
        status = progress_.empty() ? "Looking through the tabs\xE2\x80\xA6" : "Trying " + progress_ + "\xE2\x80\xA6";
    } else if (!lick_) {
        status = message_;
    } else {
        int loops = player_.Loops();
        if (cfg_.ramping) {
            if (upToSpeed_ || speed_ >= cfg_.ramp.end) status = "Up to speed";
            else status = "Ramp " + std::to_string(cfg_.ramp.start) + "\xE2\x86\x92" + std::to_string(cfg_.ramp.end) + "%, +" + std::to_string(cfg_.ramp.step) + " a time";
        }
        if (cfg_.loop && (player_.Playing() || loops > 0)) {
            std::string lp = "Loop " + std::to_string(loops + 1);
            status = status.empty() ? lp : lp + "  \xC2\xB7  " + status;
        }
        if (status.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d notes, %.1fs at full speed", lick_->lick.notes, lick_->lick.secs);
            status = buf;
        }
    }
    // The links go on the right; the status gets what's left.
    const char* wholeLabel = "Whole solo";
    const char* anotherLabel = "Another";
    const char* openLabel = "Open";
    float linksW = 0;
    for (const char* s : {wholeLabel, anotherLabel, openLabel}) linksW += ImGui::CalcTextSize(s).x + 12 * k + 2 * k;
    float y = ImGui::GetCursorPosY();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(p.muted, "%s", fit(status, small_, small_->FontSize, inner - linksW - 8 * k).c_str());
    ImGui::SameLine(14 * k + inner - linksW);
    ImGui::SetCursorPosY(y - 2 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2 * k, 0));
    ImGui::BeginDisabled(!lick_);
    if (link(wholeLabel, whole_, "Show and play the whole solo (S)")) {
        whole_ = !whole_;
        tabScroll_ = 0;
        loadSequence();
    }
    ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(searching_);
    if (link(anotherLabel, false, "A different lick for today")) startFind(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!lick_);
    if (link(openLabel, false, "Open the tab in Guitar Pro or TuxGuitar") && lick_) {
        std::wstring w = std::filesystem::u8path(lick_->lick.path).wstring();
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    (void)dl;

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::drawTab(float width, float height) {
    const Palette& p = *pal;
    const float k = scale_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.bg);
    ImGui::BeginChild("tab", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!lick_) {
        std::string m = searching_ ? "" : message_;
        ImVec2 ts = ImGui::CalcTextSize(m.c_str());
        ImGui::SetCursorPos(ImVec2((width - ts.x) / 2, (height - ts.y) / 2));
        ImGui::TextColored(p.muted, "%s", m.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }
    const auto& l = lick_->lick;
    int a = whole_ ? l.soloStart : l.startBar, b = whole_ ? l.soloEnd : l.startBar + l.bars;
    const float pad = 24 * k;
    const float scrollH = ImGui::GetStyle().ScrollbarSize;
    TabStyle st;
    st.text = col(p.text);
    st.muted = col(p.muted);
    st.line = col(p.muted, 0.45f);
    st.accent = col(p.accent);
    st.bg = col(p.bg);
    st.fret = fret_;
    st.fretSize = fret_->FontSize;
    st.label = small_;
    st.labelSize = label_->FontSize * 1.05f;
    st.scale = k;
    int tick = -1;
    double pos = player_.Position();
    if ((player_.Playing() || pos > 0) && pos >= 0) tick = player_.Seq().SecToTick(pos);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.x += pad;
    TabResult r = DrawTab(dl, origin, ImVec2(width - 2 * pad, height - scrollH), *lick_->score, l.track, a, b, tick, st);
    ImGui::Dummy(ImVec2(r.width + 2 * pad, height - scrollH - 1));
    // Keep the cursor in view, and let the wheel scroll sideways.
    float visible = width;
    if (r.cursorX >= 0 && player_.Playing()) {
        float cx = r.cursorX + pad;
        float sx = ImGui::GetScrollX();
        if (cx < sx + visible * 0.1f || cx > sx + visible * 0.8f) ImGui::SetScrollX(std::max(0.0f, cx - visible * 0.25f));
    }
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0)
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 80 * k);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace dl
